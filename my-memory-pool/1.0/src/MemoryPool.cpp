#include "../include/MemoryPool.hpp"

namespace MyMemoryPool
{
    MemoryPool::MemoryPool(size_t BlockSize) : BlockSize_(BlockSize),
                                               SlotSize_(0),
                                               firstBlock_(nullptr),
                                               curSlot_(nullptr),
                                               freeList_(nullptr),
                                               lastSlot_(nullptr)
    {
    }

    MemoryPool::~MemoryPool()
    {
        // 删除连续的block（内存块）
        Slot *cur = firstBlock_;
        while (cur != nullptr)
        {
            Slot *nxt = cur->next;
            // 转化为void指针，不需要调用析构函数
            operator delete(reinterpret_cast<void *>(cur));
            cur = nxt;
        }
    }

    void MemoryPool::init(size_t SlotSize)
    {
        assert(SlotSize > 0);
        SlotSize_ = SlotSize;
        firstBlock_ = nullptr;
        curSlot_ = nullptr;
        freeList_ = nullptr;
        lastSlot_ = nullptr;
    }
    // 分配内存
    void *MemoryPool::allocate()
    {
        // 优先使用空闲链表中的内存槽
        Slot *slot = popFreeList();
        if (slot != nullptr)
        {
            return slot;
        }

        Slot *temp;
        // 用{}限制互斥锁的作用域
        {
            std::lock_guard<std::mutex> lock(mutexForBlock_);
            if (curSlot_ >= lastSlot_)
            {
                // 当前内存块已无内存槽可用，开辟一块新的内存
                allocateNewBlock();
            }
            temp = curSlot_;
            curSlot_ += SlotSize_ / sizeof(Slot);
        }
        return temp;
    }

    // 释放内存
    void MemoryPool::deallocate(void *ptr)
    {
        if (ptr == nullptr)
            return;
        Slot *slot = reinterpret_cast<Slot *>(ptr);
        pushFreeList(slot);
    }

    void MemoryPool::allocateNewBlock()
    {
        void *newBlock = operator new(BlockSize_);
        reinterpret_cast<Slot *>(newBlock)->next = firstBlock_;
        firstBlock_ = reinterpret_cast<Slot *>(newBlock);
        // 内存池的实际可用部分
        char *body = reinterpret_cast<char *>(newBlock) + sizeof(Slot *);
        size_t paddingSize = padPointer(body, SlotSize_);
        curSlot_ = reinterpret_cast<Slot *>(body + paddingSize);
        // 超过该标记位置，则说明该内存块已无内存槽可用，需向系统申请新的内存块
        lastSlot_ = reinterpret_cast<Slot *>(reinterpret_cast<size_t>(newBlock) + BlockSize_ - SlotSize_ + 1);
        freeList_ = nullptr;
    }

    // 指针对齐
    size_t MemoryPool::padPointer(char *p, size_t align)
    {
        // align就是该内存池的槽大小
        return (align - reinterpret_cast<size_t>(p)) % align;
    }

    // 无锁入队操作
    bool MemoryPool::pushFreeList(Slot *slot)
    {
        while (true)
        {
            Slot *oldHead = freeList_.load(std::memory_order_relaxed);
            // 实现线程安全的值写入
            slot->next.store(oldHead, std::memory_order_relaxed);
            if (freeList_.compare_exchange_weak(oldHead, slot,
                                                std::memory_order_release,
                                                std::memory_order_relaxed))
            {
                return true;
            }
        }
    }

    // 无锁出队操作
    Slot *MemoryPool::popFreeList()
    {
        // 用死循环是因为CAS操作可能失败，需要重试
        while (true)
        {
            // load()函数是原子操作，用于在多线程的情况下获取freeList_的值
            // 配合pushFreeList()函数中的release形成同步
            Slot *oldHead = freeList_.load(std::memory_order_acquire);
            if (oldHead == nullptr)
            {
                return nullptr;
            }
            Slot *newHead = nullptr;
            try
            {
                newHead = oldHead->next.load(std::memory_order_relaxed);
            }
            catch (...)
            {
                // 返回失败，尝试重新申请内存
                continue;
            }

            // 尝试更新头结点 继续用原子操作
            if (freeList_.compare_exchange_weak(oldHead, newHead,
                                                std::memory_order_acquire,
                                                std::memory_order_relaxed))
            {
                return oldHead;
            }
        }
    }

    void HashBucket::initMemoryPool()
    {
        for (int i = 0; i < MEMORY_POOL_NUM; i++)
        {
            getMemoryPool(i).init((i + 1) * SLOT_BASE_SIZE);
        }
    }

    // 静态函数，获取内存池实例
    MemoryPool &HashBucket::getMemoryPool(int index)
    {
        static MemoryPool memoryPool[MEMORY_POOL_NUM];
        return memoryPool[index];
    }

    void *HashBucket::useMemory(size_t size)
    {
        if (size <= 0)
            return nullptr;
        if (size > MAX_SLOT_SIZE)
            return operator new(size);

        return getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).allocate();
    }

    void HashBucket::freeMemory(void *ptr, size_t size)
    {
        if (ptr == nullptr)
            return;
        if (size > MAX_SLOT_SIZE)
        {
            operator delete(ptr);
            return;
        }

        getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).deallocate(ptr);
    }
} // namespace memoryPool