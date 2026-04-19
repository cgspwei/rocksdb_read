# 为什么 RocksDB 中需要 SkipList？

## 一、RocksDB 的写入路径与 MemTable

RocksDB 的写入流程是：

```
Write → WAL (预写日志) → MemTable (内存表) → Immutable MemTable → SSTable (磁盘)
```

**MemTable 是写入的第一站**，所有新写入的 KV 对都先进入 MemTable。当 MemTable 满了之后，它变为 Immutable MemTable，被后台线程刷写到 SSTable。

MemTable 需要一个**有序的内存数据结构**，因为：
1. **范围查询**：RocksDB 支持 `Scan` / `Range` 操作，需要按 key 顺序迭代
2. **点查效率**：需要快速判断一个 key 是否存在
3. **有序刷写**：Immutable MemTable 刷写为 SSTable 时，数据本身是有序的，可以直接顺序写入

## 二、为什么不选其他数据结构？

| 候选结构 | 问题 |
|---------|------|
| **红黑树 / AVL 树** | 每次插入/删除需要**旋转操作**，修改多个节点指针，**难以实现无锁并发读**；且每个节点只有两个子节点，范围迭代时 cache 局部性差 |
| **B+ 树** | 节点分裂/合并代价高，需要复杂的并发控制；更适合磁盘场景，纯内存场景优势不大 |
| **哈希表** | **不支持范围查询和有序迭代**，这是 LSM-Tree 架构的硬需求 |
| **std::map** | 通常是红黑树实现，有上述所有问题；且内存分配不连续，cache 不友好 |

## 三、SkipList 的核心优势——匹配 MemTable 的需求

### 1. 读写并发安全，读无需加锁

这是 **最关键的原因**。从 `skiplist.h` 开头的注释可以看到：

```cpp
// Thread safety
// -------------
//
// Writes require external synchronization, most likely a mutex.
// Reads require a guarantee that the SkipList will not be destroyed
// while the read is in progress.  Apart from that, reads progress
// without any internal locking or synchronization.
```

**写操作需要外部同步（mutex），但读操作完全无锁！** 这在 LSM-Tree 中至关重要——MemTable 的读写比通常远大于 1:1，大量读请求和少量写请求可以并行执行，互不阻塞。

这是怎么做到的？靠两个不变式：

```cpp
// Invariants:
//
// (1) Allocated nodes are never deleted until the SkipList is
// destroyed.  This is trivially guaranteed by the code since we
// never delete any skip list nodes.
//
// (2) The contents of a Node except for the next/prev pointers are
// immutable after the Node has been linked into the SkipList.
// Only Insert() modifies the list, and it is careful to initialize
// a node and use release-stores to publish the nodes in one or
// more lists.
```

- **不变式 1**：节点一旦分配，永不删除。所以读线程不可能遇到悬空指针。
- **不变式 2**：节点插入后，内容不可变。新节点通过 **release-store**（内存屏障）发布到链表，读线程通过 **acquire-load** 看到，保证不会看到半初始化的数据。

### 2. 天然有序，支持高效范围查询

SkipList 底层是排序链表，第 0 层包含所有节点，上层是"快车道"。迭代器可以简单地沿第 0 层遍历：

```cpp
inline void SkipList<Key, Comparator>::Iterator::Next() {
  assert(Valid());
  node_ = node_->Next(0);
}
```

而查找操作通过多级索引跳过大量节点，达到 O(log N) 的时间复杂度：

```cpp
Node* SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  Node* last_bigger = nullptr;
  while (true) {
    Node* next = x->Next(level);
    int cmp =
        (next == nullptr || next == last_bigger) ? 1 : compare_(next->key, key);
    if (cmp == 0 || (cmp > 0 && level == 0)) {
      return next;
    } else if (cmp < 0) {
      x = next;
    } else {
      last_bigger = next;
      level--;
    }
  }
}
```

### 3. 实现简单，无需旋转

SkipList 的插入只需要修改相邻节点的指针（最多 `height` 个），不需要像红黑树那样旋转子树。这让并发插入的实现大为简化——`InlineSkipList` 的 `InsertConcurrently` 用 CAS（Compare-And-Swap）即可实现无锁写：

```cpp
bool CASNext(int n, Node* expected, Node* x) {
  assert(n >= 0);
  return (&next_[0] - n)->CasStrong(expected, x);
}
```

### 4. 顺序插入时优化为 O(1)

RocksDB 的工作负载中，大量写入是**顺序写**（如 bulk load）。SkipList 利用了 `prev_` 缓存来优化：

```cpp
// Used for optimizing sequential insert patterns.  Tricky.  prev_[i] for
// i up to max_height_ is the predecessor of prev_[0] and prev_height_
// is the height of prev_[0].  prev_[0] can only be equal to head before
// insertion, in which case max_height_ and prev_height_ are 1.
int32_t prev_height_;
Node** prev_;
```

`InlineSkipList` 则更进一步，引入了 **Splice**（缓存搜索路径）和 **InsertWithHint**：

```cpp
// Inserts a key allocated by AllocateKey with a hint of last insert
// position in the skip-list. If hint points to nullptr, a new hint will be
// populated, which can be used in subsequent calls.
//
// It can be used to optimize the workload where there are multiple groups
// of keys, and each key is likely to insert to a location close to the last
// inserted key in the same group. One example is sequential inserts.
bool InsertWithHint(const char* key, void** hint);
```

当 key 是顺序插入时，`Insert` 的代价从 O(log N) 降到 **摊还 O(1)**。

### 5. 内存分配友好

SkipList 的节点使用 Arena 分配器（通过 `Allocator*` 参数），所有节点分配自同一块连续内存区域，带来：
- **更好的 cache 局部性**：相邻节点在内存中也相邻
- **批量释放**：MemTable 被销毁时，Arena 整块释放，不需要逐个 delete
- **无内存碎片**：Arena 本身可以复用已释放的空间

## 四、两个版本的演进关系

| 特性 | `SkipList` (skiplist.h) | `InlineSkipList` (inlineskiplist.h) |
|------|------------------------|--------------------------------------|
| 起源 | LevelDB 遗产 | RocksDB 新设计 |
| Key 存储 | `Key const key`（节点内存储 key 值/指针） | Key 紧跟在 `next_[]` 之后（inline） |
| 每节点节省 | — | 节省 1 个指针（`sizeof(void*)`） |
| 并发写 | 不支持 | `InsertConcurrently` (CAS) |
| 搜索优化 | `prev_` 缓存 | Splice + Finger Search |
| 实际使用者 | 旧代码/测试 | `SkipListRep`（MemTable 默认实现） |

从 `skiplistrep.cc` 可以看到，**当前 MemTable 默认使用的是 `InlineSkipList`**：

```cpp
class SkipListRep : public MemTableRep {
  InlineSkipList<const MemTableRep::KeyComparator&> skip_list_;
```

`InlineSkipList` 的关键内存优化——将 key 直接嵌入节点：

```cpp
// The Node data type is more of a pointer into custom-managed memory than
// a traditional C++ struct.  The key is stored in the bytes immediately
// after the struct, and the next_ pointers for nodes with height > 1 are
// stored immediately _before_ the struct.  This avoids the need to include
// any pointer or sizing data, which reduces per-node memory overheads.
...
const char* Key() const { return reinterpret_cast<const char*>(&next_[1]); }
```

内存布局（以 height=3 为例）：

```
低地址 ←─────────────────────────────────────────────────────→ 高地址

[next_[2]] [next_[1]] [next_[0]]  │  Key Data...  │
   ↑           ↑          ↑            ↑
   │           │          │            │
   │           │     Node 起始位置      Key 紧跟 next_[1] 之后
   │           │          (next_[0])   (即 &next_[1])
   │           │
   └───────────┴── 高层指针存储在 Node 起始位置之前（负偏移）
```

这种"向后生长"的设计让 `next_` 数组无需存储长度信息，key 直接跟在 `next_[1]` 后面，**每个节点节省了一个 key 指针**（8 字节），对 MemTable 中动辄百万级别的 KV 对来说，节省的内存非常可观。

## 五、总结

RocksDB 选择 SkipList 作为 MemTable 的核心数据结构，本质上是 **LSM-Tree 写优先架构的必然选择**：

1. **无锁读** — 匹配 LSM-Tree 高读低写的场景，读不阻塞写，写不阻塞读
2. **天然有序** — 满足范围查询和有序刷写 SSTable 的需求
3. **顺序写友好** — Splice/Finger Search 优化让顺序插入接近 O(1)
4. **实现简洁** — 无需旋转，CAS 即可并发，代码可维护性高
5. **内存高效** — InlineSkipList 的紧凑布局 + Arena 分配器，cache 友好且低开销

相比之下，红黑树虽然也是 O(log N)，但旋转操作破坏了无锁读的可能性；B+ 树虽然对磁盘友好，但 MemTable 是纯内存结构，不需要 B+ 树的页面分裂/合并开销；哈希表完全不支持有序访问。**SkipList 是唯一同时满足"有序 + 无锁读 + 顺序写优化"的数据结构。**
