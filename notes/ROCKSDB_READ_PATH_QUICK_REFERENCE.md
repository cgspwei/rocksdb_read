# RocksDB Read Path - Quick Reference Guide

## Quick Navigation Map

```
USER APPLICATION
       ↓
db->Get(key)  [Public API]
       ↓
DBImpl::Get() 
├─ File: db/db_impl/db_impl.cc
├─ Line: 2442
└─ Validates read options
       ↓
DBImpl::GetImpl()
├─ File: db/db_impl/db_impl.cc
├─ Line: 2812
├─ Gets SuperVersion (sv)
├─ Checks memtables first
└─ Then checks SST files
       ↓
   ╔═══════════════════════════════════╗
   ║   SUPERVERSION STRUCTURE         ║
   ║  sv->mem (active memtable)        ║
   ║  sv->imm (immutable memtables)    ║
   ║  sv->current (SST Version)        ║
   ╚═══════════════════════════════════╝
       ↓
PHASE 1: MEMTABLE LOOKUP
├─ MemTable::Get()
│  ├─ File: db/memtable.cc
│  ├─ Line: 1485
│  ├─ Check bloom filter
│  ├─ Search skiplist
│  └─ Return if found
└─ If not found → continue to SST
       ↓
PHASE 2: SST FILE LOOKUP
├─ Version::Get()
│  ├─ File: db/version_set.cc
│  ├─ Line: 2714
│  ├─ Create GetContext
│  ├─ Create FilePicker
│  └─ For each selected file:
│
└─ FilePicker::GetNextFile()
   ├─ File: db/version_set.cc
   ├─ Line: 148-350
   ├─ Level 0: Check all files
   ├─ Level 1+: Binary search with fractional cascading
   └─ Early termination if key definitely not in remaining levels
       ↓
TableCache::Get()
├─ File: db/table_cache.cc
├─ Line: 502
├─ Check row cache
├─ Open/find TableReader
└─ Delegate to BlockBasedTable::Get()
       ↓
BlockBasedTable::Get()
├─ File: table/block_based/block_based_table_reader.cc
├─ Line: 2489
├─ Step 1: Bloom filter check
├─ Step 2: Index lookup
├─ Step 3: Data block reading
└─ Step 4: Search within block
       ↓
RETURN: Value or NotFound status
```

## Key Components at a Glance

### 1. Entry Point
- **File**: `db/db_impl/db_impl.cc`
- **Function**: `DBImpl::Get()` (Line 2442)
- **Main Logic**: `DBImpl::GetImpl()` (Line 2812)

### 2. SuperVersion (Coordinator)
- **File**: `db/column_family.h` (Lines 207-276)
- **Purpose**: Point-in-time consistent view of entire LSM
- **Members**: mem, imm, current (Version)
- **Gets via**: `GetAndRefSuperVersion()` (Line 5247 in db_impl.cc)
- **Returns via**: `ReturnAndCleanupSuperVersion()` (Line 5284 in db_impl.cc)

### 3. Memtable Layer
- **File**: `db/memtable.cc`
- **Main Function**: `MemTable::Get()` (Line 1485)
- **Bloom Filter**: Optional, fast rejection
- **Data Structure**: SkipList (O(log n) search)
- **Returns**: Found or continues to SST

### 4. Version/LSM Layer
- **File**: `db/version_set.cc`
- **Main Function**: `Version::Get()` (Line 2714)
- **File Selector**: `FilePicker` (Lines 148-350)
- **Strategy**: Check levels in order, use key ranges to minimize files

### 5. File Selection (FilePicker)
- **Level 0**: All files (overlapping ranges)
- **Level 1+**: Binary search with fractional cascading optimization
- **Early Exit**: If key definitely not in remaining levels

### 6. Table Cache
- **File**: `db/table_cache.cc`
- **Function**: `TableCache::Get()` (Line 502)
- **Purpose**: Cache opened SST files (TableReaders)
- **Optional**: Row cache for exact key-value pairs

### 7. SST File Reading (BlockBasedTable)
- **File**: `table/block_based/block_based_table_reader.cc`
- **Main Function**: `BlockBasedTable::Get()` (Line 2489)
- **Steps**:
  1. Bloom filter check (Line 2325: `FullFilterKeyMayMatch()`)
  2. Index lookup (Line 1671: `NewIndexIterator()`)
  3. Data block reading (Line 1712: `LookupAndPinBlocksInCache()`)
  4. Search within block (Line 2583: `SeekForGet()`)

### 8. Block Cache
- **File**: `table/block_based/block_based_table_reader.cc`
- **Function**: `LookupAndPinBlocksInCache()` (Line 1712)
- **Cache Types**: Data blocks, Index blocks, Filter blocks
- **Lookup**: `cache.LookupFull()` with block handle as key
- **Miss**: Read from file, decompress, insert in cache

### 9. Bloom Filters
- **Memtable**: Optional bloom filter in skiplist
  - Line 1527 (whole-key) or 1531-1533 (prefix)
- **SST**: Full bloom filter in every SST
  - Line 2325: `FullFilterKeyMayMatch()`
  - Whole-key or prefix variants
  - If bloom says "not present" → skip reading blocks

## Performance Optimization Sequence

1. **Memtable Bloom** → reject most keys not in memtable (nanoseconds)
2. **Memtable Skiplist** → search if present (microseconds)
3. **FilePicker** → identify candidate files (microseconds)
4. **SST Bloom** → reject most keys in SST (microseconds)
5. **Index lookup** → find data block (microseconds, cached)
6. **Block cache** → check if block already loaded (microseconds)
7. **Block read** → read from file if needed (milliseconds)
8. **Block decompress** → decompress if needed (microseconds)
9. **Binary search in block** → find exact key (microseconds)

## Key Statistics to Monitor

### Memtable Performance
- `MEMTABLE_HIT` / `MEMTABLE_MISS` - Where value found
- `bloom_memtable_hit_count` / `miss_count` - Bloom effectiveness

### SST Performance
- `GET_HIT_L0` / `L1` / `L2_AND_UP` - Which level had value
- `BLOOM_FILTER_USEFUL` - Bloom actually filtered something
- `BLOOM_FILTER_FULL_POSITIVE` - Bloom was permissive

### Block Cache Performance
- `BLOCK_CACHE_HIT_COUNT` / `MISS_COUNT` - Cache effectiveness
- `BLOCK_CACHE_INDEX_HIT_COUNT` - Index block hits
- `BLOCK_CACHE_FILTER_HIT_COUNT` - Filter block hits

## Common Paths

### Best Case (Hot Key in Memory)
1. Get memtable bloom → passes
2. Search memtable → found
3. **Time**: ~100-500ns

### Good Case (In Block Cache)
1. Get memtable → not found
2. Get FilePicker → find SST
3. Get SST bloom → passes
4. Get index from cache → block handle
5. Get data block from cache → found
6. **Time**: ~1-10μs

### Bad Case (Disk Read)
1. Get memtable → not found
2. Get FilePicker → find SST
3. Get SST bloom → passes
4. Get index from cache or disk → block handle
5. Get data block → not in cache, read from disk
6. Decompress → if compressed
7. Search block → found
8. **Time**: ~10-100ms

### Worst Case (Key Not Found)
1. Check all memtables → not found
2. Check all SST levels → all blooms pass or all blocks checked
3. Return NotFound
4. **Time**: ~1-100ms depending on levels

## Reference: File Locations

| Component | Path | Key Function |
|-----------|------|----------|
| DB Implementation | `db/db_impl/db_impl.cc` | DBImpl::Get() (2442), GetImpl() (2812) |
| SuperVersion | `db/column_family.h` | struct SuperVersion (207) |
| SuperVersion Mgmt | `db/db_impl/db_impl.cc` | GetAndRefSuperVersion() (5247) |
| Memtable | `db/memtable.cc` | MemTable::Get() (1485) |
| Version | `db/version_set.cc` | Version::Get() (2714) |
| File Selection | `db/version_set.cc` | class FilePicker (148) |
| Table Cache | `db/table_cache.cc` | TableCache::Get() (502) |
| SST Reader | `table/block_based/block_based_table_reader.cc` | BlockBasedTable::Get() (2489) |
| Bloom Filter | `table/block_based/block_based_table_reader.cc` | FullFilterKeyMayMatch() (2325) |
| Block Iterator | `table/block_based/block_based_table_reader.cc` | NewIndexIterator() (1671) |
| Block Cache | `table/block_based/block_based_table_reader.cc` | LookupAndPinBlocksInCache() (1712) |

## Trace Example

```
Key: "user_id:12345"
Snapshot: seq=1000

1. GetAndRefSuperVersion(cfd) → sv
2. LookupKey lkey = {user_id:12345, 1000, kTypeValue}

3. sv->mem->Get(lkey)
   - Check memtable bloom → says "maybe" (1 hash probe)
   - Search skiplist for "user_id:12345" → not found
   - Return false

4. sv->imm->Get(lkey)
   - No immutable memtables
   - Return false

5. sv->current->Get(lkey)
   - Create GetContext
   - FilePicker::GetNextFile()
     → Level 0, File 42 (key range: user_id:123xx-user_id:124xx)
   - TableCache::Get(file 42)
     → Check row cache → miss
     → Open TableReader from cache
     → BlockBasedTable::Get()
   
   6. FullFilterKeyMayMatch()
      - Check bloom for "user_id:12345" → says "maybe"
      
   7. NewIndexIterator()
      - Create index iterator
      - Seek to "user_id:12345"
      - Index entry: block_handle=0x1000, first_key="user_id:12340"
      
   8. NewDataBlockIterator(block_handle=0x1000)
      - Create cache key = {base_key, 0x1000}
      - Lookup in block cache → miss
      - Read block from file at offset 0x1000
      - Decompress block
      - Insert in cache
      - Return iterator
      
   9. Search block
      - SeekForGet("user_id:12345") → found at offset 0x500
      - ParseInternalKey → {user_id:12345, 1000, kTypeValue}
      - GetContext::SaveValue()
        - Check seq 1000 <= 1000 ✓
        - State = kFound
      - Return value
      
10. ReturnAndCleanupSuperVersion(sv)

Result: Value found in 5-50ms depending on cache hits
```

