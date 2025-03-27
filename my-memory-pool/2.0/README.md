# Common.h

## 常量定义

```C++
constexpr size_t ALIGNMENT = 8;
constexpr size_t MAX_BYTES = 256 * 1024; // 256KB
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT;
```

**ALIGNMENT**

- 内存对齐的基本单位
- 在64位系统上通常等于指针大小
- 确保所有分配的内存的起始地址都是8的倍数

**MAX_BYTES(256KB)**

- 内存池管理的最大对象大小
- 超过此大小的对象将直接使用系统分配器

**FREE_LIST_SIZE**

- 自由链表数组的槽位数量
- 每个槽位对应一种固定大小的内存块
- 256KB ÷ 8B = 32768个槽位

## BlockHeader 结构体

```C++
struct BlockHeader 
{
    size_t size;          // 内存块大小
    bool   inUse;         // 使用标志
    BlockHeader* next;    // 指向下一个内存块
};
```

**size**: 记录内存块的实际大小

- 用于释放时确定归还到哪个槽位

**inUse**: 使用状态标记

- `true` 表示此块已分配给用户
- `false` 表示此块空闲，可再次分配
- 在调试和内存泄漏检测中非常有用

**next**: 链表指针

- 将相同大小的空闲块连接成链表
- 实现O(1)时间复杂度的分配和释放

```C++
class SizeClass 
{
public:
    static size_t roundUp(size_t bytes);
    static size_t getIndex(size_t bytes);
};
```

负责尺寸映射和槽位计算

## SizeClass类

```C++
class SizeClass
    {
    public:
        static size_t roundUp(size_t bytes)
        {
            return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        }

        static size_t getIndex(size_t bytes)
        {
            // 确保bytes至少为ALIGNMENT
            bytes = std::max(bytes, ALIGNMENT);
            // 向上取整后-1
            return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
        }
    };
```



### roundUp

```C++
static size_t roundUp(size_t bytes)
{
    return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}
```

- 将请求大小向上取整到ALIGNMENT的倍数
- 确保所有内存块都严格对齐，提高访问效率

**数值示例**

假设bytes = 13:

```
bytes = 13              (二进制: 0000 1101)
bytes + 7 = 20          (二进制: 0001 0100)
~7 = -8                 (二进制: 1111 1000)
20 & (1111 1000) = 16   (二进制: 0001 0000)
```

**相当于只保留最高位**

### getIndex

```C++
static size_t getIndex(size_t bytes)
{
    bytes = std::max(bytes, ALIGNMENT);
    return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
}
```

- 计算请求大小对应的自由链表索引
- 向上取整后-1计算槽位索引
- 确保请求至少为`ALIGNMENT`大小

---

# ThreadCache.h

**作为内存池的线程本地缓存层**

定义了内存池的第一级缓存——线程本地缓存。这是整个三级缓存架构中最贴近用户的一层，也是性能优化的关键

**核心设计思想**

`ThreadCache`的核心设计思想是**为每个线程提供独立的内存缓存**，进而避免频繁的线程间同步操作，通过C++11`thread_local`的特性实现的，确保每个线程拥有自己专属的 `ThreadCache` 实例

## 1、单例模式实现

```C++
static ThreadCache* getInstance()
{
    static thread_local ThreadCache instance;
    return &instance;
}
```



- **线程本地单例：**`thread_local`关键字确保每个线程有自己独立的ThreadCache实例
- **懒惰初始化：**第一次调用`getInstance()`的时候才创建实例
- **自动销毁：**线程结束时，实例自动销毁

## 2、主要接口

```C++
void* allocate(size_t size);       // 分配内存
void deallocate(void* ptr, size_t size); // 释放内存
```

- `allocate`从线程本地缓存中获取指定大小的内存块
- `deallocate`将内存块归还给线程本地缓存

## 3、缓存层交互方法

```C++
void* fetchFromCentralCache(size_t index);
void returnToCentralCache(void* start, size_t size);
```

- `fetchFromCentralCache`当线程本地缓存无可用内存时，批量从中央缓存获取
- `returnToCentralCache`当线程本地缓存积累过多内存时，批量归还给中央内存

## 4、优化策略方法

```C++
size_t getBatchNum(size_t size);
bool shouldReturnToCentralCache(size_t index);
```

- **`getBatchNum`: **根据内存块大小动态确定批量获取数量
  - 小内存块批量多（减少交互次数）
  - 大内存块批量少（避免浪费）
- **`shouldReturnToCentralCache`**: 判断何时归还内存给中央缓存
  - 防止单个线程占用过多资源
  - 实现线程间的内存均衡

## 5、数据结构

```C++
std::array<void*, FREE_LIST_SIZE> freeList_;    
std::array<size_t, FREE_LIST_SIZE> freeListSize_;
```

`freeList_`自由链表数组，每个元素是一个指向特定大小的类内存块链表的头指针

- ```
  索引0  → 管理大小为 8 字节的内存块
  索引1  → 管理大小为 16 字节的内存块
  索引2  → 管理大小为 24 字节的内存块
  ...
  索引n  → 管理大小为 (n+1)*8 字节的内存块
  ```

- ```
  freeList_[i] → [块A] → [块B] → [块C] → nullptr
  ```

`freeListSize_`记录每个自由链表中的内存块数量，用于决定何时与`CentralCache`交互

## 工作流程示例

### 分配内存流程

1. 用户调用 `allocate(24)`
2. 计算大小索引 `index = SizeClass::getIndex(24)` → 2
3. 检查`freeList_[2]`是否有可用块
   - 如果有：直接返回链表头部，更新链表
   - 如果没有：调用 `fetchFromCentralCache(2)` 批量获取，再返回一个

### 释放内存流程

1. 用户调用 `deallocate(ptr, 24)`
2. 计算大小索引 `index = SizeClass::getIndex(24)` → 2
3. 将内存块插入 `freeList_[2]` 头部，增加 `freeListSize_[2]`
4. 检查`shouldReturnToCentralCache(2)`
   - 如果需要归还：调用 `returnToCentralCache` 批量归还一部分

# MemoryPool.h

实现了一个典型的**外观模式（Facade Pattern）**,是整个内存池系统与外界交互的**统一入口点**

## 1、在整体架构中的位置

处于内存池系统的最外层：

```
应用程序
    ↓
[MemoryPool] ← 你正在查看的组件
    ↓
ThreadCache
    ↓
CentralCache
    ↓
PageCache
    ↓
操作系统
```



## 2、调用链分析：

```C++
MemoryPool::allocate(size)
   ↓
ThreadCache::getInstance()->allocate(size)
```

## 3、线程本地实例获取

```C++
// ThreadCache.h 中的实现
static ThreadCache* getInstance() {
    static thread_local ThreadCache instance;
    return &instance;
}
```

**`thread_loacl`关键字确保每个线程拥有自己专属的`ThreadCache`实例**

## 4、实际执行流程

当一个线程调用`MemoryPool::allocate(64) `时：

1. 线程获取**自己的** ThreadCache 实例
2. 首先从线程**本地**的自由链表获取内存
3. 仅当本地缓存不足时，才会向中央缓存申请（需要同步）
4. 大多数分配操作都在线程本地完成，无需任何同步

## 5、设计优势

1. **无锁访问**：线程访问自己本地的缓存无需加锁，消除并发分配的主要瓶颈
2. **减少伪共享：**不同线程的缓存在物理上也是分离的，避免缓存行竞争
3. **局部性优化：**一个线程频繁使用的内存保留在其本地缓存中，提升缓存命中率
4. **按需调节：**每个线程可以根据自身需求动态调整本地缓存大小

# ThreadCache详解

## **`void *ThreadCache::allocate(size_t size)`：**

```C++
void *ThreadCache::allocate(size_t size)
    {
        // 处理0大小的分配请求
        if (size == 0)
        {
            size = ALIGNMENT; // 至少分配一个对齐大小
        }

        if (size > MAX_BYTES)
        {
            // 大对象直接从系统分配
            return malloc(size);
        }

        size_t index = SizeClass::getIndex(size);

        // 更新自由链表大小
        freeListSize_[index]--;

        // 检查线程本地自由链表
        // 如果 freeList_[index] 不为空，表示该链表中有可用内存块
        if (void *ptr = freeList_[index])
        {
            freeList_[index] = *reinterpret_cast<void **>(ptr); // 将freeList_[index]指向的内存块的下一个内存块地址（取决于内存块的实现）
            return ptr;
        }

        // 如果线程本地自由链表为空，则从中心缓存获取一批内存
        return fetchFromCentralCache(index);
    }
```

### 1. 零大小请求处理

```C++
if (size == 0) 
{
  size = ALIGNMENT; // 至少分配一个对齐大小
}
```

- **技术原理**：C++标准允许分配0字节内存，但必须返回有效指针
- **设计决策**：将0字节请求统一调整为最小分配单元(8字节)
- **优势**：简化后续处理逻辑，避免特殊情况判断
- **ALIGNMENT值**：通常为8字节，与指针大小和内存对齐要求相匹配

### 2. 大对象直通路径

```C++
if (size > MAX_BYTES) 
{
  // 大对象直接从系统分配
  return malloc(size);
}
```

- **阈值选择**：MAX_BYTES为256KB，基于内存使用模式分析
- **技术原理**：大对象使用率低，复用价值小，池化收益有限
- **性能考量**：大对象走特殊路径避免内存碎片化
- **实现细节**：直接调用系统malloc而非内存池机制
- **内存归属**：这部分内存由系统管理，非内存池控制范围

### 3. 大小类映射计算

```C++
size_t index = SizeClass::getIndex(size);
```

- **函数实现**：`(std::max(bytes, ALIGNMENT) + ALIGNMENT - 1) / ALIGNMENT - 1`
- **映射机制**：将任意大小映射到固定槽位，实现内存规整化
- 示例转换：
  - 1-8字节 → 索引0
  - 9-16字节 → 索引1
  - 17-24字节 → 索引2
- **数学原理**：向上取整除法后减1，确保合适的索引范围
- **空间效率**：平衡内存碎片与查找效率

### 4. 提前更新计数器

```C++
// 更新自由链表大小
freeListSize_[index]--;
```

- **乐观策略**：假设分配会成功，提前减少计数
- **性能考量**：减少条件分支，简化关键路径
- **潜在问题**：如果fetchFromCentralCache失败需恢复计数器(代码中未显示)
- **计数器用途**：用于判断何时与中央缓存交互和内存平衡策略

### 5. 快速路径：本地缓存分配

```C++
if (void *ptr = freeList_[index]) 
{
  freeList_[index] = *reinterpret_cast<void **>(ptr);
  return ptr;
}
```

- **条件声明语法**：C++17特性，在条件中声明并初始化变量

- **链表操作**：原子性取出链表头节点并更新链表头

- **内存布局**：

  ```
  取出前：
  
  freeList_[index] → [BlockA](ptr) → [BlockB] → [BlockC] → nullptr
           			├─────────┬──────────┤
           			│ next ptr│ user data│
           			└─────────┴──────────┘
  
  取出后：
  freeList_[index] → [BlockB] → [BlockC] → nullptr
  返回：ptr (BlockA的起始地址)
  ```

  **内存结构解析**

  在这种侵入式链表实现中：

  1. `ptr` 指向空闲内存块的**起始地址**
  2. 在这个起始地址位置上，**存储着**指向下一个空闲块的指针
  3. 两者完全重叠，起始地址就是存储下一个指针的地址

  ```
  内存块物理结构:
  +------------------+------------------+
  | 下一块的指针(8字节) | 用户数据区(余下空间) |
  +------------------+------------------+
  ^
  ptr指向这里
  ```

  **指针操作解析**

  1. `ptr`（类型为`void*`）指向块的开头
  2. `reinterpret_cast(ptr)` 将这个地址解释为"指向指针的指针"
  3. 解引用该指针 `*reinterpret_cast<void **>(ptr)` 获取存储在那里的指针值
  4. 这个指针值就是下一个内存块的地址

- **指针重解释**：`*reinterpret_cast<void**>(ptr)`步骤详解

  1. `reinterpret_cast(ptr)`：告诉编译器将ptr视为"指向void指针的指针"
  2. `*...`：解引用，获取存储在ptr地址的指针值(即下一个块的地址)
  3. 这种技术允许在不使用额外结构体的情况下实现嵌入式链表

- **零开销设计**：内存块本身存储链表信息，无额外空间消耗

- **类型安全考量**：使用reinterpret_cast进行显式类型转换，表明这是有意的低级操作

- **内存对齐优势**：前8字节对齐处理简化了指针操作

执行完`void *ptr = freeList_[index]`后`ptr`指向的是从链表头部获取的第一个可用内存块，

而每个内存块的前8个字节存储了下一个内存块的地址，所以用`*reinterpret_cast<void**>(ptr)`将`ptr(void*)`重新解释为一个指向`void*`类型的指针

### 6. 慢速路径：中央缓存批量获取

```C++
return fetchFromCentralCache(index);
```

- **调用时机**：仅在本地缓存为空时执行，频率较低
- **功能职责**：从共享中央缓存批量获取多个内存块
- **批量原理**：一次获取多块减少跨缓存层交互开销
- 实际操作：
  1. 计算批量大小(基于对象大小动态调整)
  2. 从CentralCache获取连续多个块
  3. 取一个返回，其余加入本地缓存

## `void ThreadCache::returnToCentralCache(void *start, size_t size)`

```C++
void ThreadCache::returnToCentralCache(void *start, size_t size)
    {
        // 根据大小计算对应的索引
        size_t index = SizeClass::getIndex(size);

        // 获取对齐后的实际块大小
        size_t alignedSize = SizeClass::roundUp(size);

        // 计算要归还内存块数量
        size_t batchNum = freeListSize_[index];
        if (batchNum <= 1)
            return; // 如果只有一个块，则不归还

        // 保留一部分在ThreadCache中（比如保留1/4）
        size_t keepNum = std::max(batchNum / 4, size_t(1));
        size_t returnNum = batchNum - keepNum;

        // 将内存块串成链表
        char *current = static_cast<char *>(start);
        // 使用对齐后的大小计算分割点
        char *splitNode = current;
        for (size_t i = 0; i < keepNum - 1; ++i)
        {
            splitNode = reinterpret_cast<char *>(*reinterpret_cast<void **>(splitNode));
            if (splitNode == nullptr)
            {
                // 如果链表提前结束，更新实际的返回数量
                keepNum = i + 1;
                returnNum = batchNum - (i + 1);
                break;
            }
        }

        if (splitNode != nullptr)
        {
            // 将要返回的部分和要保留的部分断开
            void *nextNode = *reinterpret_cast<void **>(splitNode);
            *reinterpret_cast<void **>(splitNode) = nullptr; // 断开连接

            // 更新ThreadCache的空闲链表
            freeList_[index] = start;

            // 更新自由链表大小
            freeListSize_[index] = keepNum;

            // 将剩余部分返回给CentralCache
            if (returnNum > 0 && nextNode != nullptr)
            {
                CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
            }
        }
    }
```

这个函数实现了内存池中“归还过多内存”的机制，确保单个线程不会独占过多的资源

## 1. 数据准备与临界检查

```C++
size_t index = SizeClass::getIndex(size);
size_t alignedSize = SizeClass::roundUp(size);
size_t batchNum = freeListSize_[index];
if (batchNum <= 1)
    return; // 如果只有一个块，则不归还
```

- **索引计算**：确定当前内存块所属的大小类别
- **对齐处理**：获取规范化的块大小，确保内存管理一致性
- **早期返回**：如果线程缓存中该大小类只有0-1个块，不值得归还

## 2. 内存分配策略

```C++
size_t keepNum = std::max(batchNum / 4, size_t(1));
size_t returnNum = batchNum - keepNum;
```



- **保留比例**：线程缓存保留约1/4的内存块
- **最小保留**：至少保留1个块，避免过度归还
- **最大限度共享**：将约3/4的块归还给全局缓存

这体现了内存管理中的"空间换时间"策略 - 保留一部分本地内存以加速后续分配，同时不过度囤积资源。

## 3. 链表遍历与分割点定位

```C++
char *splitNode = current;
for (size_t i = 0; i < keepNum - 1; ++i) {
    splitNode = reinterpret_cast<char *>(*reinterpret_cast<void **>(splitNode));
    if (splitNode == nullptr) {
        keepNum = i + 1;
        returnNum = batchNum - (i + 1);
        break;
    }
}
```

- **遍历技术**：利用侵入式链表结构，逐个访问节点
- **安全检查**：检测链表实际长度是否符合预期
- **动态调整**：当链表提前结束时更新保留数量
- 使用`char*`指针表明这是原始内存地址，为内存池的其他部分提供一致性

确保计数器与实际链表状态保持一致。

## 4. 链表分割与状态更新

```C++
if (splitNode != nullptr) {
    void *nextNode = *reinterpret_cast<void **>(splitNode);
    *reinterpret_cast<void **>(splitNode) = nullptr; // 断开连接
    
    freeList_[index] = start;
    freeListSize_[index] = keepNum;
    
    if (returnNum > 0 && nextNode != nullptr) {
        CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
    }
}
```

- **链表切分**：将一个链表分割为两部分 - 保留部分和归还部分
- **断开连接**：通过设置分割点的next指针为nullptr实现链表断开
- **更新线程缓存**：重置线程本地自由链表和计数器
- **归还给中央缓存**：调用中央缓存接口完成批量归还

## 5. 设计亮点

1. **批量处理**：减少线程与中央缓存的交互次数，提高效率
2. **自适应比例**：根据现有内存块数量决定归还比例
3. **防御性编程**：多处检查指针有效性，防止异常情况
4. **懒惰归还**：只有当累积足够多内存块时才触发归还

# CentralCache详解

## 核心功能

1. **内存分发中心**
   - 批量向各线程缓存提供特定大小的内存块
   - 从线程缓存接收归还的多余内存
   - 在必要时从页缓存获取大块内存并分割
2. **资源平衡器**
   - 防止某个线程独占过多内存资源
   - 允许多线程共享内存池
   - 减少整体系统内存碎片
3. **性能优化层**
   - 减少线程直接访问操作系统的频率
   - 通过批量操作提高内存分配效率
   - 缓冲层设计减轻页缓存压力

### 安全设计

```C++
std::array<std::atomic<void *>, FREE_LIST_SIZE> centralFreeList_;
std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
```

- 通过原子操作保证多线程安全
- 每个大小类独立的自旋锁实现细粒度锁
- 减少进程间竞争

### 单例模式

```C++
static CentralCache &getInstance() {
    static CentralCache instance;
    return instance;
}
```

- 确保全局唯一的中央缓存实例
- 线程安全的初始化

### 关键API

- ```C++
  void *fetchRange(size_t index, size_t batchNum);
  ```

​		为线程缓存批量提供内存

- ```C++
  void returnRange(void *start, size_t size, size_t bytes);
  ```

​		接收线程缓存归还的内存

- ```C++
  void *fetchFromPageCache(size_t size);
  ```

  ​	当中央缓存内存不足时向下层请求

### 在内存池架构中的位置

```
应用程序 ↔ 线程缓存(ThreadCache) ↔ 中央缓存(CentralCache) ↔ 页缓存(PageCache) ↔ 操作系统
```

## 页缓存与中央缓存的关键区别

### 1. 内存管理粒度

- **中央缓存**：管理预先划分好的小块内存（几字节到几KB）
- **页缓存**：以页为单位管理大块内存（通常是4KB或更大）

### 2. 与操作系统的交互

- **中央缓存**：不直接与操作系统交互
- **页缓存**：直接调用系统内存分配函数（mmap/VirtualAlloc等）

### 3. 内存组织方式

- **中央缓存**：按大小类组织内存块
- **页缓存**：按页和跨度(span)管理内存

### 4. 内存合并能力

- **中央缓存**：无法合并不同大小类的内存
- **页缓存**：能识别和合并相邻空闲页，减少碎片

## 为什么不能只用两层架构

1. **系统调用开销**
   - 如果中央缓存直接与系统交互，频繁的系统调用会严重降低性能
   - 页缓存批量请求大块内存，减少了系统调用频率
2. **内存碎片控制**
   - 没有页缓存，小块内存无法有效地归还给操作系统
   - 页缓存可以整页回收，更有效地减少外部碎片
3. **大对象处理**
   - 中央缓存主要针对小/中等大小对象优化
   - 页缓存更适合处理大对象分配
4. **内存回收策略**
   - 页缓存可实现更智能的内存归还策略，避免频繁申请/释放

## 1. 架构定位与设计思想

CentralCache是内存池中的"中间商"，实现了三层分配体系中的核心调度层：

```
应用程序 ←→ ThreadCache ←→ CentralCache ←→ PageCache ←→ 操作系统
```



**核心设计理念**：

- **批发商**模式：从PageCache批量获取、向ThreadCache批量分发
- **负载均衡**：防止单个线程缓存过多资源，实现全局资源调度
- **无锁并发**：细粒度锁和原子操作保证高性能多线程访问

## 2. 核心数据结构与成员分析

```C++
// 中心缓存的自由链表
std::array<std::atomic<void *>, FREE_LIST_SIZE> centralFreeList_;
// 用于同步的自旋锁
std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
// 每次从PageCache获取的页数
static constexpr size_t SPAN_PAGES = 8;
```



这三个关键成员承载了CentralCache的核心功能：

- [centralFreeList_](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html)：按大小类存储的原子链表指针数组
- [locks_](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html)：与大小类一一对应的自旋锁数组，实现细粒度并发控制
- [SPAN_PAGES](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html)：定义一次从PageCache获取的内存规模（32KB）

## 3. [fetchRange](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html) 函数源码解析

```c++
void *CentralCache::fetchRange(size_t index, size_t batchNum)
```



这是CentralCache最重要的函数，实现批量内存获取。核心流程图：

```
检查参数合法性
   ↓
获取对应锁
   ↓
尝试从centralFreeList_获取
   ↓
 成功?──→否──→从PageCache获取新Span
   |        ↓
   |     将Span切分为小块
   |        ↓
   |     构建两个链表：
   |     1.返回给线程缓存
   |     2.保留在中央缓存
   ↓
从已有链表提取batchNum个块
   ↓
断开链表，更新centralFreeList_
   ↓
返回内存块链表
```



### 3.1 关键代码分析 - 大块内存切分

```c++
// 将从PageCache获取的内存块切分成小块
char *start = static_cast<char *>(result);
size_t totalBlocks = (SPAN_PAGES * PageCache::PAGE_SIZE) / size;
size_t allocBlocks = std::min(batchNum, totalBlocks);
// 构建返回给ThreadCache的内存块链表
if (allocBlocks > 1) {
  // 构建链表
  for (size_t i = 1; i < allocBlocks; ++i) {
    void *current = start + (i - 1) * size;
    void *next = start + i * size;
    *reinterpret_cast<void **>(current) = next;
  }
  *reinterpret_cast<void **>(start + (allocBlocks - 1) * size) = nullptr;
}
```



这段代码展示了内存分割和链表构建的精髓：

- 精确计算每个内存块的大小和总块数
- 使用指针算术和强制类型转换构建侵入式链表
- 在内存块本身存储next指针，无额外元数据开销

### 3.2 关键代码分析 - 链表分割

```c++
// 从现有链表中获取指定数量的块
void *current = result;
void *prev = nullptr;
size_t count = 0;
while (current && count < batchNum) {
  prev = current;
  current = *reinterpret_cast<void **>(current);
  count++;
}
if (prev) {
  *reinterpret_cast<void **>(prev) = nullptr;
}
centralFreeList_[index].store(current, std::memory_order_release);
```



这段代码实现了链表的精确分割：

- 使用经典三指针（result/prev/current）遍历并分割链表
- 精确控制分割点，确保只提取所需数量的块
- 原子操作更新中央缓存状态，保证线程安全

## 4. [returnRange](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html) 函数源码解析

```c++
void CentralCache::returnRange(void *start, size_t blockCount, size_t index)
```



这个函数接收ThreadCache归还的内存：

```c++
检查参数合法性
   ↓
获取对应锁
   ↓
遍历链表找到末节点
   ↓
检查归还数量是否与预期一致
   ↓
将归还链表与现有链表合并
   ↓
更新centralFreeList_
```



### 4.1 链表合并操作

```C++
// 找到要归还的链表的最后一个节点
void *end = start;
size_t count = 1;
while (*reinterpret_cast<void **>(end) != nullptr) {
  end = *reinterpret_cast<void **>(end);
  count++;
}
// 将归还的链表连接到中心缓存的链表头部
void *current = centralFreeList_[index].load(std::memory_order_relaxed);
*reinterpret_cast<void **>(end) = current;
centralFreeList_[index].store(start, std::memory_order_release);
```



这段代码显示了高效的链表合并：

- 先找到归还链表的末节点
- 将现有链表接到归还链表尾部
- 更新中央缓存指向归还链表头部
- 通过原子操作保证线程安全

## 5. [fetchFromPageCache](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html) 函数解析

```C++
void *CentralCache::fetchFromPageCache(size_t size)
这个函数向PageCache请求内存，实现了自适应分配策略：
// 1. 计算实际需要的页数
size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
// 2. 根据大小决定分配策略
if (size <= SPAN_PAGES * PageCache::PAGE_SIZE) {
  // 小于等于32KB的请求，使用固定8页
  return PageCache::getInstance().allocateSpan(SPAN_PAGES);
} else {
  // 大于32KB的请求，按实际需求分配
  return PageCache::getInstance().allocateSpan(numPages);
}
```



这里展示了分级内存管理的思想：

- 对于小内存请求：批量分配固定大小（8页=32KB）
- 对于大内存请求：精确计算所需页数
- 向上取整确保内存总是足够的

## 6. 线程安全与并发控制

### 6.1 细粒度锁定

SpinLockGuard lock(locks_[index]);

CentralCache使用每个大小类独立的锁，优点：

- 不同大小类可并发访问，减少锁竞争
- 锁粒度小，阻塞时间短
- 提高了多线程环境下的吞吐量

### 6.2 原子操作保证

```C++
result = centralFreeList_[index].load(std::memory_order_relaxed);
centralFreeList_[index].store(start, std::memory_order_release);
```

CentralCache使用原子操作管理共享链表：

- load/store操作保证内存访问的原子性
- 恰当的内存序(memory_order)控制可见性
- 无锁读取提高并发性能

### 6.3 RAII锁管理

使用SpinLockGuard实现资源获取即初始化模式：

- 构造函数获取锁，析构函数释放锁
- 确保异常安全，不会出现死锁
- 代码简洁明了，减少错误

## 7. 性能优化要点

1. **批量操作**：一次处理多个内存块，平摊函数调用和锁操作开销
2. **自旋优化**：使用`yield()`而非纯粹自旋，减少CPU资源浪费
3. **平衡策略**：大小与块数量的动态平衡，优化内存利用率
4. **细粒度锁**：最小化临界区，减少线程等待时间
5. **无额外开销**：使用侵入式链表，内存块本身存储管理信息

## 8. 设计亮点与实现技巧

1. **单例模式**：保证全局唯一的中央缓存实例
2. **原子操作**：无锁读取提高并发性能
3. **异常安全**：try-catch确保异常情况下锁也能正确释放
4. **分级分配**：小内存固定分配，大内存按需分配
5. **内存复用**：链表节点存储在内存块本身，无额外开销
6. **防御性编程**：全面的参数检查和错误处理

# PageCache详解

## 1. PageCache 基本架构

PageCache 是内存池的底层组件，负责大块内存的分配与回收，是连接内存池与操作系统的桥梁：

```
应用程序 → ThreadCache → CentralCache → PageCache → 操作系统
```

**核心职责**：

- 以页为单位管理内存（4KB页）
- 缓存和复用不同大小的内存块
- 减少系统调用频率，提高分配效率
- 实现内存分割与合并，减少碎片

## 2. 核心数据结构

```C++
struct Span {
  void* pageAddr; // 页起始地址
  size_t numPages; // 页数
  Span* next;   // 链表指针
};
std::map<size_t, Span*> freeSpans_; // 按页数索引空闲Span
std::map<void*, Span*> spanMap_;   // 地址到Span的映射
```

这两个映射实现了高效内存管理的双向查找：

- [`freeSpans_`](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html)：按大小查找适合的内存块 —— "我需要多大的内存？"
- [`spanMap_`](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html)：按地址查找元数据 —— "这块内存的信息是什么？"

## 3. 内存分配流程

[`allocateSpan`](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html) 函数实现了高效的内存分配流程：

1. **缓存查找**：使用最小适配算法从[freeSpans_](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html)找到最小的合适Span

   `auto it = freeSpans_.lower_bound(numPages);`

2. **精确分割**：如有必要，将过大的Span分割为所需大小

   `newSpan->pageAddr = static_cast<char*>(span->pageAddr) + numPages * PAGE_SIZE;`

   `newSpan->numPages = span->numPages - numPages;`

3. **系统申请**：当缓存无合适内存时，通过[systemAlloc](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html)向系统申请

   `void* memory = systemAlloc(numPages);`

4. **元数据更新**：记录分配信息，用于将来回收

   `spanMap_[memory] = span;`

## 4. mmap 系统调用详解

[mmap](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html)是PageCache连接操作系统的关键接口，实现了物理内存的直接获取：

```C++
void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,

         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
```

**参数含义**：

- **addr(nullptr)**：让系统选择合适的映射地址
- **size**：请求的内存大小，总是页大小的整数倍
- **prot**：内存保护标志，READ|WRITE允许读写访问
- flags：
  - MAP_PRIVATE：创建私有映射，修改不影响其他进程
  - MAP_ANONYMOUS：创建匿名映射，不基于任何文件
- **fd(-1)**：匿名映射时使用-1，表示不关联文件
- **offset(0)**：在匿名映射中无意义，设为0

**内存分配机制**：

1. mmap申请的内存总是**页对齐**的（4KB边界）
2. 采用**惰性分配**策略：先建立虚拟映射，实际使用时才分配物理页
3. 返回虚拟地址，操作系统负责管理虚拟→物理的映射关系
4. 内存访问触发**页错误**时，内核分配物理页并建立映射

## 5. 内存回收与合并

[deallocateSpan](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html)函数实现了内存回收和碎片合并：

1. **查找验证**：通过[spanMap_](vscode-file://vscode-app/e:/Microsoft VS Code/resources/app/out/vs/code/electron-sandbox/workbench/workbench.html)找到对应Span

   `auto it = spanMap_.find(ptr);`

2. **相邻合并**：检查并合并物理相邻的空闲Span

   `void* nextAddr = static_cast<char*>(ptr) + numPages * PAGE_SIZE;`

   `auto nextIt = spanMap_.find(nextAddr);`

3. **回收管理**：将合并后的Span插入对应大小的空闲列表

   ```C++
   auto& list = freeSpans_[span->numPages];
   span->next = list;
   list = span;
   ```

   

## 6. 设计优势

1. **分层架构**：PageCache只负责页级管理，与CentralCache分工明确
2. **局部性优化**：通过缓存复用近期释放的内存，提高性能
3. **内存整合**：自动合并相邻内存块，减少碎片
4. **适应性分配**：根据请求大小采用不同策略，平衡效率和空间利用
5. **线程安全**：使用互斥锁保护关键数据，支持并发访问

PageCache通过这种设计有效缓冲了内存分配压力，减少了系统调用开销，为上层组件提供了高效的内存管理基础。
