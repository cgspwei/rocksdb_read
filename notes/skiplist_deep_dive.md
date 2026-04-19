# RocksDB 跳表（SkipList）深度解析

> 核心源码：
> - `memtable/skiplist.h` — 基础版 SkipList（LevelDB 遗产）
> - `memtable/inlineskiplist.h` — 优化版 InlineSkipList（生产默认使用）
> - `memtable/skiplistrep.cc` — MemTable 的跳表适配层
> - `util/atomic.h` — 原子操作封装

---

## 一、两代跳表实现概览

RocksDB 包含两个跳表实现，后者是前者的优化版本：

| 维度 | `SkipList<Key, Comparator>` | `InlineSkipList<Comparator>` |
|------|---------------------------|------------------------------|
| 源码 | `skiplist.h` | `inlineskiplist.h` |
| Key 存储 | Node 内嵌 `Key const key` 成员 | Key 内联在 Node 尾部连续内存 |
| 每节点开销 | 额外 1 个指针（指向 Key） | 0 额外指针（Key 紧跟 Node） |
| 并发插入 | 不支持（需外部互斥锁） | 支持 CAS 无锁并发插入 |
| Splice 优化 | 简单 `prev_[]` 缓存 | 完善 Splice 结构 + 部分重算 |
| Finger Search | 不支持 | 支持（MultiGet 加速） |
| Prefetch | 不支持 | 支持（查找路径预取） |
| 生产使用 | 测试/兼容 | **MemTable 默认实现** |

---

## 二、核心数据结构设计

### 2.1 InlineSkipList::Node — 关键的内存布局创新

```
内存布局（height=3 的 Node）：

低地址 ←──────────────────────────────────────────→ 高地址

┌──────────┬──────────┬──────────┬──────────┬─────────────┐
│ next_[2] │ next_[1] │ next_[0] │  Node    │   Key Data  │
│ (Atomic) │ (Atomic) │ (Atomic) │ (StashH) │ (inline)    │
└──────────┴──────────┴──────────┴──────────┴─────────────┘
▲                     ▲          ▲
│                     │          │
│   x-1 指向此处 ←────┘          │
│   (key 参数指向此处) ──────────┘
│
│  高层指针存储在 Node 结构体之前的内存中
│  next_[0] - n 通过指针算术访问第 n 层
```

**核心源码**（`inlineskiplist.h:357-421`）：

```cpp
template <class Comparator>
struct InlineSkipList<Comparator>::Node {
  // Key 的位置：next_[1] 的地址就是 Key 的起始地址
  const char* Key() const { return reinterpret_cast<const char*>(&next_[1]); }

  // 通过指针算术访问第 n 层：next_[0] - n
  Node* Next(int n) {
    assert(n >= 0);
    return ((&next_[0] - n)->Load());  // acquire 语义
  }

  void SetNext(int n, Node* x) {
    assert(n >= 0);
    (&next_[0] - n)->Store(x);  // release 语义
  }

  // CAS 操作，用于并发插入
  bool CASNext(int n, Node* expected, Node* x) {
    assert(n >= 0);
    return (&next_[0] - n)->CasStrong(expected, x);
  }

  // 无屏障变体
  Node* NoBarrier_Next(int n) {
    return (&next_[0] - n)->LoadRelaxed();
  }
  void NoBarrier_SetNext(int n, Node* x) {
    (&next_[0] - n)->StoreRelaxed(x);
  }

  // 利用 next_[0] 暂存高度（从 AllocateKey 传递给 Insert）
  void StashHeight(const int height) {
    static_assert(sizeof(int) <= sizeof(next_[0]));
    memcpy(static_cast<void*>(&next_[0]), &height, sizeof(int));
  }
  int UnstashHeight() const {
    int rv;
    memcpy(&rv, &next_[0], sizeof(int));
    return rv;
  }

 private:
  Atomic<Node*> next_[1];  // 只有 next_[0]，其余通过算术偏移
};
```

**设计精妙之处**：

1. **反向内存布局**：高层 `next_` 指针存储在 Node 结构体**之前**，通过 `(&next_[0] - n)` 访问。这避免了在 Node 内存储高度信息或变长数组
2. **Key 内联**：Key 数据紧跟 `next_[1]` 之后，省去了一次指针跳转
3. **StashHeight 技巧**：利用 `next_[0]` 在节点链接前暂存高度值，避免额外的内存开销
4. **对比 SkipList**：基础版 `Node` 包含 `Key const key` 成员，对于 `const char*` 类型需要额外 8 字节指针；InlineSkipList 将 Key 直接内联，节省一个指针

### 2.2 Splice — 搜索路径缓存

```cpp
template <class Comparator>
struct InlineSkipList<Comparator>::Splice {
  int height_ = 0;   // 当前缓存的高度
  Node** prev_;       // 每层的前驱节点
  Node** next_;       // 每层的后继节点
};
```

Splice 的不变量：`prev_[i+1]->key <= prev_[i]->key < next_[i]->key <= next_[i+1]->key`

即如果 key 被某一层的 `prev_[i]` 和 `next_[i]` 包夹，则所有更高层也必然包夹它。这个性质使得**部分重算**成为可能。

### 2.3 节点内存分配

```cpp
// inlineskiplist.h:860-880
Node* AllocateNode(size_t key_size, int height) {
  auto prefix = sizeof(Atomic<Node*>) * (height - 1);
  // 布局：[next_指针区 (prefix)] [Node] [Key数据 (key_size)]
  char* raw = allocator_->AllocateAligned(prefix + sizeof(Node) + key_size);
  Node* x = reinterpret_cast<Node*>(raw + prefix);
  x->StashHeight(height);  // 暂存高度
  return x;
}
```

使用 `AllocateAligned` 保证内存对齐，对缓存行友好。

---

## 三、插入算法流程

### 3.1 非并发插入（`Insert<false>`）

```
完整流程：

1. 恢复高度
   x->UnstashHeight() → height

2. 更新 max_height_（CAS，但非并发时只执行一次）
   while (height > max_height) {
     if (max_height_.CasWeakRelaxed(max_height, height)) break;
   }

3. 验证/重算 Splice
   ├─ 若 splice->height_ < max_height → 全量重算
   └─ 否则 → 逐层检查 splice 是否仍包夹新 key
       ├─ prev[i]->Next(i) != next[i] → splice 失效，上移
       ├─ key < prev[i] → key 在 splice 之前
       │   ├─ allow_partial_splice_fix=true: 跳过同节点层，部分重算
       │   └─ allow_partial_splice_fix=false: 全量重算
       ├─ key > next[i] → key 在 splice 之后（同上处理）
       └─ prev[i] < key < next[i] → 找到包夹层，停止

4. 链接节点
   for (i = 0; i < height; i++) {
     x->NoBarrier_SetNext(i, splice->next_[i]);  // 无屏障设置后继
     splice->prev_[i]->SetNext(i, x);             // release 设置前驱的后继
   }

5. 更新 splice 缓存
   splice->prev_[i] = x  for all i < height
```

**关键代码**（`inlineskiplist.h:1027-1224`）：

```cpp
template <bool UseCAS>
bool InlineSkipList<Comparator>::Insert(const char* key, Splice* splice,
                                        bool allow_partial_splice_fix) {
  Node* x = reinterpret_cast<Node*>(const_cast<char*>(key)) - 1;
  const DecodedKey key_decoded = compare_.decode_key(key);
  int height = x->UnstashHeight();

  // ... Splice 验证与重算（省略，见上文流程）...

  if (UseCAS) {
    // 并发路径：CAS 无锁插入（详见 3.2 节）
  } else {
    // 非并发路径：直接链接
    for (int i = 0; i < height; ++i) {
      if (i >= recompute_height &&
          splice->prev_[i]->Next(i) != splice->next_[i]) {
        FindSpliceForLevel<false>(key_decoded, splice->prev_[i], nullptr, i,
                                  &splice->prev_[i], &splice->next_[i]);
      }
      x->NoBarrier_SetNext(i, splice->next_[i]);  // 无屏障
      splice->prev_[i]->SetNext(i, x);             // release 屏障
    }
  }
  // 更新 splice
  if (splice_is_valid) {
    for (int i = 0; i < height; ++i) {
      splice->prev_[i] = x;
    }
  }
  return true;
}
```

### 3.2 并发插入（`Insert<true>` / `InsertConcurrently`）

并发插入的核心挑战：**多个线程可能同时修改同一层的链表指针**。

```
并发插入流程：

1. 恢复高度、更新 max_height_（同非并发）

2. 验证/重算 Splice（同非并发，但每个线程使用自己的栈上 Splice）

3. CAS 逐层链接（核心差异！）
   for (i = 0; i < height; i++) {
     while (true) {
       // 重复键检查
       if (i == 0 && splice->next_[i] != nullptr &&
           compare_(splice->next_[i]->Key(), key) <= 0) {
         return false;  // 重复键，插入失败
       }
       
       x->NoBarrier_SetNext(i, splice->next_[i]);
       
       // CAS: prev[i] 的 next 是否还是 splice->next_[i]？
       if (splice->prev_[i]->CASNext(i, splice->next_[i], x)) {
         break;  // CAS 成功，链接完成
       }
       
       // CAS 失败：其他线程已插入节点，需要重新搜索
       FindSpliceForLevel<false>(key_decoded, splice->prev_[i], nullptr, i,
                                 &splice->prev_[i], &splice->next_[i]);
       
       if (i > 0) {
         splice_is_valid = false;  // 标记 splice 需要完全重算
       }
     }
   }
```

**CAS 的关键点**：

```cpp
// Node::CASNext 实现
bool CASNext(int n, Node* expected, Node* x) {
  return (&next_[0] - n)->CasStrong(expected, x);
  // 等价于：compare_exchange_strong(expected, x, memory_order_acq_rel)
}
```

- `CasStrong` 使用 `memory_order_acq_rel`，既保证 release 语义（之前的写对其他线程可见），又保证 acquire 语义（能看到其他线程的写）
- CAS 失败时只重搜当前层，但标记 `splice_is_valid = false`，下次插入会完全重算 Splice

### 3.3 顺序插入优化（SkipList 版本的 prev_[] 缓存）

基础版 `SkipList` 有一个更简单的顺序插入快速路径：

```cpp
// skiplist.h:457-506
void SkipList::Insert(const Key& key) {
  // 快速路径：检查新 key 是否紧跟上次插入位置之后
  if (!KeyIsAfterNode(key, prev_[0]->NoBarrier_Next(0)) &&
      (prev_[0] == head_ || KeyIsAfterNode(key, prev_[0]))) {
    // 顺序插入！只需更新 prev_[1..max_height] = prev_[0]
    for (int i = 1; i < prev_height_; i++) {
      prev_[i] = prev_[0];
    }
  } else {
    // 非顺序，需要全量搜索
    FindLessThan(key, prev_);
  }
  // ... 链接节点 ...
  prev_[0] = x;
  prev_height_ = height;
}
```

这在 LSM-Tree 场景中极其高效，因为 MemTable 的写入通常是**顺序追加**的。

### 3.4 关于删除

**RocksDB 的跳表不支持删除**。这是刻意的架构决策：

1. **MemTable 语义**：RocksDB 使用 LSM-Tree，删除操作被记录为 "tombstone" 标记，而非真正删除跳表节点
2. **并发安全**：不删除节点使得读操作完全无锁——读者永远不会遇到悬空指针
3. **内存管理**：整个 MemTable 作为一个 Arena 分配，flush 时整体释放

源码注释明确说明（`skiplist.h:20-22`）：
> Allocated nodes are never deleted until the SkipList is destroyed.

---

## 四、查找算法流程

### 4.1 FindGreaterOrEqual — 核心查找

```cpp
// inlineskiplist.h:592-642
Status FindGreaterOrEqual(const char* key, Node** node, ...) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  Node* last_bigger = nullptr;  // 上一次"向右过大"的节点，用于优化
  const DecodedKey key_decoded = compare_.decode_key(key);
  
  while (true) {
    Node* next = x->Next(level);
    if (next != nullptr) {
      PREFETCH(next->Next(level), 0, 1);  // 预取下一层节点！
    }
    int cmp = (next == nullptr || next == last_bigger)
                  ? 1
                  : compare_(next->Key(), key_decoded);
    if (cmp == 0 || (cmp > 0 && level == 0)) {
      *node = next;
      return Status::OK();
    } else if (cmp < 0) {
      x = next;        // 当前层继续向右
    } else {
      last_bigger = next;  // 记录：next > key，下次同层跳过比较
      level--;            // 下降一层
    }
  }
}
```

**优化细节**：

1. **`last_bigger` 缓存**：当 `next > key` 时，记住 `next`。下次如果又遇到 `next == last_bigger`，直接跳过比较（已知 `next > key`）。这避免了重复的 key 比较
2. **`PREFETCH` 预取**：在访问 `next` 时，预取 `next->Next(level)`，利用 CPU 流水线掩盖内存延迟
3. **`decode_key`**：一次解码 key，后续比较复用解码结果

### 4.2 FindGreaterOrEqualWithFinger — Finger Search（MultiGet 加速）

```cpp
// inlineskiplist.h:1227-1306
Status FindGreaterOrEqualWithFinger(const char* key, Node** out,
                                     Splice* finger, ...) const {
  const DecodedKey key_decoded = compare_.decode_key(key);
  int max_height = GetMaxHeight();
  int start_level;

  if (finger->height_ == 0) {
    // 首次调用：从顶层开始全量搜索
    start_level = max_height - 1;
    finger->prev_[start_level] = head_;
    finger->next_[start_level] = nullptr;
    finger->height_ = max_height;
  } else {
    // 后续调用：从 level 0 向上走，找到仍包夹 key 的最低层
    start_level = 0;
    while (start_level < max_height - 1 &&
           KeyIsAfterNode(key_decoded, finger->next_[start_level])) {
      start_level++;
    }
    // 如果走到顶层仍不包夹，重置为从 head 开始
    if (KeyIsAfterNode(key_decoded, finger->next_[start_level])) {
      finger->prev_[start_level] = head_;
      finger->next_[start_level] = nullptr;
    }
  }
  // 从 start_level 向下逐层搜索...
}
```

**Finger Search 原理**：

```
假设上次查找 key=50，finger 缓存了搜索路径。
现在查找 key=55：

       level 3: [head] ────────────────→ [80]
       level 2: [head] ──→ [30] ───────→ [80]
       level 1: [head] → [30] → [50] ──→ [80]
       level 0: [head] → [30] → [50] → [55] → [80]

  finger.next_[0] = 50 → 55 > 50，向上走
  finger.next_[1] = 80 → 55 < 80，停在 level 1
  从 level 1 的 [50] 开始向下搜索 → O(log d) 而非 O(log N)
  （d = 55 与 50 的距离，通常 d << N）
```

**MultiGet** 批量查找正是利用这个特性：当 keys 有序时，每次查找的代价从 O(log N) 降为 O(log d)，d 是连续 key 之间的跳表距离。

### 4.3 FindSpliceForLevel — 单层搜索（带预取）

```cpp
// inlineskiplist.h:946-972
template <bool prefetch_before>
void FindSpliceForLevel(const DecodedKey& key, Node* before, Node* after,
                        int level, Node** out_prev, Node** out_next) const {
  while (true) {
    Node* next = before->Next(level);
    if (next != nullptr) {
      PREFETCH(next->Next(level), 0, 1);  // 预取同层下一个
    }
    if (prefetch_before == true) {
      if (next != nullptr && level > 0) {
        PREFETCH(next->Next(level - 1), 0, 1);  // 预取下一层！
      }
    }
    if (next == after || !KeyIsAfterNode(key, next)) {
      *out_prev = before;
      *out_next = next;
      return;
    }
    before = next;
  }
}
```

`prefetch_before=true` 时不仅预取同层节点，还预取**下一层**节点，使得下降层级时数据已在缓存中。

---

## 五、多线程并发优化策略

### 5.1 整体并发模型

```
┌─────────────────────────────────────────────────┐
│                  读者线程 (N 个)                    │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐          │
│  │ Reader1 │  │ Reader2 │  │ Reader3 │  ...     │
│  └────┬────┘  └────┬────┘  └────┬────┘          │
│       │            │            │                │
│       ▼            ▼            ▼                │
│  ┌──────────────────────────────────────┐       │
│  │       InlineSkipList (无锁读)          │       │
│  │  - acquire load 读取 next 指针         │       │
│  │  - 不加任何锁                          │       │
│  │  - 不修改任何数据                      │       │
│  └──────────────────────────────────────┘       │
└─────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────┐
│              写者线程 (M 个)                       │
│  ┌─────────┐  ┌─────────┐                        │
│  │ Writer1 │  │ Writer2 │  ...                   │
│  └────┬────┘  └────┬────┘                        │
│       │            │                              │
│       ▼            ▼                              │
│  方式1: 外部互斥锁 (Insert)                       │
│    - 单写者，不需要 CAS                           │
│    - MemTable 默认模式                            │
│                                                   │
│  方式2: CAS 无锁 (InsertConcurrently)             │
│    - 多写者并发，每层 CAS 竞争                     │
│    - 允许写写并发 + 读写并发                       │
└─────────────────────────────────────────────────┘
```

### 5.2 内存序详解

RocksDB 自定义了 `Atomic<T>` 和 `RelaxedAtomic<T>` 封装（`util/atomic.h`），明确区分内存序语义：

| 操作 | 内存序 | 语义 | 跳表中的使用 |
|------|--------|------|-------------|
| `Store()` | `release` | 之前的写对其他线程可见 | `SetNext()` — 发布新节点 |
| `Load()` | `acquire` | 能看到其他线程 release 前的写 | `Next()` — 读取后继节点 |
| `StoreRelaxed()` | `relaxed` | 无同步保证 | `NoBarrier_SetNext()` — 设置自己的后继 |
| `LoadRelaxed()` | `relaxed` | 无同步保证 | `NoBarrier_Next()`, `max_height_` |
| `CasStrong()` | `acq_rel` | 既是 acquire 也是 release | `CASNext()` — 并发链接 |

**关键洞察**：插入新节点时使用**两层屏障策略**：

```cpp
// 步骤1：无屏障设置新节点的 next 指针（只有自己能看到）
x->NoBarrier_SetNext(i, splice->next_[i]);

// 步骤2：release 屏障设置前驱的 next 指针（发布新节点）
splice->prev_[i]->SetNext(i, x);  // Store(release)
```

为什么步骤1可以无屏障？因为步骤2的 release 屏障保证了：**任何线程通过 acquire 读到 `prev->next == x` 时，一定能看到 `x->next_` 已经被正确设置**。这是一个经典的**发布-订阅**模式。

### 5.3 读写并发安全保证

读线程在**没有任何锁**的情况下安全遍历跳表，依赖三个不变量：

1. **节点不删除**：读者永远不会遇到悬空指针
2. **节点内容不可变**：key 在链接后不被修改
3. **release-acquire 配对**：读者通过 `Next()` (acquire) 看到的节点，其所有字段一定已经被初始化

```cpp
// 读者路径
Node* next = x->Next(level);  // acquire load
// 如果 next != nullptr，next->key 和 next->next_ 一定完全初始化
// 因为写者用 release store 发布了 next 指针
```

### 5.4 max_height_ 的特殊处理

```cpp
RelaxedAtomic<int> max_height_;  // 注意：RelaxedAtomic，不是 Atomic！
```

`max_height_` 使用 relaxed 内存序，因为：

1. **正确性不依赖它**：读者看到过期的 `max_height_` 值只是效率问题（从更低的层开始搜索），不会导致错误
2. **性能考虑**：`GetMaxHeight()` 在每次查找时都被调用，relaxed 序在 x86 上几乎是免费的操作

```cpp
// 写者：更新 max_height_（非并发时用 StoreRelaxed，并发时用 CAS）
max_height_.StoreRelaxed(height);
// 或
max_height_.CasWeakRelaxed(max_height, height);

// 读者：读取 max_height_（始终 relaxed）
inline int GetMaxHeight() const { return max_height_.LoadRelaxed(); }
```

---

## 六、性能优化手段

### 6.1 内存布局优化

#### （1）Key 内联 — 减少指针跳转

```
传统 SkipList (Key = const char*)：
┌──────────┐     ┌──────────┐
│  Node    │────→│ Key Data │   两次内存访问：Node → Key
│  key_    │     └──────────┘
│  next_[] │
└──────────┘

InlineSkipList：
┌──────────┬──────────┐
│  Node    │ Key Data │   一次内存访问：Node 紧跟 Key
│  next_[] │ (inline) │
└──────────┴──────────┘
```

对于 MemTable 中最常见的 `const char*` key 类型，这节省了**每节点 8 字节**的指针开销和**一次指针追逐**。

#### （2）反向 next_ 数组 — 消除高度字段

```
传统做法：
┌──────────┬───────┬──────────┬──────────┬──────────┐
│ height_  │ next_ │ next_[0] │ next_[1] │ next_[2] │
│ (4字节)  │ [3]   │          │          │          │
└──────────┴───────┴──────────┴──────────┴──────────┘

InlineSkipList 做法：
┌──────────┬──────────┬──────────┬──────────┐
│ next_[2] │ next_[1] │ next_[0] │ Key Data │  无 height_ 字段！
│ (before) │ (before) │ (in Node)│          │
└──────────┴──────────┴──────────┴──────────┘
```

Node 不存储高度信息，通过**遍历到达节点时的层级**隐式知道高度。高度只在分配到链接之间的过渡期通过 `StashHeight` 暂存。

#### （3）Arena 连续分配 — 缓存友好

所有节点通过 Arena 分配器分配，新节点倾向于在内存中相邻，提高缓存空间局部性。

### 6.2 预取（Prefetch）优化

```cpp
// FindGreaterOrEqual 中：
Node* next = x->Next(level);
if (next != nullptr) {
  PREFETCH(next->Next(level), 0, 1);  // 预取 next 在同层的下一个节点
}

// FindSpliceForLevel 中（prefetch_before=true）：
if (next != nullptr && level > 0) {
  PREFETCH(next->Next(level - 1), 0, 1);  // 预取 next 在下一层的节点
}
```

`PREFETCH(ptr, 0, 1)` 的含义：
- `0` = 读预取（非写）
- `1` = 低局部性（数据用一次就够）

这在遍历跳表时将下次可能访问的节点提前加载到 L1/L2 缓存，对于**指针追逐**模式特别有效——跳表的每次 `Next()` 都是一次随机内存访问，prefetch 可以将延迟从 ~100ns（DRAM）降低到 ~1ns（L1 缓存命中）。

### 6.3 顺序插入优化

```
Splice 缓存 + allow_partial_splice_fix 的效果：

场景1：完全顺序插入（LSM-Tree 典型场景）
  - Splice 始终命中，每层只需 O(1) 验证
  - 总插入代价：O(1)（而非 O(log N)）
  - 实测：顺序插入吞吐量提升 5-10x

场景2：局部有序（如多线程各自写不同 key 范围）
  - InsertWithHint：每个线程维护自己的 Splice
  - 部分重算：O(log d)，d 是与上次插入位置的距离
  - 比全量搜索 O(log N) 更快

场景3：完全随机
  - Splice 失效，回退到 O(log N) 全量搜索
  - 与无 Splice 的性能相同
```

### 6.4 LookaheadIterator — 局部性感知的迭代器

```cpp
// skiplistrep.cc:289-383
class LookaheadIterator : public MemTableRep::Iterator {
  void Seek(const Slice& internal_key, const char* memtable_key) override {
    if (prev_.Valid() && rep_.cmp_(encoded_key, prev_.key()) >= 0) {
      // 从上次位置开始线性搜索（最多 lookahead_ 步）
      iter_ = prev_;
      size_t cur = 0;
      while (cur++ <= rep_.lookahead_ && iter_.Valid()) {
        if (rep_.cmp_(encoded_key, iter_.key()) <= 0) return;
        Next();
      }
    }
    // 回退到 O(log N) 全量搜索
    iter_.Seek(encoded_key);
    prev_ = iter_;
  }
};
```

当 `lookahead_ > 0` 时启用，对于**点查询局部性**强的场景（如同一 user key 的多次版本查询），可以避免从 head 开始的 O(log N) 搜索。

### 6.5 RandomSeek — O(N) 随机访问的优化

```cpp
// inlineskiplist.h:703-743
Node* FindRandomEntry() const {
  Node *x = head_, *scan_node = nullptr, *limit_node = nullptr;
  std::vector<Node*> lvl_nodes;
  int level = GetMaxHeight() - 1;

  while (level >= 0) {
    lvl_nodes.clear();
    scan_node = x;
    while (scan_node != limit_node) {
      lvl_nodes.push_back(scan_node);
      scan_node = scan_node->Next(level);
    }
    uint32_t rnd_idx = rnd->Next() % lvl_nodes.size();
    x = lvl_nodes[rnd_idx];
    if (rnd_idx + 1 < lvl_nodes.size()) {
      limit_node = lvl_nodes[rnd_idx + 1];
    }
    level--;
  }
  return x == head_ ? head_->Next(0) : x;
}
```

从高层开始，每层随机选一个节点，然后缩小范围到下一层。这给出**均匀随机**的节点选择，用于 `UniqueRandomSample` 等统计功能。

---

## 七、高并发场景下的表现与权衡

### 7.1 场景对比

| 场景 | 模式 | 吞吐量 | 延迟 | 实现复杂度 |
|------|------|--------|------|-----------|
| 单写多读 | `Insert` + 外部 mutex | ★★★★ | 低 | 低 |
| 多写多读 | `InsertConcurrently` (CAS) | ★★★ | 中（CAS 竞争） | 高 |
| 批量有序写 | `InsertWithHint` | ★★★★★ | 极低 | 中 |
| 批量并发有序写 | `InsertWithHintConcurrently` | ★★★★ | 低 | 高 |

### 7.2 CAS 竞争分析

并发插入在**高度竞争**场景下的表现：

```
假设 N 个线程同时插入，height=1（最常见）：

Thread1: CAS(prev[0], old_next, new_node_1) → 成功
Thread2: CAS(prev[0], old_next, new_node_2) → 失败，重新搜索
Thread2: CAS(prev[0], new_node_1, new_node_2) → 成功

竞争开销 = 重新搜索 FindSpliceForLevel 的代价
好消息：只搜索一层，且范围已缩小
坏消息：splice 失效，下次插入需要完全重算
```

**关键权衡**：
- CAS 在低竞争时接近无锁性能
- 高竞争时退化为"乐观锁"模式，多次重试
- 相比全局 mutex，CAS 的优势是**不会阻塞读线程**

### 7.3 MemTable 中的使用方式

```cpp
// skiplistrep.cc — SkipListRep 适配层
class SkipListRep : public MemTableRep {
  InlineSkipList<const MemTableRep::KeyComparator&> skip_list_;
  
  // 非并发插入
  void Insert(KeyHandle handle) override {
    skip_list_.Insert(static_cast<char*>(handle));
  }
  
  // 并发插入
  void InsertConcurrently(KeyHandle handle) override {
    skip_list_.InsertConcurrently(static_cast<char*>(handle));
  }
  
  // 带提示插入
  void InsertWithHint(KeyHandle handle, void** hint) override {
    skip_list_.InsertWithHint(static_cast<char*>(handle), hint);
  }
};
```

MemTable 根据配置选择不同的插入方式：
- **默认**：`Insert`（外部 mutex 保护，单写多读）
- `allow_concurrent_memtable_write`：`InsertConcurrently`（CAS 并发写）
- `insert_with_hint`：`InsertWithHint`（Splice 优化）

### 7.4 测试中的并发验证

```cpp
// skiplist_test.cc:141-296
// ConcurrentTest 设计：
// - 单写者 + 多读者
// - 读者快照初始状态，验证不遗漏初始数据
// - 允许看到新插入的数据（但不能遗漏旧的）
// - 使用 <key, generation, hash> 三元组验证数据完整性

// 实际并发测试：
TEST_F(SkipTest, Concurrent1) { RunConcurrent(1); }
// ... 
TEST_F(SkipTest, Concurrent5) { RunConcurrent(5); }
```

---

## 八、总结：RocksDB 跳表的设计哲学

| 原则 | 具体实现 |
|------|---------|
| **读优先** | 读完全无锁无屏障（除 acquire load），写承担所有同步开销 |
| **空间效率** | Key 内联、反向 next 数组、StashHeight 消除高度字段 |
| **顺序友好** | Splice 缓存、prev_[] 快速路径、LookaheadIterator |
| **并发可扩展** | CAS 无锁写、per-thread Splice、relaxed max_height_ |
| **删除回避** | 不删除节点 → 无 ABA 问题、无内存回收难题、读者无锁 |
| **预取友好** | PREFETCH 同层+下一层、Arena 连续分配 |
| **灵活适配** | 4 种插入模式（Insert/Concurrent/WithHint/WithHintConcurrent） |

RocksDB 的跳表实现是一个教科书级的**工程优化案例**：在经典跳表 O(log N) 的理论复杂度之上，通过精细的内存布局、内存序控制、路径缓存和硬件预取，将实际性能推向了极致。
