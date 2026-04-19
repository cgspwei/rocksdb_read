# RocksDB 读数据流程分析

## 1. 概述

RocksDB 的读流程本质上是一个**从新到旧、从快到慢**的分层查找过程，遵循 LSM-Tree 的核心设计原则。每次 `Get()` 请求依次查询：活跃 MemTable → Immutable MemTable → L0 SST → L1+ SST，找到即停。

核心代码位置：
- `db/db_impl/db_impl.cc` — 顶层入口 `DBImpl::Get()`
- `db/version_set.cc` — 版本管理与 SST 文件层级查找 `Version::Get()`
- `db/memtable.cc` — 内存表查找 `MemTable::Get()`
- `table/block_based/block_based_table_reader.cc` — SST 文件读取 `BlockBasedTable::Get()`
- `cache/lru_cache.cc` — Block Cache 管理

---

## 2. 整体架构

```
用户调用 DB::Get(key)
        │
        ▼
   DBImpl::Get()                  ← 入口，获取 SuperVersion 快照
        │
        ├─► MemTable::Get()            ← 1. 查活跃 MemTable
        │
        ├─► Immutable MemTable::Get()  ← 2. 查不可变 MemTable（从新到旧）
        │
        └─► Version::Get()             ← 3. 按层查 SST 文件
                │
                ├─► L0: 遍历所有 SST（文件间可能重叠，从新到旧）
                ├─► L1+: 二分查找定位唯一候选文件
                │
                └─► TableCache::Get()
                        │
                        └─► BlockBasedTable::Get()
                                │
                                ├─► Bloom Filter（快速跳过不含此 key 的文件）
                                ├─► Index Block（定位 Data Block 偏移量）
                                ├─► Block Cache（热数据内存缓存）
                                └─► 磁盘 I/O（Cache Miss 时）
```

---

## 3. 第一步：入口 — `DBImpl::Get()`

**文件：** `db/db_impl/db_impl.cc`，约 L2442

```cpp
Status DBImpl::Get(const ReadOptions& read_options,
                   ColumnFamilyHandle* column_family,
                   const Slice& key, PinnableSlice* value)
```

### 3.1 获取 SuperVersion

读操作的第一步是原子性地获取 `SuperVersion`，它是当前数据库状态的一致快照：

```
SuperVersion = {
    mem_        → 活跃 MemTable（接受写入）
    imm_        → Immutable MemTable 列表（等待 Flush）
    current_    → Version（当前所有 SST 文件集合）
}
```

通过 SuperVersion，读操作无需持有全局锁，也不会被 Compaction / Flush 干扰。

**代码路径：**
```
DBImpl::Get()
  └─► GetImpl()
        └─► sv = GetReferencedSuperVersion(cfd)   ← 原子获取快照
        └─► LookupKey lkey(key, snapshot_seq)      ← 构造查询 key
        └─► sv->mem->Get()                         ← 查 MemTable
        └─► sv->imm->Get()                         ← 查 Immutable MemTable
        └─► sv->current->Get()                     ← 查 SST 文件
```

### 3.2 LookupKey 结构

每次查询都基于 `LookupKey`，包含：
- `user_key` — 用户传入的原始 key
- `sequence_number` — 快照对应的序列号（保证读一致性）
- `type` — `kValueTypeForSeek`（查找时使用）

```
LookupKey 内存布局:
┌──────────┬──────────┬──────────────┬──────────────────┐
│ key_size │ user_key │ sequence_num │ value_type (1B)  │
└──────────┴──────────┴──────────────┴──────────────────┘
```

---

## 4. 第二步：查 MemTable — `MemTable::Get()`

**文件：** `db/memtable.cc`，约 L1485

MemTable 持有最新写入的数据，优先查询。

### 4.1 Bloom Filter 预检（可选）

若开启 `memtable_whole_key_filtering`：
- 先查 MemTable 内置的 Bloom filter
- 判断 key 不存在 → 直接跳过该 MemTable，避免 SkipList 遍历

### 4.2 SkipList 查找

MemTable 底层默认使用 `InlineSkipList`：

```
SkipList 节点格式（internal key 编码）:
┌─────────────┬────────────┬──────────────────┐
│  key_size   │  user_key  │  seq_num(7B) + type(1B)  │
└─────────────┴────────────┴──────────────────┘
```

查找步骤：
1. `Iterator::Seek(lookup_key)` — 定位到第一个 `>= lookup_key` 的节点
2. 比较 `user_key` 是否匹配
3. 检查 `sequence_number <= snapshot_seq`（确保不读到未来的写入）
4. 检查 `value_type`：
   - `kTypeValue` → 返回 value
   - `kTypeDeletion` → 返回 `NotFound`（被删除）
   - `kTypeMerge` → 累积 merge operand，继续向下查

### 4.3 Immutable MemTable 查找

活跃 MemTable 未命中后，查 `ImmutableMemTableList`，**从最新到最旧**逐个检查，逻辑与 MemTable::Get() 相同。

---

## 5. 第三步：查 SST 文件 — `Version::Get()`

**文件：** `db/version_set.cc`，约 L2714

### 5.1 FilePicker — 文件选择器

`Version::Get()` 使用 `FilePicker` 按层遍历候选 SST 文件：

```
FilePicker 查找顺序:
  L0 → L1 → L2 → ... → Ln
  每层找到匹配文件后立即调用 TableCache::Get()
  找到结果则停止（除非需要合并 Merge operands）
```

### 5.2 L0 层（特殊处理）

L0 文件由 MemTable 直接 Flush 而来，**文件之间 key range 可能重叠**：
- 按文件**从新到旧**逐个检查（file number 越大越新）
- 每个文件都需要检查其 key range 是否包含目标 key

```
L0: [file5: a-z][file4: b-y][file3: a-m]...
     ↑最新                              ↑最旧
     逐个检查（不可跳过）
```

### 5.3 L1+ 层（二分查找优化）

L1 及以上层的文件 **key range 互不重叠**，每层至多一个文件包含目标 key：

```
L1: [a-c][d-f][g-k][l-z]
             ↑
          二分定位，O(log n)
```

此外，RocksDB 利用 **Fractional Cascading** 优化：上一层的查找结果可以缩小下一层的二分查找范围，进一步减少比较次数。

---

## 6. 第四步：SST 文件读取 — `BlockBasedTable::Get()`

**文件：** `table/block_based/block_based_table_reader.cc`，约 L2489

这是读路径中层次最多、最复杂的部分。

### 6.1 SST 文件整体结构

```
SST 文件布局（BlockBasedTable 格式）:
┌──────────────────────────────┐
│      Data Blocks             │  ← 实际 KV 数据，按 key 排序
├──────────────────────────────┤
│      Meta Blocks             │
│   - Filter Block (Bloom)     │  ← 用于快速判断 key 是否存在
│   - Index Block              │  ← 每个 Data Block 的最大 key + 偏移量
│   - Compression Dict Block   │
│   - Range Deletion Block     │
├──────────────────────────────┤
│      Metaindex Block         │  ← Meta Block 的索引
├──────────────────────────────┤
│      Footer                  │  ← 文件元信息，魔数校验
└──────────────────────────────┘
```

### 6.2 Bloom Filter 过滤

**文件：** `table/block_based/block_based_table_reader.cc`，约 L2325

Bloom filter 是跳过不必要磁盘 I/O 的关键，误判率约 1%（可通过 `bits_per_key` 配置）：

```
Full Filter（默认，整个文件一个 Bloom filter）:
  检查 user_key → 返回"不存在" → 跳过整个 SST 文件 ✓

Partitioned Filter（大文件优化）:
  1. 查 Filter Partition Index（定位对应 filter partition）
  2. 查具体的 Filter Block
  → 减少单个 filter block 的内存占用
```

若 Bloom filter 判断 key **可能存在**（含误判），继续向下查找；判断**不存在**则直接返回，无磁盘 I/O。

### 6.3 Index Block 查找

Index Block 记录每个 Data Block 的分隔 key 和文件偏移量，通常常驻内存或 Block Cache 高优先级区：

```
Index Block 格式:
  [分隔key1 → DataBlock1 offset+size]
  [分隔key2 → DataBlock2 offset+size]
  ...

查找：二分搜索找到第一个 分隔key >= target_key 的条目
      → 得到目标 Data Block 的 (offset, size)
```

支持三种 Index 类型：
- `BinarySearchIndex` — 标准二分查找索引
- `HashIndex` — 对前缀哈希加速
- `PartitionedIndex` — 两级分区索引，减少常驻内存开销

### 6.4 Block Cache 查询

**文件：** `cache/lru_cache.cc`

```
Block Cache 查询流程：

1. 构造 cache key = {file_number, block_offset}
2. cache->Lookup(cache_key)
   ├─► Cache Hit  → 直接使用内存中的 Block 数据，返回 ✓（无磁盘 I/O）
   └─► Cache Miss → 触发磁盘读取

3. 磁盘读取（Cache Miss）：
   └─► file->Read(offset, size, &block_data)   ← 随机读
   └─► 解压缩（Snappy / Zstd / LZ4 / Zlib）
   └─► 校验 Checksum（CRC32c 或 xxHash64）
   └─► cache->Insert(cache_key, block)          ← 插入 Cache 供复用
```

Block Cache 内部优先级：
```
HIGH priority → Index Block / Filter Block（元数据，优先保留）
LOW priority  → Data Block（普通数据，可被淘汰）
```

### 6.5 Data Block 内查找

Data Block 使用**前缀压缩**存储 key，配合 Restart Points 支持二分：

```
Data Block 内部格式：
┌──────────────────────────────────────────────────────┐
│ Entry1: shared=0,  delta_key="apple",  value="v1"    │  ← Restart Point
│ Entry2: shared=2,  delta_key="ricot",  value="v2"    │  ("ap" + "ricot")
│ Entry3: shared=5,  delta_key="ication",value="v3"    │
│ Entry4: shared=0,  delta_key="banana", value="v4"    │  ← Restart Point
│ ...                                                   │
│ [Restart Point Array: offset of entry1, entry4, ...] │
│ [num_restarts]                                        │
└──────────────────────────────────────────────────────┘
```

查找步骤：
1. 在 Restart Point 数组中**二分定位**大致区间
2. 从该 Restart Point 起**线性扫描**，还原完整 key 后比较
3. 找到 `user_key` 匹配且 `seq <= snapshot_seq` 的最新版本

---

## 7. 第五步：结果处理与返回

### 7.1 Merge Operator

若写入时使用了 `Merge()` 操作，读取时需要**从上到下收集所有 merge operands**，最终调用 `MergeOperator::FullMerge()` 合并成最终 value。

### 7.2 Range Deletion 处理

每个 SST 文件可能包含 Range Deletion（范围删除）。读取时需要检查目标 key 是否落入某个已删除的 key range，若落入则视为不存在。

### 7.3 最终返回

```
找到 kTypeValue   → 返回 value，Status::OK()
找到 kTypeDeletion → 返回 Status::NotFound()
未在任何层找到    → 返回 Status::NotFound()
读取 I/O 错误     → 返回 Status::IOError(...)
```

---

## 8. 性能关键路径

| 场景 | 典型延迟 | 关键因素 |
|------|----------|----------|
| MemTable 命中 | ~1 µs | SkipList 查找，纯内存操作 |
| Immutable MemTable 命中 | ~1–2 µs | 同上，可能遍历多个 |
| Block Cache 命中（Data Block）| ~5–10 µs | 内存哈希查找 + block 内二分 |
| Bloom Filter 过滤（避免 I/O）| ~5 µs | 仅内存计算，跳过磁盘 |
| SSD 磁盘读取（Cache Miss）| ~50–200 µs | 随机读 + 解压 + 校验 |
| HDD 磁盘读取（Cache Miss）| ~5–15 ms | 机械寻道 |

**最坏情况（key 不存在时）：**
- 遍历全部 MemTable → L0 所有文件 → 每层各一个文件
- 若无 Bloom filter：每个文件都需读取 Index Block 和 Data Block
- 若有 Bloom filter：大多数文件可通过 filter 快速跳过（节省 ~99% I/O）

---

## 9. 关键设计总结

| 设计 | 目标 | 实现机制 |
|------|------|----------|
| SuperVersion | 无锁一致性读 | 原子引用计数快照 |
| MemTable 优先 | 读最新数据 | 先 mem → imm → SST |
| Bloom Filter | 减少磁盘 I/O | 每个 SST 文件独立 filter |
| Block Cache | 热数据内存化 | LRU + 优先级分层 |
| L1+ 二分查找 | 降低读放大 | 层内 key range 不重叠 |
| 前缀压缩 | 减小 Data Block 体积 | Restart Point + delta key |
| Sequence Number | MVCC 快照隔离 | 每条 KV 携带写入序号 |
