#pragma once
#include "Common.hpp"

namespace MyMemoryPool
{
    class CentralCache
    {
    public:
        // 懒惰初始化 单例模式 只有在被调用时才会初始化
        static CentralCache &getInstance()
        {
            static CentralCache instance;
            return instance;
        }

        // 从中心缓存获取内存 batchNum是批量获取的数量
        void *fetchRange(size_t index, size_t batchNum);
        // 归还内存到中心缓存
        void returnRange(void *start, size_t blockNum, size_t index);

    private:
        CentralCache()
        {
            // 将所有原子指针初始化为nullptr
            for (auto &ptr : centralFreeList_)
            {
                ptr.store(nullptr, std::memory_order_relaxed);
            }
            // 初始化所有锁
            for (auto &lock : locks_)
            {
                lock.clear();
            }
        }
        // 从页缓存获取内存
        void *fetchFromPageCache(size_t size);

    private:
        // 中心缓存的自由链表
        std::array<std::atomic<void *>, FREE_LIST_SIZE> centralFreeList_;

        // 用于同步的自旋锁
        std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
    };
} // namespace MyMemoryPool