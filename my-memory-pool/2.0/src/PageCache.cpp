#include "../include/PageCache.hpp"
#include <cstring>

namespace MyMemoryPool
{
    void *PageCache::allocateSpan(size_t numPages)
    {
        std::lock_guard<std::mutex> lock(PageCache::mutex_);

        // 查找合适的空闲span
        // lower_bound函数返回第一个大于等于numPages的元素的迭代器
        // 最小适配算法
        auto it = PageCache::freeSpans_.lower_bound(numPages);

        if (it != freeSpans_.end())
        {
            Span *span = it->second;

            // 将取出的span从原有的空闲链表freeSpans_[it->first]中移除
            if (span->next)
            {
                freeSpans_[it->first] = span->next;
            }
            else
            {
                freeSpans_.erase(it);
            }

            // 如果span大于需要的numPages则进行分割
            if (span->numPages > numPages)
            {
                Span *newSpan = new Span;
                // 分割出来的新span起始地址
                newSpan->pageAddr = static_cast<char *>(span->pageAddr) +
                                    numPages * PAGE_SIZE;
                // 分割出来的新span页数
                newSpan->numPages = span->numPages - numPages;
                newSpan->next = nullptr;

                // 将超出部分放回空闲Span*列表头部
                auto &list = freeSpans_[newSpan->numPages];
                newSpan->next = list;
                list = newSpan;

                // 更新先前取出的span的页数
                span->numPages = numPages;
                return span->pageAddr;
            }

            // 记录span信息用于回收
            spanMap_[span->pageAddr] = span;
            return span->pageAddr;
        }

        // 没有合适的span，向系统申请
        void *memory = systemAlloc(numPages);
        if (memory == nullptr)
        {
            return nullptr;
        }

        // 创建新的span
        Span *span = new Span;
        span->pageAddr = memory;
        span->numPages = numPages;
        span->next = nullptr;

        // 记录span信息用于回收
        spanMap_[memory] = span;
        return memory;
    }

    // 回收span
    void PageCache::deallocateSpan(void *ptr, size_t numPages)
    {
        std::lock_guard<std::mutex> lock(PageCache::mutex_);

        // 查找对应的span，没找到代表不是PageCache分配的内存，直接返回
        auto it = spanMap_.find(ptr);
        if (it == spanMap_.end())
        {
            return;
        }

        Span *span = it->second;

        void *nextAddr = static_cast<char *>(ptr) + numPages * PAGE_SIZE;
        auto nextIt = spanMap_.find(nextAddr);

        // 如果nextSpan存在且未被分配，则合并
        if (nextIt != spanMap_.end())
        {
            Span *nextSpan = nextIt->second;

            // 检查nextSpan是否在空闲链表中
            // 在空闲链表中说明未被分配
            bool found = false;
            auto &nextList = freeSpans_[nextSpan->numPages];

            // 检查是否是头结点
            if (nextList == nextSpan)
            {
                nextList = nextSpan->next;
                found = true;
            }
            else if (nextList)
            {
                Span *prev = nextList;
                while (prev->next)
                {
                    if (prev->next == nextSpan)
                    {
                        prev->next = nextSpan->next;
                        found = true;
                        break;
                    }
                    prev = prev->next;
                }
            }

            if (found)
            {
                // 合并两个span
                span->numPages += nextSpan->numPages;
                spanMap_.erase(nextSpan->pageAddr);
                delete nextSpan;
            }

            // 将span放回空闲链表
            auto &list = freeSpans_[span->numPages];
            span->next = list;
            list = span;
        }
    }

    // 向系统申请内存
    void *PageCache::systemAlloc(size_t numPages)
    {
        size_t size = numPages * PAGE_SIZE;

        // 使用mmap向系统申请内存
        // mmap函数原型：void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
        // addr：期望映射的地址，一般为nullptr，由系统自动分配
        // length：映射的内存大小
        // prot：内存保护标志，PROT_READ | PROT_WRITE表示可读可写
        // flags：映射选项，MAP_PRIVATE | MAP_ANONYMOUS表示映射的是匿名内存
        // fd：文件描述符，一般为-1
        // offset：文件映射的偏移量，一般为0
        void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        // 申请失败
        if (ptr == MAP_FAILED)
        {
            return nullptr;
        }

        memset(ptr, 0, size);
        return ptr;
    }
} // namespace MyMemoryPool