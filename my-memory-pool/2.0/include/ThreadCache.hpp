#pragma once
#include "Common.hpp"

namespace MyMemoryPool
{

    // 线程本地缓存
    class ThreadCache
    {
    public:
        // 懒惰初始化 单例模式 只有在被调用时才会初始化
        static ThreadCache *getInstance()
        {
            static thread_local ThreadCache instance;
            return &instance;
        }

        void *allocate(size_t size);
        void deallocate(void *ptr, size_t size);

    private:
        ThreadCache() = default;

        // 从中心缓存获取内存
        void *fetchFromCentralCache(size_t index);

        // 归还内存到中心缓存
        void returnToCentralCache(void *start, size_t size);

        // 计算批量获取内存块的数量
        size_t getBatchNum(size_t size);

        // 判断是否需要归还内存给中心缓存
        bool shouldReturnToCentralCache(size_t index);

    private:
        // 每个线程的自由链表数组
        // 数组的每个元素是一个指针，指向一个空闲链表，每个空闲链表的内存块大小是不同的
        // 具体大小是  (index +1) * ALIGNMENT
        std::array<void *, FREE_LIST_SIZE> freeList_;
        std::array<size_t, FREE_LIST_SIZE> freeListSize_; // 自由链表大小统计
    };
} // namespace MyMemoryPool