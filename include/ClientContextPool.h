#pragma once
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <functional>
#include <chrono>
#include <cassert>

// 前置声明，避免循环依赖
class RedisServer;

// ClientContext对象池
// 管理Redis服务器中的客户端上下文对象
class ClientContextPool {
public:
    // 客户端上下文结构，对齐到缓存行以减少伪共享
    struct alignas(64) ClientContext {
        int fd;                          // 客户端socket文件描述符
        std::vector<char> read_buffer;   // 读缓冲区
        std::vector<char> write_buffer;  // 写缓冲区
        size_t read_pos;                 // 当前读取位置
        size_t write_pos;                // 当前写入位置
        bool is_reading;                 // 当前是否在读取状态
        std::chrono::steady_clock::time_point last_active; // 最后活跃时间

        // 构造函数
        explicit ClientContext(int client_fd = -1, size_t initial_buffer_size = 256 * 1024) 
            : fd(client_fd)
            , read_buffer(initial_buffer_size)
            , write_buffer(initial_buffer_size)
            , read_pos(0)
            , write_pos(0)
            , is_reading(true)
            , last_active(std::chrono::steady_clock::now()) {}
            
        // 重置函数，用于对象回收前的清理
        void reset(int client_fd) {
            fd = client_fd;
            read_pos = 0;
            write_pos = 0;
            is_reading = true;
            last_active = std::chrono::steady_clock::now();
            
            // 保留缓冲区容量，但清空内容
            read_buffer.clear();
            write_buffer.clear();
        }
    };

    // 自定义删除器，回收ClientContext而不是删除它
    class ContextDeleter {
    public:
        ContextDeleter(ClientContextPool* pool = nullptr) : pool_(pool) {}
        
        void operator()(ClientContext* context) {
            if (pool_) {
                pool_->release(context);
            } else {
                delete context;
            }
        }
        
    private:
        ClientContextPool* pool_;
    };
    
    // 使用共享指针管理ClientContext生命周期
    using ClientContextPtr = std::shared_ptr<ClientContext>;

    // 构造函数
    explicit ClientContextPool(size_t initial_size = 64, size_t max_pool_size = 200)
        : max_pool_size_(max_pool_size) {
        // 预分配对象
        for (size_t i = 0; i < initial_size; ++i) {
            available_contexts_.push_back(new ClientContext());
        }
    }
    
    // 析构函数
    ~ClientContextPool() {
        // 清理所有对象
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* context : available_contexts_) {
            delete context;
        }
    }
    
    // 分配一个ClientContext对象
    ClientContextPtr acquire(int client_fd) {
        ClientContext* context = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!available_contexts_.empty()) {
                context = available_contexts_.back();
                available_contexts_.pop_back();
            }
        }
        
        // 如果池为空，创建新对象
        if (!context) {
            context = new ClientContext(client_fd);
        } else {
            // 重置对象状态
            context->reset(client_fd);
        }
        
        // 使用自定义删除器创建共享指针
        return ClientContextPtr(context, ContextDeleter(this));
    }
    
    // 释放一个ClientContext对象回池
    void release(ClientContext* context) {
        if (!context) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        // 检查池是否已满
        if (available_contexts_.size() < max_pool_size_) {
            available_contexts_.push_back(context);
        } else {
            delete context;
        }
    }
    
    // 获取池大小
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return available_contexts_.size();
    }
    
    // 缩减池大小，释放多余对象
    void shrink(size_t target_size = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        while (available_contexts_.size() > target_size) {
            ClientContext* context = available_contexts_.back();
            available_contexts_.pop_back();
            delete context;
        }
    }

private:
    // 禁止拷贝和赋值
    ClientContextPool(const ClientContextPool&) = delete;
    ClientContextPool& operator=(const ClientContextPool&) = delete;
    
    std::deque<ClientContext*> available_contexts_; // 可用对象队列
    mutable std::mutex mutex_;                      // 保护池的互斥锁
    size_t max_pool_size_;                          // 池的最大大小
}; 