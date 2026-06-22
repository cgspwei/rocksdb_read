# RocksDB 智能指针使用模式

> RocksDB 中 `unique_ptr` 用了 400+ 文件、4200+ 处；`shared_ptr` 用了 300+ 文件、2200+ 处。本文聚焦"怎么用的"，不重复基础概念。

---

## 一、总体分工原则

```
unique_ptr  → 独占所有权（内部组件、工厂产出、临时构造）
shared_ptr  → 共享所有权（全局单例、缓存对象、跨组件共享）
裸指针     → 非拥有观察（性能关键路径，外部管理生命周期）
weak_ptr   → 打破循环引用（极少，仅 2 处）
```

---

## 二、unique_ptr 的使用模式

### 模式 1：Pimpl（Pointer to Implementation）

把类的**实现细节**藏进一个前向声明的私有 `Rep` / `Impl` 类，头文件只暴露接口。

```
头文件（.h）                          实现文件（.cc）
┌────────────────────┐              ┌────────────────────────────────┐
│ class VersionBuilder│              │ class VersionBuilder::Rep {    │
│ {                  │              │   // 这里才有真正的字段         │
│   class Rep;       │   ←── 只有   │   FileOptions file_options_;   │
│   unique_ptr<Rep>  │     前向声明  │   std::vector<FileMetaData> ..│
│   rep_;            │              │   std::unordered_map<...> ...  │
│   Apply(...);      │              │   // 几百行实现细节             │
│ }                  │              │ };                              │
└────────────────────┘              └────────────────────────────────┘
```

#### RocksDB 中的两个例子

**例1：VersionBuilder（`db/version_builder.h:116-118`）**

```cpp
class VersionBuilder {
 public:
  // 头文件只有 7 个公开方法
  Status Apply(const VersionEdit* edit);
  Status SaveTo(VersionStorageInfo* vstorage) const;
  // ...

 private:
  class Rep;                       // 只有前向声明，不知道 Rep 长什么样
  std::unique_ptr<Rep> savepoint_; // 存档点（用于 point-in-time recovery）
  std::unique_ptr<Rep> rep_;       // 当前状态
};
```

`Rep` 的真实定义在 `db/version_builder.cc` 里，包含几百行字段和私有方法，外部代码完全看不到。

两个 `Rep` 的设计是为了 point-in-time recovery：`CreateOrReplaceSavePoint()` 时把当前 `rep_` 的状态复制到 `savepoint_`，后续 `Apply(edit)` 只改 `rep_`，恢复时可以回到 `savepoint_` 的状态。两个 `unique_ptr<Rep>` 表达了清晰语义：**两个独立版本状态，各自拥有，互不干扰**。

**例2：ThreadPoolImpl（`util/threadpool_imp.h:101-117`）**

```cpp
class ThreadPoolImpl : public ThreadPool {
 public:
  // 10+ 个公开方法
  void SubmitJob(std::function<void()>&&) override;
  // ...

  struct Impl;  // 注意这里用 struct（也是合法的 Pimpl 写法）

 private:
  // 注释直接说明了为什么用 Pimpl：
  // "We propose a pimpl idiom in order to easily replace the thread pool impl
  //  w/o touching the header file but providing a different .cc"
  std::unique_ptr<Impl> impl_;
};
```

注释里提到了额外好处：**不改头文件就能换实现**（通过 CMake 选项替换不同平台的线程池实现）。

#### Pimpl 解决了什么问题？

**问题1：头文件依赖爆炸**

```cpp
// 没有 Pimpl 的话，VersionBuilder.h 必须 #include 所有这些
#include <unordered_map>
#include "db/file_metadata.h"   // 引入一大堆传递依赖
#include "db/version_edit.h"
// ...几十个 include

// 用了 Pimpl，头文件只需要前向声明，不需要 include 任何东西
class Rep;
```

RocksDB 这个规模的项目，一个核心头文件被几百个文件引用，省去几十个 `#include` 能显著缩短编译时间。

**问题2：ABI 稳定性**

改了 `Rep` 的字段不会影响 `.h` 文件，不需要重新编译所有引用了头文件的文件：

```
修改 VersionBuilder::Rep 的内部字段
→ 只需要重编译 version_builder.cc
→ 所有用到 VersionBuilder 的文件无需重编译
```

**问题3：隐藏实现细节**

`Rep` 里可以随意重构，用任何内部数据结构，对外完全不可见。

#### 为什么 Pimpl 必须用 unique_ptr 而不是裸指针？

```cpp
// 用裸指针的话：必须手写析构，且析构时需要 Rep 的完整定义
class VersionBuilder {
  ~VersionBuilder() { delete rep_; }  // 手写，且头文件里必须能看到 Rep 的定义
  Rep* rep_;
};

// 用 unique_ptr：析构自动，且可以推迟到 .cc 文件
class VersionBuilder {
  ~VersionBuilder();           // 只需在 .cc 中定义为 = default 即可
  std::unique_ptr<Rep> rep_;
};
```

有一个**必须注意的陷阱**：`unique_ptr<Rep>` 的析构需要 `Rep` 的完整定义，析构函数不能内联在头文件里：

```cpp
// version_builder.h
class VersionBuilder {
  ~VersionBuilder();           // 必须显式声明，不能让编译器隐式生成
  std::unique_ptr<Rep> rep_;
};

// version_builder.cc
class VersionBuilder::Rep { ... };    // Rep 完整定义在这里

VersionBuilder::~VersionBuilder() = default;  // OK：此时 Rep 已完整
```

如果把析构函数放在头文件里（或者让编译器隐式生成），编译器在处理头文件时看不到 `Rep` 的完整定义，会报错：

```
error: use of undefined type 'VersionBuilder::Rep'
```

#### 对比总结

| 对比项 | 裸指针 Pimpl | unique_ptr Pimpl |
|--------|------------|-----------------|
| 析构 | 必须手写 `~Class() { delete p_; }` | 自动，`.cc` 中 `= default` 即可 |
| 头文件完整性 | 析构时也需要完整类型 | 析构推迟到 `.cc`，头文件只需前向声明 |
| 可移动 | 需要手写移动构造 | 天然可移动 |
| 异常安全 | 构造中途抛异常会泄漏 | RAII，自动清理 |

### 模式 2：拥有者持有独享资源

```cpp
// db/version_set.h:1783
std::unique_ptr<ColumnFamilySet> column_family_set_;

// db/version_set.h:1823
std::unique_ptr<log::Writer> descriptor_log_;

// db/error_handler.h:125
std::unique_ptr<port::Thread> recovery_thread_;

// table/sst_file_dumper.h:92-93
std::unique_ptr<TableReader> table_reader_;
std::unique_ptr<RandomAccessFileReader> file_;
```

每个成员变量表达一个清晰的语义：**"我独有它，我负责它的生命周期"**。

### 模式 3：工厂方法返回 unique_ptr

RocksDB 的工厂方法有两种形式，语义都是**调用者获得所有权**，但返回方式不同。

#### 子模式 A：直接返回 unique_ptr

适用于"创建对象是唯一目的，失败时用异常或返回 nullptr"的场景。

```cpp
// include/rocksdb/db.h:1052-1054
// Iterator 工厂：每次调用创建新的 Iterator，调用者持有
virtual std::unique_ptr<Iterator> NewCoalescingIterator(
    const ReadOptions& options,
    const std::vector<ColumnFamilyHandle*>& column_families) = 0;

// include/rocksdb/env.h:2096
// 组合 Env：调用者拿走新建的 Env，随时可以用
std::unique_ptr<Env> NewCompositeEnv(const std::shared_ptr<FileSystem>& fs);

// include/rocksdb/sst_file_reader.h:58
// 创建遍历 SST 文件的 Iterator
std::unique_ptr<Iterator> NewTableIterator(
    const ReadOptions& options = ReadOptions(),
    std::shared_ptr<const TableProperties>* table_properties = nullptr);

// util/compression.h:652-654
// 多态工厂：根据 compression_type 决定返回哪种 Compressor 子类
static std::unique_ptr<StreamingCompress> Create(
    CompressionType compression_type, const CompressionOptions& opts,
    uint32_t compress_format_version, size_t max_output_len);
```

#### 子模式 B：用 unique_ptr* 作为出参（Status 作返回值）

适用于"需要同时返回对象和错误码"的场景。C++ 不能同时返回两个值，所以 RocksDB 用 `Status` 作返回值，把对象放在出参里。

```cpp
// include/rocksdb/db.h:170-174
// DB::Open 签名：Status 是返回值，DB 对象通过 unique_ptr* 出参交出
static Status Open(const Options& options, const std::string& name,
                   std::unique_ptr<DB>* dbptr);

// include/rocksdb/db.h:196-200
// 只读打开，同样模式
static Status OpenForReadOnly(const Options& options, const std::string& name,
                               std::unique_ptr<DB>* dbptr,
                               bool error_if_wal_file_exists = false);
```

**调用方写法**：

```cpp
std::unique_ptr<DB> db;
Status s = DB::Open(options, "/tmp/testdb", &db);
if (!s.ok()) {
    // 打开失败时，db 仍然是 nullptr，不需要任何清理
    LOG(s.ToString());
    return;
}
// 此后 db 就是有效对象，unique_ptr 负责关闭和析构
db->Put(write_options, "key", "value");
```

**为什么不用 `pair<Status, unique_ptr<DB>>`？**

- `DB::Open` 是一个历史悠久的公共 API（C++17 之前设计），当时 structured bindings 还没有
- 出参风格在 RocksDB 的 API 中贯穿始终（几乎所有 `Status` 返回的方法都用出参传结果）
- 失败时 `dbptr` 保持 `nullptr`，比返回无效的 `pair` 语义更清晰

#### 子模式 C：多态工厂（返回基类 unique_ptr）

这是面向对象里最经典的工厂模式：**返回类型是基类，实际创建的是某个具体子类**。

以 `StreamingCompress::Create()` 为例（`util/compression.cc:1760-1798`）：

```cpp
// 基类：纯抽象接口
class StreamingCompress {
 public:
  static std::unique_ptr<StreamingCompress> Create(
      CompressionType compression_type, ...);
  virtual int Compress(const char* input, size_t input_size, ...) = 0;
  virtual ~StreamingCompress() = default;
};

// 实现（util/compression.cc）：
std::unique_ptr<StreamingCompress> StreamingCompress::Create(
    CompressionType compression_type, const CompressionOptions& opts,
    uint32_t compress_format_version, size_t max_output_len) {
  switch (compression_type) {
    case kSnappyCompression:
      return std::make_unique<BuiltinSnappyCompressorV2>(
          opts, compress_format_version, max_output_len);
    case kZlibCompression:
      return std::make_unique<BuiltinZlibCompressorV2>(
          opts, compress_format_version, max_output_len);
    case kLZ4Compression:
      return std::make_unique<BuiltinLZ4CompressorV2>(
          opts, compress_format_version, max_output_len);
    // ... 其他压缩算法
    default:
      return nullptr;  // 不支持的类型返回 nullptr
  }
}
```

**调用方完全不知道具体类型**：

```cpp
// 调用方只和基类 StreamingCompress 打交道
auto compressor = StreamingCompress::Create(kSnappyCompression, opts, ...);
if (!compressor) return Status::NotSupported("compression not available");

// 通过虚函数调用，实际执行 BuiltinSnappyCompressorV2::Compress
int ret = compressor->Compress(input_data, input_size, &output, &output_len);
```

**为什么返回类型是 `unique_ptr<基类>` 而不是原始指针？**

```
原始指针:  调用者必须负责 delete，容易忘，对象析构时要知道具体类型
unique_ptr: 离开作用域自动析构，基类有 virtual ~StreamingCompress()，
            unique_ptr 调用 delete 时走虚析构，正确释放具体类型
```

`unique_ptr` + `virtual 析构函数` = 多态工厂的标准搭配。

#### 历史对比：旧 API 返回裸指针

RocksDB 中也有一些旧 API 仍然返回裸指针（来不及更新）：

```cpp
// include/rocksdb/db.h (旧风格)
virtual Iterator* NewIterator(const ReadOptions& options,
                               ColumnFamilyHandle* column_family) = 0;

// include/rocksdb/sst_file_reader.h (旧风格)
Iterator* NewIterator(const ReadOptions& options, ...);
```

这是历史遗留。旧 API 返回裸指针意味着调用者必须手动 `delete`：

```cpp
Iterator* iter = db->NewIterator(read_options, cf);
// ... 用完后必须手动清理
delete iter;
```

新 API（如 `NewCoalescingIterator`、`NewTableIterator`）已经改为返回 `unique_ptr`。在给同一类增加新方法时，RocksDB 会优先使用 `unique_ptr` 返回风格。

#### 对比三种工厂方式

| 形式 | 示例 | 适用场景 |
|------|------|----------|
| 直接返回 `unique_ptr<T>` | `NewCoalescingIterator()` | 只需要新建对象，无需报告详细错误 |
| `unique_ptr<T>*` 出参 + `Status` | `DB::Open()` | 需要同时返回对象和详细错误码 |
| 返回 `unique_ptr<基类>` | `StreamingCompress::Create()` | 多态工厂，屏蔽具体实现类型 |
| 返回裸指针（旧风格） | `NewIterator()` | 历史遗留，调用者手动 delete |

### 模式 4：make_unique 构造 + move 转移所有权

RocksDB 中用 `make_unique` 构造，再用 `std::move` 沿着构造链向上传递所有权。

```cpp
// db/version_set.cc:6930-6938
// 步骤1: 构造 WritableFileWriter，移入 file
auto file_writer = std::make_unique<WritableFileWriter>(
    std::move(file), fname, opts, clock_, io_tracer_, ...);
// 步骤2: 构造 log::Writer，移入 file_writer
return std::make_unique<log::Writer>(
    std::move(file_writer), /*log_number=*/0, ...);

// db/version_set.cc:1993-1994
// 将 file 移入 RandomAccessFileReader
std::unique_ptr<RandomAccessFileReader> file_reader(
    new RandomAccessFileReader(std::move(file), file_name, ...));

// db/db_impl/db_impl_open.cc:2663-2664
auto impl = std::make_unique<DBImpl>(db_options, dbname, ...);
```

**关键观察**：`make_unique<T>(std::move(x), args...)` 是 RocksDB 中最常见的构造模式。`x` 的所有权从旧 owner 转移到新构造的对象。

### 模式 5：vector<unique_ptr> 表示拥有多个对象

```cpp
// db/db_impl/db_impl.h:164
std::vector<std::unique_ptr<FSDirectory>> data_dirs_;

// db/range_del_aggregator.h:362
std::vector<std::unique_ptr<TruncatedRangeDelIterator>> iters_;

// db/blob/blob_file_partition_manager.h:275
std::vector<std::unique_ptr<Partition>> partitions_;

// include/rocksdb/db.h:2137-2144
virtual Status CommitFileIngestionHandles(
    std::vector<std::unique_ptr<FileIngestionHandle>> handles) = 0;
```

注意在公共 API 中使用 `vector<unique_ptr>` 作为参数类型，表达"我来接管这一批对象的所有权"。

### 模式 6：自定义删除器

RocksDB 在需要特定释放逻辑时使用自定义删除器。

```cpp
// memory/memory_allocator_impl.h:29
// 专用删除器类型别名
using CacheAllocationPtr = std::unique_ptr<char[], CacheAllocationDeleter>;

// db/arena_wrapped_db_iter.h:41-42
// 用删除器做引用计数减一（不是 delete）
using ColumnFamilyDataRef =
    std::unique_ptr<ColumnFamilyData, ColumnFamilyDataUnrefDeleter>;

// util/aligned_buffer.h:191-193
// lambda 删除器（需要通过 std::function 擦除类型）
buf_ = std::unique_ptr<void, std::function<void(void*)>>(
    static_cast<void*>(new_buf),
    [](void* p) { delete[] static_cast<char*>(p); });
```

**设计要点**：
- `CacheAllocationPtr`：通过类型别名隐藏数组删除器的细节
- `ColumnFamilyDataRef`：删除器做的是 `Unref()` 而非 `delete`，语义优雅
- Lambda 删除器：灵活但有代价（`std::function` 类型擦除引入额外开销）

---

## 三、shared_ptr 的使用模式

### 模式 1：全局单例 / 跨组件共享

以下对象在 RocksDB 中被整个 DB 实例及多个 Column Family 共享，天然适合 `shared_ptr`：

```cpp
// 各种 Options / Config 中
std::shared_ptr<Cache> block_cache = nullptr;       // table.h:390
std::shared_ptr<FileSystem> fs_;                     // 各处
std::shared_ptr<Statistics> stats_;                  // 各处
std::shared_ptr<SystemClock> clock_;                 // 各处
std::shared_ptr<MemoryAllocator> memory_allocator;   // cache.h

// 工厂方法返回 shared_ptr
// include/rocksdb/file_system.h:427
static std::shared_ptr<FileSystem> Default();

// include/rocksdb/cache.h:278
inline std::shared_ptr<Cache> NewLRUCache(size_t capacity, ...);

// include/rocksdb/statistics.h:905
std::shared_ptr<Statistics> CreateDBStatistics();
```

**为什么用 shared_ptr 而不是 unique_ptr**：这些对象的生命周期与 DB 实例相关联，但又被多个内部组件引用。使用 `unique_ptr` 需要精确控制析构顺序，`shared_ptr` 允许自然释放。

### 模式 2：观察者模式的 shared_from_this

```cpp
// cache/cache_reservation_manager.h:60-62
template <CacheEntryRole R>
class CacheReservationManagerImpl
    : public CacheReservationManager,
      public std::enable_shared_from_this<CacheReservationManagerImpl<R>> {

// include/rocksdb/advanced_compression.h:423
class CompressionManager
    : public std::enable_shared_from_this<CompressionManager> {
```

使用 `enable_shared_from_this` 说明这些对象预期被 `shared_ptr` 管理，且内部需要把自己注册到其他地方。RocksDB 中只用了 4 处，比较克制。

---

## 四、weak_ptr 的使用（极少，仅 2 处）

```cpp
// include/rocksdb/io_dispatcher.h:249
// 注释明确说明了设计意图：避免循环引用
std::weak_ptr<IODispatcherImplData> dispatcher_data_;

// include/rocksdb/utilities/object_registry.h:577
// 注册但不阻止对象析构
std::map<std::string, std::weak_ptr<Customizable>> managed_objects_;
```

`weak_ptr` 只在需要"观察但不拥有"的 `shared_ptr` 场景中使用。RocksDB 绝大多数"观察不拥有"的场景直接用裸指针。

---

## 五、热点路径：什么时候不用智能指针

RocksDB 在性能关键路径上刻意不使用智能指针：

```cpp
// db/db_impl/db_impl.cc: 热点路径用裸指针
ColumnFamilyData* cfd_;       // 由 ColumnFamilySet 统一管理
Version* current_;             // 由 VersionSet + 引用计数管理
SuperVersion* sv_;             // 由 SuperVersion 机制管理
```

**原因**：
1. 避免 `shared_ptr` 的原子引用计数开销（每次拷贝 10-30 个 CPU 周期）
2. 避免 `unique_ptr` 的析构链（嵌套 unique_ptr 的递归析构可能很深）
3. Arena 分配的内存不需要逐个析构，一次性释放即可

---

## 六、PinnableSlice：不用智能指针的内存管理

`PinnableSlice` 是 RocksDB 读路径的核心优化，它故意不用智能指针：

```cpp
// include/rocksdb/slice.h
class PinnableSlice : public Slice, public Cleanable {
  std::string self_space_;    // 内联存储：小 value 不用堆分配
  std::string* buf_;          // 指向 self_space_ 或外部 buffer
};
```

**两种模式**：
- **Pin 模式**（`PinSlice`）：指向外部数据（如 block cache 中的内存），注册 cleanup 回调，不拷贝数据
- **Self 模式**（`PinSelf`）：拷贝数据到 `self_space_`，自己拥有

选择智能指针的替代方案是因为：
1. 零拷贝 — 直接指到 block cache 内存，没有引用计数
2. 内联小对象 — 小于 `self_space_` 的 value 直接存，避免堆分配
3. 用 `Cleanable` 回调链代替析构函数，不依赖 unique_ptr 的 RAII

---

## 七、总结：决策树

```
需要管理对象的生命周期？
├─ 否 → 裸指针（非拥有观察，由外部生命周期机制管理）
└─ 是 → 所有权是否独占？
    ├─ 是 → unique_ptr（默认首选）
    │       ├─ 类成员 → 模式2
    │       ├─ 工厂产出 → 模式3
    │       └─ 特殊释放 → 模式6（自定义删除器）
    └─ 否 → shared_ptr
            ├─ 全局单例 → 模式1
            ├─ 跨组件共享 → 模式1
            ├─ 需要 shared_from_this → 模式2
            └─ 有循环引用风险 → 一侧改用 weak_ptr
```

**核心经验**：
- `unique_ptr` 是 RocksDB 的默认选择，覆盖绝大多数场景
- `shared_ptr` 只在全局单例和跨组件共享时使用
- 热点路径用裸指针 + 外部生命周期（arena、版本机制、引用计数）
- `make_unique` + `std::move` 是最常见的构造和转移模式
- 公共 API 用 `unique_ptr` 返回新建对象，用 `shared_ptr` 接收共享配置
