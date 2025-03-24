#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>

namespace MyMemoryPool
{
#define MEMORY_POOL_NUM 64 // 内存池数量
#define SLOT_BASE_SIZE 8   // 槽基础大小
#define MAX_SLOT_SIZE 512  // 最大槽大小

    // 槽结构体
    /*
        具体的内存槽大小无法确定，因为每个内存池的槽大小不同(8的倍数)
        所以这个槽结构体的 sizeof 不是实际的槽大小
     */
    struct Slot
    {
        std::atomic<Slot *> next; // 原子指针
    };

    // 具体的内存池实例，每个内存池实例的槽大小不同，用于实际内存分配
    class MemoryPool
    {
    public:
        MemoryPool(size_t BlockSize = 4096);
        ~MemoryPool();

        void init(size_t size);

        void *allocate();           // 分配内存
        void deallocate(void *ptr); // 释放内存
    private:
        void allocateNewBlock();                  // 分配新的内存块
        size_t padPointer(char *p, size_t align); // 对齐内存指针

        // 使用 CAS 操作进行无锁入队和出队
        bool pushFreeList(Slot *slot); // 入队
        Slot *popFreeList();           // 出队
    private:
        int BlockSize_;                // 内存块大小
        int SlotSize_;                 // 槽大小
        Slot *firstBlock_;             // 指向内存池管理的首个实际内存块
        Slot *curSlot_;                // 指向当前未被使用过的槽
        std::atomic<Slot *> freeList_; // 指向空闲的槽(被使用过后又被释放的槽)
        Slot *lastSlot_;               // 作为当前内存块中最后能够存放元素的位置标识(超过该位置需申请新的内存块)
        std::mutex mutexForBlock_;     // 保证多线程情况下避免不必要的重复开辟内存导致的浪费行为
    };

    // 内存池管理类，静态类，用于管理内存池实例
    class HashBucket
    {
    public:
        static void initMemoryPool();                // 初始化内存池,在使用内存池前调用
        static MemoryPool &getMemoryPool(int index); // 获取内存池实例

        // 调用内存池分配内存
        static void *useMemory(size_t size);
        // 调用内存池释放内存
        static void freeMemory(void *ptr, size_t size);

        template <typename T, typename... Args>
        friend T *newElement(Args &&...args); // 内存池分配对象

        template <typename T>
        friend void deleteElement(T *ptr); // 内存池释放对象
    };

    template <typename T, typename... Args>
    T *newElement(Args &&...args)
    {
        T *ptr = nullptr;
        ptr = reinterpret_cast<T *>(HashBucket::useMemory(sizeof(T)));
        if (ptr != nullptr)
        {
            new (ptr) T(std::forward<Args>(args)...);
        }
        return ptr;
    }
    template <typename T>
    void deleteElement(T *ptr)
    {
        // 先调用析构函数
        if (ptr)
        {
            ptr->~T();
            // 再回收内存
            HashBucket::freeMemory(reinterpret_cast<void *>(ptr), sizeof(T));
        }
    }
} // namespace MemoryPool