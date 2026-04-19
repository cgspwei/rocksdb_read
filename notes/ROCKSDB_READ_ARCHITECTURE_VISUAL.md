# RocksDB Read Path - Architecture Visualization

## System Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                      USER APPLICATION                          │
│                        db->Get(key)                             │
└────────────────────────────┬──────────────────────────────────┘
                             │
                             ▼
                    ┌────────────────────┐
                    │   DBImpl::Get()     │
                    │    (Line 2442)     │
                    │  - Validate opts   │
                    │  - Delegate        │
                    └────────┬───────────┘
                             │
                             ▼
                    ┌────────────────────────────────┐
                    │   DBImpl::GetImpl()              │
                    │    (Line 2812)                 │
                    │  - Get SuperVersion (ref)      │
                    │  - Create LookupKey            │
                    │  - Check memtables             │
                    │  - Check SST files             │
                    │  - Return & cleanup SV         │
                    └────────┬───────────────────────┘
                             │
         ╔═══════════════════╩═══════════════════╗
         │                                       │
         ▼                                       ▼
    ╔────────────────────┐              ╔──────────────────────┐
    │ SUPERVERSION (sv)  │              │   Memory View        │
    ├────────────────────┤              ├──────────────────────┤
    │ mem:               │ ─────────┐   │ • Immutable (old)    │
    │ imm:               │ ─────────┼─→ │ • Immutable (newer)  │
    │ current: Version   │ ─────────┤   │ • Active             │
    │ version_number     │           │   │ • Write buffer       │
    └────────────────────┘           │   └──────────────────────┘
                                     │
                                     ▼
                    ┌──────────────────────────────┐
                    │ PHASE 1: MEMTABLE LOOKUP     │
                    ├──────────────────────────────┤
                    │ sv->mem->Get(lkey)           │
                    │ ├─ MemTable::Get()           │
                    │ │  ├─ Range tombstones       │
                    │ │  ├─ Bloom filter check     │
                    │ │  └─ Skiplist search        │
                    │ │     └─ GetFromTable()      │
                    │ │        └─ SaveValue        │
                    │ │           callback        │
                    │ └─ Return if found           │
                    │                              │
                    │ sv->imm->Get(lkey)           │
                    │ └─ Check immutable           │
                    │    memtables                 │
                    └──────────┬───────────────────┘
                               │ [Not found]
                               ▼
                    ┌──────────────────────────────┐
                    │ PHASE 2: SST FILE LOOKUP     │
                    ├──────────────────────────────┤
                    │ sv->current->Get(lkey)       │
                    │ (Version::Get at line 2714)  │
                    └──────────┬───────────────────┘
                               │
         ╔═════════════════════╩════════════════════╗
         │                                          │
         ▼                                          ▼
    ┌────────────────────┐          ┌─────────────────────┐
    │   GetContext       │          │    FilePicker       │
    ├────────────────────┤          ├─────────────────────┤
    │ State machine:     │          │ LSM Level Strategy: │
    │ • kNotFound        │          │ • L0: all files     │
    │ • kMerge           │          │ • L1+: binary search│
    │ • kFound           │          │        + fractional │
    │ • kDeleted         │          │        cascading    │
    │ • kCorrupt         │          │                     │
    │                    │          │ GetNextFile() →     │
    │ Callbacks:         │          │ FdWithKeyRange*     │
    │ • SaveValue()      │          └────────┬────────────┘
    │ • ReportCounters() │                   │
    └────────────────────┘                   │ [For each file]
                                             ▼
                                ┌──────────────────────────────┐
                                │   TableCache::Get()          │
                                │    (Line 502)                │
                                ├──────────────────────────────┤
                                │ 1. Check row cache           │
                                │ 2. FindTable / Cache lookup  │
                                │ 3. Check range tombstones    │
                                │ 4. t->Get() (delegate)       │
                                └──────────────┬───────────────┘
                                               │
                                ▼
                   ┌────────────────────────────────────┐
                   │ BlockBasedTable::Get()             │
                   │ (Line 2489)                        │
                   └────────────────────────────────────┘
                     │
        ┌────────────┼────────────┐
        │            │            │
        ▼            ▼            ▼
    Step 1:      Step 2:      Step 3:
    BLOOM        INDEX        DATA
    FILTER       LOOKUP       BLOCK
        │            │            │
        ▼            ▼            ▼
   ┌────────┐  ┌──────────┐  ┌─────────────────┐
   │ Filter │  │ Index    │  │ Block Cache:    │
   │        │  │ Block    │  │                 │
   │Check   │  │ (cached) │  │ 1. Create key   │
   │key     │  │          │  │ 2. Lookup       │
   │MayMatch│  │Seek in   │  │ 3. If miss:     │
   │(Line   │  │index to  │  │    • Read file  │
   │2325)   │  │find data │  │    • Decompress │
   │        │  │block     │  │    • Insert     │
   │No? →   │  │handle    │  │ 4. Return block │
   │Skip    │  │(Line     │  │ (Line 1712)     │
   │        │  │1671)     │  │                 │
   └────────┘  └──────────┘  └────────┬────────┘
        │            │                 │
        └────────────┴─────────┬───────┘
                               │
                      Step 4: SEARCH BLOCK
                               │
                               ▼
                   ┌───────────────────────────┐
                   │ Data Block Iterator       │
                   ├───────────────────────────┤
                   │ SeekForGet(key)           │
                   │ ├─ Binary search or       │
                   │ │  hash index             │
                   │ └─ Find entry position    │
                   │                           │
                   │ Iterate entries:          │
                   │ ├─ ParseInternalKey      │
                   │ ├─ SaveValue()           │
                   │ │  ├─ Check seq # vis    │
                   │ │  ├─ Merge operands?    │
                   │ │  └─ Found!             │
                   │ └─ Return or continue    │
                   └───────────┬───────────────┘
                               │
                      [Found → Return value]
                      [Not found → Next file]
                               │
                               ▼
                   ┌───────────────────────────┐
                   │ ReturnAndCleanup          │
                   │ SuperVersion             │
                   │ (Unref, may delete)       │
                   └───────────────────────────┘
                               │
                               ▼
                   ┌───────────────────────────┐
                   │ Return Status + Value     │
                   │ to User Application       │
                   └───────────────────────────┘
```

## Memtable Internal Structure

```
┌───────────────────────────────────────────────────────────┐
│              MemTable (In-Memory)                         │
├───────────────────────────────────────────────────────────┤
│                                                           │
│  ┌─────────────────────────────────┐                    │
│  │ Optional Bloom Filter           │ (Line 1527-1533)   │
│  │ • Whole-key or prefix           │                    │
│  │ • Fast reject: O(1)             │                    │
│  └─────────────────┬───────────────┘                    │
│                    │ "Maybe"                             │
│                    ▼                                      │
│  ┌────────────────────────────────────────┐             │
│  │     SkipList (Data Structure)          │             │
│  │                                        │             │
│  │  L3:        [5]                        │             │
│  │             /                          │             │
│  │  L2:   [3]──[5]──[8]                   │             │
│  │        /    /     /                    │             │
│  │  L1:  [1]──[3]──[5]──[7]──[8]──[9]    │             │
│  │       / \  / \  / \ / \ / \ / \ / \   │             │
│  │  L0: [1][2][3][4][5][6][7][8][9]     │             │
│  │       K:V K:V K:V K:V K:V K:V K:V    │             │
│  │                                        │             │
│  │  Search: O(log N) on average          │             │
│  │  Insert: O(log N) on average          │             │
│  │                                        │             │
│  └────────────────────────────────────────┘             │
│                                                           │
│  Range Tombstones:                                       │
│  • Separate iterator tracks delete ranges               │
│  • Marks keys as "definitely deleted"                   │
│                                                           │
└───────────────────────────────────────────────────────────┘
```

## SST File Organization

```
┌─────────────────────────────────────────────────────────────┐
│            SST File (Disk)                                 │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────────────────────────────┐                  │
│  │ Data Block 0 (Compressed)           │                  │
│  │ [KEY0:VAL0]                         │                  │
│  │ [KEY1:VAL1]                         │                  │
│  │ ...                                 │                  │
│  │ [KEY99:VAL99]                       │                  │
│  │ [Checksum]                          │                  │
│  └─────────────────────────────────────┘ ◄── Offset 0x1000│
│                                                             │
│  ┌─────────────────────────────────────┐                  │
│  │ Data Block 1 (Compressed)           │                  │
│  │ [KEY100:VAL100]                     │                  │
│  │ ...                                 │                  │
│  │ [KEY199:VAL199]                     │                  │
│  │ [Checksum]                          │                  │
│  └─────────────────────────────────────┘ ◄── Offset 0x2000│
│                                                             │
│  [More Data Blocks...]                                     │
│                                                             │
│  ┌─────────────────────────────────────┐                  │
│  │ Index Block (Compressed)            │                  │
│  │ Entry 0: KEY99 -> BlockHandle(0x1000)                 │
│  │ Entry 1: KEY199 -> BlockHandle(0x2000)                │
│  │ ...                                 │                  │
│  └─────────────────────────────────────┘ ◄── Cached often │
│                                                             │
│  ┌─────────────────────────────────────┐                  │
│  │ Bloom Filter Block                  │                  │
│  │ • BitSet with k hash functions      │                  │
│  │ • 0 = definitely not present        │                  │
│  │ • 1 = maybe present                 │                  │
│  └─────────────────────────────────────┘ ◄── Cached often │
│                                                             │
│  ┌─────────────────────────────────────┐                  │
│  │ Compression Dictionary (optional)   │                  │
│  └─────────────────────────────────────┘                  │
│                                                             │
│  ┌─────────────────────────────────────┐                  │
│  │ Footer (Metadata)                   │                  │
│  │ • Index block handle                │                  │
│  │ • Bloom filter handle               │                  │
│  │ • Checksum                          │                  │
│  │ • Table format version              │                  │
│  └─────────────────────────────────────┘                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## Block Cache Layer

```
┌───────────────────────────────────────────────────┐
│          Block Cache (LRU in Memory)              │
├───────────────────────────────────────────────────┤
│                                                   │
│  ┌──────────────────────────────────────────┐   │
│  │ Hash Table: CacheKey → Block             │   │
│  │                                          │   │
│  │ Bucket 0: [CacheKey1 → Block*]           │   │
│  │           └─ Compressed? → Decompress   │   │
│  │                                          │   │
│  │ Bucket 1: [CacheKey2 → Block*]           │   │
│  │           [CacheKey3 → Block*]           │   │
│  │                                          │   │
│  │ ...                                      │   │
│  │                                          │   │
│  │ Bucket N: [CacheKeyM → Block*]           │   │
│  └──────────────────────────────────────────┘   │
│                  ▲                               │
│    ┌─────────────┴─────────────┐                │
│    │                           │                │
│    │ HIT                       │ MISS           │
│    │ Return cached            │ Read from      │
│    │ Increment               │ file           │
│    │ priority                │ Decompress     │
│    │                         │ Insert in      │
│    │                         │ cache          │
│    │                         │ Evict old      │
│    │                         │ if needed      │
│    │                         │                │
│  ┌─┴──────────┐          ┌──┴──────────────┐  │
│  │ Statistics │          │  Statistics     │  │
│  │ • Hit rate │          │  • Insertion    │  │
│  │ • Size     │          │  • Evictions    │  │
│  │ • Ratio    │          │  • Read bytes   │  │
│  └────────────┘          └─────────────────┘  │
│                                                   │
│  Priority Tiers:                                │
│  ┌────┬─────────────────┬──────────┐           │
│  │ P1 │ Index blocks    │ Highest  │           │
│  │ P2 │ Filter blocks   │ High     │           │
│  │ P3 │ Data blocks     │ Normal   │           │
│  │ P4 │ Compression dict│ Low      │           │
│  └────┴─────────────────┴──────────┘           │
│                                                   │
└───────────────────────────────────────────────────┘
```

## LSM Level Structure

```
┌─────────────────────────────────────────────────────────┐
│                LSM Tree Structure                       │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  MEMTABLE (Latest writes)                              │
│  ┌──────────────────────────────────┐                 │
│  │ Active MemTable (In-Memory)      │ ← writes go here
│  │ Keys: key1, key5, key8           │                 │
│  └──────────────────────────────────┘                 │
│           ▲                                             │
│           │ Flush when full                            │
│           ▼                                             │
│  ┌──────────────────────────────────┐                 │
│  │ Immutable MemTable #2            │                 │
│  │ Keys: key1-key10                 │                 │
│  └──────────────────────────────────┘                 │
│           ▲                                             │
│           │                                             │
│           ▼                                             │
│  ┌──────────────────────────────────┐                 │
│  │ Immutable MemTable #1            │                 │
│  │ Keys: key1-key100                │                 │
│  └──────────────────────────────────┘                 │
│           ▲                                             │
│           │ Minor Compaction                           │
│           ▼                                             │
│  LEVEL 0 (Newest SST files)                            │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐         │
│  │ L0 File 3 │  │ L0 File 2 │  │ L0 File 1 │         │
│  │ key2-key9 │  │ key1-key8 │  │ key5-key7 │         │
│  └───────────┘  └───────────┘  └───────────┘ Overlaps!
│           ▲ (All must be checked for reads)            │
│           │                                             │
│           │ Compaction (when > n files)                │
│           ▼                                             │
│  LEVEL 1 (Larger, non-overlapping)                     │
│  ┌──────────────────┐  ┌──────────────────┐           │
│  │ L1 File 1        │  │ L1 File 2        │           │
│  │ key1-key50       │  │ key51-key100     │ No overlap!
│  └──────────────────┘  └──────────────────┘           │
│           ▲                                             │
│           │ Compaction (background)                    │
│           ▼                                             │
│  LEVEL 2 (Even larger)                                 │
│  ┌──────────────────┐  ┌──────────────────┐           │
│  │ L2 File 1        │  │ L2 File 2        │           │
│  │ key1-key1000     │  │ key1001-key2000  │           │
│  └──────────────────┘  └──────────────────┘           │
│           ▲                                             │
│           │                                             │
│           ▼                                             │
│  [More levels...]                                       │
│                                                         │
│  Read Strategy:                                         │
│  1. Check memtable first (newest data)                 │
│  2. Check L0 files (all)                               │
│  3. Check L1 files (binary search)                     │
│  4. Check L2+ files (binary search)                    │
│  5. Return if found or NotFound                        │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## Read Path Performance Timeline

```
Request Timeline for Different Scenarios:

BEST CASE - Hot key in active memtable
├─ 0-1 ns:     Function call overhead
├─ 1-10 ns:    GetAndRefSuperVersion (thread-local)
├─ 10-50 ns:   LookupKey creation
├─ 50-100 ns:  Memtable bloom check
├─ 100-300 ns: Skiplist search (O(log n))
├─ 300-500 ns: SaveValue callback & return
└─ ~500 ns TOTAL

GOOD CASE - Key in block cache
├─ 0-1 ns:     Function calls
├─ 1-50 ns:    Get SuperVersion
├─ 50-100 ns:  Memtable check (not found)
├─ 100-500 ns: FilePicker (select SST)
├─ 500-1000 ns: TableCache lookup (find TableReader)
├─ 1-2 μs:     Bloom filter check (cached)
├─ 1-2 μs:     Index lookup (cached)
├─ 1-2 μs:     Block cache hit (decompress if needed)
├─ 1-5 μs:     Binary search in block
└─ ~10 μs TOTAL

OK CASE - Index/filter in cache, data block miss
├─ 0-1 ns:     Function calls
├─ 1-50 ns:    Get SuperVersion
├─ 50-100 ns:  Memtable check
├─ 100-500 ns: FilePicker
├─ 500-1000 ns: TableCache
├─ 1-2 μs:     Bloom check (cached)
├─ 1-2 μs:     Index lookup (cached)
├─ 100-500 μs: Read data block from disk ⚠️
├─ 100-1000 μs: Decompress block ⚠️
├─ 1-5 μs:     Binary search
└─ ~100+ μs TOTAL

BAD CASE - Key not found (full scan)
├─ 0-1 ns:     Function calls
├─ 1-50 ns:    Get SuperVersion
├─ 50-100 ns:  Memtable check
├─ 100-2000 ns: FilePicker (check all levels)
├─ 1-10 μs:    For each file:
│  ├─ TableCache (may load TableReader)
│  ├─ Bloom check (maybe miss = fast)
│  └─ Index/block lookups
├─ 100-500 μs: Possible disk reads for index/data
└─ ~1-100 ms TOTAL

⚠️ = Potential bottleneck
```

## FilePicker Algorithm - Fractional Cascading

```
Binary Key Search with Fractional Cascading:

Key: "user_id:50000"

LEVEL 0 (Check all - overlapping allowed):
File0: [key0-key30]        ✓ Check (no overlap guarantee)
File1: [key20-key80]       ✓ Check (overlaps with File0)
File2: [key60-key90]       ✓ Check (overlaps with others)

LEVEL 1 (Binary search - non-overlapping):
All files sorted: F1[k0-k20], F2[k21-k40], F3[k41-k60], F4[k61-k100]
                                                     ▲
Search: key=50000 → Falls between k41-k60 → Check F3
        (Prev file at L0 had last key at L0_file2)
        FileIndexer says: "Check F3 at L1" ← Fractional cascading!
        (Avoids re-binary-searching entire L1)

LEVEL 2 (Binary search - non-overlapping):
Previous FileIndexer result: "Check F2-F3 range"
        F1[k0-k100], F2[k101-k200], F3[k201-k300]
                     ▲
Search: key=50000 → Still in F1 range
        FileIndexer narrows search: F1 has it
        (Didn't need to binary search entire L2)

LEVEL 3+ (Similar pattern):
Continue with increasingly narrowed search window

Result: Found at L2 File2
        Total binary searches: 3-4 instead of 7 (# levels)
        Time: O(log L) instead of O(L log F)
        (L = levels, F = files per level)
```

---

## Key Performance Metrics Collection Points

```
Statistics Collection Points:

MEMTABLE_HIT ─┬─ MemTable::Get() found = true
              └─ Line 2994 in db_impl.cc
              
MEMTABLE_MISS ─┬─ MemTable::Get() found = false
               └─ Line 3052 in db_impl.cc

BLOOM_FILTER_USEFUL ─┬─ FullFilterKeyMayMatch() returns false
                     └─ Line 2345 in block_based_table_reader.cc

BLOOM_FILTER_FULL_POSITIVE ─┬─ FullFilterKeyMayMatch() returns true
                            └─ Line 2342 in block_based_table_reader.cc

GET_HIT_L0/L1/L2_AND_UP ─┬─ Version::Get() when value found
                        ├─ Line 2820-2825 in version_set.cc
                        └─ Based on fp.GetHitFileLevel()

BLOCK_CACHE_HIT_COUNT ─┬─ LookupAndPinBlocksInCache() returns
                       │  cached block
                       └─ Line 271-272 in block_based_table_reader.cc

BLOCK_CACHE_MISS_COUNT ─┬─ Block not in cache, must read
                        └─ Line 335 in block_based_table_reader.cc
```

