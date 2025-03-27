#pragma once

#include <cstddef>
#include <atomic>
#include <array>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <map>
#include <sys/mman.h>
#include <cstring>
#include <cassert>
#include <iostream>

namespace MyMemoryPool
{
    // 对齐数和大小定义
    constexpr size_t ALIGNMENT = 8;                          // 对齐数
    constexpr size_t MAX_BYTES = 256 * 1024;                 // 256KB
    constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // 对齐数等于指针void*的大小
    constexpr size_t THREAD_MAX_SIZE = 64;                   // 线程本地自由链表大小上限

    // 大小类管理
    class SizeClass
    {
    public:
        // 对内存大小进行向上取整
        static size_t roundUp(size_t bytes)
        {
            return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        }

        static size_t getIndex(size_t bytes)
        {
            // 确保bytes至少为ALIGNMENT
            bytes = std::max(bytes, ALIGNMENT);
            // 向上取整后-1 获取对应空闲链表的索引
            return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
        }
    };

    // 自旋锁类
    class SpinLockGuard
    {
    private:
        std::atomic_flag &lock_;

    public:
        // explicit关键字用于修饰只有一个参数的构造函数，表示该构造函数是显式的
        // 禁用隐式转换 例如 SpinLockGuard guard = lock; 是不允许的
        explicit SpinLockGuard(std::atomic_flag &lock) : lock_(lock)
        {
            // 自旋等待锁释放
            // 用acquire内存序 获取锁 防止读取上移
            while (lock_.test_and_set(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }

        ~SpinLockGuard()
        {
            // 释放锁
            lock_.clear(std::memory_order_release);
        }

        // 禁止拷贝构造和赋值
        SpinLockGuard(const SpinLockGuard &) = delete;
        SpinLockGuard &operator=(const SpinLockGuard &) = delete;
    };
} // namespace MyMemoryPool
