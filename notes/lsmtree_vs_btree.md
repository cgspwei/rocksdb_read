# LSM-Tree 在现代存储下的适用性分析

> 核心问题：
> 1. SSD/NVMe 时代，LSM-Tree 对比 B-Tree 的优势还在吗？
> 2. 随机点查场景下，RocksDB 和 Memcache 怎么选？

---

## 一、为什么 RocksDB / LevelDB / HBase 都用 LSM-Tree 而不是 B-Tree？

### 一句话答案

**B-Tree 优化读，LSM-Tree 优化写。** 现代互联网场景（日志、消息队列、时序数据、KV 存储）通常是写多读少或读写混合，LSM-Tree 在这种场景下性能远超 B-Tree。

### 核心差异

```
B-Tree 写入：
  定位目标页 → 读入内存 → 修改 → 写回磁盘（随机 I/O）
  一次写入 = 至少一次随机 I/O

LSM-Tree 写入：
  Put(key, value) → WAL（顺序写）→ MemTable（内存）→ 返回
  一次写入 = 一次顺序 I/O（WAL）+ 内存操作
```

### 三个核心优势

**1. 写入性能（最关键）**

| | B-Tree | LSM-Tree |
|---|---|---|
| 写入方式 | 随机写（in-place update） | 顺序写（append-only） |
| 一次 Put 的磁盘 I/O | 随机 I/O（HDD ~10ms） | 顺序 I/O（HDD ~0.5ms 摊销） |
| 写放大 | 2-10x（页级写 + 双写缓冲等） | 10-30x（compaction 反复重写） |

注：B-Tree 看似"原地更新"，但实际写放大并不低：
- 修改一行可能要重写整个页（典型 16KB）
- InnoDB 的 double write buffer 让数据写两次
- 索引页分裂时还要重写父页

LSM-Tree 用更高的写放大换写吞吐。虽然 Compaction 导致一份数据被写多次，但全都是顺序 I/O。对于 HDD，顺序写比随机写快 100 倍；对于 SSD，差距缩小但仍有 2-3 倍（低队列深度下）。

**2. 空间利用率**

B-Tree 的页分裂会导致碎片（页通常只有 50-75% 满）。LSM-Tree 的 SST 文件是紧凑打包的，compaction 后几乎没有碎片。RocksDB 的 SST 文件可以达到 90%+ 的空间利用率。

**3. 压缩友好**

SST 文件是一次性生成、不可变的，可以整体压缩（Snappy/LZ4/ZSTD）。B-Tree 的页是可变大小的，压缩后更新会导致页大小变化，处理起来很复杂。

### LSM-Tree 的真实代价

| 问题 | 表现 | RocksDB 的优化 |
|------|------|-------------|
| 读放大 | 一次 Get 可能要查多个 SST 文件 | Bloom filter 快速过滤不存在的 key |
| 写放大 | Compaction 反复重写数据，10-30x | Level/Universal Compaction 策略调优 |
| 空间放大 | 旧版本数据在 compaction 前占用空间 | 控制 compaction 频率 |
| 写停顿（Write Stall） | Compaction 跟不上写入速度 | 限速 + 后台线程池 |
| 尾延迟抖动 | Compaction 占用 I/O 导致 P99/P999 抖动 | Rate limiter + 优先级控制 |
| SSD 寿命 | 写放大放大 SSD 内部磨损 | 选择适合工作负载的 compaction 策略 |
| 内存占用 | Bloom filter + index block + memtable，约占数据量 1-3% | Partitioned Index/Filter、二级 cache |

**尾延迟问题尤其需要注意**：LSM-Tree 的平均延迟通常优秀，但 P99/P999 受 compaction 影响明显。对延迟敏感的服务（如交易系统）需要慎重评估。

### 为什么这三家都选 LSM-Tree？

- **LevelDB**（Google）：为 Chrome 的 IndexedDB 设计，SSD 上的轻量 KV 存储
- **RocksDB**（Meta）：从 LevelDB fork，面向 SSD 和 Flash 做了大量优化，需要极高的写入吞吐
- **HBase**（Apache）：面向 Hadoop 生态的大数据存储。HDFS 早期不支持任何修改，后续版本支持 append 但不支持随机写 — LSM-Tree 天然匹配

三者的共同场景：**写入量巨大，需要极高的写入吞吐，可以接受读路径的轻微代价**。

### 反例：B-Tree 仍然有竞争力

不能因为 RocksDB 流行就认为 B-Tree 已经过时。几个反例：

- **WiredTiger**（MongoDB 默认引擎）：基于 B-Tree 的变种，针对多核和 SSD 做了优化，性能在通用场景与 LSM-Tree 相当，且尾延迟更稳定
- **InnoDB**（MySQL 默认引擎）：B+Tree，OLTP 场景仍然是事实标准
- **FoundationDB**：用 B-Tree 实现强一致分布式数据库
- **Bw-Tree、Be-Tree** 等新型 B-Tree 变种：针对 SSD 和多核做了专门优化，缩小了和 LSM-Tree 的写性能差距

**B-Tree 在这些场景仍占优**：
- 范围查询多、点查多、写入相对适中的 OLTP 场景
- 对延迟稳定性要求高的场景（金融交易）
- 数据量适中、内存能容纳大部分 B-Tree 内部页的场景
- 需要原地更新语义、不希望承担 compaction 复杂度的场景

---

## 二、SSD / NVMe 时代，LSM-Tree 还适用吗？

### 差距缩小了，但没有消失

```
                    HDD            SATA SSD       NVMe SSD（QD=1）
随机读 (4K)         5-10ms         0.1-0.2ms      0.05-0.1ms
顺序写 (256K)       0.5-1ms        0.05-0.1ms     0.01-0.02ms
随机写 (4K)         8-15ms         0.1-0.2ms      0.05-0.1ms

随机写 vs 顺序写     10-20x         2-3x           2-3x（QD=1）
                                                  接近 1x（高 QD）
```

注：高队列深度（QD=32+）下，NVMe 随机写和顺序写带宽接近，因为内部并行度被充分利用。但实际应用很少能维持持续的高队列深度。

随机写和顺序写的延迟差距从 HDD 时代的 100x 缩小到了 NVMe 的 2-3x。**但结论没有反转。**

### 优势不再主要是 IOPS，而是这些原因

**1. SSD 内部 GC（垃圾回收）问题**

SSD 的 FTL（Flash Translation Layer）在内部也会做垃圾回收。大量小随机写会导致 SSD 内部写放大，降低寿命。LSM-Tree 的大块顺序写对 SSD 的 FTL 更友好：

- B-Tree 的 4K/16K 随机写 → SSD 内部可能放大 2-3x
- LSM-Tree 的 64K-256K 顺序写 → SSD 内部几乎不放大

但要注意：LSM-Tree 自身的 compaction 写放大（10-30x）远高于 B-Tree 的逻辑写放大，对 SSD 总写入量并不一定更友好。**真正的优势是写入模式（顺序大块）而非总量**。

**2. 带宽利用率**

单次随机写延迟差距小了，但 LSM-Tree 的批量写入（WriteGroup 组提交）能更充分地利用 SSD 的带宽：

- B-Tree：每次写入触发一次随机 I/O，无法合并
- LSM-Tree：积累一批写入，一次大块顺序写完成，带宽利用率更高

**3. 实际性能数据**

RocksDB 在 NVMe SSD 上的典型表现（引自 db_bench 公开报告，仅供参考，实际性能高度依赖 key/value 大小、并发度、是否 fsync 等）：

- 写入吞吐：fillrandom 在小 value（100B）、不强制 fsync 的场景下可达 50 万+ ops/sec
- 读延迟：cache 命中时微秒级，cache miss 时 100us-1ms 级别
- 同等硬件 InnoDB 的写入吞吐：通常为 RocksDB 的 1/3 到 1/5

**这些数据需要严格区分场景**：是否 fsync、key 分布、value 大小、并发度都会大幅影响结果。任何引用都应该带上完整的测试条件。

### 现代硬件与 LSM-Tree 的演进

- **ZNS SSD（Zoned Namespace）**：把 SSD 按 zone 划分，要求顺序写，正好匹配 LSM-Tree 的写入模式。这是 LSM-Tree 的天然契合硬件
- **io_uring**：Linux 5.1+ 的异步 I/O 接口，让随机 I/O 性能大幅提升，缩小了 LSM-Tree 顺序写和 B-Tree 随机写的延迟差距
- **PMem / Optane**（已停产，但代表方向）：字节寻址持久内存，曾促使 B-Tree 重新成为研究热点，但商业化不成功
- **大容量内存**：服务器内存常规 1TB+，工作集 fit 内存的场景越来越多，缓存层效果接近内存数据库

### 结论

LSM-Tree 的优势从"HDD 时代的必须"变成了"SSD 时代的更优"，但优势仍在。**结论应该这样表述**：

- 写多读少 / 写吞吐至关重要 → **LSM-Tree 仍是首选**
- 读多写少 / 延迟稳定性优先 → **B-Tree 仍有优势**
- 写读均衡 / 通用 OLTP → **两者都可，看具体延迟和吞吐目标**

---

## 三、随机点查场景：RocksDB vs Memcache？

### 本质不同

| | RocksDB | Memcache | Redis |
|---|---|---|---|
| 本质 | 持久化存储引擎 | 纯内存缓存 | 内存数据库 + 可选持久化 |
| 数据存在哪 | 磁盘（SSD）+ 内存 cache | 纯内存 | 内存为主，可 RDB/AOF 持久化 |
| 重启后 | 数据在 | 数据丢光 | RDB/AOF 可恢复（有少量丢失） |
| 数据量上限 | TB 级 | 内存大小（GB 级） | 内存大小（GB 级） |
| 读延迟 | 内存命中 <100us，磁盘 100us-1ms | <100us | <100us |
| 数据结构 | KV | KV | KV、List、Set、Hash、ZSet 等 |
| 典型用途 | 数据库底层存储 | 数据库前面的缓存层 | 缓存 / 内存数据库 / 消息队列 |

### 关键概念：工作集（Working Set）

判断用哪个引擎，最重要的是评估**工作集大小**——即一段时间内被频繁访问的数据量。

```
工作集 vs 内存：

工作集 < 可用内存
  → RocksDB block cache 命中率 95%+，读性能接近纯内存
  → Memcache/Redis 完全 fit 内存
  → 选择取决于：是否需要持久化、数据总量

工作集 > 可用内存
  → RocksDB 必然有部分读 miss 到磁盘，延迟分布拉长
  → Memcache/Redis 无法容纳全部数据，需要复杂的分片/淘汰策略
  → RocksDB 通常是更好的选择
```

### 场景选择决策

```
持久化要求 + 数据量 > 内存          → RocksDB
持久化要求 + 数据量 < 内存 + 微秒级延迟 → Redis（带 RDB/AOF）
持久化要求 + 数据量 < 内存 + 简单 KV   → RocksDB 或 Redis 都可
可丢数据 + 极致延迟               → Memcache
可丢数据 + 复杂数据结构            → Redis
```

### 实际架构通常是分层的

```
App
 ├─→ 本地进程内缓存（caffeine / 自实现 LRU，纳秒级）
 │     ↓ miss
 ├─→ Memcache / Redis（热数据，微秒级）
 │     ↓ miss
 └─→ RocksDB / MySQL（全量数据，毫秒级，持久化）
```

这是 Meta、字节、阿里等大厂的典型架构。**RocksDB 和 Memcache/Redis 不是替代关系，而是协作关系**。

### 如果只用 RocksDB 做随机点查

完全可以，前提是接受这些权衡：

- **延迟**：cache 命中 100us 级别，cache miss 100us-1ms 级别（NVMe SSD）
- **工作集 fit 内存时**：RocksDB 的 block cache 让绝大部分读命中内存，延迟接近纯内存
- **工作集 > 内存时**：部分读命中磁盘，P99 延迟会显著拉长

RocksDB 对随机点查的优化：

- **Bloom filter**：快速判断 key 不存在，避免无效磁盘 I/O（典型过滤率 99%+）
- **Block cache**：LRU/Clock 缓存热点数据块
- **Row cache**（可选）：缓存完整的 key-value 对，进一步降低延迟
- **MultiGet**：批量点查的 IO 合并 + 并发预取
- **Pin L0/L1 in cache**：高优先级缓存最热的层级

### 什么时候不应该用 RocksDB？

| 场景 | 推荐方案 |
|------|---------|
| 纯内存、数据可丢、需要极致延迟 | Memcache |
| 需要丰富数据结构（List/Set/ZSet） | Redis |
| 需要复杂查询（JOIN、聚合） | 传统关系型数据库 |
| 数据量很小（<100MB） | SQLite 或直接内存 map |
| 需要强一致性的分布式事务 | Spanner / CockroachDB / TiDB |
| 对尾延迟（P99/P999）极度敏感 | B-Tree 引擎或纯内存 |

---

## 四、总结

- **LSM-Tree 在 SSD 时代仍然占优**，但优势从 IOPS 转移到了 SSD 内部 GC 友好度、带宽利用率和写入吞吐。**B-Tree 没有过时**，在读多写少和延迟稳定性敏感的场景仍是首选
- **RocksDB 是"磁盘级容量 + 内存级热数据速度"的持久化引擎**，适合需要持久化且数据量可能超过内存的场景
- **RocksDB 真实代价不能忽视**：10-30x 写放大、compaction 引入的 P99 抖动、1-3% 的内存额外占用
- **RocksDB 和 Memcache/Redis 不矛盾**，实际架构中通常分层组合：本地缓存 + 分布式缓存 + 持久化引擎
- **判断用哪个引擎，最重要的是评估工作集大小**，而不是数据总量
