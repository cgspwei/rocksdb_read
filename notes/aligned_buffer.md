# `util/aligned_buffer.h` 文件解读

这个文件提供了对齐缓冲区管理的工具类，主要用于 **Direct I/O** 场景。核心思想是：操作系统要求 Direct I/O 的缓冲区地址和大小都必须对齐到磁盘扇区/页面边界，否则 I/O 请求会失败。

---

## 一、三个辅助函数

### `TruncateToPageBoundary` (L26-30)

```cpp
s -= (s & (page_size - 1));
```

将 `s` 向下截断到 page_size 的整数倍。利用了位运算技巧——当 `page_size` 是 2 的幂时，`s & (page_size - 1)` 取得余数，减去余数即截断。等价于 `s - s % page_size`。

### `Roundup` (L36) / `Rounddown` (L42)

向上/向下取整到 `y` 的倍数。`Roundup` 用于分配时计算对齐后的容量，`Rounddown` 用于计算对齐边界。

---

## 二、`AlignedBuffer` 类（L58-254）

这是文件的核心类，管理一个**起始地址和大小都对齐**的缓冲区。

### 成员变量

| 变量 | 含义 |
|------|------|
| `alignment_` | 对齐要求（如 4096，必须是 2 的幂） |
| `buf_` | 底层内存的智能指针（`FSAllocationPtr`，带自定义 deleter） |
| `capacity_` | 对齐后的可用容量 |
| `cursize_` | 当前已使用大小 |
| `bufstart_` | 指向对齐后起始地址的指针 |

**关键设计**：`buf_` 指向原始分配的内存，`bufstart_` 指向其中第一个对齐的位置。两者可能不同——因为 `new[]` 返回的地址不一定满足对齐要求。

### 核心方法：`AllocateNewBuffer`（L147-180）

这是理解整个类的关键：

```cpp
size_t new_capacity = Roundup(requested_capacity, alignment_);      // ① 容量向上对齐
char* new_buf = new char[new_capacity + alignment_];                // ② 多分配 alignment_ 字节
char* new_bufstart = reinterpret_cast<char*>(
    (reinterpret_cast<uintptr_t>(new_buf) + (alignment_ - 1)) &
    ~static_cast<uintptr_t>(alignment_ - 1));                       // ③ 找到第一个对齐地址
```

**内存布局**：

```
new_buf          new_bufstart                    new_buf + new_capacity + alignment_
|                 |                                                               |
v                 v                                                               v
[  padding  ][          aligned buffer (capacity_)          ][  tail padding  ]
             ^                                                  ^
             |________________ capacity_ 可用空间 ________________|
```

- ② 多分配 `alignment_` 字节是为了保证无论如何都能在 `[new_buf, new_buf + alignment_]` 范围内找到一个对齐地址
- ③ 是经典的指针对齐算法：`(addr + align - 1) & ~(align - 1)`，相当于向上取整到 alignment 的倍数

`copy_data` 参数支持在扩容时将旧数据拷贝到新缓冲区，**避免重新从磁盘读取**——这是性能关键点。

### 其他方法

| 方法 | 作用 |
|------|------|
| `Append` (L190) | 追加数据，不超过剩余容量 |
| `Read` (L207) | 从指定偏移读取数据 |
| `PadToAlignmentWith` (L221) | 用填充字节将当前大小补齐到对齐边界 |
| `RefitTail` (L239) | 部分刷写后，将尾部数据移到缓冲区开头 |
| `SetBuffer` (L121) | 接管外部已分配的缓冲区（如文件系统返回的），避免额外分配 |
| `Release` (L104) | 释放所有权，返回底层智能指针 |

---

## 三、`GrowableBuffer` 类（L258-321）

一个轻量的可增长缓冲区，类似于 `std::string` 但**避免初始化清零**。

### 与 `AlignedBuffer` 的区别

| 特性 | AlignedBuffer | GrowableBuffer |
|------|--------------|----------------|
| 对齐支持 | ✅ 严格对齐 | ❌ 无对齐 |
| 主要用途 | Direct I/O | 通用数据缓冲 |
| 分配方式 | `new[]` + 手动对齐 | `malloc` |
| 增长策略 | 手动指定新容量 | 2 倍扩容 |

### `ResetForSize` 中的缓存预热（L306-309）

```cpp
for (size_t i = 0; i < new_capacity; i += CACHE_LINE_SIZE) {
    data_[i] = 1;
}
```

这段代码**故意触碰每个 cache line 的第一个字节**，将新分配的内存预加载到 CPU 缓存。这是一种典型的性能优化——避免后续首次访问时产生 page fault 或 cache miss。这正好体现了 Performance Considerations 中 CPU Cache Efficiency 的原则。

### `malloc_usable_size`（L301-302）

当平台支持时，使用 `malloc_usable_size` 获取实际分配的可用大小（通常比请求的大，因为 malloc 实现会有自己的对齐/粒度），避免浪费额外空间。

---

## 总结

这个文件体现了 RocksDB 性能哲学的多个方面：

1. **内存分配优化**：`AlignedBuffer` 通过一次 `new[]` + 手动对齐，避免了使用 `posix_memalign` 等平台特定 API 的依赖；`GrowableBuffer` 用 `malloc` 避免了 `std::string` 的零初始化开销
2. **避免重复 I/O**：`AllocateNewBuffer` 的 `copy_data` 选项允许扩容时保留旧数据，避免重新读磁盘
3. **CPU 缓存友好**：`GrowableBuffer` 的缓存预热、`AlignedBuffer` 的 `RefitTail` 避免频繁分配
4. **Direct I/O 支持**：整个 `AlignedBuffer` 类就是为 Direct I/O 而设计的，确保缓冲区地址和大小都满足内核对齐要求
