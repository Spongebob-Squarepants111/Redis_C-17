#pragma once
#include <vector>
#include <mutex>
#include <memory>
#include <cassert>
#include <functional>
#include <type_traits>
#include <algorithm>
#include <array>
#include <limits>

// 缓存行大小
#define CACHE_LINE_SIZE 64

/**
 * 改进的内存池实现，具有以下特点：
 * 1. 不使用unordered_set跟踪活跃对象，降低CPU和内存开销
 * 2. 内部使用链表管理空闲块，避免vector扩容开销
 * 3. 引入多个大小类别的内存块，更灵活高效
 * 4. 使用自适应增长策略，动态调整块大小
 */

// 内存块池，用于高效管理大小相似的内存块
class MemoryBlockPool {
public:
    // 构造函数，指定块大小和初始块数
    explicit MemoryBlockPool(size_t block_size, size_t initial_blocks = 16);
    
    // 析构函数，释放所有内存块
    ~MemoryBlockPool();
    
    // 分配一个内存块
    void* allocate();
    
    // 释放一个内存块
    void deallocate(void* block);
    
    // 清空池并释放所有内存
    void clear();
    
    // 获取块大小
    size_t block_size() const;
    
    // 获取当前已分配的块数量
    size_t allocated_blocks() const;
    
    // 获取已分配的区域数量
    size_t allocated_chunks() const;
    
private:
    // 根据块大小计算每个区域包含的块数量
    // 小块使用更多数量，大块使用更少数量
    static size_t calculate_blocks_per_chunk(size_t block_size);
    
    // 分配一个新的内存区域
    void allocate_chunk();
    
    // 预分配指定数量的块
    void prealloc(size_t num_blocks);
    
    // 块大小
    size_t block_size_;
    
    // 每个区域包含的块数量
    size_t blocks_per_chunk_;
    
    // 空闲块链表头部
    void* next_free_;
    
    // 已分配的块数量
    size_t allocated_blocks_ = 0;
    
    // 已分配的内存区域
    std::vector<void*> allocated_chunks_;
    
    // 保护池的互斥锁
    mutable std::mutex mutex_;
};

/**
 * 分层内存池，管理多种大小的内存块
 * 根据请求的大小自动选择合适的内存块池
 */
// 已移除 TieredMemoryPool（未在当前工程中使用）

// 内存池，用于特定类型对象的高效内存管理
template <typename T>
class MemoryPool {
public:
    // 构造函数，指定块大小（为 0 时使用 sizeof(T)）
    explicit MemoryPool(size_t block_size = 0) 
        : block_size_(block_size == 0 ? sizeof(T) : block_size) {
            block_pool_ = std::make_unique<MemoryBlockPool>(block_size_);
    }
    
    // 析构函数
    ~MemoryPool() = default;
    
    // 分配并构造一个对象
    template<typename... Args>
    T* allocate(Args&&... args) {
        // 分配内存
        void* mem = block_pool_->allocate();
        
        // 构造对象
        return new(mem) T(std::forward<Args>(args)...);
    }
    
    // 析构并释放一个对象
    void deallocate(T* obj) {
        if (!obj) return;
        
        // 调用析构函数
        obj->~T();
        
        // 释放内存
            block_pool_->deallocate(obj);
    }
    
    // 清空池
    void clear() {
        if (block_pool_) {
            block_pool_->clear();
        }
    }
    
    // 获取块大小
    size_t block_size() const {
        return block_size_;
    }
    
    // 统计接口已移除（未在当前工程中使用）

private:
    // 块大小
    size_t block_size_;
    
    // 专用内存块池
    std::unique_ptr<MemoryBlockPool> block_pool_;
};

// 对象池，改进版本，避免unordered_set开销，与MemoryPool更好地集成
// 已移除 ObjectPool（未在当前工程中使用）