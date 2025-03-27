#include "../include/CentralCache.hpp"
#include "../include/PageCache.hpp"

namespace MyMemoryPool
{
    // 每次从PageCache获取span大小（以页为单位）
    static constexpr size_t SPAN_PAGES = 8;

    void *CentralCache::fetchRange(size_t index, size_t batchNum)
    {
        // 索引检查，当索引大于等于FREE_LIST_SIZE时，说明申请内存过大应直接向系统申请
        if (index >= FREE_LIST_SIZE || batchNum == 0)
        {
            return nullptr;
        }

        // 自旋锁保护 函数作用域结束时自动释放锁
        SpinLockGuard lock(locks_[index]);

        void *result = nullptr;

        try
        {
            // 尝试从中心缓存获取内存块
            result = centralFreeList_[index].load(std::memory_order_relaxed);

            // 如果中心缓存为空，从页缓存获取新的内存块
            if (result == nullptr)
            {
                // 如果中心缓存为空，从页缓存获取新的内存块
                size_t size = (index + 1) * ALIGNMENT;
                result = fetchFromPageCache(size);

                if (result == nullptr)
                {
                    return nullptr;
                }

                // 将从PageCache获取的内存块切分成小块
                char *start = static_cast<char *>(result);
                size_t totalBlocks = (SPAN_PAGES * PageCache::PAGE_SIZE) / size;
                size_t allocBlocks = std::min(batchNum, totalBlocks);

                // 构建返回给ThreadCache的内存块链表
                if (allocBlocks > 1)
                {
                    // 确保至少有两个块才构建链表
                    // 构建链表
                    // 循环实际执行了allocBlocks-1次
                    for (size_t i = 1; i < allocBlocks; ++i)
                    {
                        void *current = start + (i - 1) * size;
                        void *next = start + i * size;
                        *reinterpret_cast<void **>(current) = next;
                    }
                    *reinterpret_cast<void **>(start + (allocBlocks - 1) * size) = nullptr;
                }

                // 构建保留在CentralCache的链表
                if (totalBlocks > allocBlocks)
                {
                    // 指针指向剩余内存块的起始地址
                    void *remainStart = start + allocBlocks * size;
                    for (size_t i = allocBlocks + 1; i < totalBlocks; ++i)
                    {
                        void *current = start + (i - 1) * size;
                        void *next = start + i * size;
                        *reinterpret_cast<void **>(current) = next;
                    }
                    *reinterpret_cast<void **>(start + (totalBlocks - 1) * size) = nullptr;

                    centralFreeList_[index].store(remainStart, std::memory_order_release);
                }
            }
            else
            {
                // 从现有链表中获取指定数量的块
                void *current = result;
                void *prev = nullptr;
                size_t count = 0;

                while (current && count < batchNum)
                {
                    prev = current;
                    current = *reinterpret_cast<void **>(current);
                    count++;
                }

                // 断开链表
                if (prev != nullptr)
                {
                    *reinterpret_cast<void **>(prev) = nullptr;
                }

                // 更新中心缓存链表头
                centralFreeList_[index].store(current, std::memory_order_release);
            }
        }
        catch (...)
        {
            throw;
        }

        return result;
    }

    void CentralCache::returnRange(void *start, size_t blockNum, size_t index)
    {
        if (!start || index >= FREE_LIST_SIZE || blockNum == 0)
        {
            return;
        }

        // 自旋锁保护 函数作用域结束时自动释放锁
        SpinLockGuard lock(locks_[index]);

        try
        {
            // 找到归还内存的尾指针
            void *end = start;
            size_t count = 1;
            while (*reinterpret_cast<void **>(end) && count < blockNum)
            {
                end = *reinterpret_cast<void **>(end);
                count++;
            }

            if (count > blockNum)
            {
                // 如果归还的内存块数量大于blockNum，则输出调试语句
                std::cout << "returnRange error: count > blockNum" << std::endl;
            }

            // 将内存块链表插入到中心缓存
            void *current = centralFreeList_[index].load(std::memory_order_relaxed);
            *reinterpret_cast<void **>(end) = current;
            centralFreeList_[index].store(start, std::memory_order_release);
        }
        catch (...)
        {
            throw;
        }
    }

    // 从页缓存获取内存
    void *CentralCache::fetchFromPageCache(size_t size)
    {
        // 1. 计算实际需要的页数
        size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

        // 2. 根据大小决定分配策略
        if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
        {
            // 小于等于32KB的请求，使用固定8页
            return PageCache::getInstance().allocateSpan(SPAN_PAGES);
        }
        else
        {
            // 大于32KB的请求，按实际需求分配
            return PageCache::getInstance().allocateSpan(numPages);
        }
    }
} // namespace MyMemoryPool