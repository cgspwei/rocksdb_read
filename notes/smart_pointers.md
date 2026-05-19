# C++ 智能指针深度解析

## 1. unique_ptr：独占所有权的零开销抽象

### 1.1 核心设计

`unique_ptr` 的设计目标是**零开销**——在编译后的代码中，它与裸指针没有任何性能差异。其所有权语义由编译器在编译期强制执行，而非运行时。

```cpp
// 内存布局等价
Widget* raw = new Widget;          // 裸指针：8 字节
std::unique_ptr<Widget> up(new Widget);  // 也是 8 字节，无额外开销
```

**关键机制**：删除了拷贝构造和拷贝赋值，仅允许移动语义。

```cpp
std::unique_ptr<Widget> a = std::make_unique<Widget>();
// std::unique_ptr<Widget> b = a;              // 编译错误：拷贝被删除
std::unique_ptr<Widget> b = std::move(a);      // OK：所有权转移，a 变为 nullptr
```

### 1.2 自定义删除器

`unique_ptr` 的删除器是模板参数的一部分，在编译期绑定，无运行时开销。

```cpp
// 用 lambda 作删除器 —— 注意模板参数要写全
auto deleter = [](FILE* f) { if (f) fclose(f); };
std::unique_ptr<FILE, decltype(deleter)> file(fopen("data.txt", "r"), deleter);
```

**与 shared_ptr 的关键区别**：`unique_ptr` 的删除器类型是模板参数，直接影响类型；`shared_ptr` 的删除器类型在构造时擦除，不影响类型。这意味着：

```cpp
// unique_ptr：删除器不同 = 类型不同
std::unique_ptr<Widget> p1;
std::unique_ptr<Widget, void(*)(Widget*)> p2;  // 与 p1 类型不同

// shared_ptr：删除器不同，类型仍相同
std::shared_ptr<Widget> s1;
std::shared_ptr<Widget> s2(p, [](Widget* w) { custom_free(w); });  // 与 s1 类型相同
```

**代价**：`unique_ptr` 带自定义删除器时，对象大小 = 指针大小 + 删除器大小。如果删除器是无状态的（如无捕获 lambda），空基类优化（EBO）可以将其压缩到 0 字节；如果删除器有状态（如函数指针），则增加一个指针大小。

```cpp
// 无捕获 lambda：EBO 生效，sizeof = 8（仅裸指针）
auto dl = [](Widget* p) { delete p; };
std::unique_ptr<Widget, decltype(dl)> p1(nullptr, dl);
static_assert(sizeof(p1) == sizeof(Widget*));

// 函数指针：EBO 不生效，sizeof = 16（指针 + 函数指针）
std::unique_ptr<Widget, void(*)(Widget*)> p2(nullptr, &custom_delete);
static_assert(sizeof(p2) == 2 * sizeof(Widget*));
```

### 1.3 常见陷阱

#### 陷阱 1：误用 release() 忘记接管

```cpp
// 错误：release() 放弃所有权但不释放内存，造成泄漏
std::unique_ptr<Widget> p = std::make_unique<Widget>();
p.release();  // 内存泄漏！裸指针没人释放

// 正确：要么让另一个智能指针接管
auto raw = p.release();
std::unique_ptr<Widget> q(raw);  // 所有权转移给 q

// 正确：要么显式 delete（不推荐，但有时必要）
delete p.release();
```

#### 陷阱 2：reset() 与 raw pointer 混用导致 double free

```cpp
Widget* raw = new Widget;
std::unique_ptr<Widget> p(raw);
std::unique_ptr<Widget> q(raw);  // 错误！两个 unique_ptr 管理同一裸指针
// 离开作用域时 double free
```

#### 陷阱 3：在容器中存储 unique_ptr 后移动失败

```cpp
std::vector<std::unique_ptr<Widget>> vec;
vec.push_back(std::make_unique<Widget>());   // OK，右值移动
auto w = std::make_unique<Widget>();
vec.push_back(w);                             // 编译错误：试图拷贝
vec.push_back(std::move(w));                  // OK，显式移动
```

---

## 2. shared_ptr：共享所有权的引用计数指针

### 2.1 内存布局与控制块

`shared_ptr` 不是零开销的。每个 `shared_ptr` 内部有两个指针：

```
shared_ptr 内部布局：
┌──────────────┐
│ ptr_         │ ──→ 实际对象 (Widget)
│ control_block│ ──→ 控制块 (堆上分配)
└──────────────┘

控制块布局：
┌──────────────────────┐
│ strong_count (long)  │  强引用计数
│ weak_count   (long)  │  弱引用计数
│ allocator (可选)     │
│ deleter   (可选)     │
│ [被管理对象]         │  ← 仅 make_shared 时内嵌于此
└──────────────────────┘
```

**一次 `make_shared` = 一次堆分配**（对象和控制块合并），但一次 `shared_ptr<T>(new T)` = **两次堆分配**（对象和控制块分开）。

```cpp
auto sp1 = std::make_shared<Widget>();          // 1 次分配：对象 + 控制块合并
auto sp2 = std::shared_ptr<Widget>(new Widget); // 2 次分配：对象和控制块分开
```

### 2.2 性能开销详细分析

#### 引用计数的原子操作

每次拷贝、移动、销毁 `shared_ptr` 都要修改引用计数。引用计数使用**原子操作**实现线程安全：

```cpp
// 拷贝构造：strong_count.fetch_add(1, std::memory_order_relaxed)
auto sp2 = sp1;

// 析构：if (strong_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
//         destroy_object();
```

**开销量化**：

| 操作 | 开销 |
|------|------|
| 拷贝构造 | 1 次原子 increment（约 10-30 个 CPU 周期 on x86） |
| 析构 | 1 次原子 decrement + 条件分支（可能触发对象析构和控制块释放） |
| 移动构造 | 仅指针拷贝，不碰引用计数（约 2-3 个 CPU 周期） |
| 解引用 `*sp` | 与裸指针完全相同（1 次内存间接寻址） |

**对比**：裸指针拷贝约 1 个周期，`shared_ptr` 拷贝约 10-30 个周期。在**极高频率**的引用计数增减场景中（如每秒百万次），这个差距有影响。但在大多数实际场景中，I/O 和业务逻辑远比原子操作慢。

#### 缓存局部性

`shared_ptr` 的两次间接寻址（指针→控制块→对象）可能导致缓存未命中。`make_shared` 通过合并分配缓解此问题——对象紧挨着控制块，访问计数后访问对象的缓存命中率更高。

```cpp
// make_shared：对象和控制块在同一缓存行附近
auto sp = std::make_shared<Widget>();  // 好：缓存友好

// 分开分配：对象和控制块可能在内存上相距很远
auto sp = std::shared_ptr<Widget>(new Widget);  // 差：可能缓存未命中
```

#### `std::atomic<std::shared_ptr>`（C++20）

C++20 提供了 `std::atomic<std::shared_ptr>`，但底层用全局 spinlock 或类似机制，性能远不如直接用 `std::atomic<T*>`。对于高并发读写同一 `shared_ptr` 的场景，考虑：

```cpp
// 低并发：C++20 atomic<shared_ptr>
std::atomic<std::shared_ptr<Widget>> atomic_sp;

// 高并发：用读写锁 + shared_ptr
std::shared_ptr<Widget> sp;
std::shared_mutex mtx;
auto read = [&] { return std::shared_lock(mtx), sp; };  // 读：共享锁
auto write = [&](auto new_sp) { std::unique_lock(mtx), sp = std::move(new_sp); };  // 写：独占锁

// 极致性能：用裸指针 + 手动生命周期管理（如 RCU 模式）
```

### 2.3 常见陷阱

#### 陷阱 1：循环引用导致内存泄漏

这是 `shared_ptr` 最经典的陷阱：

```cpp
struct Node {
    std::shared_ptr<Node> next;
    std::shared_ptr<Node> prev;  // 应该用 weak_ptr！
};

auto a = std::make_shared<Node>();
auto b = std::make_shared<Node>();
a->next = b;
b->prev = a;  // 循环引用！a 和 b 的引用计数永远不会降到 0
// 泄漏！
```

**解决方案**：将其中一方改为 `weak_ptr`，见第 3 节。

#### 陷阱 2：从 this 指针创建 shared_ptr（双重控制块）

```cpp
class Processor {
public:
    void start() {
        // 错误！创建了一个全新的控制块，引用计数从 1 开始
        // 与外部持有的 shared_ptr 的控制块完全无关
        std::shared_ptr<Processor> sp(this);  // 双重控制块 → double free
        callback_registry_.register(sp);
    }
};

// 正确做法：继承 enable_shared_from_this
class Processor : public std::enable_shared_from_this<Processor> {
public:
    void start() {
        auto sp = shared_from_this();  // 使用同一控制块
        callback_registry_.register(sp);
    }
};

auto p = std::make_shared<Processor>();  // 必须通过 make_shared 创建
p->start();  // OK
```

**注意**：调用 `shared_from_this()` 的前提是对象已经被 `shared_ptr` 管理。在构造函数中调用是未定义行为（此时对象尚未被 `shared_ptr` 接管）。

#### 陷阱 3：误解线程安全范围

```cpp
// shared_ptr 的线程安全是"控制块级别"的，不是"对象级别"的
auto sp = std::make_shared<Widget>();

// 线程 A                    // 线程 B
sp2 = sp;                   sp3 = sp;
// OK：引用计数的增减是原子的

sp2 = sp;                   *sp = other_value;
// 引用计数安全，但 Widget 对象的并发读写不是安全的
// 需要外部同步来保护 Widget 本身
```

**规则**：
- **安全**：多个线程同时拷贝/销毁同一个 `shared_ptr`（引用计数原子操作）
- **安全**：多个线程各自读不同的 `shared_ptr` 副本（即使指向同一对象）
- **不安全**：多个线程对同一个 `shared_ptr` 实例赋值（修改指针本身需同步）
- **不安全**：多个线程通过 `shared_ptr` 并发读写所指向的对象（对象本身需同步）

#### 陷阱 4：临时 shared_ptr 悬空

```cpp
// 错误：临时 shared_ptr 在语句结束后析构，裸指针悬空
Widget* raw = std::make_shared<Widget>().get();
// shared_ptr 已经析构，raw 是悬空指针

// 常见于函数参数：
void process(Widget* w);
process(std::make_shared<Widget>().get());  // 危险：取决于 process 是否保存指针
```

#### 陷阱 5：数组与 shared_ptr

```cpp
// C++17 之前：shared_ptr 不支持 T[]
std::shared_ptr<int[]> arr(new int[10]);  // C++17 才 OK

// C++17 之前需要自定义删除器
std::shared_ptr<int> arr(new int[10], std::default_delete<int[]>());

// 推荐：用 vector 或 unique_ptr<T[]>
auto arr = std::make_unique<int[]>(10);  // OK
auto vec = std::make_shared<std::vector<int>>(10);  // 更好
```

---

## 3. weak_ptr：打破循环引用的观察者

### 3.1 核心机制

`weak_ptr` 是 `shared_ptr` 的"观察者"——它可以从 `shared_ptr` 创建，但不增加强引用计数，不影响对象生命周期。要访问对象，必须先调用 `lock()` 提升为 `shared_ptr`。

```cpp
auto sp = std::make_shared<Widget>(42);
std::weak_ptr<Widget> wp = sp;

// 访问对象：必须 lock()
if (auto locked = wp.lock()) {
    // locked 是 shared_ptr，对象一定还活着
    locked->doWork();
} else {
    // 对象已经被释放
}
```

**`lock()` 的原子性**：`lock()` 等价于 `shared_ptr<T>(wp)`，它原子地检查强引用计数并递增——如果对象还活着则成功，否则返回空的 `shared_ptr`。不存在 lock 成功但对象在返回前被释放的竞态。

### 3.2 打破循环引用的模式

#### 模式 1：双向链表

```cpp
struct Node {
    std::shared_ptr<Node> next;
    std::weak_ptr<Node>   prev;  // 反向引用用 weak_ptr
};

auto a = std::make_shared<Node>();
auto b = std::make_shared<Node>();
a->next = b;
b->prev = a;  // weak_ptr 不增加强引用计数
// a 离开作用域时，b->prev.lock() 返回 nullptr，a 正常析构
```

#### 模式 2：观察者/回调注册

```cpp
class EventManager {
    std::vector<std::weak_ptr<EventListener>> listeners_;
public:
    void register_listener(std::weak_ptr<EventListener> listener) {
        listeners_.push_back(listener);
    }
    void notify() {
        // 先清理已销毁的监听器，再通知存活的
        std::erase_if(listeners_, [](const auto& wp) { return wp.expired(); });
        for (auto& wp : listeners_) {
            if (auto sp = wp.lock()) {
                sp->on_event();
            }
        }
    }
};
```

#### 模式 3：缓存

```cpp
class Cache {
    std::unordered_map<Key, std::weak_ptr<Value>> cache_;
public:
    std::shared_ptr<Value> get(Key key) {
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            if (auto sp = it->second.lock()) {
                return sp;  // 缓存命中
            }
            cache_.erase(it);  // 对象已过期，清理
        }
        auto sp = std::make_shared<Value>(load_from_disk(key));
        cache_[key] = sp;
        return sp;
    }
};
```

### 3.3 weak_ptr 的开销

`weak_ptr` 增加的是**弱引用计数**（`weak_count`），不是强引用计数。弱引用计数的存在是为了知道何时可以释放控制块本身：

```
控制块释放条件：
  strong_count == 0  →  析构被管理对象
  strong_count == 0 && weak_count == 0  →  释放控制块
```

所以 `weak_ptr` 本身也有开销——它阻止控制块的释放。每个 `weak_ptr` 占两个指针（与 `shared_ptr` 相同），且拷贝/析构时修改弱引用计数（原子操作）。

### 3.4 weak_ptr 常见误用

#### 误用 1：lock() 后不检查

```cpp
auto sp = wp.lock();
sp->doWork();  // 如果 wp 已过期，sp 是 nullptr，这里崩溃
```

#### 误用 2：先 expired() 再 lock()（TOCTOU 竞态）

```cpp
if (!wp.expired()) {
    // 危险：wp 可能在 expired() 和 lock() 之间变为过期
    auto sp = wp.lock();
    sp->doWork();  // 可能崩溃
}

// 正确：直接 lock() 并检查
if (auto sp = wp.lock()) {
    sp->doWork();  // 安全
}
```

---

## 4. 选用决策指南

```
需要管理堆对象的生命周期吗？
├─ 否 → 用裸指针或引用（非拥有）
└─ 是 → 所有权是否独占？
    ├─ 是 → unique_ptr（默认首选）
    └─ 否 → 需要共享所有权吗？
        ├─ 是 → shared_ptr
        │       └─ 有循环引用？→ 用 weak_ptr 打断一侧
        └─ 否 → 考虑裸指针 + 外部生命周期管理（如 arena、对象池）
```

**核心原则**：
- **`unique_ptr` 为默认选择**：零开销，语义最清晰
- **`shared_ptr` 仅在真正需要共享所有权时使用**：有运行时开销，引入循环引用风险
- **`weak_ptr` 仅用于观察 `shared_ptr` 管理的对象**：不独立使用
- **裸指针/引用用于非拥有观察**：不参与生命周期管理

---

## 5. 在 RocksDB 中的应用模式

RocksDB 中 `shared_ptr` 和 `unique_ptr` 都大量使用，但有明确的分工：

### unique_ptr：独占资源

```cpp
// 文件读写器：独占所有权
std::unique_ptr<RandomAccessFileReader> file_reader_;

// Table Builder：独占构建过程
std::unique_ptr<TableBuilder> table_builder_;

// Compaction Job：独占运行过程
std::unique_ptr<CompactionJob> compaction_job_;
```

### shared_ptr：共享/缓存资源

```cpp
// Block Cache：缓存的数据块被多个读者共享
std::shared_ptr<TableReader> table_reader_;

// Env/FileSystem：全局共享
std::shared_ptr<Env> env_;
std::shared_ptr<FileSystem> fs_;

// Statistics：多个组件共享同一统计实例
std::shared_ptr<Statistics> stats_;
```

### 裸指针：非拥有观察（最常见）

```cpp
// DBImpl 内部：大量裸指针指向由 arena 或其他机制管理的对象
ColumnFamilyData* cfd_;  // 由 ColumnFamilySet 管理
Version* current_;        // 由 VersionSet 管理
SuperVersion* sv_;        // 由 SuperVersionCache 管理
```

RocksDB 在性能关键路径（cache 内部、memtable、compaction 循环）偏好裸指针 + 外部生命周期管理（arena、版本机制），避免 `shared_ptr` 的原子操作开销。
