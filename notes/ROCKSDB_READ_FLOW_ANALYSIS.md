# RocksDB Complete Read Data Flow Analysis

## Executive Summary

This document traces the complete execution path from a user calling `DB::Get()` through the RocksDB architecture down to reading individual key-value pairs from SST files. The read path involves multiple layers including memtables, version management, table caching, bloom filters, block caching, and SST file reading.

---

## 1. Top-Level Get() Entry Point

### File: `db/db_impl/db_impl.cc`

#### Primary Functions:

**Line 2442: `Status DBImpl::Get()`**
```cpp
Status DBImpl::Get(const ReadOptions& _read_options,
                   ColumnFamilyHandle* column_family, const Slice& key,
                   PinnableSlice* value, std::string* timestamp)
```

**Purpose**: Public API entry point for single key lookups

**Responsibilities**:
1. Validates ReadOptions (io_activity must be kUnknown or kGet)
2. Sets io_activity to kGet if not specified
3. Delegates to `DBImpl::GetImpl()`

**Line 2464: `Status DBImpl::GetImpl()`** (overload 1)
```cpp
Status DBImpl::GetImpl(const ReadOptions& read_options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     PinnableSlice* value, std::string* timestamp)
```

**Purpose**: Wrapper that constructs GetImplOptions and calls main GetImpl

**Line 2812: `Status DBImpl::GetImpl()`** (overload 2 - Main Implementation)
```cpp
Status DBImpl::GetImpl(const ReadOptions& read_options, const Slice& key,
                     GetImplOptions& get_impl_options)
```

**Key Steps** (Lines 2812-3149):
1. **Line 2868**: Acquire SuperVersion via `GetAndRefSuperVersion(cfd)`
2. **Line 2942**: Create LookupKey with (key, snapshot, timestamp)
3. **Lines 2980-3031**: Check memtables first (mem and imm)
4. **Lines 3035-3052**: If not found in memtables, call `sv->current->Get()` (Version::Get)
5. **Line 3144**: Return and cleanup SuperVersion with `ReturnAndCleanupSuperVersion()`

**Critical Coordination**:
- The SuperVersion (sv) contains:
  - `sv->mem`: Current active memtable (MemTable*)
  - `sv->imm`: Immutable memtable list (MemTableListVersion*)
  - `sv->current`: Current version for SST files (Version*)

---

## 2. SuperVersion - Coordinating Reads Across LSM Layers

### File: `db/column_family.h`

**Structure Definition** (Lines 207-276):
```cpp
struct SuperVersion {
  ColumnFamilyData* cfd;              // Column family metadata
  ReadOnlyMemTable* mem;              // Current memtable (read view)
  MemTableListVersion* imm;           // Immutable memtable list
  Version* current;                   // Current SST version
  MutableCFOptions mutable_cf_options; // Configuration
  uint64_t version_number;            // Version number
  WriteStallCondition write_stall_condition;
  std::string full_history_ts_low;
  std::shared_ptr<const SeqnoToTimeMapping> seqno_to_time_mapping;
  // Reference counting
  std::atomic<uint32_t> refs;
}
```

**Purpose**: Provides point-in-time consistent view of the entire LSM tree

**Read Path Role**:
1. Allows consistent reads across memtable + immutable memtables + SST files
2. Prevents data visibility changes mid-read operation
3. Reference counting ensures data isn't deleted while being read

### File: `db/db_impl/db_impl.cc`

**Line 5247: `SuperVersion* DBImpl::GetAndRefSuperVersion(ColumnFamilyData* cfd)`**
```cpp
SuperVersion* DBImpl::GetAndRefSuperVersion(ColumnFamilyData* cfd) {
  return cfd->GetThreadLocalSuperVersion(this);
}
```
- Acquires a reference-counted SuperVersion
- Uses thread-local storage for efficiency
- Prevents concurrent modifications

**Line 5284: `void DBImpl::ReturnAndCleanupSuperVersion()`**
- Releases the SuperVersion reference
- Triggers cleanup if no more references exist

---

## 3. Memtable Read Path

### File: `db/memtable.cc`

**Line 1485: `bool MemTable::Get()`**
```cpp
bool MemTable::Get(const LookupKey& key, std::string* value,
                   PinnableWideColumns* columns, std::string* timestamp,
                   Status* s, MergeContext* merge_context,
                   SequenceNumber* max_covering_tombstone_seq,
                   SequenceNumber* seq, const ReadOptions& read_opts,
                   bool immutable_memtable, ReadCallback* callback,
                   bool* is_blob_index, bool do_merge)
```

**Key Steps** (Lines 1485-1563):

1. **Lines 1500-1516**: Create range tombstone iterator to check for deleted keys
   - Range tombstones can cover entire key ranges
   - Used to determine if key is deleted

2. **Lines 1520-1537**: Bloom Filter Check (if present)
   - **Line 1527**: If whole-key filtering enabled: check `bloom_filter_->MayContain()`
   - **Line 1531-1533**: Else if prefix filtering: extract prefix and check
   - **Line 1539**: If bloom says "definitely not present", return false (kMaxSequenceNumber)
   - This is an optimization to skip expensive skiplist lookups

3. **Line 1547-1549**: Call `GetFromTable()` to actually search the in-memory structure
   - Uses `table_->Get(key, &saver, SaveValue)` where table_ is the skiplist structure
   - SaveValue is the callback that processes found entries

4. **Lines 1554-1560**: Handle merge context if needed

**Line 1565: `void MemTable::GetFromTable()`**
- Creates a Saver struct with all the context
- Calls the underlying table structure's Get method
- Table structure is typically SkipListRep for performance

---

## 4. Version and Level-Based File Selection

### File: `db/version_set.cc`

**Line 2714: `void Version::Get()`**
```cpp
void Version::Get(const ReadOptions& read_options, const LookupKey& k,
                  PinnableSlice* value, PinnableWideColumns* columns,
                  std::string* timestamp, Status* status,
                  MergeContext* merge_context,
                  SequenceNumber* max_covering_tombstone_seq,
                  PinnedIteratorsManager* pinned_iters_mgr, bool* value_found,
                  bool* key_exists, SequenceNumber* seq, ReadCallback* callback,
                  bool* is_blob, bool do_merge)
```

**Key Architecture** (Lines 2714-2920):

1. **Lines 2732-2736**: Setup tracing context for block cache monitoring
   - Enables tracing of block cache hits/misses during reads

2. **Lines 2746-2753**: Create GetContext
   - Central context object that:
     - Tracks search state (NotFound, Merge, Found, Deleted)
     - Accumulates merge operands
     - Coordinates callbacks and statistics

3. **Lines 2760-2763**: Create FilePicker
   - Efficiently selects files across LSM levels
   - Uses key range filtering and fractional cascading

4. **Lines 2766-2883**: Main search loop - iterate through files:
   ```cpp
   FdWithKeyRange* f = fp.GetNextFile();
   while (f != nullptr) {
     // Call TableCache::Get for this file's SST
     *status = table_cache_->Get(
         read_options, *internal_comparator(), *f->file_metadata, ikey,
         &get_context, ...);
     
     // Check GetContext state:
     switch (get_context.State()) {
       case GetContext::kNotFound:     // Keep searching
         break;
       case GetContext::kMerge:        // Need to continue for more merge operands
         break;
       case GetContext::kFound:        // Stop - key found
         return;
       case GetContext::kDeleted:      // Stop - key was deleted
         *status = Status::NotFound();
         return;
     }
     f = fp.GetNextFile();
   }
   ```

**FilePicker Algorithm** (Lines 148-350 in version_set.cc):

**Line 148: `class FilePicker`**
```cpp
class FilePicker {
  FilePicker(const Slice& user_key, const Slice& ikey,
             autovector<LevelFilesBrief>* file_levels, unsigned int num_levels,
             FileIndexer* file_indexer, const Comparator* user_comparator,
             const InternalKeyComparator* internal_comparator)
```

**File Selection Strategy** (Lines 183-250):
1. **Level 0**: All files can have overlapping ranges
   - Must check all L0 files (return in reverse insertion order)
   
2. **Level 1+**: Files are sorted by key range and non-overlapping
   - Use binary search to find potential files
   - FileIndexer uses "fractional cascading" optimization:
     - Maintains pointers to narrow down search range in next level
     - Avoids redundant binary searches

3. **Key Range Filtering** (Lines 201-234):
   - Compare user key against file's smallest and largest keys
   - Skip files where key is outside range
   - Use FileIndexer to predict which file in next level

4. **Early Termination** (Lines 237-239):
   - If key is smaller than file's smallest key in level > 0:
     - Key cannot exist in any higher level
     - Stop searching

**Return Values**:
- `GetContext::kFound` - Value found, return immediately
- `GetContext::kDeleted` - Tombstone found, return NotFound status
- `GetContext::kMerge` - Merge operand found, continue searching for base value
- `GetContext::kNotFound` - Not in this file, continue to next file

---

## 5. Table Cache and SST File Opening

### File: `db/table_cache.cc`

**Line 502: `Status TableCache::Get()`**
```cpp
Status TableCache::Get(const ReadOptions& options,
                       const InternalKeyComparator& internal_comparator,
                       const FileMetaData& file_meta, const Slice& k,
                       GetContext* get_context,
                       const MutableCFOptions& mutable_cf_options,
                       HistogramImpl* file_read_hist, bool skip_filters,
                       int level, size_t max_file_size_for_l0_meta_pin)
```

**Key Steps** (Lines 502-587):

1. **Lines 518-527**: Check Row Cache (if enabled)
   - Row cache stores exact key-value pairs
   - Faster than re-reading SST and decompressing

2. **Lines 533-539**: Open/Find TableReader via `FindTable()`
   - TableCache caches opened TableReader instances
   - Caches are keyed by file number/size
   - Avoids repeatedly opening the same SST file
   - Returns handle to cached TableReader

3. **Lines 540-557**: Check Range Tombstones in SST
   - Create range tombstone iterator
   - Check if key is covered by a delete range
   - Update max_covering_tombstone_seq if needed

4. **Lines 559-561**: Delegate to TableReader::Get()
   - For BlockBasedTable: calls `BlockBasedTable::Get()`
   - TableReader is typically BlockBasedTableReader

**Handle Management** (Lines 583-585):
- Cache handles maintain reference counting
- Release handle after use to allow eviction

---

## 6. BlockBasedTable - SST File Format and Block Reading

### File: `table/block_based/block_based_table_reader.cc`

**Line 2489: `Status BlockBasedTable::Get()`**
```cpp
Status BlockBasedTable::Get(const ReadOptions& read_options, const Slice& key,
                            GetContext* get_context,
                            const SliceTransform* prefix_extractor,
                            bool skip_filters)
```

**Complete Read Flow** (Lines 2489-2670):

### Step 1: Bloom Filter Check (Lines 2502-2521)
**Line 2518-2520: Check Full Filter**
```cpp
const bool may_match =
    FullFilterKeyMayMatch(filter, key, prefix_extractor, get_context,
                          &lookup_context, read_options);
```

**Line 2325: `bool BlockBasedTable::FullFilterKeyMayMatch()`**
```cpp
bool BlockBasedTable::FullFilterKeyMayMatch(
    FilterBlockReader* filter, const Slice& internal_key,
    const SliceTransform* prefix_extractor, GetContext* get_context,
    BlockCacheLookupContext* lookup_context,
    const ReadOptions& read_options)
```

**Bloom Filter Variants** (Lines 2338-2365):
1. **Whole-Key Filtering** (Line 2338-2347):
   - `filter->KeyMayMatch(user_key_without_ts)`
   - Most accurate, checks exact key
   - Statistics: BLOOM_FILTER_FULL_POSITIVE or BLOOM_FILTER_USEFUL

2. **Prefix Filtering** (Lines 2348-2363):
   - Extract prefix using SliceTransform
   - `filter->PrefixMayMatch(prefix)`
   - More permissive, faster for range queries
   - Statistics: BLOOM_FILTER_PREFIX_CHECKED

**If Bloom says "not present"**:
- Record BLOOM_FILTER_USEFUL statistic
- Return early - skip reading data blocks

**If Bloom says "might exist"**:
- Record BLOOM_FILTER_FULL_POSITIVE statistic
- Continue to data blocks

### Step 2: Index Lookup (Lines 2523-2532)
**Line 2530-2532: Create Index Iterator**
```cpp
auto iiter = NewIndexIterator(read_options, need_upper_bound_check, 
                              &iiter_on_stack, get_context, &lookup_context);
```

**Line 1671: `InternalIteratorBase<IndexValue>* BlockBasedTable::NewIndexIterator()`**
- Creates iterator over index block entries
- Index block contains: key ranges → data block handles
- Index is typically cached in block cache

**Line 2542: Index Iterator Seek and Iterate**
```cpp
for (iiter->Seek(key); iiter->Valid() && !done; iiter->Next()) {
  IndexValue v = iiter->value();  // v.handle points to data block
  
  // Check if key falls in this block's range
  if (!skip_filters && CompareWithoutTimestamp(key, v.first_internal_key) < 0) {
    break;  // Key is before this block, stop searching
  }
  
  // Read and search this data block
  // ...
}
```

### Step 3: Data Block Reading and Caching (Lines 2542-2625)

**Line 2563-2567: Read Data Block**
```cpp
NewDataBlockIterator<DataBlockIter>(
    read_options, v.handle, &biter, BlockType::kData, get_context,
    &lookup_data_block_context, /*prefetch_buffer=*/nullptr,
    /*for_compaction=*/false, /*async_read=*/false, tmp_status,
    /*use_block_cache_for_lookup=*/true);
```

**Key Parameters**:
- `v.handle`: BlockHandle specifying block location in SST
- `use_block_cache_for_lookup=true`: Consult block cache

**Block Cache Lookup** (in NewDataBlockIterator):
1. **Create cache key**: `CacheKey key_data = GetCacheKey(base_cache_key, handle)`
2. **Lookup in cache**: `cache.LookupFull(key, &create_ctx, ...)`
3. **Cache Hit**: Return cached Block immediately
4. **Cache Miss**:
   - Read block from file
   - Decompress if needed
   - Parse block structure
   - Insert into cache

### Step 4: Search Within Data Block (Lines 2583-2625)

**Line 2583: Hash Index or Binary Search**
```cpp
bool may_exist = biter.SeekForGet(key);
```

**Line 2594-2619: Iterate Block Entries**
```cpp
for (; biter.Valid(); biter.Next()) {
  ParsedInternalKey parsed_key;
  Status pik_status = ParseInternalKey(biter.key(), &parsed_key, false);
  
  // Call SaveValue callback - this is how GetContext accumulates results
  bool ret = get_context->SaveValue(
      parsed_key, biter.value(), &matched, &read_status,
      biter.IsValuePinned() ? &biter : nullptr);
  
  if (!ret) {
    if (get_context->State() == GetContext::GetState::kFound) {
      does_referenced_key_exist = true;
      referenced_data_size = biter.key().size() + biter.value().size();
    }
    done = true;
    break;  // Stop searching
  }
}
```

**GetContext::SaveValue** Behavior:
- Takes parsed_key (user_key, value_type, sequence) and value
- Checks sequence number visibility
- Accumulates merge operands or returns found value
- Updates GetContext state

---

## 7. Block Cache Architecture

### File: `table/block_based/block_based_table_reader.cc`

**Block Cache Components**:
1. **Cache Key** (Line 1736):
   - Combines: base_cache_key (table) + block_handle (offset)
   - Ensures unique identification of blocks across SST files

2. **Cache Types** (Lines 270-309):
   - **Data blocks**: Actual key-value pairs
   - **Index blocks**: Key ranges → data block handles
   - **Filter blocks**: Bloom filter data
   - **Compression dictionary**: Used for decompression

3. **Cache Priority** (Line 1748):
   - Different block types have different eviction priorities
   - Index/filter blocks: higher priority (keep in cache)
   - Data blocks: lower priority (can evict when space needed)

4. **Cache Statistics** (Lines 271-310):
   ```
   BLOCK_CACHE_HIT_COUNT / BLOCK_CACHE_MISS_COUNT
   BLOCK_CACHE_INDEX_HIT_COUNT / BLOCK_CACHE_INDEX_MISS_COUNT
   BLOCK_CACHE_FILTER_HIT_COUNT / BLOCK_CACHE_FILTER_MISS_COUNT
   ```

**Block Reading** (Line 1712-1760):
```cpp
template <typename TBlocklike>
Status BlockBasedTable::LookupAndPinBlocksInCache(
    const ReadOptions& ro, const BlockHandle& handle,
    CachableEntry<TBlocklike>* out_parsed_block)
```

Steps:
1. Get or create compression dictionary if needed
2. Create cache key from base_cache_key + handle
3. Lookup in cache with `block_cache.LookupFull()`
4. If miss: read block from file, decompress, insert in cache
5. Return CachableEntry with block data and cache handle

---

## 8. Bloom Filter Details

### Implementation Strategy

**Full Filter vs Prefix Filter**:
1. **Full Filter** (Whole-Key):
   - Every key added to bloom filter
   - `filter->KeyMayMatch(user_key)` checks presence
   - High precision, more false negatives (no false positives)

2. **Prefix Filter**:
   - Only prefixes added to bloom filter
   - `filter->PrefixMayMatch(prefix)` checks presence
   - Better for range queries, more false positives

**Bloom Filter Checks in Memtable** (db/memtable.cc Lines 1522-1541):
- **Line 1527**: Whole-key check if enabled
- **Line 1531-1533**: Prefix check if enabled and key matches domain
- **Line 1539**: If bloom says "definitely not", return false
- Otherwise proceed to skiplist lookup

**Bloom Filter Checks in SST** (Lines 2518-2520):
- Called before any data block reads
- Single check can eliminate entire SST file
- Major performance optimization

**Statistics Recorded**:
- `BLOOM_FILTER_USEFUL`: Cases where bloom actually filtered out key
- `BLOOM_FILTER_FULL_POSITIVE`: Cases where key passed bloom check
- `BLOOM_FILTER_PREFIX_CHECKED`: Prefix bloom checks
- `BLOOM_FILTER_PREFIX_USEFUL`: Prefix filter was useful

---

## 9. Sequence Number and Visibility

### LookupKey Structure
**Lines 2942**: `LookupKey lkey(key, snapshot, read_options.timestamp)`

- Encodes: user_key + sequence_number + value_type
- Sequence number from snapshot determines visibility
- Used for all comparisons and lookups

### Sequence Number Filtering
**In GetContext::SaveValue**:
1. Parse internal key to get sequence number
2. Compare against snapshot sequence number
3. Only return values with seq <= snapshot
4. Accumulate multiple values for merge operations

### Timestamp Support
- User-defined timestamps added after user key
- Timestamp extracted when present
- Affects key comparisons and range checks

---

## 10. Complete Read Path Summary

```
USER CALL: db->Get(key, &value)
    ↓
DBImpl::Get() [Line 2442, db_impl.cc]
    ↓
DBImpl::GetImpl(GetImplOptions) [Line 2812, db_impl.cc]
    ├─ GetAndRefSuperVersion() → sv [Line 5247]
    ├─ LookupKey lkey(key, snapshot, timestamp) [Line 2942]
    │
    ├─ PHASE 1: Check Memtables [Lines 2980-3031]
    │   ├─ sv->mem->Get(lkey, ...) [Line 2983]
    │   │   └─ MemTable::Get() [Line 1485, memtable.cc]
    │   │       ├─ Check range tombstones
    │   │       ├─ Check bloom filter (if present)
    │   │       └─ Search skiplist via GetFromTable()
    │   │
    │   └─ sv->imm->Get(lkey, ...) [Line 2996]
    │       └─ MemTableListVersion::Get()
    │           └─ Iterate immutable memtables
    │
    ├─ PHASE 2: Check SST Files [Lines 3035-3052]
    │   └─ sv->current->Get() [Line 3037, version_set.cc]
    │       └─ Version::Get() [Line 2714, version_set.cc]
    │           ├─ Create GetContext [Lines 2746-2753]
    │           ├─ Create FilePicker [Lines 2760-2763]
    │           │
    │           └─ For each file from FilePicker [Lines 2766-2883]:
    │               ├─ FilePicker::GetNextFile() [Line 2764]
    │               │   └─ Selects files across levels using key ranges
    │               │
    │               └─ TableCache::Get() [Line 2780, table_cache.cc]
    │                   ├─ Check row cache [Lines 518-527]
    │                   ├─ Open/Find TableReader [Lines 533-539]
    │                   ├─ Check range tombstones [Lines 540-557]
    │                   │
    │                   └─ BlockBasedTable::Get() [Line 2489, block_based_table_reader.cc]
    │                       ├─ Check bloom filter [Lines 2518-2520]
    │                       │   └─ FullFilterKeyMayMatch() [Line 2325]
    │                       │       └─ Whole-key or prefix bloom check
    │                       │
    │                       ├─ Create index iterator [Lines 2523-2532]
    │                       │   └─ NewIndexIterator() [Line 1671]
    │                       │
    │                       ├─ Seek in index [Line 2542]
    │                       │
    │                       └─ For each data block [Lines 2563-2625]:
    │                           ├─ Create data block iterator [Lines 2563-2567]
    │                           │   └─ NewDataBlockIterator()
    │                           │       ├─ Check block cache [Line 1747]
    │                           │       ├─ If miss: read from file + decompress
    │                           │       └─ Insert in cache
    │                           │
    │                           └─ Search data block [Lines 2583-2625]
    │                               ├─ SeekForGet(key) [Line 2583]
    │                               └─ For each entry: GetContext::SaveValue()
    │                                   ├─ Parse internal key
    │                                   ├─ Check sequence number visibility
    │                                   └─ Return or accumulate merge operands
    │
    └─ ReturnAndCleanupSuperVersion(sv) [Line 3144]

RETURN: Status and value
```

---

## 11. Key Data Structures

### GetContext
**Purpose**: Central coordinator for search within SST files

**State Machine**:
- `kNotFound`: Key not yet found
- `kMerge`: Merge operand found, need to continue searching
- `kFound`: Key found and value retrieved
- `kDeleted`: Tombstone found, key is deleted
- `kCorrupt`: Corruption detected

**Methods**:
- `SaveValue(ParsedInternalKey, value)`: Process found entry
- `State()`: Get current search state
- `ReportCounters()`: Report statistics

### LookupKey
**Components**:
- User key
- Sequence number (from snapshot)
- Value type (flags)

**Purpose**: Unified lookup key for comparisons across LSM

### FilePicker
**Purpose**: Efficiently iterate through files that might contain key

**Algorithm**:
- Level 0: Check all files (in reverse insertion order)
- Level 1+: Use binary search with FileIndexer optimization
- FileIndexer tracks "fractional cascading" pointers

### SuperVersion
**Components**:
- mem: Read-only view of active memtable
- imm: Immutable memtable list
- current: Current Version of SST files
- version_number: Enables concurrent version management

**Thread Safety**: Reference counted, prevents concurrent deletion

---

## 12. Important Statistics and Metrics

### Memtable Statistics
- `MEMTABLE_HIT`: Key found in memtable
- `MEMTABLE_MISS`: Key not in memtable, searched SSTs
- `bloom_memtable_hit_count`: Memtable bloom filter positive
- `bloom_memtable_miss_count`: Memtable bloom filter negative
- `get_from_memtable_count`: Total memtable get operations

### SST File Statistics
- `GET_HIT_L0`, `GET_HIT_L1`, `GET_HIT_L2_AND_UP`: Level statistics
- `BLOOM_FILTER_USEFUL`: Bloom filter prevented block reading
- `BLOOM_FILTER_FULL_POSITIVE`: Bloom filter didn't eliminate key

### Block Cache Statistics
- `BLOCK_CACHE_HIT_COUNT`: Block found in cache
- `BLOCK_CACHE_MISS_COUNT`: Block not in cache, read from file
- `BLOCK_CACHE_INDEX_HIT_COUNT`: Index block cache hit
- `BLOCK_CACHE_FILTER_HIT_COUNT`: Filter block cache hit

### Performance Metrics
- `get_snapshot_time`: Time to acquire snapshot
- `get_from_memtable_time`: Time spent in memtable lookup
- `get_from_output_files_time`: Time spent in SST file lookup
- `get_post_process_time`: Post-processing and merge time

---

## 13. Important Files and Line References

| Component | File | Key Lines | Function |
|-----------|------|-----------|----------|
| Top-level API | `db/db_impl/db_impl.cc` | 2442, 2464, 2812 | Get(), GetImpl() |
| SuperVersion | `db/column_family.h` | 207-276 | struct SuperVersion |
| SuperVersion Mgmt | `db/db_impl/db_impl.cc` | 5247, 5284 | GetAndRefSuperVersion(), ReturnAndCleanupSuperVersion() |
| Memtable Read | `db/memtable.cc` | 1485, 1565 | MemTable::Get(), GetFromTable() |
| Version Read | `db/version_set.cc` | 2714 | Version::Get() |
| FilePicker | `db/version_set.cc` | 148-350 | class FilePicker |
| TableCache | `db/table_cache.cc` | 502 | TableCache::Get() |
| BlockBasedTable | `table/block_based/block_based_table_reader.cc` | 2489 | BlockBasedTable::Get() |
| Bloom Filter | `table/block_based/block_based_table_reader.cc` | 2325 | FullFilterKeyMayMatch() |
| Index Iterator | `table/block_based/block_based_table_reader.cc` | 1671 | NewIndexIterator() |
| Block Cache | `table/block_based/block_based_table_reader.cc` | 1712 | LookupAndPinBlocksInCache() |

---

## 14. Optimization Techniques in Read Path

### 1. Bloom Filters
- **Memtable Bloom**: Fast rejection without skiplist lookup
- **SST Bloom**: Avoid reading data blocks entirely
- **Whole-key vs Prefix**: Trade-off between precision and false positive rate

### 2. Block Cache
- **Cache Data Blocks**: Avoid repeated decompression
- **Cache Index Blocks**: Keep index in memory
- **Cache Filter Blocks**: Keep bloom filter in memory
- **Priority Tiers**: Index/filter blocks have higher priority

### 3. FilePicker Optimization (Fractional Cascading)
- **Binary search at each level** to find candidate files
- **FileIndexer maintains pointers** to next level's files
- **Avoids redundant binary searches** across levels

### 4. TableCache
- **Caches opened TableReader instances**
- Avoids repeatedly parsing SST file metadata
- Uses LRU eviction

### 5. SuperVersion
- **Point-in-time consistency** without locking entire DB
- **Thread-local storage** for efficiency
- **Reference counting** prevents concurrent deletion issues

### 6. Row Cache (Optional)
- **Caches exact key-value pairs** across SST boundaries
- **Faster than block cache** for frequently accessed keys
- Can reduce both SST reads and decompression

### 7. Prefetching
- **Level 0 prefetching** (Line 171-177 in version_set.cc)
- Prepares table readers to optimize subsequent seeks
- Improves cache locality

---

## 15. Complexity Analysis

| Operation | Time Complexity | Notes |
|-----------|-----------------|-------|
| Memtable lookup | O(log M) | M = memtable size, skiplist with O(log M) levels |
| Level 0 scan | O(L0 files) | All L0 files checked (can overlap) |
| Level 1+ lookup | O(log F) | F = files in level, binary search via FileIndexer |
| Data block search | O(log B) | B = entries in block, binary search or hash index |
| Bloom filter check | O(1) | Single hash probe per filter |
| **Total worst case** | O(L * log F + log B) | L = number of levels, F = files, B = block entries |
| **Typical case** | O(log M + log B) | Usually found in memtable or top SST level |

---

## 16. Concurrency Considerations

### Read-Only Operations
- Multiple readers can proceed simultaneously
- No locking needed beyond SuperVersion reference counting
- Different threads get thread-local SuperVersion instances

### SuperVersion Snapshots
- Each reader captures SuperVersion at read time
- Prevents concurrent modifications during read
- Reference counting ensures proper cleanup

### Block Cache Thread-Safety
- LRU cache implementation handles concurrent access
- Separate locks per bucket for efficiency
- Lookups don't block other cache operations

### Memtable Access
- Memtable skip list uses atomic operations
- Concurrent readers can search without locks
- Writes go to separate write buffer

---

## 17. Conclusion

The RocksDB read path implements a sophisticated multi-level lookup strategy:

1. **Fast Path**: Memtable bloom filter + skiplist (nanoseconds)
2. **Medium Path**: SST bloom filter + block cache (microseconds)
3. **Slow Path**: Block decompression + disk I/O (milliseconds)

Key optimizations:
- **Bloom filters** eliminate unnecessary I/O
- **Block cache** reduces decompression
- **FilePicker** minimizes SST files checked
- **SuperVersion** enables lock-free consistent reads
- **Reference counting** ensures safe concurrent access

This architecture allows RocksDB to handle high-throughput read workloads while maintaining consistency and correctness.
