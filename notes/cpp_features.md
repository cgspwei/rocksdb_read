# RocksDB C++ 特性使用分析

RocksDB 编译目标为 C++20，但实际代码风格偏保守，属于**适度现代**水平。

## 大量使用的特性

- **智能指针**：`unique_ptr`/`shared_ptr` 大量使用，约 300+ 文件
- **Lambda**：极度常用，回调、STL 算法谓词、作用域操作到处都是
- **模板**：重度使用，尤其是 ribbon 过滤器、cache、table reader 等性能关键路径
  - 例：`util/ribbon_impl.h`、`util/ribbon_alg.h` 深度模板元编程
  - `include/rocksdb/data_structure.h` 使用 `if constexpr` 分发
- **多重继承**：用于接口组合（如 `Options : public DBOptions, public ColumnFamilyOptions`、`PinnableSlice : public Slice, public Cleanable`）

## 有限使用的 C++17 特性

| 特性 | 使用程度 | 示例位置 |
|------|---------|---------|
| 结构化绑定 `auto [x, y] = ...` | 中等（约 30+ 处） | `util/compression.cc`、`db/compaction/compaction_job.cc` |
| `if constexpr` | 少量（约 20 处） | `table/block_based/block_based_table_reader.cc`、`cache/clock_cache.cc` |
| `std::optional` | 中等 | `file/sst_file_manager_impl.h`、`db/memtable.cc` |
| `std::variant` | 仅一个模块 | `utilities/secondary_index/` |
| `std::string_view` | 极少（与 Slice 互操作） | `include/rocksdb/slice.h` 中的转换构造函数 |

## 完全不用的 C++17/20 特性

- **Concepts (C++20)**：未使用，仅在注释中提及设计概念
- **Ranges/Views (C++20)**：未使用，`db/compaction/compaction_outputs.h` 有注释说"可被 `std::ranges::join_view()` 替代"
- **Coroutines (C++20)**：未使用，测试中有 `ROCKSDB_GTEST_SKIP("This test requires coroutine support")`
- **Fold expressions (C++17)**：未使用，变参模板仍用 C++11 风格的 `std::forward<Args>(args)...`
- **虚继承**：零使用

## 核心原因

1. **性能优先**：cache 内部用侵入式链表（裸指针）、arena 分配器，不用标准库抽象
2. **自研替代标准库**：`Slice` 替代 `string_view`，`autovector` 替代小容器，`Random` 替代 `mt19937`
3. **编译器兼容性**：需要支持 GCC/Clang/MSVC 多平台

## 代码风格总结

C++14 为底、选择性采用 C++17，C++20 特性几乎未触及。代码本质是**面向对象 + 模板 + 裸指针性能优化**的风格，不追求语言特性的新潮。热点路径（数据访问、迭代、压缩循环）激进优化，冷路径（DB 打开、配置解析）优先可读性。
