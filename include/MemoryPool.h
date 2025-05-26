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
    explicit MemoryBlockPool(size_t block_size, size_t initial_blocks = 16)
        : block_size_(block_size)
        , blocks_per_chunk_(calculate_blocks_per_chunk(block_size))
        , next_free_(nullptr) {
        prealloc(initial_blocks);
    }
    
    // 析构函数，释放所有内存块
    ~MemoryBlockPool() {
        clear();
    }
    
    // 分配一个内存块
    void* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!next_free_) {
            // 没有空闲块，分配新的区域
            allocate_chunk();
            if (!next_free_) {
                throw std::bad_alloc();
            }
        }
        
        // 获取一个空闲块
        void* block = next_free_;
        // 更新链表头指向下一个空闲块
        next_free_ = *reinterpret_cast<void**>(next_free_);
        
        // 更新统计
        ++allocated_blocks_;
        
        return block;
    }
    
    // 释放一个内存块
    void deallocate(void* block) {
        if (!block) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 简化内存块验证，只检查是否为nullptr
        // 节省CPU开销，放弃复杂检查
        
        // 使用链表管理空闲块：将释放的块添加到链表头
        *reinterpret_cast<void**>(block) = next_free_;
        next_free_ = block;
        
        // 更新统计
        if (allocated_blocks_ > 0) {
            --allocated_blocks_;
        }
    }
    
    // 清空池并释放所有内存
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 释放所有分配的区域
        for (void* chunk : allocated_chunks_) {
            ::free(chunk);
        }
        
        allocated_chunks_.clear();
        next_free_ = nullptr;
        allocated_blocks_ = 0;
    }
    
    // 获取块大小
    size_t block_size() const {
        return block_size_;
    }
    
    // 获取当前已分配的块数量
    size_t allocated_blocks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocated_blocks_;
    }
    
    // 获取已分配的区域数量
    size_t allocated_chunks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocated_chunks_.size();
    }
    
private:
    // 根据块大小计算每个区域包含的块数量
    // 小块使用更多数量，大块使用更少数量
    static size_t calculate_blocks_per_chunk(size_t block_size) {
        if (block_size <= 32) return 512;
        if (block_size <= 64) return 256;
        if (block_size <= 128) return 128;
        if (block_size <= 256) return 64;
        if (block_size <= 512) return 32;
        if (block_size <= 1024) return 16;
        if (block_size <= 2048) return 8;
        if (block_size <= 4096) return 4;
        return 1; // 对于非常大的块
    }
    
    // 分配一个新的内存区域
    void allocate_chunk() {
        size_t align = std::max(size_t(16), block_size_); // 根据块大小确定对齐要求
        
        // 计算区域大小，包括对齐
        size_t chunk_size = blocks_per_chunk_ * block_size_;
        
        // 分配新区域
        void* chunk = aligned_alloc(align, chunk_size);
        if (!chunk) {
            throw std::bad_alloc();
        }
        
        // 记录新区域
        allocated_chunks_.push_back(chunk);
        
        // 构建单链表：将区域划分为块并链接起来
        char* chunk_ptr = static_cast<char*>(chunk);
        
        // 将新分配的块都添加到链表中
        // 最后一个块的next指针设为当前的next_free_，连接已有的空闲链表
        for (size_t i = 0; i < blocks_per_chunk_ - 1; ++i) {
            void* current_block = chunk_ptr + i * block_size_;
            void* next_block = chunk_ptr + (i + 1) * block_size_;
            
            // 每个块的前几个字节存储下一个块的地址
            *reinterpret_cast<void**>(current_block) = next_block;
        }
        
        // 最后一个块指向原来的头部
        void* last_block = chunk_ptr + (blocks_per_chunk_ - 1) * block_size_;
        *reinterpret_cast<void**>(last_block) = next_free_;
        
        // 更新空闲链表头部为新分配区域的第一个块
        next_free_ = chunk_ptr;
    }
    
    // 预分配指定数量的块
    void prealloc(size_t num_blocks) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 计算需要的区域数
        size_t chunks_needed = (num_blocks + blocks_per_chunk_ - 1) / blocks_per_chunk_;
        
        // 分配区域
        for (size_t i = 0; i < chunks_needed; ++i) {
            allocate_chunk();
        }
    }
    
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
class TieredMemoryPool {
public:
    // 构造函数
    TieredMemoryPool() {
        // 初始化不同大小级别的内存块池
        size_t sizes[] = {
            32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384
        };
        
        for (size_t i = 0; i < NUM_POOLS; ++i) {
            if (i < sizeof(sizes) / sizeof(sizes[0])) {
                pools_[i] = std::make_unique<MemoryBlockPool>(sizes[i]);
                pool_sizes_[i] = sizes[i];
            } else {
                // 额外的池使用更大的值
                pool_sizes_[i] = std::numeric_limits<size_t>::max();
            }
        }
    }
    
    // 析构函数
    ~TieredMemoryPool() = default;
    
    // 分配指定大小的内存
    void* allocate(size_t size) {
        // 找到合适大小的内存池
        int pool_index = find_pool(size);
        if (pool_index < 0) {
            // 请求的大小太大，使用系统malloc
            ++oversized_allocs_;
            return ::malloc(size);
        }
        
        // 使用选定的内存池分配内存
        return pools_[pool_index]->allocate();
    }
    
    // 释放内存
    void deallocate(void* ptr, size_t size) {
        if (!ptr) return;
        
        // 找到合适大小的内存池
        int pool_index = find_pool(size);
        if (pool_index < 0) {
            // 大尺寸内存使用系统free
            ::free(ptr);
            return;
        }
        
        // 使用选定的内存池释放内存
        pools_[pool_index]->deallocate(ptr);
    }
    
    // 清空所有内存池
    void clear() {
        for (auto& pool : pools_) {
            if (pool) {
                pool->clear();
            }
        }
    }
    
    // 获取内存池统计信息
    struct Stats {
        size_t oversized_allocations;
        std::array<size_t, 10> pool_sizes;
        std::array<size_t, 10> allocated_blocks;
        std::array<size_t, 10> allocated_chunks;
    };
    
    Stats get_stats() const {
        Stats stats;
        stats.oversized_allocations = oversized_allocs_;
        
        for (size_t i = 0; i < NUM_POOLS; ++i) {
            stats.pool_sizes[i] = pool_sizes_[i];
            if (pools_[i]) {
                stats.allocated_blocks[i] = pools_[i]->allocated_blocks();
                stats.allocated_chunks[i] = pools_[i]->allocated_chunks();
            } else {
                stats.allocated_blocks[i] = 0;
                stats.allocated_chunks[i] = 0;
            }
        }
        
        return stats;
    }
    
private:
    static constexpr size_t NUM_POOLS = 10;  // 内存池数量
    
    // 内存池数组，每个池管理不同大小的内存块
    std::array<std::unique_ptr<MemoryBlockPool>, NUM_POOLS> pools_;
    
    // 每个池管理的内存块大小
    std::array<size_t, NUM_POOLS> pool_sizes_;
    
    // 过大内存分配计数
    std::atomic<size_t> oversized_allocs_{0};
    
    // 查找合适的内存池索引
    int find_pool(size_t size) const {
        for (size_t i = 0; i < NUM_POOLS; ++i) {
            if (size <= pool_sizes_[i]) {
                return static_cast<int>(i);
            }
        }
        return -1; // 没有合适的池
    }
};

// 内存池，用于特定类型对象的高效内存管理
template <typename T>
class MemoryPool {
public:
    // 静态全局内存池
    static TieredMemoryPool& global_pool() {
        static TieredMemoryPool pool;
        return pool;
    }
    
    // 构造函数，指定块大小
    explicit MemoryPool(size_t block_size = 0) 
        : block_size_(block_size == 0 ? sizeof(T) : block_size),
          use_global_pool_(block_size == 0) {
            
        // 如果大小匹配类型大小，可以使用全局池
        if (!use_global_pool_) {
            // 创建专用内存池
            block_pool_ = std::make_unique<MemoryBlockPool>(block_size_);
        }
    }
    
    // 析构函数
    ~MemoryPool() = default;
    
    // 分配并构造一个对象
    template<typename... Args>
    T* allocate(Args&&... args) {
        // 分配内存
        void* mem;
        
        if (use_global_pool_) {
            // 使用全局分层内存池
            mem = global_pool().allocate(block_size_);
        } else {
            // 使用专用内存池
            mem = block_pool_->allocate();
        }
        
        // 构造对象
        return new(mem) T(std::forward<Args>(args)...);
    }
    
    // 析构并释放一个对象
    void deallocate(T* obj) {
        if (!obj) return;
        
        // 调用析构函数
        obj->~T();
        
        // 释放内存
        if (use_global_pool_) {
            global_pool().deallocate(obj, block_size_);
        } else {
            block_pool_->deallocate(obj);
        }
    }
    
    // 清空池
    void clear() {
        if (!use_global_pool_ && block_pool_) {
            block_pool_->clear();
        }
        // 全局池不清空，因为可能有其他地方在使用
    }
    
    // 获取块大小
    size_t block_size() const {
        return block_size_;
    }
    
    // 获取池统计信息
    auto get_stats() const {
        if (use_global_pool_) {
            return global_pool().get_stats();
        } else if (block_pool_) {
            return std::make_tuple(
                block_pool_->block_size(),
                block_pool_->allocated_blocks(),
                block_pool_->allocated_chunks()
            );
        }
        return std::make_tuple(block_size_, 0, 0);
    }

private:
    // 块大小
    size_t block_size_;
    
    // 是否使用全局池
    bool use_global_pool_;
    
    // 专用内存块池
    std::unique_ptr<MemoryBlockPool> block_pool_;
};

// 对象池，改进版本，避免unordered_set开销，与MemoryPool更好地集成
template <typename T>
class ObjectPool {
public:
    // 构造函数，指定初始池大小
    explicit ObjectPool(size_t initial_size = 64) 
        : memory_pool_(sizeof(T)) {
        prealloc(initial_size);
    }
    
    // 析构函数，释放所有对象
    ~ObjectPool() {
        clear();
    }
    
    // 获取一个对象，如果池为空则创建新对象
    template<typename... Args>
    T* acquire(Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (free_objects_.empty()) {
            // 池为空，创建新对象
            return memory_pool_.allocate(std::forward<Args>(args)...);
        }
        
        // 从池中取出最后一个对象
        T* obj = free_objects_.back();
        free_objects_.pop_back();
        
        // 对象是已构造的，使用placement new重新初始化
        new(obj) T(std::forward<Args>(args)...);
        
        return obj;
    }
    
    // 将对象返回到池中
    void release(T* obj) {
        if (!obj) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 调用析构函数但不释放内存
        obj->~T();
        
        // 将对象放回池中
        if (free_objects_.size() < MAX_POOL_SIZE) {
            free_objects_.push_back(obj);
        } else {
            // 池已满，直接释放内存
            memory_pool_.deallocate(reinterpret_cast<T*>(obj));
        }
    }
    
    // 清空池并释放所有对象
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 删除所有池中的对象
        for (T* obj : free_objects_) {
            memory_pool_.deallocate(obj);
        }
        
        free_objects_.clear();
        memory_pool_.clear();
    }
    
    // 获取当前池大小
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return free_objects_.size();
    }
    
    // 预分配指定数量的对象
    template<typename... Args>
    void prealloc(size_t count, Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 扩展池大小
        size_t old_size = free_objects_.size();
        free_objects_.reserve(old_size + count);
        
        // 分配新对象
        for (size_t i = 0; i < count; ++i) {
            T* obj = memory_pool_.allocate(std::forward<Args>(args)...);
            obj->~T(); // 调用析构函数，因为预分配的对象暂时不使用
            free_objects_.push_back(obj);
        }
    }
    
private:
    static constexpr size_t MAX_POOL_SIZE = 10000; // 最大池大小，防止无限增长
    
    // 对象池
    std::vector<T*> free_objects_;
    
    // 内存池，用于实际的内存分配
    MemoryPool<T> memory_pool_;
    
    // 保护池的互斥锁
    mutable std::mutex mutex_;
}; 