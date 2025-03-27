#pragma once
#include "ThreadCache.hpp"

namespace MyMemoryPool
{

    // 作为用户接口的内存池类
    class MemoryPool
    {
    public:
        static void *allocate(size_t size)
        {
            return ThreadCache::getInstance()->allocate(size);
        }

        static void deallocate(void *ptr, size_t size)
        {
            ThreadCache::getInstance()->deallocate(ptr, size);
        }
    };
} // namespace MyMemoryPool