#pragma once
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <functional>
#include <chrono>
#include <cassert>
#include <atomic>
#include <list>
#include <unordered_map>
#include <cstring>

// 前置声明，避免循环依赖
class RedisServer;

// 智能缓冲区管理器，用于跨连接复用大型缓冲区
class BufferManager {
public:
    // 缓冲区大小级别
    enum class Size {
        Small = 4 * 1024,     // 4KB
        Medium = 16 * 1024,   // 16KB
        Large = 64 * 1024,    // 64KB
        XLarge = 256 * 1024   // 256KB
    };
    
    // 获取单例
    static BufferManager& instance() {
        static BufferManager instance;
        return instance;
    }
    
    // 获取指定大小的缓冲区
    std::vector<char>* acquire(Size size) {
        std::vector<char>* buffer = nullptr;
        
        // 选择对应的池
        auto& pool = get_pool(size);
        {
            std::lock_guard<std::mutex> lock(pool.mutex);
            if (!pool.buffers.empty()) {
                buffer = pool.buffers.back();
                pool.buffers.pop_back();
            }
        }
        
        // 如果池为空，创建新缓冲区
        if (!buffer) {
            buffer = new std::vector<char>(static_cast<size_t>(size));
        }
        
        return buffer;
    }
    
    // 释放缓冲区
    void release(std::vector<char>* buffer) {
        if (!buffer) return;
        
        // 确定缓冲区大小级别
        Size size = Size::Small;
        size_t capacity = buffer->capacity();
        
        if (capacity >= static_cast<size_t>(Size::XLarge)) {
            size = Size::XLarge;
        } else if (capacity >= static_cast<size_t>(Size::Large)) {
            size = Size::Large;
        } else if (capacity >= static_cast<size_t>(Size::Medium)) {
            size = Size::Medium;
        }
        
        // 清空缓冲区内容但保留容量
        buffer->clear();
        
        // 归还到对应的池
        auto& pool = get_pool(size);
        {
            std::lock_guard<std::mutex> lock(pool.mutex);
            // 控制池大小，避免内存泄漏
            if (pool.buffers.size() < MAX_POOL_SIZE) {
                pool.buffers.push_back(buffer);
            } else {
                delete buffer;
            }
        }
    }
    
    // 销毁所有缓冲区
    void clear() {
        clear_pool(small_pool_);
        clear_pool(medium_pool_);
        clear_pool(large_pool_);
        clear_pool(xlarge_pool_);
    }
    
private:
    static constexpr size_t MAX_POOL_SIZE = 100; // 每个池的最大大小
    
    struct BufferPool {
        std::vector<std::vector<char>*> buffers;
        std::mutex mutex;
    };
    
    BufferPool small_pool_;   // 4KB缓冲区池
    BufferPool medium_pool_;  // 16KB缓冲区池
    BufferPool large_pool_;   // 64KB缓冲区池
    BufferPool xlarge_pool_;  // 256KB缓冲区池
    
    // 私有构造函数
    BufferManager() = default;
    
    // 获取对应大小的池
    BufferPool& get_pool(Size size) {
        switch (size) {
            case Size::Medium: return medium_pool_;
            case Size::Large: return large_pool_;
            case Size::XLarge: return xlarge_pool_;
            default: return small_pool_;
        }
    }
    
    // 清空指定池
    void clear_pool(BufferPool& pool) {
        std::lock_guard<std::mutex> lock(pool.mutex);
        for (auto* buffer : pool.buffers) {
            delete buffer;
        }
        pool.buffers.clear();
    }
};

// ClientContext对象池
// 管理Redis服务器中的客户端上下文对象
class ClientContextPool {
public:
    // 客户端上下文结构，对齐到缓存行以减少伪共享
    struct alignas(64) ClientContext {
        // 缓冲区的初始和最大大小
        static constexpr size_t INITIAL_BUFFER_SIZE = 8 * 1024;   // 8KB初始大小
        static constexpr size_t MAX_BUFFER_SIZE = 512 * 1024;     // 512KB最大大小
        static constexpr float BUFFER_GROW_FACTOR = 1.5f;         // 缓冲区增长因子
        
        int fd;                          // 客户端socket文件描述符
        std::vector<char> read_buffer;   // 读缓冲区
        std::vector<char> write_buffer;  // 写缓冲区
        size_t read_pos;                 // 当前读取位置
        size_t write_pos;                // 当前写入位置
        bool is_reading;                 // 当前是否在读取状态
        std::chrono::steady_clock::time_point last_active; // 最后活跃时间
        bool has_large_buffer;           // 是否使用了大缓冲区
        bool needs_upgrade;              // 是否需要升级缓冲区
        std::mutex write_mutex;          // 写缓冲区互斥锁，用于多线程并发写入
        bool should_close;               // 是否应该关闭连接的标记

        // 构造函数
        explicit ClientContext(int client_fd = -1) 
            : fd(client_fd)
            , read_buffer(INITIAL_BUFFER_SIZE)
            , write_buffer(INITIAL_BUFFER_SIZE)
            , read_pos(0)
            , write_pos(0)
            , is_reading(true)
            , last_active(std::chrono::steady_clock::now())
            , has_large_buffer(false)
            , needs_upgrade(false)
            , should_close(false) {}
            
        // 析构函数
        ~ClientContext() {
            // 在析构时检查是否使用了大缓冲区，如果是则归还
            if (has_large_buffer) {
                return_buffers();
            }
        }
            
        // 重置函数，用于对象回收前的清理
        void reset(int client_fd) {
            fd = client_fd;
            read_pos = 0;
            write_pos = 0;
            is_reading = true;
            last_active = std::chrono::steady_clock::now();
            
            // 如果使用了大缓冲区，归还并重新分配小缓冲区
            if (has_large_buffer) {
                return_buffers();
                read_buffer.resize(INITIAL_BUFFER_SIZE);
                write_buffer.resize(INITIAL_BUFFER_SIZE);
                has_large_buffer = false;
            } else {
                // 保留一定的缓冲区容量，但清空内容
                // 如果缓冲区过大，缩小到初始大小以节省内存
                if (read_buffer.capacity() > INITIAL_BUFFER_SIZE * 2) {
                    read_buffer = std::vector<char>(INITIAL_BUFFER_SIZE);
                } else {
                    read_buffer.clear();
                    read_buffer.shrink_to_fit();
                    read_buffer.resize(INITIAL_BUFFER_SIZE);
                }
                
                if (write_buffer.capacity() > INITIAL_BUFFER_SIZE * 2) {
                    write_buffer = std::vector<char>(INITIAL_BUFFER_SIZE);
                } else {
                    write_buffer.clear();
                    write_buffer.shrink_to_fit();
                    write_buffer.resize(INITIAL_BUFFER_SIZE);
                }
            }
            needs_upgrade = false;
        }
        
        // 归还大型缓冲区到缓冲区管理器
        void return_buffers() {
            if (!has_large_buffer) return;
            
            // 归还读缓冲区
            if (read_buffer.capacity() > INITIAL_BUFFER_SIZE * 2) {
                auto* buffer_ptr = new std::vector<char>(std::move(read_buffer));
                BufferManager::instance().release(buffer_ptr);
                read_buffer = std::vector<char>(INITIAL_BUFFER_SIZE);
            }
            
            // 归还写缓冲区
            if (write_buffer.capacity() > INITIAL_BUFFER_SIZE * 2) {
                auto* buffer_ptr = new std::vector<char>(std::move(write_buffer));
                BufferManager::instance().release(buffer_ptr);
                write_buffer = std::vector<char>(INITIAL_BUFFER_SIZE);
            }
            
            has_large_buffer = false;
        }
        
        // 智能扩展缓冲区，根据需求自动增长或使用共享缓冲区
        std::vector<char>& upgrade_buffer(std::vector<char>& buffer, size_t required_size) {
            // 如果不需要扩展，直接返回
            if (buffer.size() >= required_size) return buffer;
            
            // 决定合适的缓冲区大小级别
            BufferManager::Size size_level = BufferManager::Size::Small;
            if (required_size > static_cast<size_t>(BufferManager::Size::Large)) {
                size_level = BufferManager::Size::XLarge;
            } else if (required_size > static_cast<size_t>(BufferManager::Size::Medium)) {
                size_level = BufferManager::Size::Large;
            } else if (required_size > static_cast<size_t>(BufferManager::Size::Small)) {
                size_level = BufferManager::Size::Medium;
            }
            
            // 从缓冲区池获取适当大小的缓冲区
            std::vector<char>* new_buffer = BufferManager::instance().acquire(size_level);
            
            // 复制原始数据
            if (!buffer.empty()) {
                std::copy(buffer.begin(), buffer.end(), new_buffer->begin());
            }
            
            // 交换缓冲区
            std::swap(buffer, *new_buffer);
            
            // 释放原始缓冲区
            BufferManager::instance().release(new_buffer);
            
            has_large_buffer = true;
            return buffer;
        }
        
        // 确保读缓冲区有足够的空间容纳额外的数据
        void ensure_read_capacity(size_t additional_size) {
            size_t required_capacity = read_pos + additional_size;
            
            // 如果当前缓冲区足够大，直接返回
            if (read_buffer.size() >= required_capacity) {
                return;
            }
            
            // 计算新的缓冲区大小，按照增长因子增长
            size_t new_size = read_buffer.size();
            while (new_size < required_capacity) {
                new_size = static_cast<size_t>(new_size * BUFFER_GROW_FACTOR);
            }
            
            // 限制最大大小
            new_size = std::min(new_size, MAX_BUFFER_SIZE);
            
            // 如果新大小超过阈值，标记需要升级到大缓冲区
            if (new_size > INITIAL_BUFFER_SIZE * 4 && !has_large_buffer) {
                needs_upgrade = true;
            }
            
            // 直接调整缓冲区大小
            read_buffer.resize(new_size);
        }
        
        // 确保写缓冲区有足够的空间容纳额外的数据
        void ensure_write_capacity(size_t additional_size) {
            // 使用互斥锁保护写缓冲区
            std::lock_guard<std::mutex> lock(write_mutex);
            
            size_t required_capacity = write_pos + additional_size;
            
            // 如果当前缓冲区足够大，直接返回
            if (write_buffer.size() >= required_capacity) {
                return;
            }
            
            // 计算新的缓冲区大小，按照增长因子增长
            size_t new_size = write_buffer.size();
            while (new_size < required_capacity) {
                new_size = static_cast<size_t>(new_size * BUFFER_GROW_FACTOR);
            }
            
            // 限制最大大小
            new_size = std::min(new_size, MAX_BUFFER_SIZE);
            
            // 如果新大小超过阈值，标记需要升级到大缓冲区
            if (new_size > INITIAL_BUFFER_SIZE * 4 && !has_large_buffer) {
                needs_upgrade = true;
            }
            
            // 直接调整缓冲区大小
            write_buffer.resize(new_size);
        }
        
        // 压缩读缓冲区，移除已处理的数据
        void compact_read_buffer() {
            // 如果缓冲区为空或所有数据都已处理，直接重置
            if (read_pos == 0) {
                return;
            }
            
            // 如果缓冲区使用率低于25%且大小超过初始大小的4倍，考虑缩小
            if (read_pos > 0 && read_buffer.size() > INITIAL_BUFFER_SIZE * 4) {
                size_t usage_ratio = (read_pos * 100) / read_buffer.size();
                if (usage_ratio < 25) {
                    // 创建更小的缓冲区
                    size_t new_size = std::max(INITIAL_BUFFER_SIZE, read_pos * 2);
                    std::vector<char> new_buffer(new_size);
                    
                    // 复制未处理的数据
                    if (read_pos > 0) {
                        std::copy(read_buffer.begin(), read_buffer.begin() + read_pos, new_buffer.begin());
                    }
                    
                    // 替换缓冲区
                    read_buffer = std::move(new_buffer);
                    return;
                }
            }
            
            // 正常压缩：将未处理的数据移到缓冲区开头
            if (read_pos > 0) {
                // 源和目标是同一个缓冲区，使用memmove防止重叠
                std::memmove(read_buffer.data(), read_buffer.data(), read_pos);
            }
        }
    };

    // 自定义删除器，回收ClientContext而不是删除它
    class ContextDeleter {
    public:
        ContextDeleter(ClientContextPool* pool = nullptr, int fd = -1) 
            : pool_(pool), fd_(fd) {}
        
        void operator()(ClientContext* context) {
            if (pool_) {
                pool_->release(context, fd_);
            } else {
                delete context;
            }
        }
        
    private:
        ClientContextPool* pool_;
        int fd_;
    };
    
    // 使用共享指针管理ClientContext生命周期
    using ClientContextPtr = std::shared_ptr<ClientContext>;

    // 构造函数
    explicit ClientContextPool(size_t initial_size = 50, size_t max_pool_size = 500, size_t num_shards = 16)
        : max_pool_size_(max_pool_size)
        , shards_(num_shards)
        , num_shards_(num_shards)
        , total_available_(0) {
        
        // 计算每个分片的初始大小
        size_t contexts_per_shard = initial_size / num_shards;
        if (contexts_per_shard == 0) contexts_per_shard = 1;
        
        // 初始化所有分片
        for (size_t i = 0; i < num_shards; ++i) {
            shards_[i].max_size = max_pool_size / num_shards;
            
            // 预分配对象
            for (size_t j = 0; j < contexts_per_shard; ++j) {
                shards_[i].available_contexts.push_back(new ClientContext());
                total_available_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    
    // 析构函数
    ~ClientContextPool() {
        // 清理所有对象
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            for (auto* context : shard.available_contexts) {
                delete context;
            }
        }
        
        // 清理缓冲区管理器
        BufferManager::instance().clear();
    }
    
    // 分配一个ClientContext对象
    ClientContextPtr acquire(int client_fd) {
        // 获取分片索引
        size_t shard_idx = get_shard_index(client_fd);
        auto& shard = shards_[shard_idx];
        
        ClientContext* context = nullptr;
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            if (!shard.available_contexts.empty()) {
                context = shard.available_contexts.back();
                shard.available_contexts.pop_back();
                total_available_.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        
        // 如果分片内池为空，创建新对象
        if (!context) {
            context = new ClientContext(client_fd);
        } else {
            // 重置对象状态
            context->reset(client_fd);
        }
        
        // 使用自定义删除器创建共享指针
        return ClientContextPtr(context, ContextDeleter(this, client_fd));
    }
    
    // 释放一个ClientContext对象回池
    void release(ClientContext* context, int client_fd) {
        if (!context) return;
        
        // 如果使用了大缓冲区，先归还
        if (context->has_large_buffer) {
            context->return_buffers();
        }
        
        // 获取分片索引
        size_t shard_idx = get_shard_index(client_fd);
        auto& shard = shards_[shard_idx];
        
        std::lock_guard<std::mutex> lock(shard.mutex);
        // 检查分片池是否已满
        if (shard.available_contexts.size() < shard.max_size) {
            shard.available_contexts.push_back(context);
            total_available_.fetch_add(1, std::memory_order_relaxed);
        } else {
            delete context;
        }
    }
    
    // 获取池大小
    size_t size() const {
        return total_available_.load(std::memory_order_relaxed);
    }
    
    // 缩减池大小，释放多余对象
    void shrink(size_t target_size = 0) {
        size_t target_per_shard = target_size / num_shards_;
        
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            while (shard.available_contexts.size() > target_per_shard) {
                ClientContext* context = shard.available_contexts.back();
                shard.available_contexts.pop_back();
                total_available_.fetch_sub(1, std::memory_order_relaxed);
                delete context;
            }
        }
    }

private:
    // 禁止拷贝和赋值
    ClientContextPool(const ClientContextPool&) = delete;
    ClientContextPool& operator=(const ClientContextPool&) = delete;
    
    // 计算客户端fd应该使用哪个分片
    size_t get_shard_index(int client_fd) const {
        return static_cast<size_t>(client_fd) % num_shards_;
    }
    
    // 分片结构
    struct alignas(64) PoolShard {
        std::deque<ClientContext*> available_contexts; // 可用对象队列
        alignas(64) mutable std::mutex mutex;          // 保护分片的互斥锁
        size_t max_size;                               // 分片的最大大小
    };
    
    std::vector<PoolShard> shards_;                // 分片数组
    const size_t num_shards_;                      // 分片数量
    size_t max_pool_size_;                         // 池的最大大小
    alignas(64) std::atomic<size_t> total_available_; // 总可用对象数量
}; 