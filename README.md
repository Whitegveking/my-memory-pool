# My Memory Pool

一个高性能的C++内存池实现，采用多级缓存设计，可显著提升内存密集型应用的性能。

## 项目概述

My Memory Pool是一个轻量级、高效的内存分配器，使用三级缓存机制来优化内存分配和释放过程，减少系统调用开销，降低内存碎片，提高程序执行效率。

## 功能特点

- **多级缓存架构**：采用ThreadCache、CentralCache和PageCache三级缓存结构
- **线程局部存储**：每个线程拥有独立的ThreadCache，减少线程间竞争
- **批量内存管理**：支持内存批量申请和释放，降低系统调用频率
- **自适应分配策略**：根据内存块大小动态调整批量获取数量
- **内存合并机制**：自动合并相邻空闲内存块，减少碎片
- **支持大内存分配**：小内存使用内存池，大内存直接使用系统分配
- **线程安全**：使用互斥锁和自旋锁保证多线程环境下的安全性

## 架构设计

### 三级缓存结构

1. **ThreadCache**：
   - 线程本地缓存，无锁设计，每个线程独立拥有
   - 按不同大小类管理自由链表
   - 当本地缓存不足时，从CentralCache批量获取内存
   - 当本地缓存过多时，回收部分内存到CentralCache
2. **CentralCache**：
   - 全局共享的中心缓存，使用自旋锁保护
   - 管理多个大小类的空闲内存链表
   - 当内存不足时，从PageCache获取内存并切分
   - 接收ThreadCache归还的内存
3. **PageCache**：
   - 以页(4KB)为单位管理内存
   - 使用Span数据结构管理连续页面
   - 负责向系统申请和释放内存
   - 实现内存的合并和分裂

### 关键数据结构

- **自由链表**：管理相同大小的内存块
- **Span**：管理连续的页面集合
- **SizeClass**：计算内存对齐大小和索引

## 安装与使用

### 编译项目

```bash
git clone https://github.com/yourusername/my-memory-pool.git
cd my-memory-pool
mkdir build && cd build
cmake ..
make
```

### 运行测试

```bash
cd build
./unit_test
./perf_test
```

