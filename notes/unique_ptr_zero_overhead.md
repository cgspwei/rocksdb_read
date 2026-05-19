# unique_ptr 零开销原理详解

## 一、三个层面的保证

### 1. 内存布局相同

```cpp
Widget* raw;                        // 8 字节
std::unique_ptr<Widget> up;         // 也是 8 字节
```

`unique_ptr` 内部只存一个裸指针，没有引用计数、没有控制块。默认删除器是空类，通过继承（而非成员变量）存储，EBO 让它占 0 字节，不膨胀 `unique_ptr` 的大小。

### 2. 所有权约束在编译期完成

```cpp
unique_ptr<Widget> a = make_unique<Widget>();
unique_ptr<Widget> b = a;        // 编译错误，不是运行时异常
unique_ptr<Widget> b = move(a);  // OK，a 变 nullptr
```

通过删除拷贝构造函数实现。违规在编译期就报错，**不插入任何运行时检查代码**。

`shared_ptr` 相反——它必须在运行时维护引用计数，才能知道什么时候该释放。

### 3. 生成的机器码完全等价

```cpp
// 裸指针
Widget* p = new Widget;
p->doWork();
delete p;

// unique_ptr
auto p = make_unique<Widget>();
p->doWork();
// 析构自动 delete
```

两者在 `-O2` 下生成**完全相同的汇编**：

```asm
call    Widget::doWork()
mov     rdi, rbx
call    operator delete(void*)   ; 完全一样
```

`unique_ptr` 的析构函数内联后就是 `delete ptr_`，解引用 `operator->()` 内联后就是一次内存间接寻址——编译器优化后没有任何额外指令。

---

## 二、unique_ptr 内部结构详解

### 第一步：最朴素的实现

先看最直觉的实现方式：

```cpp
template <typename T, typename D = default_delete<T>>
class unique_ptr {
    T* ptr_;      // 指向被管理的对象
    D deleter_;   // 删除器
};
```

这里有一个问题。`default_delete<T>` 是无状态的空类：

```cpp
template <typename T>
struct default_delete {
    void operator()(T* p) { delete p; }
    // 没有任何数据成员，是空类
};
```

它没有任何数据需要存储，理论上不应该占空间。但 C++ 有一个规定：**每个对象必须有唯一的地址**，所以即使是空类，`sizeof` 也至少是 1：

```cpp
static_assert(sizeof(default_delete<Widget>) == 1);  // 不是 0
```

于是问题来了：

```
朴素实现的内存布局：
┌──────────────────────┬───┬───────────────────────┐
│     ptr_  (8字节)    │ D │  padding (7字节补对齐) │
└──────────────────────┴───┴───────────────────────┘
总计：16 字节，是裸指针的两倍！不是零开销。
```

### 第二步：EBO 是什么

EBO（Empty Base Optimization，空基类优化）是 C++ 标准允许的一个特殊规则：

> **空类作为基类时，允许不占用任何空间。**

```cpp
struct Empty {
    void foo() {}
    // 无数据成员
};

// 作为成员变量：至少 1 字节
struct AsMember {
    int x;
    Empty e;   // 占 1 字节，加上补齐共 4 字节
};
static_assert(sizeof(AsMember) == 8);  // int(4) + Empty(1) + padding(3)

// 作为基类：0 字节
struct AsBase : Empty {
    int x;
};
static_assert(sizeof(AsBase) == 4);    // 只有 int(4)，Empty 不占空间
```

为什么作为基类可以 0 字节？因为基类和派生类对象的地址可以相同——它们不是两个独立的对象，共享同一个地址也不违反"每个对象有唯一地址"的规则。

### 第三步：compressed_pair

`unique_ptr` 的实现利用 EBO，把删除器从成员变量改为基类：

```cpp
// 原理示意（简化）：
// 如果 D 是空类，就继承它；否则作为成员
template <typename T1, typename T2, bool D2IsEmpty = std::is_empty_v<T2>>
struct compressed_pair;

// D 是空类 → 继承，EBO 生效
template <typename T1, typename T2>
struct compressed_pair<T1, T2, true> : private T2 {
    T1 first;
    T2& second() { return *this; }  // this 就是 T2 基类
};

// D 不是空类 → 普通成员存储
template <typename T1, typename T2>
struct compressed_pair<T1, T2, false> {
    T1 first;
    T2 second;
};
```

`unique_ptr` 用 `compressed_pair` 同时存储指针和删除器：

```cpp
template <typename T, typename D = default_delete<T>>
class unique_ptr {
    compressed_pair<T*, D> data_;
    //              ↑     ↑
    //           指针   删除器
};
```

### 第四步：两种情况的内存布局对比

**情况一：默认删除器（无状态空类）**

```
D = default_delete<Widget>，is_empty = true
compressed_pair 继承 D，EBO 生效

内存布局：
┌──────────────────────┐
│     ptr_  (8字节)    │   ← 就是裸指针
└──────────────────────┘
D 作为基类，占 0 字节，sizeof(unique_ptr) == 8 ✓
```

**情况二：有状态删除器（如函数指针）**

```cpp
auto up = unique_ptr<Widget, void(*)(Widget*)>(p, &custom_delete);
```

```
D = void(*)(Widget*)，is_empty = false
compressed_pair 把 D 作为普通成员存储

内存布局：
┌──────────────────────┬──────────────────────┐
│     ptr_  (8字节)    │  deleter_  (8字节)   │
└──────────────────────┴──────────────────────┘
sizeof(unique_ptr) == 16，多了一个函数指针
```

### 一张图总结

```
default_delete（无状态）:
  unique_ptr
  ├─ [继承 default_delete] —— 0 字节（EBO）
  └─ ptr_                  —— 8 字节
  总计：8 字节 == sizeof(Widget*)  ✓ 零开销

有状态删除器:
  unique_ptr
  ├─ ptr_      —— 8 字节
  └─ deleter_  —— 8 字节（函数指针）
  总计：16 字节 > sizeof(Widget*)  ✗ 有额外开销
```

### 关键结论

"默认删除器通过继承存储，EBO 让它占 0 字节"的完整逻辑链：

1. `default_delete<T>` 是空类，无数据成员
2. 空类作为**成员变量**至少占 1 字节（加上对齐可能更多）
3. 空类作为**基类**可以占 0 字节（EBO）
4. `compressed_pair` 在编译期检测删除器是否为空类，是则用继承，否则用成员
5. 结果：默认情况下 `unique_ptr` 只有一个裸指针，`sizeof == 8`，与裸指针完全一致
