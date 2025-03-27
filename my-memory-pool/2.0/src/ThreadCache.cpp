#include "../include/ThreadCache.hpp"
#include "../include/CentralCache.hpp"

namespace MyMemoryPool
{
    void *ThreadCache::allocate(size_t size)
    {
        assert(size >= 0);
        if (size == 0)
        {
            size = ALIGNMENT;
        }

        if (size > MAX_BYTES) // 256KB
        {
            // 大对象直接从系统分配
            return malloc(size);
        }

        size_t index = SizeClass::getIndex(size);

        // 从自由链表中获取
        freeListSize_[index]--;

        if (void *ptr = freeList_[index])
        {
            // freeList_[index] = freeList_[index]->next
            freeList_[index] = *reinterpret_cast<void **>(ptr);
            return ptr;
        }

        // 从中心缓存获取
        return fetchFromCentralCache(index);
    }

    void ThreadCache::deallocate(void *ptr, size_t size)
    {
        if (size > MAX_BYTES)
        {
            // 大对象是由malloc分配的 用free释放
            free(ptr);
            return;
        }

        // 确定内存块在哪条自由链表
        size_t index = SizeClass::getIndex(size);
        // 插入到线程本地自由链表
        // ptr->next = freeList_[index]
        *reinterpret_cast<void **>(ptr) = freeList_[index];
        freeList_[index] = ptr;
        // 更新自由链表的大小
        freeListSize_[index]++;

        // 判断是否需要将部分内存回收给中心缓存
        if (shouldReturnToCentralCache(index))
        {
            returnToCentralCache(freeList_[index], size);
        }
    }

    bool ThreadCache::shouldReturnToCentralCache(size_t index)
    {
        // 设置自由链表大小的最大值
        return freeListSize_[index] > THREAD_MAX_SIZE;
    }

    // 当线程本地自由链表不足时，从中心缓存获取内存
    void *ThreadCache::fetchFromCentralCache(size_t index)
    {
        // 计算单个内存块的大小
        size_t size = (index + 1) * ALIGNMENT;
        // 根据内存块大小确定需要获取的数量
        size_t batchNum = getBatchNum(size);
        // 从中心缓存获取内存
        void *start = CentralCache::getInstance().fetchRange(index, batchNum);
        if (start == nullptr)
        {
            return nullptr;
        }

        // 更新自由链表大小
        freeListSize_[index] += batchNum;

        // 取出一个内存块用于分配
        void *result = start;
        if (batchNum > 1)
        {
            freeList_[index] = *reinterpret_cast<void **>(start);
        }

        return result;
    }

    // 将多余的线程本地缓存归还给中心缓存
    void ThreadCache::returnToCentralCache(void *start, size_t size)
    {
        size_t index = SizeClass::getIndex(size);
        // 计算要归还内存块数量
        size_t batchNum = freeListSize_[index];
        if (batchNum <= 1)
        {
            return; // 如果只有一个块，则不归还
        }

        // 保留一部分在ThreadCache中 (1/4)
        size_t keepNum = std::max(batchNum / 4, size_t(1));
        size_t returnNum = batchNum - keepNum;

        // 得出分割点
        char *current = static_cast<char *>(start);
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
            // 将返回部分与保留部分分开
            void *nextNode = *reinterpret_cast<void **>(splitNode);
            *reinterpret_cast<void **>(splitNode) = nullptr;

            // 更新自由链表的状态
            freeList_[index] = start;
            // 更新自由链表大小
            freeListSize_[index] = keepNum;

            if (returnNum > 0 && nextNode != nullptr)
            {
                // 归还给中心缓存
                CentralCache::getInstance().returnRange(nextNode, returnNum, index);
            }
        }
    }

    // 计算批量获取内存块的数量
    size_t ThreadCache::getBatchNum(size_t size)
    {
        // 基准：每次批量获取不超过4KB内存
        constexpr size_t MAX_BATCH_SIZE = 4 * 1024; // 4KB

        // 根据对象大小设置合理的基准批量数
        size_t baseNum;
        if (size <= 32)
            baseNum = 64; // 64 * 32 = 2KB
        else if (size <= 64)
            baseNum = 32; // 32 * 64 = 2KB
        else if (size <= 128)
            baseNum = 16; // 16 * 128 = 2KB
        else if (size <= 256)
            baseNum = 8; // 8 * 256 = 2KB
        else if (size <= 512)
            baseNum = 4; // 4 * 512 = 2KB
        else if (size <= 1024)
            baseNum = 2; // 2 * 1024 = 2KB
        else
            baseNum = 1; // 大于1024的对象每次只从中心缓存取1个

        // 计算最大批量数
        size_t maxNum = std::max(size_t(1), MAX_BATCH_SIZE / size);

        // 取最小值，但确保至少返回1
        return std::max(size_t(1), std::min(maxNum, baseNum));
    }
} // namespace memoryPool