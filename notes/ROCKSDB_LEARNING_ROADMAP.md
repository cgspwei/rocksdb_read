# RocksDB 源码学习路线图

> **目标**：系统学习 RocksDB 的架构设计、关键执行路径和自研组件，提升系统架构和 C++ 编程能力。

---

## 零、前置原则

### 不要试图通读代码
RocksDB 有 400+ 文件、几十万行代码。通读是不可能的，也没必要。学习方式是：
1. **以问题驱动**：每个阶段围绕一个具体问题（"一个 Put 请求到底发生了什么？"）
2. **先主干后枝叶**：先理解架构骨架，再深入优化细节
3. **边读边记**：每学完一个模块，写自己的笔记（你已经在做了）
4. **画图**：用 ASCII art 或工具画流程图、类图，视觉化理解比纯读代码高效

### 已完成的笔记
你已经有了以下笔记（在 `notes/` 目录下），这些可以作为起点和参考：
- `read_path.md` — 读路径完整分析
- `write_path.md` — 写路径完整分析
- `memtable.md` — MemTable 实现分析
- `skiplist_deep_dive.md` — 跳表深度解析
- `slice_and_arena.md` — Slice 和 Arena 内存管理
- `smart_pointers.md` — 智能指针使用分析
- `cpp_features.md` — C++ 特性使用全景

---

## 第一阶段：建立全局认知（1-2 周）

**目标**：理解 RocksDB 是什么、LSM-Tree 基本原理、顶层架构是什么。

### 1.1 先看文档，不碰代码

| 优先级 | 内容 | 位置 |
|--------|------|------|
| 必读 | RocksDB Wiki（架构概述） | `wiki/` 目录 |
| 必读 | LSM-Tree 论文原文或中文解读 | 网上搜索 "LSM-Tree paper" |
| 推荐 | RocksDB 官方博客 | rocksdb.org/blog |

### 1.2 通读公共 API

从用户视角理解 RocksDB 能做什么，这是理解内部实现的基础。

| 文件 | 内容 | 预计时间 |
|------|------|---------|
| `include/rocksdb/db.h` (2399 行) | DB 打开、Put、Get、Write、Iterator、ColumnFamily | 2 小时 |
| `include/rocksdb/options.h` (3223 行) | DBOptions、ColumnFamilyOptions、ReadOptions、WriteOptions | 1.5 小时 |
| `include/rocksdb/status.h` (635 行) | Status 类型，错误传播模式 | 30 分钟 |
| `include/rocksdb/slice.h` (305 行) | Slice 类型，零拷贝字节引用 | 20 分钟 |
| `include/rocksdb/write_batch.h` (549 行) | WriteBatch，原子批量写 | 30 分钟 |
| `include/rocksdb/iterator.h` (121 行) | Iterator 接口 | 15 分钟 |
| `include/rocksdb/cache.h` (562 行) | Cache 接口 | 20 分钟 |

**关键问题**：
- `DB::Put()` 和 `DB::Write()` 有什么区别？什么时候用哪个？
- `DB::Get()` 和 `Iterator::Seek()` 有什么区别？
- ColumnFamily 是什么？为什么需要它？
- Snapshot 提供什么语义？

### 1.3 理解 LSM-Tree 核心概念

不需要看代码，先建立概念模型：

```
写入（Write Path）：
  Put(key, value) → WAL（持久化）→ MemTable（内存有序结构）→ 返回

刷盘（Flush）：
  MemTable 写满 → Immutable MemTable → 后台线程 Flush 到磁盘 → L0 SST 文件

合并（Compaction）：
  L0 SST 文件过多 → 选若干文件和 L1 合并 → 产生新的 L1 文件
  L1 太大 → 选 L1 文件和 L2 合并 → ...
  逐层向下合并，保持每层大小在目标范围内

读取（Read Path）：
  Get(key) → MemTable → Immutable MemTables → L0 SST → L1 SST → ... → 找到即停
```

---

## 第二阶段：数据编码基础（1 周）

**目标**：理解 RocksDB 如何编码和存储数据，这是理解所有上层逻辑的前提。

### 2.1 编码层

| 文件 | 内容 | 关键 C++ 技巧 |
|------|------|-------------|
| `util/coding.h` (373 行) + `util/coding.cc` | 变长整数（varint）、定长整数编解码 | 位运算、字节序处理、`reinterpret_cast` |
| `db/dbformat.h` (约 500 行) | InternalKey、LookupKey、ValueType、SequenceNumber | 组合模式、`Slice` 的子切片操作 |
| `db/dbformat.cc` | 序列化/反序列化实现 | `memcmp`、`PackSequenceAndType` |

**关键问题**：
- InternalKey 的格式是什么？为什么要把 sequence number 放在 key 后面？
- ValueType 有哪些？kTypeDeletion 和 kTypeSingleDeletion 的区别是什么？
- 为什么 InternalKey 要按 (user_key, descending_seq) 排序？

**C++ 学习点**：
- 位运算实战：`util/coding.h` 中的 `EncodeVarint32` 和 `GetVarint32` 是教科书级的位操作代码
- 字节序处理：`EncodeFixed64` / `DecodeFixed64` 中的大端/小端转换

### 2.2 压缩与校验

| 文件 | 内容 | 关键 C++ 技巧 |
|------|------|-------------|
| `util/compression.h` (759 行) | 压缩/解压抽象，多算法支持 | 工厂模式、策略模式 |
| `util/compression.cc` (1987 行) | Snappy/LZ4/ZSTD/Zlib 具体实现 | RAII、`unique_ptr` 管理压缩上下文 |
| `util/crc32c.cc` (1295 行) | 硬件加速 CRC32C 校验 | SSE4.2 内联汇编、ARM NEON 指令 |

---

## 第三阶段：MemTable — 写入的第一站（1 周）

**目标**：深入理解 MemTable 的数据结构和并发模型。

### 3.1 MemTable 接口

| 文件 | 内容 |
|------|------|
| `db/memtable.h` + `db/memtable.cc` | MemTable 类，封装 SkipList + Bloom Filter + Arena |
| `include/rocksdb/memtablerep.h` | MemTableRep 抽象接口 |

### 3.2 SkipList — 核心数据结构（重点）

| 文件 | 内容 | 难度 | 预计时间 |
|------|------|------|---------|
| `memtable/skiplist.h` (518 行) | 基础 SkipList（LevelDB 遗产），无并发 | ⭐⭐ | 1 小时 |
| `memtable/inlineskiplist.h` (1422 行) | **InlineSkipList**（生产默认），无锁并发 | ⭐⭐⭐⭐⭐ | 4-6 小时 |
| `memtable/skiplistrep.cc` (425 行) | SkipList 到 MemTableRep 的适配层 | ⭐⭐ | 30 分钟 |

**InlineSkipList 的 C++ 学习点**（这是 RocksDB 最值得学的自研组件之一）：
1. **内存布局创新**：Node 使用柔性数组（flexible array），key 紧跟在 node 结构体后面，减少一次指针间接访问
2. **无锁并发插入**：使用 CAS（Compare-And-Swap）实现 `Insert()`，不需要外部互斥锁
3. **内存序（Memory Order）**：`std::memory_order_release` / `acquire` / `relaxed` 的实战应用
4. **Splice 优化**：缓存上一次查找的路径，减少重复遍历
5. **Finger Search**：从最近访问位置开始搜索，利用局部性

**关键问题**：
- 为什么 InlineSkipList 比 SkipList 快？内存布局差异带来的缓存友好性如何？
- CAS 插入的过程中，如果多个线程同时插入相邻的 key，如何保证正确性？
- `Node::Next()` 和 `Node::SetNext()` 中不同 memory order 的用途是什么？

### 3.3 Arena — 内存分配器

| 文件 | 内容 | 关键 C++ 技巧 |
|------|------|-------------|
| `memory/arena.h` + `memory/arena.cc` | Arena 分配器 | 内存对齐、`posix_memalign`、组件化内存管理 |
| `memory/concurrent_arena.h` + `.cc` | 并发 Arena | 线程局部内存池 |

---

## 第四阶段：SST 文件格式（1-2 周）

**目标**：理解 RocksDB 的持久化格式，这是读路径的基础。

### 4.1 Block 结构

| 文件 | 内容 | 预计时间 |
|------|------|---------|
| `table/block_based/block.h` (1051 行) | Block 类声明：restart point、二分查找 | 30 分钟 |
| `table/block_based/block.cc` (1617 行) | Block 实现：`Seek()`、`BinarySeek()` | 1 小时 |
| `table/block_based/block_builder.cc` (443 行) | 从 KV 对构建 Block，restart point 编码 | 30 分钟 |

**关键问题**：
- Restart point 是什么？为什么每 16 个 key 设置一个 restart point？
- Block 的二分查找是怎么做的？为什么不在整个 block 上做二分？
- 前缀压缩（prefix compression）的原理是什么？

### 4.2 SST 文件整体结构

| 文件 | 内容 | 预计时间 |
|------|------|---------|
| `table/block_based/block_based_table_builder.cc` (3073 行) | 构建 SST 文件：data block → index block → filter block → meta block → footer | 2 小时 |
| `table/block_based/block_based_table_reader.cc` (3758 行) | 读取 SST 文件：footer → index block → data block → 查找 | 2 小时 |

**关键问题**：
- 一个 SST 文件从开始到结束有哪些组成部分？
- Index block 和 data block 的关系是什么？两层索引是怎么工作的？
- Bloom filter 存在哪里？怎么被使用的？
- Footer 的作用是什么？

---

## 第五阶段：读路径全景（1 周）

**目标**：从一个 `Get()` 调用追踪到底，理解完整的读路径。

> 你已经有了 `notes/read_path.md`，本阶段是基于已有笔记，深入源码验证和补充细节。

### 5.1 读路径主线

按调用链追踪（建议用 IDE 的 "Go to Definition" 逐层深入）：

```
DBImpl::Get()                          ← db/db_impl/db_impl.cc
  → 获取 SuperVersion 快照
  → MemTable::Get()                    ← db/memtable.cc（你已学过）
  → MemTableList::Get()                ← db/memtable_list.cc（查 Immutable MemTables）
  → Version::Get()                     ← db/version_set.cc
    → 遍历 L0 SST（文件间重叠，从新到旧）
    → 遍历 L1+ SST（文件不重叠，二分定位）
      → BlockBasedTable::Get()         ← table/block_based/block_based_table_reader.cc（你已学过）
        → Filter block 检查 Bloom filter
        → Index block 二分定位
        → Data block 读取 → 二分查找
        → Cache 查找/插入             ← cache/lru_cache.cc
```

**关键文件**：

| 文件 | 内容 | 预计时间 |
|------|------|---------|
| `db/db_impl/db_impl.cc` | Get() 入口，SuperVersion 管理 | 1 小时 |
| `db/memtable_list.cc` | Immutable MemTable 列表遍历 | 30 分钟 |
| `db/version_set.cc` | Version::Get()，SST 文件层级查找 | 1 小时 |
| `table/block_based/block_based_table_iterator.cc` (1214 行) | 两层迭代器（index block → data block） | 1 小时 |
| `cache/lru_cache.cc` (736 行) | LRU Cache 实现 | 1 小时 |

**关键问题**：
- SuperVersion 是什么？为什么读路径需要获取它的快照（引用计数）？
- L0 和 L1+ 的读取策略为什么不同？
- Block Cache 的 key 是什么？怎么保证不同 SST 文件的 block 不会冲突？
- Table Cache 和 Block Cache 的区别是什么？

### 5.2 性能关键细节

| 主题 | 文件 | 内容 |
|------|------|------|
| FilePrefetchBuffer | `file/file_prefetch_buffer.h` + `.cc` | 预读优化：顺序扫描时提前异步读取后续 block |
| PinnableSlice | `include/rocksdb/slice.h` → `PinnableSlice` | 避免数据拷贝：结果可以直接 pin 在 block cache 中 |
| MultiGet | `db/db_impl/db_impl.cc` → `MultiGet()` | 批量读优化：一次 IO 读取多个 key |
| TableCache | `db/table_cache.cc` | SST Reader 缓存：避免重复打开文件 |

---

## 第六阶段：写路径全景（1 周）

**目标**：从一个 `Put()` 调用追踪到底，理解完整的写路径。

> 你已经有了 `notes/write_path.md`，本阶段深入源码。

### 6.1 写路径主线

```
DBImpl::Write()                        ← db/db_impl/db_impl_write.cc
  → WriteThread::JoinBatchGroup()     ← db/write_thread.cc
    → Leader 收集 Follower，组成 WriteGroup
  → 写 WAL（Write-Ahead Log）
    → log::Writer::AddRecord()        ← db/log_writer.cc
    → WritableFileWriter::Sync()      ← file/writable_file_writer.cc
  → WriteBatchInternal::InsertInto()  ← db/write_batch.cc
    → MemTable::Add()                 ← db/memtable.cc（你已学过）
  → 通知 Follower 完成
  → 返回
```

**关键文件**：

| 文件 | 内容 | 预计时间 |
|------|------|---------|
| `db/db_impl/db_impl_write.cc` (3614 行) | Write()、WriteImpl()，写路径主逻辑 | 2 小时 |
| `db/write_thread.cc` (约 500 行) | **WriteGroup 组提交机制**（重点） | 2 小时 |
| `db/write_batch.cc` (约 600 行) | WriteBatch 序列化、回放、插入 MemTable | 1 小时 |
| `db/log_writer.cc` (约 300 行) | WAL 物理格式：Record 分块、CRC 校验 | 1 小时 |
| `file/writable_file_writer.cc` (约 800 行) | 文件写入封装：缓冲、Direct I/O、Sync | 1 小时 |

**关键问题**：
- WriteGroup 组提交的 Leader/Follower 模式是什么？为什么这样设计？
- WAL 的 Record 格式是什么？一个 WriteBatch 太大时怎么拆分成多个 Record？
- `WriteOptions::sync` 和 `WriteOptions::disableWAL` 的区别？
- 写 stall 是什么？什么条件触发？

### 6.2 写路径优化

| 主题 | 文件 | 内容 |
|------|------|------|
| Pipelined Write | `db/db_impl/db_impl_write.cc` | 流水线写：WAL 写入和 MemTable 写入可重叠 |
| Parallel Write | `db/db_impl/db_impl_write.cc` | 并行写：多个 WriteGroup 可并发执行 |
| Write Stall | `db/write_controller.h` + `.cc` | 写限速：MemTable 满、L0 文件过多时减速 |
| Two Phase Commit | `utilities/transactions/` | 分布式事务的两阶段提交 |

---

## 第七阶段：Compaction（1-2 周）

**目标**：理解 LSM-Tree 的合并策略和实现。

### 7.1 Compaction 触发与选择

| 文件 | 内容 | 预计时间 |
|------|------|---------|
| `db/compaction/compaction_picker.h` + `.cc` (1674 行) | CompactionPicker 基类：文件评分、范围扩展 | 1 小时 |
| `db/compaction/compaction_picker_level.cc` (1009 行) | **Level Compaction**：按层级大小触发 | 1.5 小时 |
| `db/compaction/compaction_picker_universal.cc` (1872 行) | **Universal Compaction**：按 sorted runs 触发 | 1.5 小时 |
| `db/compaction/compaction_picker_fifo.cc` (791 行) | FIFO Compaction：按时间/大小删除 | 30 分钟 |

**关键问题**：
- Level Compaction 的 score 怎么计算的？
- Level Compaction 选完 L1 文件后，为什么还要扩展 L0 文件范围？
- Universal Compaction 的 size ratio 和 max size amplification 是什么意思？

### 7.2 Compaction 执行

| 文件 | 内容 | 预计时间 |
|------|------|---------|
| `db/compaction/compaction_job.cc` (3325 行) | **CompactionJob**：执行一次合并（重点） | 2-3 小时 |
| `db/compaction/compaction_iterator.cc` (2095 行) | **CompactionIterator**：多路归并 + 过滤 | 2 小时 |

**CompactionJob 的执行流程**：
```
CompactionJob::Run()
  → 打开所有输入 SST 文件
  → 创建 CompactionIterator（多路归并）
  → 对每个 key：
    → CompactionFilter 过滤（用户自定义）
    → 处理 Merge 操作
    → 处理过期 Snapshot（垃圾回收旧版本）
    → 写入新的 SST 文件
  → 生成 OutputLevel 的新 SST 文件
  → 更新 Version（MANIFEST）
  → 删除旧的 SST 文件
```

**关键问题**：
- CompactionIterator 的多路归并是怎么实现的？用的是什么数据结构？
- 在 Compaction 过程中，如何处理同一个 key 的多个版本？
- Snapshot 对 Compaction 有什么影响？（为什么不能删除被 snapshot 引用的旧版本？）

---

## 第八阶段：Cache 系统（1 周）

**目标**：理解 RocksDB 的缓存层次和 LRU/Clock 算法。

| 文件 | 内容 | 预计时间 |
|------|------|---------|
| `cache/lru_cache.cc` (736 行) | LRU Cache：哈希表 + LRU 双向链表 | 2 小时 |
| `cache/clock_cache.cc` (3627 行) | Clock Cache：近似 LRU，更低的锁竞争 | 2 小时 |
| `cache/sharded_cache.cc` (147 行) | 分片：将 Cache 分成 N 个 shard，减少锁竞争 | 30 分钟 |
| `cache/compressed_secondary_cache.cc` (480 行) | 二级缓存：被淘汰的 block 压缩后存入二级缓存 | 1 小时 |

**C++ 学习点**：
- LRU Cache 的哈希表 + 双向链表实现：`LRUHandleTable` 的可增长哈希表 + `LRUHandle` 的侵入式链表
- Clock Cache 的 Clock 算法：用一个引用位近似 LRU，避免维护链表
- Sharded Cache：用哈希分片减少锁竞争，每个 shard 有独立的互斥锁

**关键问题**：
- Block Cache 的淘汰策略是什么？LRU 和 Clock 的区别？
- 什么是 Table Cache？它和 Block Cache 的关系是什么？
- 二级缓存（Secondary Cache）解决了什么问题？

---

## 第九阶段：版本管理与 MANIFEST（1 周）

**目标**：理解 RocksDB 如何管理 SST 文件的版本和元数据。

| 文件 | 内容 | 预计时间 |
|------|------|---------|
| `db/version_set.h` + `db/version_set.cc` (约 6000+ 行) | Version、VersionSet、VersionEdit、MANIFEST 读写 | 3 小时 |
| `db/version_edit.h` + `db/version_edit.cc` (约 1000 行) | VersionEdit：版本变更记录 | 1 小时 |
| `db/builder.cc` | 构建新 SST 文件（Flush 和 Compaction 共用） | 30 分钟 |
| `db/column_family.cc` | ColumnFamily 管理 | 1 小时 |

**关键问题**：
- Version 和 VersionEdit 的区别是什么？
- SuperVersion 和 Version 的关系？
- MANIFEST 文件的作用是什么？恢复时如何使用？
- 为什么需要引用计数（Ref() / Unref()）来管理 Version？

---

## 第十阶段：后台任务调度（3 天）

| 文件 | 内容 |
|------|------|
| `db/db_impl/db_impl_compaction_flush.cc` (5341 行) | Flush 和 Compaction 的调度入口 |
| `env/env.cc` (1325 行) | Env 中的线程池管理 |
| `util/threadpool_imp.cc` (约 500 行) | 线程池实现 |

**关键问题**：
- Flush 和 Compaction 是怎么被触发和调度的？
- 多个 Column Family 共享同一个线程池时如何调度？
- 什么是 Write Stall？什么条件触发？如何缓解？

---

## 附录 A：C++ 能力提升对照

| 学习主题 | 对应 C++ 技巧 | 源文件 |
|---------|-------------|--------|
| 内存管理 | Arena 分配器、柔性数组、placement new | `memory/arena.h`, `memtable/inlineskiplist.h` |
| 无锁并发 | CAS、memory order、hazard pointer | `memtable/inlineskiplist.h`, `util/atomic.h` |
| RAII 模式 | unique_ptr、MutexLock、Cleanable | `util/mutexlock.h`, `include/rocksdb/cleanable.h` |
| 模板元编程 | if constexpr、SFINAE、_t 别名 | `util/math.h`, `util/ribbon_alg.h`, `table/block_based/block_based_table_reader.cc` |
| 位运算 | varint 编解码、CRC32C 硬件加速 | `util/coding.h`, `util/crc32c.cc` |
| 虚接口设计 | TableFactory、Env、Cache 的抽象层次 | `include/rocksdb/table.h`, `include/rocksdb/env.h` |
| 引用计数 | Version、SuperVersion 的生命周期管理 | `db/version_set.h` |
| 侵入式数据结构 | LRUHandle 链表、InlineSkipList Node | `cache/lru_cache.cc`, `memtable/inlineskiplist.h` |
| 工厂模式 | TableFactory、MemTableRepFactory | `include/rocksdb/table.h`, `include/rocksdb/memtablerep.h` |

## 附录 B：面试常见问题速查

### 基础
- LSM-Tree 的读写放大分别是什么？RocksDB 如何优化？
- RocksDB 的写路径（Put → WAL → MemTable → Flush）完整流程
- RocksDB 的读路径（Get → MemTable → Immutable → SST）完整流程
- Bloom filter 在 RocksDB 中的作用和使用方式

### 进阶
- WriteGroup 组提交的 Leader/Follower 模式，为什么能提高吞吐？
- Level Compaction vs Universal Compaction 的区别和适用场景
- Block Cache 和 Table Cache 的区别
- Write Stall 是什么？什么条件触发？
- SuperVersion 是什么？为什么读路径需要它？

### C++ 专项
- InlineSkipList 如何实现无锁并发插入？
- Arena 分配器相比 malloc/free 的优势？
- RocksDB 为什么自研 Slice 而不是用 std::string_view？
- PinnableSlice 的设计目的是什么？

## 附录 C：建议的每日学习节奏

```
第1天：读公共 API 头文件（db.h + options.h）
第2天：LSM-Tree 概念 + 编码基础（dbformat.h + coding.h）
第3天：MemTable 接口 + 基础 SkipList
第4-5天：InlineSkipList 深度（无锁并发、内存布局）
第6天：Arena 分配器
第7天：Block 结构（block.h + block_builder.cc）
第8-9天：SST 文件格式（builder + reader）
第10天：Block Cache（LRU）
第11-12天：读路径全景（Get 调用链）
第13-14天：写路径全景（Write 调用链）
第15-16天：Compaction（Picker + Job）
第17-18天：版本管理（Version + MANIFEST）
第19-20天：整理笔记、画图、回顾
```
