# 内存池和内存槽的关系

## 图示

```
内存池 MemoryPool
│
├── 内存块1 (BlockSize_大小，如4KB)
│   ├── 槽1 (SlotSize_大小，如8字节)
│   ├── 槽2
│   ├── 槽3
│   └── ...
│
├── 内存块2
│   ├── 槽N+1
│   ├── 槽N+2
│   └── ...
│
└── ...
```

## 内存池和内存槽的关系

内存池（MemoryPool）和内存槽（Slot）的关系类似“仓库”和“槽位”之间的关系，是一种包含与被包含的关系。

内存池包含内存槽。

## 层级结构

**1、内存池(MemoryPool)**

- 是整个内存管理系统的核心组件
- 向系统申请大块的内存并管理
- 每个内存池实例管理固定大小的内存槽（8字节、16字节等）

**2、内存块(Block)**

- 内存池内部批量申请的连续内存区域（由`Blocksize_`**决定大小**）
- 一个内存池可以包含多个内存块（通过`firstBlock_`链接）

**3、内存槽(Slot)**

- 内存池中最小的分配单位
- 每个内存块被划分为多个大小相同的槽
- 槽的大小由`SlotSize_`决定（8的倍数）

## 工作关系

### 1、内存池管理槽位

- `curSlot_` 指向当前未使用的槽
- `lastSlot_` 指向块末尾的边界
- `freeSlot_` 链接所有已经释放的槽

### 2、内存分配流程

```
请求内存 -> 内存池返回一个空闲槽 -> 用户使用该空闲槽
```

### 3、内存回收流程

```
释放内存 -> 将槽放回内存池的空闲列表 -> 槽可以在之后被分配
```

---

# MemoryPool 与HashBucket的关系

## 层次结构关系

**MemoryPool(底层）**

- **角色：**执行者，管理**内存单元**
- **管理**固定大小的内存块（`BlockSize_`）
- 划分内存块为**特定大小**的槽位(`SlotSize_`)
- 处理内存的实际分配（`allocate`）和回收（`deallocate`）
- **维护**空闲内存链表(`freeList_`)
- 每个`MemoryPool`实例只管理固定大小的内存槽

**HashBucket（上层）**

- **角色：**协调者，内存池**管理器**
- **管理**多个**不同大小**的`MemoryPool`实例
- 提供**统一**的内存分配接口
- 根据请求大小**选择**合适的内存池
- **处理**超大内存请求（使用系统分配）
- **特点：**静态类，提供**全局访问**点

总体来说，`HashBucket`负责根据对象的大小选择合适的内存池，而内存池中具体如何分配内存由`MemoryPool`决定

## 内存池的具体结构

**HashBucket管理的是不同规格的内存池**

- 总共有`MEMORY_POOL_NUM`个不同的内存池
- 每个内存池负责一种特定大小的内存分配

**单独MemoryPool的内部结构**

- **相同的槽大小：**每个内存池内部的所有槽位大小（`SlotSize_`）相同
- **内存块大小：**默认每个内存池的块大小（`BlockSize_`）都是4096字节，在创建时可以自定义

**具体的内存大小分布**

```
第0个内存池: SlotSize_ = 8字节
第1个内存池: SlotSize_ = 16字节
第2个内存池: SlotSize_ = 24字节
...
第63个内存池: SlotSize_ = 512字节
```



**内存池的内部视图**

```
内存块(4096字节)
+----------------------------------------------------+
| Slot(24字节) | Slot(24字节) | ... | Slot(24字节) |
+----------------------------------------------------+
     ^第一个槽      ^第二个槽            ^最后一个槽
```



---

# Slot结构体详解

## 1、基本结构与设计意图

```C++
struct Slot
{
  std::atomic<Slot*>next;  
};
```

## 2、内存复用机制

**使用了内存复用的技术**

- 当这个槽位**空闲**的时候，`next`指针指向下一个空闲槽，`Slot`发挥作为链表节点的作用
- 当这个槽位**被分配出去**的时候，`next`指针的内存空间作为用户数据的一部分，不再作为指针，分配的空间全部可以用来存储用户的数据,`Slot`仅仅作为返回给用户的数据块

这种复用技术避免了额外的内存开销，槽位不需要额外的管理结构，**槽的实际大小由`Slotsize_`决定，而不是`sizeof(Slot)`**

每次根据**用户给出的数据类型计算大小**并分配内存槽

## 3、原子类型`next`指针

### 1、多线程并发保护

**多个线程**可能**同时**调用`allocate()`和`deallocate()`函数，这意味着可能有**多个线程**同时在操作空闲链表，如果用普通指针的话，当多个线程同时修改和读取这个指针的时候会导致**数据竞争**，这在C++中是**未定义**的行为，可能会导致：

- **内存破坏**
- **指针丢失**
- **程序崩溃**

### 2、无锁队列实现

如`pushFreeList`

```C++
bool MemoryPool::pushFreeList(Slot* slot)
{
    while (true)
    {
        // 获取当前头节点
        Slot* oldHead = freeList_.load(std::memory_order_relaxed);
        // 将新节点的 next 指向当前头节点
        slot->next.store(oldHead, std::memory_order_relaxed);

        // 尝试将新节点设置为头节点
        if (freeList_.compare_exchange_weak(oldHead, slot,
         std::memory_order_release, std::memory_order_relaxed))
        {
            return true;
        }
        // 失败：说明另一个线程可能已经修改了 freeList_
        // CAS 失败则重试
    }
}
```

#### **CAS(Compare-And-Swap)操作详解**

- CAS是一种原子操作
- 实现无锁并发算法
- 能够在不使用互斥锁的情况下安全地修改共享数据

**基本原理**

**CAS操作接受三个参数：**

1. **内存位置** (要操作的变量)
2. **期望值** (预期该变量当前的值)
3. **新值** (想要设置的值)

**工作流程：**

```
如果(内存位置的当前值 == 期望值)
    将内存位置的值修改为新值
    返回成功
否则
    不做任何修改
    返回失败
```

**内存池代码中的CAS操作**

```C++
if (freeList_.compare_exchange_weak(oldHead, slot,
 std::memory_order_release, std::memory_order_relaxed))
{
    return true;
}
```

- 检查`freeList_`当前值是否等于`oldHead`
- 如果相等，将`freeList_`设置为`slot`并返回`true`
- 否则将`oldHead`更新为`freeList_`的实际值并返回`false`

**操作实际步骤**

1. 读取当前链表头：`oldHead = freeList_.load(...)`
2. 准备新节点：`slot->next.store(oldHead, ...)`
3. 尝试原子更新链表头：`freeList_.compare_exchange_weak(oldHead, slot, ...)`
4. 如果其他线程在步骤1和步骤3之间修改了链表头，CAS会失败，循环重试

#### CAS操作的优势

1. **避免锁开销**：不需要互斥锁的获取/释放操作
2. **减少阻塞**：失败的线程不会被挂起，而是立即重试
3. **提高并发性**：允许多个线程同时尝试操作
4. **避免死锁**：没有锁，就不会有死锁问题

#### CAS操作的挑战

1. **ABA问题**：值从A变为B再变回A，CAS误认为没变化
2. **自旋开销**：高争用场景下可能导致CPU资源浪费
3. **单变量限制**：只能原子地更新单个内存位置

CAS实现的无锁并发，可以显著提高多线程环境下的性能

### 3、内存序详解

```C++
if (freeList_.compare_exchange_weak(oldHead, slot,
 std::memory_order_release, std::memory_order_relaxed))
{
    return true;
}
```

在以上代码中用到了`compare_exchange_weak`函数，以下是函数签名

```C++
bool compare_exchange_weak(T& expected, T desired,
                          memory_order success,
                          memory_order failure);
```

**参数含义：**

1. `expected` - 期望的当前值
2. `desired` - 如果比较成功要设置的新值
3. `success` - **操作成功时**使用的内存序
4. `failure` - **操作失败时**使用的内存序

#### 不同内存序的对比

从弱到强排序：

| 内存序  | 原子性 | 重排序限制       | 可见性保证 | 性能影响 |
| ------- | ------ | ---------------- | ---------- | -------- |
| relaxed | 有     | 最少             | 最少       | 最快     |
| acquire | 有     | 禁止后续读写上移 | 读取同步   | 中等     |
| release | 有     | 禁止前序读写下移 | 写入同步   | 中等     |
| seq_cst | 有     | 全局顺序         | 全部同步   | 最慢     |

#### 内存序选择的原因

1、**成功路径**

当CAS操作成功时，成功将`freeList_`从`oldHead`更新为`slot`

- **较强的同步需求：**由于成功执行了写入操作，需要较强的同步（写入同步），其他线程需要看到新节点的所有初始化操作
- **作用：**确保在此操作之前对`slot`的所有写入对后续读取`freeList_`的线程可见
- **数据流向：**将本线程的内存修改release给其他线程

2、**失败路径**

当CAS失败，即其他线程已经修改`freeList_`时

- **只需要弱同步：**只是读取了当前值，准备重试
- **作用：**更新`oldHead`为当前头结点值，不需要建立同步关系
- **性能优化：**降低内存栅栏开销，因为会立即循环重试

#### 在popFreeList中的对比

```C++
if (freeList_.compare_exchange_weak(oldHead, newHead,
 std::memory_order_acquire, std::memory_order_relaxed))
```

这里使用`std::memory_order_acquire`是因为成功后会**读取**获取到的节点内容（返回`oldHead`供调用者使用），需要确保能看到其他线程对该节点的完整初始化。

**性能影响**

这种精细控制内存序的设计能显著提高性能：

- **减少CPU栅栏指令**：较弱的内存序需要更少的CPU栅栏指令
- **只在必要时同步**：只在实际需要的路径上使用更强的同步保证
- **提高并发效率**：在高频调用的内存管理操作中尤为重要

#### 为什么这两个操作只需要relaxed内存序

在无锁队列的实现中，这两行代码只使用`std::memory_order_relaxed`有着深刻的性能和设计考虑：

```c++
Slot* oldHead = freeList_.load(std::memory_order_relaxed);
slot->next.store(oldHead, std::memory_order_relaxed);
```

**设计原理**

以`relaxed`内存序执行这两个操作是因为：

1. **准备阶段不需要同步**：这两行代码只是为CAS操作做准备工作

   - 读取头节点只是获取当前值，供后续本地使用
   - 设置新节点的next指针此时对其他线程不可见

2. **延迟同步点**：真正的同步发生在后面的CAS操作

   ```C++
   if (freeList_.compare_exchange_weak(oldHead, slot,
    std::memory_order_release, std::memory_order_relaxed))
   ```

   - 只有CAS成功时，才需要建立线程间的同步关系
   - 此时使用[memory_order_release](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html)保证之前的所有写操作对其他线程可见

3. **渐进式可见性**：内存可见性是有成本的

   - 节点此时还未"发布"给其他线程，不需要立即对其他线程可见
   - 真正的"发布点"是CAS操作成功的时刻

# operator new 和 new 的区别

| 特性             | operator new       | new                |
| ---------------- | ------------------ | ------------------ |
| 类型             | 函数               | 操作符             |
| 功能             | 仅分配原始内存     | 分配内存并构造对象 |
| 返回类型         | `void*`            | 对象类型指针       |
| 使用语法         | operator new(size) | `new Type(args)`   |
| 是否调用构造函数 | 否                 | 是                 |

# 具体代码分析

## `newElement(Args&&... args)`

```C++
T* newElement(Args&&... args)
{
    T* p = nullptr;
    // 根据元素大小选取合适的内存池分配内存
    if ((p = reinterpret_cast<T*>(HashBucket::useMemory(sizeof(T)))) != nullptr)
        // 在分配的内存上构造对象
        new(p) T(std::forward<Args>(args)...);

    return p;
}
```

使用了定位`new` 而不是标准的C++ `new`操作符

- 不分配内存，只调用构造函数
- 使用已有的内存，如已经分配好的p

## `~MemoryPool()`

```C++
MemoryPool::~MemoryPool()
{
    // 把连续的block删除
    Slot* cur = firstBlock_;
    while (cur)
    {
        Slot* next = cur->next;
        // 等同于 free(reinterpret_cast<void*>(firstBlock_));
        // 转化为 void 指针，因为 void 类型不需要调用析构函数，只释放空间
        operator delete(reinterpret_cast<void*>(cur));
        cur = next;
    }
}
```

**每个内存块的布局：**

```
┌───────────────────────────────────────────────┐
│ Slot结构(头部) │ 实际数据区域(多个内存槽)      │
└───────────────────────────────────────────────┘
↑               ↑
firstBlock_     body
```

### 内存分配器的工作原理

1. **分配记录**：当你调用 `operator new(BlockSize_)` 分配内存时，内存分配器会在**内部记录**这块内存的**实际大小**

2. **元数据存储**：大多数内存分配器在实际返回给用户的内存区域之前，会存储一个隐藏的"头部"，包含如下信息：

   ```
   [元数据区域|用户可见内存区域]
    ↑      ↑
   (隐藏)   (返回的指针)
   ```

   

3. **指针关联**：内存分配器维护着指针和分配大小之间的映射关系

### 释放过程解析

当执行 `operator delete(reinterpret_cast<void*>(cur))` 时：

1. 内存分配器使用传入的指针查找关联的元数据
2. 找到该指针最初分配时的完整尺寸（这里是 `BlockSize_`）
3. 释放包括元数据在内的整个内存区域

### 举例说明

假设`BlockSize_` = 4096：

```C++
分配: void* block = operator new(4096);
// 系统内部记录: 指针 block 对应 4096 字节
使用: 将其转为 Slot*，用于链表管理
// 这只影响编译器对内存的解释方式，不影响内存分配器的记录
释放: operator delete(reinterpret_cast<void*>(block));
// 系统内部查找: 指针 block 对应 4096 字节，释放整块
```

关键点是：**指针的类型转换不会更改内存分配器内部维护的关于该内存块的信息**。无论你如何使用或解释这块内存，当释放时，系统依然知道它最初分配的确切大小。

所以会正确释放完整的内存块而不是只释放一个头部的`Slot`结构体

## `void *MemoryPool::allocate()`

```C++
void* MemoryPool::allocate()
{
    // 优先使用空闲链表中的内存槽
    Slot* slot = popFreeList();
    if (slot != nullptr)
        return slot;

    Slot* temp;
    {   
        std::lock_guard<std::mutex> lock(mutexForBlock_);
        if (curSlot_ >= lastSlot_)
        {
            // 当前内存块已无内存槽可用，开辟一块新的内存
            allocateNewBlock();
        }
    
        temp = curSlot_;
        // 这里不能直接 curSlot_ += SlotSize_ 因为curSlot_是Slot*类型，所以需要除以SlotSize_再加1
        curSlot_ += SlotSize_ / sizeof(Slot);
    }
    
    return temp; 
}
```

想让`curSlot_`前进`SlotSize_`个字节，如果直接`+= SlotSize_`，前进的是`n * sizeof(指针类型)`个字节，所以要先除以`sizeof(Slot)`