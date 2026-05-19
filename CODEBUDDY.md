# CODEBUDDY.md

This file provides guidance to CodeBuddy Code when working with code in this repository.

## Build System

This is RocksDB — a persistent key-value store using an LSM-tree architecture. There are 3 build systems (Make, CMake, BUCK), but **only Make is used locally**.

### Build Commands

```bash
make dbg                    # Build all with full debug (-O0, DEBUG_LEVEL=2) — for unit test development
make all                    # Build with -O2, assertions enabled (DEBUG_LEVEL=1)
make release                # Build optimized (DEBUG_LEVEL=0)
make clean                  # Clean all build artifacts
make format-auto            # Auto-apply clang-format to changed files (non-interactive)
make db_bench               # Build benchmark tool (use DEBUG_LEVEL=0 for perf testing)
```

### Running Tests

```bash
make <test_name>            # Build a specific test, e.g., make cache_test, make db_basic_test
./<test_name>               # Run the test binary
./<test_name> --gtest_filter="*TestName*"   # Run specific test case
make check                  # Build all + run all tests + format checks (full CI)
make check-progress         # JSON progress output while make check runs
```

### Final Verification

```bash
make clean
make check                                    # Full build + test + format
ASSERT_STATUS_CHECKED=1 make check            # Verify all Status objects are properly checked
```

### Stress/Flakiness Testing

```bash
COERCE_CONTEXT_SWITCH=1 make <test_binary>
./<test_binary> --gtest_filter="*YourTestName*" --gtest_repeat=5
```

## Adding New Source Files

When adding a new `.cc` file, update **all three**: `Makefile`, `CMakeLists.txt`, and `src.mk`. Then regenerate the BUCK file:
```bash
python3 buckifier/buckify_rocksdb.py
```

## Architecture

### Core Data Flow (LSM-Tree)

```
Write Path:  WriteBatch → WAL → Memtable (SkipList)
Read Path:   Point Lookup → SuperVersion → Memtable search → SST files → Block Cache
Compaction:  Memtable flush → L0 SST files → Level compaction (level/fifo/universal styles)
```

### Key Directories

| Directory | Purpose |
|-----------|---------|
| `db/db_impl/` | Main `DBImpl` class, split by concern: `db_impl_write.cc`, `db_impl_compaction_flush.cc`, `db_impl_open.cc`, `db_impl_files.cc`, `db_impl_follower.cc`, `db_impl_readonly.cc` |
| `db/compaction/` | Compaction logic: iterator, job, picker (level/fifo/universal), outputs, service |
| `db/blob/` | Blob storage: file builder/reader, cache, GC, partition manager |
| `table/block_based/` | Block-based SST format: block reader/writer, cache, filters, index, prefetcher |
| `memtable/` | Memtable implementations: SkipList (default), HashSkipList, HashLinkedList, VectorRep |
| `cache/` | Cache: LRU, clock, secondary cache (compressed, tiered) |
| `env/` | Environment/filesystem abstraction (POSIX, mock, encryption) |
| `file/` | File I/O: prefetch, read/write, SST file management |
| `include/rocksdb/` | Public API headers (~90 files) |
| `options/` | Configuration: DB options, CF options, parsing, customizable framework |
| `monitoring/` | Statistics, histograms, perf context, IO stats, thread status |
| `utilities/` | Optional features: transactions, backup, checkpoint, blob_db, merge operators |
| `tools/` | CLI tools: `ldb`, `sst_dump`, `db_bench` (368K lines), `trace_analyzer` |
| `db_stress_tool/` | Stress testing framework (37 files) |
| `test_util/` | Test infrastructure: sync points, test harness, utilities |
| `port/` | Platform portability: POSIX, Windows |
| `memory/` | Memory allocation: arena, jemalloc, concurrent arena |

### API Layers

```
C++ Public API (include/rocksdb/*.h)  ←→  C API Bindings (include/rocksdb/c.h)  ←→  Java/JNI (java/)
                    ↓
C++ Implementation (db/db_impl/*.cc, db/c.cc, java/rocksjni/*.cc)
```

### Component Documentation

Design docs live in `docs/components/`:
- `read_flow/` — Point lookup, MultiGet, block cache, iterator scan, prefetching/async IO
- `write_flow/` — Write APIs, WAL, memtable insert, sequence numbers, crash recovery
- `stress_test/` — Expected state trace, design

## Critical Rules from CLAUDE.md

- **Performance is critical** — RocksDB is a high-performance storage engine. Always evaluate changes from a performance perspective. Minimize allocations and copies in hot paths.
- **Hot path vs cold path** — Hot path (data access, iteration, compaction loops): optimize aggressively. Cold path (DB open, config parsing): prioritize readability.
- **Use `Status` type** for error handling consistently. Never silently ignore failures.
- **Thread safety** — Document synchronization assumptions. Use correct memory ordering (`acquire`/`release` vs `seq_cst`).
- **Backwards compatibility** — Breaking changes require extensive justification and deprecation plans.
- **No `sleep()` in tests** — Use sync points (`test_util/sync_point.h`) instead to avoid flaky tests.
- **Cap unit tests at 60 seconds** timeout.
- **Use `Random` from `util/random.h`** (not `std::mt19937`) in randomized tests, with `SCOPED_TRACE` for reproducibility.
- **Dedup test code** — Extract helper functions for repeated patterns. Use table-driven tests.

## Task-Specific References

| Task | Reference |
|------|-----------|
| Adding a public API | `claude_md/add_public_api.md` |
| Adding an option | `claude_md/add_option.md` |
| Removing a deprecated option | `claude_md/remove_option.md` |
| Code review guidelines | `claude_md/code_review.md` |

When adding a new option, also update: `db_stress_tool/` (gflags), `tools/db_bench_tool.cc`, `tools/db_crashtest.py`, and add a release note to `unreleased_history/`.
