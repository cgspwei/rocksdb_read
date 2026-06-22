# RocksDB 中 auto 类型推导的使用准则

## 问题

`auto` 让变量类型不直观，读代码时不能一眼看出变量是什么类型。RocksDB 用了上万个 `auto`，为什么代码仍然可读？

## 顾虑是合理的

```cpp
auto result = do_something();
auto iter = container.find(key);
```

确实没法一眼看出类型。社区有"Almost Always Auto"和"Never Auto"两种极端，RocksDB 走的是**中间派**——有选择、有上下文地使用。

## RocksDB 中 auto 的三种使用模式

### 1. 类型已经写在右边（最常用）

右边有明确的类型构造，用 `auto` 不损失可读性。

```cpp
// db/db_impl/db_impl_open.cc
auto s = db->Get(options, key, &value);              // Status，一看就知道
auto impl = std::make_unique<DBImpl>(...);            // DBImpl，右边就有
auto cf_handle = db->CreateColumnFamily(...);          // ColumnFamilyHandle*
auto result = std::make_shared<Cache>(...);            // Cache，右边就有

// db/version_set.cc
auto file_writer = std::make_unique<WritableFileWriter>(...);  // 类型在右边
```

**原则**：右边有 `make_unique<T>` / `make_shared<T>` / 类型构造 / 明显返回类型时，用 `auto`。

### 2. 迭代器类型冗长

迭代器的完整类型名又长又没信息量，`auto` 反而更清晰。

```cpp
// 不写 auto 的话——灾难
std::vector<std::pair<Slice, std::unique_ptr<InternalIterator>>>::iterator
    iter = container.begin();

// 用 auto
for (auto& file : level_files) { ... }
for (auto& entry : prepared_iters_) { ... }
for (auto iter = range_.begin(); iter != range_.end(); ++iter) { ... }
```

### 3. 返回值类型不重要或约定俗成

```cpp
auto s = SomeFunction();   // RocksDB 中 s 几乎总是 Status
auto it = map.find(key);   // 关心"能找到什么"，不关心 iterator 的具体类型名
```

## 什么时候 RocksDB 不用 auto

| 场景 | 原因 |
|------|------|
| 函数参数声明 | 接口边界必须明确：`Status Get(const ReadOptions&, ...)` |
| 成员变量声明 | 头文件必须可读：`std::atomic<bool> shutting_down_` |
| 简单基础类型 | `int x = 0` 比 `auto x = 0` 更清晰，int 本身就很短 |
| 返回值类型不明确 | 读代码的人必须跳转才能知道类型，不友好 |
| 需要类型转换/截断 | `uint64_t seqno = func()` 明确表达意图 |

## 实用准则（可以带回你自己的项目）

```
右边有 make_unique/make_shared/类型构造    → auto ✓
返回 Status 的函数（约定俗成）              → auto ✓
range-based for 循环                       → auto ✓
迭代器声明                                  → auto ✓
函数参数                                    → 明确类型 ✗
成员变量                                    → 明确类型 ✗
简单基础类型 (int/double/bool)              → 明确类型 ✗
返回值类型一眼看不清                        → 明确类型 ✗
需要特定宽度/符号的类型                     → 明确类型 ✗
```

## 核心原则

> **如果右边一看就知道类型，用 `auto`；如果读代码的人必须翻定义才知道是什么类型，就写具体类型。**

RocksDB 遵循这个原则，所以虽然 `auto` 用了上万处（约 400+ 文件、10000+ 处），代码仍然可读，反而因为消除了冗长的迭代器类型名和模板类型名而更加清晰。
