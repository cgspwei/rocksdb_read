# RocksDB MemTable 实现分析

## 1. 概述

MemTable 是 RocksDB 的内存写入缓冲区，所有写入操作首先进入 MemTable，当 MemTable 写满后变为 Immutable MemTable，等待被 Flush 到磁盘成为 SST 文件。RocksDB 为每个 Column Family 维护一个 Active MemTable（接受写入）和零个或多个 Immutable MemTable。

核心代码位置：
- `db/memtable.h` / `db/memtable.cc` — MemTable 主类
- `memtable/inlineskiplist.h` — InlineSkipList（默认数据结构）
- `memtable/skiplist.h` — 基础 SkipList
- `include/rocksdb/memtablerep.h` — MemTableRep 抽象接口

## 2. 类层次结构

```
ReadOnlyMemTable (抽象基类)
  └── MemTable (final, 可写 MemTable)
        └── 内部持有:
              - table_ : unique_ptr<MemTableRep>    // 点键数据存储
              - range_del_table_ : unique_ptr<MemTableRep>  // 范围删除数据存储
              - arena_ : ConcurrentArena  // 内存分配器
              - bloom_filter_ : unique_ptr<DynamicBloom>  // 布隆过滤器
```

### ReadOnlyMemTable vs MemTable

- `ReadOnlyMemTable`：定义了只读接口（Get、NewIterator 等），用于 Immutable MemTable
- `MemTable`：继承 `ReadOnlyMemTable`，增加了 `Add`、`Update`、`UpdateCallback` 等写入方法

## 3. 数据存储格式

### 3.1 Entry 编码格式

MemTable 中的每个条目（Entry）按以下格式编码（参见 `MemTable::Add`）：

```
+----------------+------------------+------------------+----------------+----------+
| key_size       | key bytes        | seq_and_type     | value_size     | value    | checksum |
| (varint32)     | (user_key)       | (8 bytes)        | (varint32)     | bytes    | (可选)    |
+----------------+------------------+------------------+----------------+----------+----------+
                 |<--- internal_key_size = user_key_size + 8 --->|
```

其中 `seq_and_type` 通过 `PackSequenceAndType(seq, type)` 将序列号和值类型打包为一个 64 位整数：
- 低 8 位为值类型（ValueType）：kTypeValue、kTypeDeletion、kTypeMerge、kTypeRangeDeletion 等
- 高 56 位为序列号（SequenceNumber）

### 3.2 Key 排序规则

MemTable 使用 `InternalKeyComparator` 进行排序，排序规则为：
1. **先按 user key 升序**
2. **相同 user key 按序列号降序**（更新的条目排在前面）

这保证了查询时总是先找到最新版本的数据。

## 4. MemTableRep — 可插拔的存储后端

MemTable 的实际数据存储通过 `MemTableRep` 抽象接口实现，支持不同的底层数据结构。

### 4.1 MemTableRep 接口

```cpp
class MemTableRep {
 public:
  virtual KeyHandle Allocate(const size_t len, char** buf);
  virtual void Insert(KeyHandle handle) = 0;
  virtual bool InsertKey(KeyHandle handle);
  virtual void InsertWithHint(KeyHandle handle, void** hint);
  virtual void InsertConcurrently(KeyHandle handle);
  virtual bool Contains(const char* key) const = 0;
  virtual void Get(const LookupKey& k, void* callback_args,
                   bool (*callback_func)(void* arg, const char* entry));
  virtual Iterator* GetIterator(Arena* arena = nullptr) = 0;
  // ...
};
```

### 4.2 内置的 MemTableRep 实现

| 实现 | 工厂类 | 数据结构 | 特点 |
|------|--------|---------|------|
| **SkipListRep** | `SkipListFactory` | InlineSkipList | **默认实现**，支持并发读写 |
| **HashSkipListRep** | `NewHashSkipListRepFactory` | 哈希表 + SkipList | 适合 prefix:suffix 模式，前缀内迭代 |
| **HashLinkListRep** | `NewHashLinkListRepFactory` | 哈希表 + LinkedList/SkipList | 小桶用链表，大桶自动转 SkipList |
| **VectorRep** | `VectorRepFactory` | std::vector | 写多读少场景，迭代时排序 |

### 4.3 MemTableRepFactory

通过 `Options::memtable_factory` 配置，例如：
```cpp
Options options;
options.memtable_factory = std::make_shared<SkipListFactory>();  // 默认
options.memtable_factory = std::make_shared<HashSkipListRepFactory>();
```

## 5. InlineSkipList — 核心数据结构

### 5.1 设计特点

`InlineSkipList` 是 RocksDB 默认的 MemTable 底层存储，相比传统 SkipList 做了以下优化：

1. **内存内联**：key 存储在 Node 内部（而非通过指针指向），节省 1 个指针/节点，改善缓存局部性
2. **并发支持**：`InsertConcurrently()` 使用 CAS（Compare-And-Swap）实现无锁并发写入
3. **Splice 优化**：缓存搜索路径（prev/next），顺序插入时从 O(log N) 降为 O(log D)，D 为距离上次插入的位置距离

### 5.2 Node 内存布局

```
                     Node 内存布局
+------------------------------------------------------+
| next_[-(h-1)] | ... | next_[-1] | next_[0] | key... |
| (高层指针)      |     |           | (0层指针) | (内联) |
+------------------------------------------------------+
^                            ^            ^
|<-- 高层 next 指针空间 -->|  Node起始   key起始
```

关键技巧：
- `next_[0]` 是最低层（level 0）指针，更高层的指针存储在 Node 起始地址**之前**
- Key 紧跟在 `next_[0]` 之后存储
- 通过 `StashHeight()` 临时在 `next_[0]` 中存储节点高度，从 `AllocateKey` 传递到 `Insert`

### 5.3 插入流程

```
Insert(key):
  1. 从 AllocateKey 返回的 Node 中 UnstashHeight() 获取高度
  2. 如果高度 > max_height_，CAS 更新 max_height_
  3. 验证/重构 Splice（缓存的搜索路径）：
     - 如果 Splice 仍 bracketing 当前 key → 直接使用
     - 否则 → 从失效层开始 RecomputeSpliceLevels
  4. 在每一层执行插入：
     - 非并发：直接 SetNext
     - 并发(UseCAS=true)：CASNext 循环直到成功
  5. 更新 Splice 缓存
```

### 5.4 查找流程

```
FindGreaterOrEqual(key):
  从最高层开始：
    x = head_
    level = max_height - 1
    while true:
      next = x->Next(level)
      if next == nullptr || next.key >= key:
        if level == 0: return next
        level-- (下降一层)
      else:
        x = next (在本层继续前进)
```

### 5.5 Finger Search（MultiGet 优化）

`FindGreaterOrEqualWithFinger` 使用上一次搜索的路径作为起点（finger），从 level 0 向上找到仍然 bracketing 当前 key 的层，然后从该层向下搜索，将 MultiGet 的每次查找代价从 O(log N) 降低到 O(log D)。

## 6. MemTable 写入流程

### 6.1 Add 操作

```cpp
Status MemTable::Add(SequenceNumber seq, ValueType type,
                     const Slice& key, const Slice& value, ...) {
  // 1. 编码 Entry: key_size + user_key + seq_and_type + value_size + value + checksum
  // 2. 通过 table_->Allocate() 分配内存
  // 3. 填充编码数据
  // 4. 插入到 MemTableRep:
  //    - 非并发: InsertKey / InsertKeyWithHint
  //    - 并发:   InsertKeyConcurrently / InsertKeyWithHintConcurrently
  // 5. 更新计数器: num_entries_, data_size_, num_deletes_ 等
  // 6. 更新 Bloom Filter（prefix bloom / whole key bloom）
  // 7. 更新 flush 状态: UpdateFlushState()
}
```

### 6.2 Insert Hint 优化

当配置了 `memtable_insert_with_hint_prefix_extractor` 时，MemTable 会为每个前缀维护一个插入位置 hint（`insert_hints_`），避免每次插入都从 head 开始搜索，对前缀有序的写入模式特别有效。

### 6.3 并发写入

当 `allow_concurrent = true` 时：
- 使用 `InsertKeyConcurrently()` 进行 CAS 插入
- 计数器更新延迟到 `BatchPostProcess()` 中批量完成
- 使用 `MemTablePostProcessInfo` 结构收集批次内的计数变化

## 7. MemTable 读取流程

### 7.1 Get 操作

```cpp
bool MemTable::Get(const LookupKey& key, std::string* value, ...) {
  // 1. 检查 bloom_filter_ 是否可能包含该 key
  // 2. 通过 table_->Get() 查找，回调处理匹配的条目
  // 3. GetFromTable() 中处理不同类型的值：
  //    - kTypeValue: 直接返回值
  //    - kTypeDeletion: 返回 NotFound
  //    - kTypeMerge: 收集 merge operands 并执行合并
  // 4. 检查 range_del_table_ 是否有覆盖的 range tombstone
}
```

### 7.2 查找优先级

读取时从 MemTable 中查找的顺序：
1. 先在 range_del_table_ 中查找覆盖的 Range Deletion
2. 在 table_ 中查找最新的条目（序列号降序，第一个匹配的就是最新版本）
3. 处理 Merge 操作链

### 7.3 Bloom Filter 加速

MemTable 支持两种 Bloom Filter：
- **Prefix Bloom**：基于 key 前缀（通过 `prefix_extractor` 提取）
- **Whole Key Bloom**：基于完整 user key

在 Get 操作前先检查 Bloom Filter，如果不在则直接跳过，避免不必要的 SkipList 查找。

## 8. MemTable 生命周期

### 8.1 状态转换

```
Active MemTable → (写满/手动触发) → Immutable MemTable → (Flush 完成) → 释放
```

关键方法：
- `MarkImmutable()`：标记为不可变，调用 `table_->MarkReadOnly()`，停止 arena 分配
- `MarkFlushed()`：标记已刷盘，调用 `table_->MarkFlushed()`
- 引用计数：`Ref()` / `Unref()`，引用归零时析构

### 8.2 Flush 触发条件

`ShouldFlushNow()` 判断逻辑：
1. 是否被标记为需要 flush（`IsMarkedForFlush()`）
2. Range Deletion 数量是否超过 `memtable_max_range_deletions`
3. 内存使用量判断：
   - 已分配内存 + 一个 arena block < write_buffer_size → 不 flush
   - 已分配内存 > write_buffer_size + 允许超分配量 → flush
   - 边界情况：arena 最后一个 block 使用率超过 75% → flush

### 8.3 MemTableList

`MemTableList` / `MemTableListVersion` 管理一组 Immutable MemTable：
- 按从新到旧的顺序存储
- 支持 Get、MultiGet 等跨多个 MemTable 的查询
- 维护 history（已 flush 但可能仍在被读取的 MemTable）

## 9. 范围删除（Range Deletion）

### 9.1 独立存储

Range Deletion 条目存储在独立的 `range_del_table_`（始终使用 SkipList），与点键数据分开存储，原因：
- Range Deletion 可以与读取路径并发插入（`AddLogicallyRedundantRangeTombstone`）
- 便于构建 `FragmentedRangeTombstoneList`

### 9.2 FragmentedRangeTombstoneList

当 MemTable 变为 Immutable 时，调用 `ConstructFragmentedRangeTombstones()` 构建碎片化的 Range Tombstone 列表，用于高效的范围删除查询。

## 10. In-Place Update

当启用 `inplace_update_support` 时：
- MemTable 维护一组读写锁 `locks_`（数量由 `inplace_update_num_locks` 控制）
- `Update()` 方法尝试原地更新值（要求新值不超过旧值大小）
- `UpdateCallback()` 使用用户定义的 `inplace_callback` 进行原地更新
- 注意：inplace_update 与 snapshot 不兼容（`IsSnapshotSupported()` 返回 false）

## 11. 内存管理

### 11.1 ConcurrentArena

MemTable 使用 `ConcurrentArena` 作为内存分配器，特点：
- 线程安全的内存分配
- 支持大页内存（`memtable_huge_page_size`）
- 按 block 分配，减少 malloc 调用
- `AllocTracker` 跟踪内存使用量，与 `WriteBufferManager` 协作

### 11.2 WriteBufferManager

`WriteBufferManager` 全局管理所有 MemTable 的内存使用：
- 当总内存超过 `write_buffer_size` 时触发 flush
- 支持 cost to cache 模式，将 MemTable 内存计入 block cache 配额

## 12. 关键配置选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `write_buffer_size` | 64MB | 单个 MemTable 大小上限 |
| `memtable_factory` | SkipListFactory | MemTable 存储后端 |
| `arena_block_size` | 0(自动) | Arena 分配块大小 |
| `memtable_prefix_bloom_size_ratio` | 0 | Prefix Bloom Filter 大小比例 |
| `memtable_whole_key_filtering` | true | 是否启用 whole key bloom |
| `memtable_huge_page_size` | 0 | 大页内存大小 |
| `inplace_update_support` | false | 是否支持原地更新 |
| `max_successive_merges` | 0 | 连续 merge 操作上限 |
| `memtable_insert_with_hint_prefix_extractor` | nullptr | 插入 hint 的前缀提取器 |
| `memtable_protection_bytes_per_key` | 0 | 每个 key 的校验和字节数 |

## 13. 性能优化总结

1. **InlineSkipList**：key 内联存储，减少指针追踪，提升缓存命中率
2. **Splice 缓存**：顺序插入优化，避免每次从头搜索
3. **Insert Hint**：前缀分组，同组连续插入加速
4. **Finger Search**：MultiGet 中利用上一次搜索位置加速
5. **Bloom Filter**：快速排除不存在的 key
6. **并发写入**：CAS 无锁插入，提升多线程写性能
7. **ConcurrentArena**：线程安全的内存分配，减少锁竞争
8. **LookaheadIterator**：在 Seek 时从上次位置线性查找若干步，优化连续 key 访问
