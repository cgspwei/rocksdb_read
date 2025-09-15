# CODEBUDDY.md

This file provides essential information for CodeBuddy Code instances working in the RocksDB codebase.

## Build Commands

### Production Builds
- `make static_lib` - Build librocksdb.a (static library, release mode) - **recommended for production**
- `make shared_lib` - Build librocksdb.so (shared library, release mode)
- `make install` - Install RocksDB system-wide (release mode)

### Development Builds
- `make all` - Build static library, tools, and unit tests (debug mode)
- `make dbg` - Build with DEBUG_LEVEL=2 (no optimizations, ultimate debug mode)

### CMake Alternative
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
```

## Testing Commands

### Core Testing
- `make check` - Run all unit tests (builds in debug mode, includes format/style checks)
- `make check_some` - Run subset of tests (faster)
- `make J=<N> check` - Run tests in parallel with N jobs

### Specialized Testing
- `make asan_check` - Run tests with AddressSanitizer
- `make ubsan_check` - Run tests with UndefinedBehaviorSanitizer  
- `make valgrind_check` - Run tests under Valgrind
- `make crash_test` - Run crash consistency tests

### Individual Tests
- `./<test_name>` - Run specific test binary (e.g., `./db_test`)
- Tests are built automatically when running `make check` or `make all`

### Code Quality
- `make check-format` - Check code formatting (requires clang-format)
- `make check-sources` - Validate source file organization
- `python tools/check_all_python.py` - Check Python scripts

## Architecture Overview

### Core Components

**Database Engine (`db/`)**
- `db_impl/` - Main database implementation with multi-threading support
- `version_set.*` - Manages database versions and metadata
- `memtable.*` - In-memory write buffer (MemTable)
- `wal_manager.*` - Write-Ahead Log management
- `compaction/` - LSM-tree compaction logic

**Storage Layer (`table/`)**
- `block_based/` - Block-based SSTable format (default)
- `plain/` - Plain table format for specific use cases
- `sst_file_writer.*` - SSTable creation utilities
- `format.*` - On-disk format definitions

**Utilities (`util/`)**
- Core utilities: coding, hashing, compression, threading
- `bloom.*` - Bloom filter implementations
- `rate_limiter.*` - I/O rate limiting
- `thread_local.*` - Thread-local storage management

**Public API (`include/rocksdb/`)**
- `db.h` - Main database interface (primary entry point)
- `options.h` - Configuration options
- `iterator.h` - Data iteration interface
- `utilities/` - Higher-level utilities and extensions

### Key Architectural Concepts

**LSM-Tree Design**: RocksDB uses Log-Structured Merge-tree with:
- MemTable (in-memory) → Immutable MemTable → SSTable files (on-disk)
- Multi-level compaction strategy for optimizing read/write/space amplification
- Column families for logical data separation

**Threading Model**: 
- Separate threads for flushing, compaction, and background tasks
- Thread-safe operations with fine-grained locking
- Configurable thread pools via `env/` abstraction

**Pluggable Components**:
- Comparators, merge operators, compaction filters
- Custom table formats, compression algorithms
- Configurable via `options.h` and `configurable.h`

## Debug Levels
- `DEBUG_LEVEL=0` - Release mode (production)
- `DEBUG_LEVEL=1` - Debug with -O2 optimization (default development)
- `DEBUG_LEVEL=2` - Full debug, no optimization (ultimate debugging)

## Important Notes

- **C++20 Required**: GCC >= 11 or Clang >= 10
- **Thread Safety**: Most operations are thread-safe; see documentation for specifics
- **Chinese Comments**: This codebase contains Chinese annotations for learning purposes
- **Memory Management**: Extensive use of smart pointers and RAII patterns
- **Error Handling**: Uses `Status` class for comprehensive error reporting

## Development Workflow

1. Use `make check` to verify changes don't break existing functionality
2. Individual test files can be run directly after building
3. Format code with clang-format before submitting changes
4. Consider using `make asan_check` for memory error detection during development