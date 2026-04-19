# RocksDB 写数据流程分析

## 1. 概述

RocksDB 的写流程以 **LSM-Tree** 为核心，所有写入先进 WAL（Write-Ahead Log）保证持久性，再写入内存 MemTable 提供读可见性。当 MemTable 写满后转为 Immutable MemTable，由后台线程 Flush 到磁盘成为 SST 文件。

核心代码位置：
- `db/db_impl/db_impl_write.cc` — 写流程主逻辑，`DBImpl::Write()` / `DBImpl::WriteImpl()`
- `db/write_thread.cc` — WriteGroup 组提交机制
- `db/log_writer.cc` — WAL 物理日志写入
- `db/memtable.cc` — MemTable 写入，`MemTable::Add()`
- `db/write_batch.cc` — WriteBatch 序列化与回放
- `db/write_controller.h` / `db/write_controller.cc` — 写限速与 Write Stall

---

## 2. 整体架构

```
用户调用 DB::Put(key, value) 或 DB::Write(write_batch)
        │
        ▼
   DBImpl::WriteImpl()            ← 写流程主入口
        │
        ├─► WriteThread::JoinBatchGroup()
        │         │
        │         ├─► 成为 Leader → 收集 Follower 组成 WriteGroup
        │         └─► 成为 Follower → 等待 Leader 完成
        │
        ▼（Leader 执行）
   WriteGroupToWAL()              ← 写 WAL（顺序写，持久化保证）
        │
        ▼
   InsertInto() / 并发写 MemTable  ← 写内存（所有写者可并行）
        │
        ▼
   ExitAsBatchGroupLeader()       ← 唤醒所有 Follower，完成组提交
        │
        ▼
   MemTable::ShouldFlushNow()     ← 判断是否触发 Flush
        │
        └─► 触发 Flush → Immutable MemTable → 后台 Flush 线程 → SST 文件
                                                      │
                                                      └─► 触发 Compaction
```

---

## 3. 第一步：写入入口 — `DBImpl::WriteImpl()`

**文件：** `db/db_impl/db_impl_write.cc`，约 L865

### 3.1 调用链

```
DB::Put(key, value)
  └─► WriteBatch batch; batch.Put(key, value)
  └─► DB::Write(WriteOptions, &batch)
        └─► DBImpl::WriteImpl(WriteOptions, &batch, callback, ...)
```

### 3.2 三种写路径

`WriteImpl()` 根据配置选择三种写路径之一：

```
WriteImpl()
  ├─► unordered_write=true  → UnorderedWriteMemtable()
  │     每个写者独立分配 sequence number，完全并行，不保证顺序
  │
  ├─► enable_pipelined_write=true → PipelinedWriteImpl()
  │     WAL 写入和 MemTable 写入解耦为两条流水线，重叠执行
  │
  └─► 默认 → 标准 WriteGroup 路径（本文重点）
```

### 3.3 Write Stall 检查

进入写路径前，先检查是否需要限速：

```cpp
// db/db_impl/db_impl_write.cc
PERF_TIMER_GUARD(write_pre_and_post_process_time);
WriteThread::Writer w(options, my_batch, callback, ...);
write_thread_.JoinBatchGroup(&w);   // 可能在这里阻塞等待
```

若系统触发了 Write Stall（MemTable 过多、L0 文件过多、Pending Compaction 过大），新写者会在 `JoinBatchGroup()` 内阻塞，直到 stall 解除。

---

## 4. 第二步：组提交 — WriteGroup

**文件：** `db/write_thread.cc`

### 4.1 核心思想

多个并发写者不各自写 WAL，而是由一个 **Leader** 收集若干 **Follower** 组成 `WriteGroup`，一次性批量写入 WAL，摊薄 fsync 开销（Group Commit）。

```
并发写者:        W1   W2   W3   W4   W5
                 │    │    │    │    │
JoinBatchGroup:  Leader   Follower  Follower...
                 │
                 收集兼容的 Follower → WriteGroup{W1,W2,W3}
                 │
                 一次 WAL 写入（包含所有 batch）
                 │
                 并行写入 MemTable
                 │
                 唤醒所有 Follower → 返回给各写者
```

### 4.2 关键数据结构

```cpp
// db/write_thread.h
struct Writer {
    WriteBatch* batch;
    uint64_t sequence;          // 分配的起始 sequence number
    WriteGroup* write_group;
    Writer* link_older;         // 链表：指向更旧的写者
    Writer* link_newer;         // 链表：指向更新的写者
    std::atomic<uint8_t> state; // 当前状态（见 State 枚举）
};

struct WriteGroup {
    Writer* leader;
    Writer* last_writer;
    uint64_t last_sequence;
    std::atomic<size_t> running; // 还在运行的写者数量
    size_t size;
    // 支持 Iterator 遍历所有成员
};
```

Writer 状态机：
```
INIT(1)
  └─► GROUP_LEADER(2)           ← 成为 leader，负责 WAL 写
  └─► PARALLEL_MEMTABLE_WRITER(8) ← 并行写 MemTable
  └─► MEMTABLE_WRITER_LEADER(4) ← MemTable 写的 leader
  └─► COMPLETED(16)             ← 写入完成，可以返回
  └─► LOCKED_WAITING(32)        ← 阻塞等待中（慢路径）
```

### 4.3 Leader 选举与 Follower 收集

```cpp
// write_thread.cc ~L401
void WriteThread::JoinBatchGroup(Writer* w) {
    // 原子 CAS 将自己加入链表尾部
    // 若成功设置为链表头 → 成为 Leader
    // 否则 → 成为 Follower，调用 AwaitState() 等待
}

// write_thread.cc ~L440
size_t WriteThread::EnterAsBatchGroupLeader(Writer* leader,
                                             WriteGroup* write_group) {
    // 从 leader 向前遍历链表，收集兼容的 Follower
    // 兼容条件：
    //   - sync 标志一致（不混合 sync/非sync）
    //   - no_slowdown 一致
    //   - disable_wal 一致
    //   - 总 batch 大小不超过 max_write_batch_group_size_bytes
}
```

### 4.4 自适应等待 — `AwaitState()`

Follower 等待时采用三阶段策略，平衡 CPU 与延迟：

```
Phase 1: Busy Spin（0~200 次）
  └─► port::AsmVolatilePause()  ← CPU pause 指令，降低功耗
  └─► 适合 Leader 很快完成的场景

Phase 2: Yield（up to max_yield_usec）
  └─► std::this_thread::yield()
  └─► 让出 CPU，允许其他线程运行

Phase 3: Block（条件变量等待）
  └─► state_mutex + state_cv
  └─► Leader 完成后调用 notify_one() 唤醒
```

自适应 credit 机制：
- yield 成功（Leader 快速完成）→ credit + 131072
- yield 超时 → credit - 131072
- credit 高 → 更多 Yield，credit 低 → 更早进入 Block

---

## 5. 第三步：WAL 写入

**文件：** `db/db_impl/db_impl_write.cc` + `db/log_writer.cc`

### 5.1 合并 Batch

```cpp
// db_impl_write.cc ~L2209
void DBImpl::MergeBatch(const WriteThread::WriteGroup& write_group,
                        WriteBatch* tmp_batch, WriteBatch** merged_batch) {
    if (write_group.size == 1) {
        // 单写者：直接使用其 batch，避免拷贝
        *merged_batch = write_group.leader->batch;
    } else {
        // 多写者：合并所有 batch 到 tmp_batch
        for (auto& writer : write_group) {
            WriteBatchInternal::Append(tmp_batch, writer.batch);
        }
        *merged_batch = tmp_batch;
    }
}
```

### 5.2 分配 Sequence Number

```cpp
// 每条 KV 记录对应一个递增的 sequence number
// batch 内的记录连续分配：
//   batch 起始 seq = last_sequence + 1
//   batch 内第 i 条记录 seq = start_seq + i
last_sequence += WriteBatchInternal::Count(merged_batch);
```

Sequence Number 作用：
- MVCC 快照隔离（读操作只看 `seq <= snapshot_seq` 的数据）
- Compaction 时判断数据是否过期

### 5.3 物理日志写入 — `log::Writer::AddRecord()`

**文件：** `db/log_writer.cc`，约 L89

WAL 以 **固定 32KB Block** 为单位组织，每条记录写成一到多个 **Fragment**：

```
WAL 文件物理结构：
┌─────────────────────────────────────────────────────────┐
│  Block 0 (32KB)                                         │
│  ┌────────────────────────────────────────────────────┐ │
│  │ Record Header (7B legacy / 11B recyclable)         │ │
│  │   checksum(4B) | length(2B) | type(1B)             │ │
│  │   [+ log_num(4B) recyclable only]                  │ │
│  ├────────────────────────────────────────────────────┤ │
│  │ Record Data (WriteBatch 序列化内容)                  │ │
│  └────────────────────────────────────────────────────┘ │
│  ┌──────────────────────┐ ┌──────────────────────────┐  │
│  │ 另一条完整记录        │ │ 跨 Block 记录的 First 片  │  │
│  └──────────────────────┘ └──────────────────────────┘  │
├─────────────────────────────────────────────────────────┤
│  Block 1 (32KB)                                         │
│  ┌──────────────────────────────────────────────────┐   │
│  │ 跨 Block 记录的 Middle 片                         │   │
│  └──────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────┐   │
│  │ 跨 Block 记录的 Last 片                           │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

Fragment 类型：
```
kFullType   (1) — 整条记录在一个 Block 内
kFirstType  (2) — 跨 Block 记录的第一片
kMiddleType (3) — 跨 Block 记录的中间片
kLastType   (4) — 跨 Block 记录的最后一片
```

每个 Fragment 都有独立的 **CRC32C 校验**，恢复时逐片验证。

### 5.4 sync/fsync

根据 `WriteOptions::sync` 决定是否强制刷盘：

```
sync=false（默认）→ 写入内核 Page Cache，可能丢最后一批写入（OS crash 场景）
sync=true         → 调用 fsync() / fdatasync()，保证落盘，延迟更高
```

同一 WriteGroup 内所有 Writer 的 sync 标志必须一致，不同标志的写者不会被合并到同一组。

---

## 6. 第四步：写入 MemTable

**文件：** `db/memtable.cc`，约 L1037

### 6.1 WriteBatch 回放

WAL 写完后，`WriteBatch` 内容被**回放**到 MemTable：

```cpp
// db/write_batch.cc
// MemTableInserter 实现 WriteBatch::Handler 接口
class MemTableInserter : public WriteBatch::Handler {
    void Put(const Slice& key, const Slice& value) override {
        mem_->Add(sequence_, kTypeValue, key, value);
        sequence_++;
    }
    void Delete(const Slice& key) override {
        mem_->Add(sequence_, kTypeDeletion, key, {});
        sequence_++;
    }
    void Merge(const Slice& key, const Slice& value) override {
        mem_->Add(sequence_, kTypeMerge, key, value);
        sequence_++;
    }
};
```

### 6.2 MemTable::Add() — 实际写入

**文件：** `db/memtable.cc`，约 L1037

每条记录在 MemTable 中以如下格式存储：

```
MemTable 内部 Key 编码:
┌────────────┬───────────────┬─────────────────────┬─────────────────┐
│ key_size   │  user_key     │  seq_num(7B)+type(1B)│  value_size     │
│ (varint)   │  (N bytes)    │  (8 bytes total)    │  (varint)       │
└────────────┴───────────────┴─────────────────────┴─────────────────┘
后接：value 数据（variable length）
     [可选] protection_checksum（若开启 write_protection）
```

写入步骤：
1. 从 Arena 分配内存（Arena 以 64KB 为单位批量申请，避免频繁 malloc）
2. 按上述格式序列化 internal key + value
3. 调用 `MemTableRep::Insert()` 插入底层数据结构（SkipList / HashSkipList 等）
4. 更新 Bloom Filter（若开启）

### 6.3 并发写入 MemTable

若开启 `allow_concurrent_memtable_write=true`（默认开启）：

```
WriteGroup 成员并行执行 MemTable 写入：

  Leader      Follower1   Follower2
    │             │           │
    └─ Add(k1)   └─ Add(k2)  └─ Add(k3)
         ↓             ↓           ↓
    InlineSkipList::InsertConcurrently()
         ↓
    CAS 操作保证 SkipList 节点正确插入（无锁）
```

并发写完成后，Leader 调用 `ExitAsBatchGroupLeader()` 唤醒所有 Follower。

---

## 7. 第五步：Flush 触发

**文件：** `db/memtable.cc`，约 L257

### 7.1 Flush 时机判断 — `ShouldFlushNow()`

每次 MemTable 写入后检查是否需要 Flush：

```cpp
bool MemTable::ShouldFlushNow() {
    // 条件1：已被标记为需要 Flush（外部强制触发）
    if (flush_state_ == FLUSH_REQUESTED) return true;

    // 条件2：Range Deletion 数量超阈值
    if (num_range_deletes_ >= ...) return true;

    // 条件3：Arena 剩余空间 < 64KB/4（防止下一次写入大幅超过 write_buffer_size）
    // 条件4：已用内存 >= write_buffer_size（正常 Flush 条件）
    size_t write_buffer_size = write_buffer_size_.load(...);
    size_t approx_mem_usage = ApproximateMemoryUsage();
    return approx_mem_usage >= write_buffer_size;
}
```

### 7.2 Flush 流程

```
MemTable::ShouldFlushNow() == true
  └─► UpdateFlushState()
        └─► 原子 CAS: FLUSH_NOT_REQUESTED → FLUSH_REQUESTED

DBImpl::HandleWriteBufferManagerFlush()
  └─► SwitchMemtable()       ← active MemTable → Immutable MemTable
  └─► ScheduleFlush()        ← 提交到 FlushScheduler 队列

后台 Flush 线程（BGWorkFlush）:
  └─► FlushMemTable()
        └─► BuildTable()     ← 遍历 Immutable MemTable，写 SST 文件
        └─► InstallSuperVersion()  ← 新 Version 注册到 VersionSet
        └─► 删除 Immutable MemTable
        └─► 可能触发 Compaction
```

### 7.3 多 MemTable 堆积

写入速度 > Flush 速度时，Immutable MemTable 会堆积：

```
active MemTable: [ 当前写入 ]
imm[0]: [ 等待 Flush ]   ← 最新的 Immutable
imm[1]: [ 等待 Flush ]
imm[2]: [ 正在 Flush ]   ← 后台线程处理中
```

当 Immutable MemTable 数量超过 `max_write_buffer_number` 时，触发 **Write Stall**（写入被暂停直到 Flush 跟上）。

---

## 8. Write Stall — 写限速机制

**文件：** `db/write_controller.h` / `db/write_controller.cc`

### 8.1 三种 Stall 类型

```
StopWriteToken      → 完全停止写入（硬阻塞）
  触发条件：
    - L0 文件数 >= level0_stop_writes_trigger
    - Pending Compaction bytes >= hard_pending_compaction_bytes_limit
    - Immutable MemTable 数 >= max_write_buffer_number

DelayWriteToken     → 限速写入（软限流）
  触发条件：
    - L0 文件数 >= level0_slowdown_writes_trigger
    - Pending Compaction bytes >= soft_pending_compaction_bytes_limit
    - Immutable MemTable 数过多

CompactionPressureToken → 仅发送信号，不直接限速
```

### 8.2 Stall 实现原理

```cpp
// write_thread.cc ~L328
void WriteThread::BeginWriteStall() {
    // 在链表中插入哨兵节点 write_stall_dummy_
    // 新写者加入链表时会排在哨兵后面，被阻塞
    // no_slowdown=true 的写者直接返回 Status::Incomplete()（不等待）
}

void WriteThread::EndWriteStall() {
    // 移除哨兵节点
    // 递增 stall_ended_count
    // 唤醒所有等待的写者
}
```

### 8.3 WriteController 限速计算

```
DelayWriteToken 下的限速：
  write_rate = max_write_rate * (1 - compaction_pressure)
  每次写入前按 token bucket 计算等待时间
  
  延迟 = bytes_to_write / delayed_write_rate
```

### 8.4 Stall 触发/解除条件总结

| 触发条件 | Stall 类型 | 解除条件 |
|----------|-----------|----------|
| L0 文件 >= stop_trigger | Stop | Compaction 减少 L0 文件 |
| L0 文件 >= slowdown_trigger | Delay | Compaction 减少 L0 文件 |
| Imm MemTable >= max_write_buffer_number | Stop | Flush 完成 |
| Pending compaction >= hard_limit | Stop | Compaction 减少 pending |
| Pending compaction >= soft_limit | Delay | Compaction 减少 pending |

---

## 9. Flush → Compaction 触发

Flush 完成后，新产生的 L0 SST 文件可能触发 Compaction：

```
FlushJob::Run()
  └─► 写入 L0 SST 文件
  └─► InstallSuperVersion()
  └─► 检查 Compaction 条件:
        - L0 文件数 >= level0_file_num_compaction_trigger → 触发 L0→L1 Compaction
        - 各层总文件大小超过 max_bytes_for_level_base → 触发层间 Compaction
  └─► SchedulePendingCompaction()
        └─► BGWorkCompaction()  ← 后台 Compaction 线程
```

Compaction 负责将多个 SST 文件归并排序，减少读放大，同时清理过期版本和删除标记。

---

## 10. 流水线写（Pipelined Write）

**文件：** `db/db_impl/db_impl_write.cc`，约 L1084

默认写路径中，WAL 写完才能写 MemTable，存在串行等待。Pipelined Write 将两个阶段解耦：

```
标准写路径（串行）：
  Writer1: [WAL write]─────────[MemTable write]
  Writer2:             [WAL write]─────────[MemTable write]

流水线写（并行）：
  Writer1: [WAL write][MemTable write]
  Writer2:  ─────────[WAL write][MemTable write]
                      ↑
              Writer1 WAL 结束时立即唤醒 Writer2 开始 WAL
              同时 Writer1 并行写 MemTable
```

流水线写的关键指针：
```cpp
Writer* newest_memtable_writer_;  // 最新的 MemTable 写者
Writer* newest_writer_;           // 最新的 WAL 写者
```

---

## 11. WriteBatch 格式

**文件：** `db/write_batch.cc`

WriteBatch 是写操作的基本单元，内部格式：

```
WriteBatch 二进制格式：
┌─────────────────────────────────────────┐
│ sequence_number (8 bytes)               │  ← batch 起始 seq（写入时填充）
│ count (4 bytes)                         │  ← batch 内记录数
├─────────────────────────────────────────┤
│ Record 1:                               │
│   type (1 byte)  kTypeValue=1           │
│   key  (varint_len + data)              │
│   value(varint_len + data)              │
├─────────────────────────────────────────┤
│ Record 2:                               │
│   type (1 byte)  kTypeDeletion=0        │
│   key  (varint_len + data)              │
├─────────────────────────────────────────┤
│ ...                                     │
└─────────────────────────────────────────┘
```

支持的操作类型：
```
kTypeValue         (0x1) — Put
kTypeDeletion      (0x0) — Delete
kTypeMerge         (0x2) — Merge
kTypeLogData       (0x3) — 仅写 WAL，不写 MemTable
kTypeSingleDeletion(0x7) — 仅删除最新版本（优化）
kTypeRangeDeletion (0xF) — 范围删除
kTypeBlobIndex     (0x11)— Blob 文件引用（BlobDB）
```

---

## 12. 性能关键路径

| 场景 | 典型延迟 | 关键因素 |
|------|----------|----------|
| MemTable 写入（无 sync）| ~1–5 µs | SkipList CAS 插入，纯内存 |
| WAL 写入（无 sync）| ~5–20 µs | 顺序写，内核 Page Cache |
| WAL 写入（sync=true）| ~100–500 µs | fsync，SSD 落盘 |
| Write Stall（Delay）| 额外 ~ms 延迟 | Token bucket 限速 |
| Write Stall（Stop）| 完全阻塞 | 等待 Flush/Compaction |

**Group Commit 收益：**
- 100 个并发写者时，WAL fsync 开销可被 100 个写者共享
- 相比各自 fsync，吞吐量可提升 10–100 倍

---

## 13. 关键设计总结

| 设计 | 目标 | 实现机制 |
|------|------|----------|
| Group Commit | 摊薄 WAL fsync 开销 | Leader 收集 Follower，批量写 WAL |
| WAL 顺序写 | 最大化磁盘写吞吐 | Append-only，固定 32KB Block |
| Arena 分配 | 减少 MemTable 内存碎片 | 批量申请，统一释放 |
| 并发 MemTable 写 | 提高内存写并行度 | InlineSkipList 无锁 CAS |
| Write Stall | 防止 MemTable/L0 积压失控 | 哨兵节点 + 条件变量阻塞 |
| Sequence Number | MVCC 写入顺序保证 | 全局单调递增，batch 内连续分配 |
| Pipelined Write | WAL 与 MemTable 写重叠 | 双指针分离两条流水线 |
