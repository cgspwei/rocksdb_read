# Slice 与 Arena 深度解析

## 1. Slice：零拷贝的字符串视图

### 1.1 设计动机

RocksDB 的核心操作是比较和传递 key/value 数据。如果每次传递都拷贝 `std::string`，会产生大量堆分配和内存拷贝。Slice 的设计目标很简单：**只观察，不拥有**。

```cpp
class Slice {
  const char* data_;  // 指向外部存储，不拥有
  size_t size_;       // 数据长度
  // Intentionally copyable
};
```

`sizeof(Slice) = 16`（一个指针 + 一个 size_t），拷贝代价极低。与 `std::string` 的对比：

| 操作 | std::string | Slice |
|------|------------|-------|
| 拷贝 | 堆分配 + memcpy | 16 字节指针拷贝 |
| 赋值 | 可能重分配 | 16 字节指针赋值 |
| 生命周期 | 自管理（RAII） | 非拥有，依赖外部 |
| 可修改 | 是 | 否（const char*） |

### 1.2 隐式构造函数的设计取舍

Slice 提供了从 `const char*`、`std::string`、`std::string_view` 的隐式构造函数：

```cpp
/* implicit */ Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
/* implicit */ Slice(const char* s) : data_(s) { size_ = (s == nullptr) ? 0 : strlen(s); }
/* implicit */ Slice(const std::string_view& sv) : data_(sv.data()), size_(sv.size()) {}
```

隐式构造让 API 更简洁（`func("hello")` 而不需要 `func(Slice("hello"))`），但也带来风险——临时 `std::string` 析构后 Slice 悬空：

```cpp
// 危险：临时 string 析构后，Slice 悬空
Slice s = std::string("temp");  // s 指向已析构的临时对象

// 安全：string 存活期间 Slice 有效
std::string str = "persistent";
Slice s = str;  // OK，str 仍存活
```

### 1.3 compare() 与 RocksDB 的字节序

RocksDB 的 key 排序是**字节序**（lexicographic），不是自然序。`compare()` 是最核心的操作，出现在每次 SkipList 查找和 SST 文件搜索中：

```cpp
inline int Slice::compare(const Slice& b) const {
  const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
  int r = memcmp(data_, b.data_, min_len);  // 先比较公共前缀
  if (r == 0) {
    if (size_ < b.size_) r = -1;   // 短的是前缀，排前面
    else if (size_ > b.size_) r = +1;
  }
  return r;
}
```

**性能关键点**：`memcmp` 在现代 CPU 上会被编译为 SIMD 指令（SSE/AVX），对齐的内存访问更快。这就是 Arena 的 `AllocateAligned` 存在的意义之一。

### 1.4 remove_prefix / remove_suffix：O(1) 切片

```cpp
void remove_prefix(size_t n) {
  assert(n <= size());
  data_ += n;      // 仅移动指针
  size_ -= n;
}

void remove_suffix(size_t n) {
  assert(n <= size());
  size_ -= n;      // 仅缩小长度
}
```

这在解析 InternalKey 时极为高效。InternalKey = UserKey + 8字节 PackSequenceAndType，解包只需：

```cpp
Slice user_key(key.data(), key.size() - 8);       // remove_suffix 语义
Slice trailer(key.data() + key.size() - 8, 8);     // remove_prefix 语义
```

### 1.5 Slice vs std::string_view

RocksDB 自研 Slice 而非用 C++17 的 `string_view`，原因有三：

1. **历史性**：Slice 继承自 LevelDB（2011 年），远早于 `string_view`（C++17, 2017 年）
2. **功能扩展**：Slice 有 `compare()`、`starts_with()`、`ends_with()`、`difference_offset()`、`DecodeHex()` 等 `string_view` 没有的方法
3. **PinnableSlice 体系**：`PinnableSlice : public Slice, public Cleanable` 的继承体系无法用 `string_view` 替代

RocksDB 在 `Slice` 中提供了与 `string_view` 的互操作桥梁：

```cpp
Slice(const std::string_view& sv) : data_(sv.data()), size_(sv.size()) {}
std::string_view ToStringView() const { return std::string_view(data_, size_); }
```

### 1.6 PinnableSlice：零拷贝读取的关键

PinnableSlice 是读路径避免 memcpy 的核心机制。它有两种工作模式：

```
                     PinnableSlice
                    /            \
             PinSlice()        PinSelf()
            （零拷贝）         （拷贝）
               │                  │
        指向外部内存          拷贝到内部 string
        注册 Cleanup          data_ 指向 buf_
        释放时触发清理         不依赖外部生命周期
```

**PinSlice（零拷贝路径）**——用于 SST 文件的 Block Cache 命中：

```cpp
void PinSlice(const Slice& s, CleanupFunction f, void* arg1, void* arg2) {
  pinned_ = true;
  data_ = s.data();       // 直接指向 block cache 中的数据块
  size_ = s.size();
  RegisterCleanup(f, arg1, arg2);  // 注册清理函数，释放 block cache 引用
}
```

数据直接指向 Block Cache 中被 pin 住的内存块，Cleanup 函数负责在 PinnableSlice 析构时释放 Block Cache 的引用。全程零拷贝。

**PinSelf（拷贝路径）**——用于 MemTable 命中：

```cpp
void PinSelf(const Slice& slice) {
  buf_->assign(slice.data(), slice.size());  // 拷贝到内部 std::string
  data_ = buf_->data();
  size_ = buf_->size();
}
```

MemTable 的数据存储在 Arena 中，Arena 的生命周期由 MemTable 的引用计数管理。如果零拷贝引用 Arena 内存，MemTable 可能在用户消费数据前被 flush 并释放，导致悬空指针。所以 MemTable 命中时必须拷贝。

**Merge 操作的启发式决策**：

```cpp
// 合并操作数总大小 >= 32KB 且平均 >= 256 字节 → 零拷贝
if (total_bytes >= 32768 && avg_bytes >= 256) {
  merge_operands->PinSlice(sl, nullptr);  // 零拷贝，依赖 SuperVersion 引用计数
} else {
  merge_operands->PinSelf(sl);            // 拷贝
}
```

### 1.7 OptSlice：比 optional\<Slice\> 更高效

```cpp
class OptSlice {
  Slice slice_;  // size_ == SIZE_MAX 表示"无值"
public:
  bool has_value() const noexcept { return slice_.size() != SIZE_MAX; }
};
```

`sizeof(OptSlice) = sizeof(Slice) = 16`，与 `sizeof(std::optional<Slice>) = 24` 相比省了 8 字节。在 RocksDB 的读路径上，OptSlice 被频繁构造和传递，8 字节的差距在高频调用下有意义。利用 `SIZE_MAX` 这个"不可能的 Slice 长度"来编码空状态，是一种典型的 data packing 技巧。

---

## 2. Arena：批量分配，整体释放

### 2.1 设计动机

传统 `new/malloc` 的问题在 RocksDB 的写路径上尤为突出：

- 每次 `new` 是一次系统调用 + 数据结构开销（glibc malloc 约 32 字节元数据）
- MemTable 一次写入需要分配 key + value + SkipList 节点，频繁的 malloc/free 导致内存碎片
- 每次写路径都需要 `delete`，但 MemTable 的生命周期是批量管理的（flush 时整体释放）

Arena 的核心思路：**预分配大块内存，从中线性切分；不需要单独 free，整体释放**。

### 2.2 内存布局

```
Arena 内存布局：

┌─────────────────────────────────────────────────────────────────┐
│ inline_block_[2048]  （栈上/对象内嵌，首次分配免堆申请）         │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ aligned_alloc_ptr_ → [已对齐分配]  |  [剩余空间] ← unaligned │
│  └──────────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│ blocks_[0] (kBlockSize=4096)  ← 堆分配                         │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ aligned_alloc → [已分配] | [空闲] ← unaligned_alloc      │   │
│  └──────────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│ blocks_[1] ... blocks_[N]                                      │
├─────────────────────────────────────────────────────────────────┤
│ huge_blocks_[0] ...  ← mmap + MAP_HUGETLB（可选）              │
└─────────────────────────────────────────────────────────────────┘
```

### 2.3 双向分配：对齐从左、非对齐从右

Arena 最精妙的设计之一是对齐分配和非对齐分配从同一块的两端向中间生长：

```cpp
// 对齐分配：从左往右
char* aligned_alloc_ptr_;

// 非对齐分配：从右往左
char* unaligned_alloc_ptr_;

// 中间剩余空间
size_t alloc_bytes_remaining_;
```

```
block 起始地址                                              block 结束地址
│                                                            │
▼                                                            ▼
┌─────────────────┬──────────────────────┬───────────────────┐
│ aligned_allocs  │   剩余空间            │  unaligned_allocs │
│ (从左往右生长)   │  alloc_bytes_remaining_ │  (从右往左生长)   │
└─────────────────┴──────────────────────┴───────────────────┘
▲                                                            ▲
aligned_alloc_ptr_                                unaligned_alloc_ptr_
```

**为什么？** 如果只从一端分配，每次对齐分配都需要 padding 浪费空间。两端分配让对齐分配天然对齐（从对齐的起始地址开始），非对齐分配也从对齐的末尾地址开始（减去需要的字节数），两种分配的 padding 损失都最小化。

Allocate 的实现：

```cpp
char* Arena::Allocate(size_t bytes) {
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    unaligned_alloc_ptr_ -= bytes;       // 从右端减
    alloc_bytes_remaining_ -= bytes;
    return unaligned_alloc_ptr_;
  }
  return AllocateFallback(bytes, false);
}
```

AllocateAligned 的实现：

```cpp
char* Arena::AllocateAligned(size_t bytes, ...) {
  // 计算当前 aligned_alloc_ptr_ 需要多少 padding
  size_t current_mod =
      reinterpret_cast<uintptr_t>(aligned_alloc_ptr_) & (kAlignUnit - 1);
  size_t slop = (current_mod == 0 ? 0 : kAlignUnit - current_mod);
  size_t needed = bytes + slop;
  if (needed <= alloc_bytes_remaining_) {
    result = aligned_alloc_ptr_ + slop;    // 跳过 padding
    aligned_alloc_ptr_ += needed;           // 从左端加
    alloc_bytes_remaining_ -= needed;
    return result;
  }
  return AllocateFallback(bytes, true);
}
```

### 2.4 内联块：小 MemTable 的零堆分配优化

```cpp
static constexpr size_t kInlineSize = 2048;
alignas(std::max_align_t) char inline_block_[kInlineSize];
```

Arena 内嵌了 2048 字节的 inline block。初始化时直接使用它：

```cpp
Arena::Arena(...) {
  alloc_bytes_remaining_ = sizeof(inline_block_);  // 2048
  aligned_alloc_ptr_ = inline_block_;
  unaligned_alloc_ptr_ = inline_block_ + alloc_bytes_remaining_;
}
```

对于小型 MemTable（例如空或只有少量写入），这 2048 字节就够了，**完全避免堆分配**。只有在 inline block 耗尽后才分配第一个堆块。

### 2.5 大块分配策略：1/4 阈值

```cpp
char* Arena::AllocateFallback(size_t bytes, bool aligned) {
  if (bytes > kBlockSize / 4) {
    // 超过块大小的 1/4，单独分配一个"不规则块"
    ++irregular_block_num;
    return AllocateNewBlock(bytes);
  }
  // 否则，浪费当前块剩余空间，分配新块
  size = kBlockSize;
  block_head = AllocateNewBlock(size);
  alloc_bytes_remaining_ = size - bytes;
  // ...
}
```

1/4 阈值的权衡：
- 如果大请求占用当前新块的一部分，剩余空间浪费率高（例如块 4096 字节，请求 2048 字节，浪费 50%）
- 单独分配不规则块让当前新块保持完整，后续小请求仍可高效使用
- 不规则块由 `irregular_block_num` 统计，用于评估 Arena 的碎片化程度

### 2.6 Huge Page 支持

```cpp
char* Arena::AllocateFromHugePage(size_t bytes) {
  MemMapping mm = MemMapping::AllocateHuge(bytes);  // mmap + MAP_HUGETLB
  auto addr = static_cast<char*>(mm.Get());
  if (addr) {
    huge_blocks_.push_back(std::move(mm));
    // ...
  }
  return addr;
}
```

Huge Page（2MB 或 1GB 页面）减少 TLB miss，对大规模 MemTable 的性能有显著影响。Arena 在分配新块时优先尝试 Huge Page，失败则回退到普通 malloc。需要系统预留 Huge Page：

```bash
sysctl -w vm.nr_hugepages=20
```

### 2.7 AllocTracker：与 WriteBufferManager 联动

```cpp
class AllocTracker {
  WriteBufferManager* write_buffer_manager_;
  std::atomic<size_t> bytes_allocated_;
};
```

Arena 的每次分配都通过 `AllocTracker` 向 `WriteBufferManager` 报告。WriteBufferManager 用于控制所有 MemTable 的总内存使用——当超过阈值时触发 flush。这让 Arena 不是孤立的内存池，而是与全局内存管控机制联动的。

### 2.8 析构：整体释放的威力

```cpp
Arena::~Arena() {
  if (tracker_ != nullptr) {
    tracker_->FreeMem();
  }
  // blocks_ 和 huge_blocks_ 的 unique_ptr / MemMapping 自动释放
}
```

Arena 析构时不需要逐个释放内部对象。`blocks_` 中所有 `unique_ptr<char[]>` 一起释放，`huge_blocks_` 中所有 `MemMapping` 一起 unmap。几万次 Allocate 只需要几次（等于块数）free/munmap，而不是几万次。这就是"批量分配，整体释放"的威力。

---

## 3. ConcurrentArena：多线程友好的 Arena

### 3.1 为什么不能直接用 Arena

MemTable 的写操作是多线程的（多个 write thread 并发写入），但 Arena 的 `Allocate` 和 `AllocateFallback` 没有任何同步机制。直接加全局锁会引入严重竞争。

### 3.2 分片设计

```
ConcurrentArena 内存层次：

Thread 0 (cpu=0) ──→ Shard 0 ──→ ┌─────────────┐
Thread 1 (cpu=1) ──→ Shard 1 ──→ │  本地缓存    │──→ Arena（主分配器）
Thread 2 (cpu=2) ──→ Shard 2 ──→ │  SpinMutex   │
Thread 3 (cpu=3) ──→ Shard 3 ──→ └─────────────┘
   ...                              ↑
                                    │ Shard 空间不足时，从 Arena 批量获取
```

每个 Shard 有：
- 40 字节 padding（避免 false sharing）
- 一个 SpinMutex（比 std::mutex 轻量得多）
- 一个 `free_begin_` 指针和 `allocated_and_unused_` 计数

**分配流程**：

```
1. 请求 bytes 字节
2. bytes > shard_block_size / 4？
   → 是：直接从 Arena 分配（大请求不值得走 shard）
   → 否：找本 CPU 对应的 Shard
3. Shard 有足够空间？
   → 是：从 Shard 的 free_begin_ 切出
   → 否：从 Arena 批量获取一个 shard_block_size 大块，放入 Shard
4. 从 Shard 切出返回
```

**竞争时的 Repick**：如果目标 Shard 的 SpinMutex 被 hold，线程会尝试另一个 Shard（`Repick()`），而不是自旋等待。这减少了热点 Shard 的竞争。

### 3.3 对齐/非对齐分配的兼容

ConcurrentArena 的 Shard 层也采用了双向分配策略：

```cpp
if ((bytes % sizeof(void*)) == 0) {
  rv = s->free_begin_;           // 对齐：从左端
  s->free_begin_ += bytes;
} else {
  rv = s->free_begin_ + avail - bytes;  // 非对齐：从右端
}
```

与底层 Arena 一致，最大化空间利用率。

---

## 4. Slice + Arena 协作：写路径完整流程

MemTable::Add() 是 Slice 和 Arena 协作的典型场景：

```
用户调用 DB::Put(key, value)
        │
        ▼
MemTable::Add(sequence, type, key, value)
        │
        ▼
1. 计算 encoded_len = key_size + value_size + 元数据开销
        │
        ▼
2. table->Allocate(encoded_len, &buf)
   → ConcurrentArena::Allocate()
     → Shard 切分 或 Arena 新块
   → 返回 buf（Arena 内存）
        │
        ▼
3. 填充 buf：
   ┌───────────────────────────────────────────────────────┐
   │ varint32(key_len) │ key_bytes │ pack(seq,type) │ varint32(val_len) │ val_bytes │ checksum │
   └───────────────────────────────────────────────────────┘
   ↑ buf                ↑ key_slice 指向这里     ↑ value 在这里
        │
        ▼
4. Slice key_slice(p, key_size)  ← 零拷贝，直接引用 Arena 内存
        │
        ▼
5. table->Insert(key_handle)     ← SkipList 节点也分配在 Arena 中
        │
        ▼
6. 节点布局（InlineSkipList）：
   ┌─────────────────────────────────────────────────────────┐
   │ next_[-(h-1)] ... next_[-1] │ Node │ key+value (Arena) │
   └─────────────────────────────────────────────────────────┘
   ↑ raw                         ↑ Node*
                                 Key() = &next_[1] → 指向 key 数据
```

**关键点**：
- Key 和 Value 数据**只存在一份**，就在 Arena 中
- Slice 只是这份数据的"视图"，零拷贝传递
- SkipList 节点、key、value 在 Arena 中是**连续分配**的，缓存友好
- 整个 MemTable 只有一个 Arena，flush 时一次性释放

## 5. Slice + Arena 协作：读路径完整流程

### 5.1 MemTable 读取

```
用户调用 DB::Get(key)
        │
        ▼
MemTable::Get() → SkipList 查找
        │
        ▼
1. 迭代器定位到节点 → Node::Key() 返回 Arena 中的 key 指针
        │
        ▼
2. GetLengthPrefixedSlice(Key()) 解码出 user_key
   GetLengthPrefixedSlice(key + key_len) 解码出 value
        │
        ▼
3. value Slice 指向 Arena 内存
        │
        ▼
4. PinnableSlice::PinSelf(value)  ← 必须拷贝！
   原因：Arena 随 MemTable 释放，用户可能在 MemTable flush 后才消费 value
```

### 5.2 Block Cache 读取（零拷贝）

```
DB::Get(key) → SST 文件查找 → Block Cache 查找
        │
        ▼
1. Block Cache 命中 → 返回被 pin 住的数据块指针
        │
        ▼
2. 从数据块中提取 value Slice
        │
        ▼
3. PinnableSlice::PinSlice(value, cleanup_fn, block_cache_ref)
   ← 零拷贝！
   data_ 直接指向 Block Cache 中的内存
   Cleanup 函数在 PinnableSlice 析构时释放 Block Cache 引用
        │
        ▼
4. 用户消费 value → PinnableSlice 析构 → Cleanup 释放 Block Cache 引用
```

### 5.3 生命周期对比

```
MemTable 命中：
  Arena 内存 ─── MemTable 引用计数 ─── flush 时释放
  PinnableSlice 拷贝了数据 → 不依赖 Arena 生命周期 → 安全 ✓

Block Cache 命中：
  Block Cache 内存 ─── LRU 引用计数 ─── PinnableSlice 析构时释放
  PinnableSlice 零拷贝引用 → 通过 Cleanup 管理 → 安全 ✓
```

## 6. ScopedArenaPtr：Arena 内对象的析构保障

Arena 分配的内存由 Arena 统一释放，不会调用单个对象的析构函数。但有些对象（如 Iterator）需要析构来做清理工作（关闭文件描述符、释放回调等）。

```cpp
template <typename T>
struct Destroyer {
  void operator()(T* ptr) { ptr->~T(); }  // 只调析构，不 free
};

template <typename T>
using ScopedArenaPtr = std::unique_ptr<T, Destroyer<T>>;
```

典型用法：

```cpp
Arena arena;
auto* mem = arena.AllocateAligned(sizeof(SomeIterator));
auto* iter = new (mem) SomeIterator(...);          // placement new
ScopedArenaPtr<SomeIterator> guard(iter);           // RAII 守卫
// ... 使用 iter ...
// guard 析构时调用 ~SomeIterator()
// arena 析构时释放内存
```

这解决了 Arena 的一个根本限制：**Arena 只管内存释放，不管对象析构**。对于 POD 类型（如 key/value 字节序列）这不是问题，但对于有非平凡析构函数的对象，必须用 `ScopedArenaPtr` 包一层。

## 7. 设计哲学总结

| 维度 | Slice | Arena |
|------|-------|-------|
| 核心思想 | 只观察不拥有 | 批量分配整体释放 |
| 替代什么 | std::string 的拷贝 | new/delete 的碎片化 |
| 生命周期 | 依赖外部（Arena/Block Cache） | 随 MemTable 整体释放 |
| 线程安全 | 只读安全（const 方法） | ConcurrentArena 分片 |
| 性能收益 | 零拷贝传递 | 减少 malloc/free 调用 |
| 代价 | 悬空指针风险 | 无法单独释放、无析构调用 |

**两者结合的威力**：Arena 提供稳定的内存存储，Slice 提供零拷贝的数据视图。这是 RocksDB 高性能 I/O 路径的基础——数据在写入时一次性拷贝进 Arena，之后的查找、比较、传递全部通过 Slice 零拷贝完成，直到最终返回给用户时才根据数据来源决定是否需要拷贝（PinnableSlice 的 PinSlice vs PinSelf）。
