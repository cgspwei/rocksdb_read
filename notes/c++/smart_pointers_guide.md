# C++ 智能指针使用指南

> 本文整理 C++ 智能指针的分类、选型原则和使用惯例，作为阅读 `smart_pointers_in_rocksdb.md` 的基础知识储备。

---

## 一、智能指针的分类

C++ 标准库提供三种智能指针（C++11 引入）：

| 类型 | 所有权 | 引用计数 | 典型用途 |
|------|--------|----------|----------|
| `std::unique_ptr` | 独占 | 无 | 默认首选，替代拥有所有权的裸指针 |
| `std::shared_ptr` | 共享 | 有（原子操作） | 需要多个 owner 的场景 |
| `std::weak_ptr` | 无（弱引用） | 不增加计数 | 打破循环引用、观察 shared_ptr |

另外有一个 `std::auto_ptr`（C++98），语义混乱（拷贝即转移所有权），已在 **C++17 中正式移除**，现代代码不应使用。

---

## 二、用 unique_ptr 替代裸指针

### 2.1 核心原则

现代 C++ 的核心原则：**用 `unique_ptr` 替代所有拥有所有权的裸指针**。

```
拥有所有权 → unique_ptr
不拥有所有权 → 裸指针或引用
```

### 2.2 拥有所有权时 — 必须替换

```cpp
// C++03 风格：裸指针持有资源，需要手写 delete
MyClass* obj = new MyClass();
// ... 如果在中间抛异常，obj 就泄漏了
delete obj;

// 现代 C++：unique_ptr 自动释放
auto obj = std::make_unique<MyClass>();
// 离开作用域自动析构，抛异常也安全
```

### 2.3 不拥有所有权时 — 保持裸指针

```cpp
// 这只是"观察"别人创建的对象，不负责释放
void process(const MyClass* obj) {
    // 读或写 obj，但不 delete 它
}
```

**为什么这里用 `unique_ptr` 反而不对？**

因为 `unique_ptr` 作为值传递参数表达的是**转移所有权**的语义：

```cpp
void process(std::unique_ptr<MyClass> obj) {
    // obj 离开作用域时析构
}

auto obj = std::make_unique<MyClass>();
process(std::move(obj));   // 调用者必须 move，交出所有权
// 此后 obj 是 nullptr，不能再用了！
obj->doSomething();        // 空指针，崩溃
```

对比正确写法：

```cpp
void process(const MyClass* obj) {   // 裸指针：我只是借来看
    // 不负责释放
}

auto obj = std::make_unique<MyClass>();
process(obj.get());         // 传裸指针，所有权还在我这里
obj->doSomething();         // 没问题
```

三种签名表达了完全不同的语义：

| 签名 | 语义 | 调用后 |
|------|------|--------|
| `void f(unique_ptr<T> obj)` | **我接管了，用完就销毁** | 调用者失去所有权 |
| `void f(const T* obj)` | **我就看看，不负责** | 调用者继续持有 |
| `void f(T& obj)` | **我借用一下** | 调用者继续持有 |

### 2.4 函数参数什么时候用 unique_ptr

当函数需要**接管对象的所有权**时，参数才用 `unique_ptr`。具体三种场景：

**场景 1：工厂/构造器的依赖注入**

```cpp
// 构造器：我给你一个线程，你负责管理它
class RecoveryHandler {
 public:
  explicit RecoveryHandler(std::unique_ptr<port::Thread> thread)
      : recovery_thread_(std::move(thread)) {}
 private:
  std::unique_ptr<port::Thread> recovery_thread_;
};

// 调用方
auto handler = RecoveryHandler(std::make_unique<port::Thread>(...));
```

函数说："把这个给我，我存起来当成员，以后我来管"。

**场景 2：容器类接管一批对象**

```cpp
// 给你一批 Iterator，我接管所有权，用完后统一销毁
virtual Status CommitFileIngestionHandles(
    std::vector<std::unique_ptr<FileIngestionHandle>> handles) = 0;

// 调用方
std::vector<std::unique_ptr<FileIngestionHandle>> handles;
handles.push_back(std::make_unique<FileIngestionHandle>(...));
db->CommitFileIngestionHandles(std::move(handles));
// 此后 handles 被清空了
```

**场景 3：Sink 函数（最终消费）**

```cpp
// 我把 task 给你，你怎么执行、什么时候销毁，我不管了
void SubmitJob(std::function<void()>&& func) {
    auto task = std::make_unique<Job>(std::move(func));
    job_queue_.push(std::move(task));  // 交给队列管理
}
```

**判断规则**：

```
参数用 unique_ptr 的前提：函数内部会接管所有权
  ├─ 存为成员 → 场景1
  ├─ 存进容器 → 场景2
  └─ 消费后不管 → 场景3

否则 → 用裸指针/引用
```

简单记法：**函数参数出现 `unique_ptr` = 函数在说"给我，你的就是我的了"**。如果函数只是借来看看，永远不要用 `unique_ptr` 做参数。

### 2.5 推荐的使用模式

```
创建对象      → make_unique（永远不要写 new）
传递所有权    → unique_ptr 或 std::move(unique_ptr)
借阅/观察     → 裸指针 或 引用
```

---

## 三、为什么推荐 unique_ptr

### 3.1 异常安全

```cpp
void bad() {
    auto* a = new ResourceA();  // 成功
    auto* b = new ResourceB();  // 如果这里抛异常，a 泄漏了
    delete b;
    delete a;
}

void good() {
    auto a = std::make_unique<ResourceA>();  // 成功
    auto b = std::make_unique<ResourceB>();  // 如果抛异常，a 自动释放
    // 两个都在作用域结束时自动析构
}
```

### 3.2 自文档化

看类型就知道所有权的归属，不需要翻文档或注释：

```cpp
void foo(std::unique_ptr<Widget> w);   // foo 接管 Widget 的所有权
void bar(const Widget* w);             // bar 只是借用，不管生命周期
```

### 3.3 零开销

`unique_ptr` 的大小就是一个普通指针（通常 8 字节），没有引用计数的原子操作开销。默认删除器的调用通过编译器优化后与手写 `delete` 性能相同。

### 3.4 make_unique 比 new 更安全

```cpp
// 潜在问题：new 成功后 Widget 构造函数抛异常，内存泄漏
auto p = std::unique_ptr<Widget>(new Widget(args));

// make_unique 保证：分配和构造是一体的，不会泄漏
auto p = std::make_unique<Widget>(args);
```

C++ 规范保证 `make_unique` 的分配和构造之间没有插入点，即使构造抛异常也能正确释放已分配内存。

---

## 四、三种智能指针的对比

| 特性 | unique_ptr | shared_ptr | weak_ptr |
|------|-----------|------------|----------|
| 拷贝 | 不可拷贝 | 引用计数+1 | 不影响计数 |
| 移动 | 可以 | 可以 | 可以 |
| 大小 | 1个指针 | 2个指针（对象+控制块） | 2个指针 |
| 开销 | 零开销 | 原子引用计数 | 无额外开销 |
| 使用场景 | 默认首选 | 多个owner | 打破循环 |
| 数组支持 | `unique_ptr<T[]>` | C++17起支持 | 不支持 |

---

## 五、何时用哪种

```
需要管理对象的生命周期？
├─ 否 → 裸指针（非拥有观察）
└─ 是 → 所有权是否独占？
    ├─ 是 → unique_ptr（默认首选）
    └─ 否 → shared_ptr
             └─ 有循环引用风险 → 一侧改用 weak_ptr
```

**经验法则**：
- 先写 `unique_ptr`，发现不够用再换成 `shared_ptr`
- 如果代码中出现 `shared_ptr` 比 `unique_ptr` 多，通常说明所有权设计有问题
- `weak_ptr` 是最后的选择，大部分场景用不上
