#pragma once
#include <vector>
#include <mutex>
#include <memory>
#include <cassert>
#include <functional>
#include <unordered_set>

// 缓存行大小
#define CACHE_LINE_SIZE 64

// 内存池模板类
template <typename T>
class MemoryPool {
public:
    // 默认内存块大小常量
    static constexpr size_t DEFAULT_BLOCK_SIZE = 1024;

    // 构造函数，指定初始块大小和每个块中对象数量
    explicit MemoryPool(size_t block_size = DEFAULT_BLOCK_SIZE)
        : block_size_(block_size), free_objects_(nullptr) {
        // 分配第一个内存块
        allocate_block();
    }

    // 析构函数
    ~MemoryPool() {
        // 释放所有内存块
        for (auto& block : memory_blocks_) {
            ::operator delete(block);
        }
    }

    // 从内存池中分配一个对象
    template<typename... Args>
    T* allocate(Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 如果没有空闲对象，分配新的内存块
        if (free_objects_ == nullptr) {
            allocate_block();
        }
        
        // 获取一个空闲对象并进行正确的类型转换
        void* memory = free_objects_;
        free_objects_ = free_objects_->next;
        
        // 添加到活跃对象集合
        active_objects_.insert(memory);
        
        // 使用placement new构造对象
        return new(memory) T(std::forward<Args>(args)...);
    }

    // 将对象释放回内存池
    void deallocate(T* object) {
        if (!object) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 检查是否为有效的对象地址
        void* memory = static_cast<void*>(object);
        auto it = active_objects_.find(memory);
        if (it == active_objects_.end()) {
            // 不是由我们分配的对象或已经被释放
            return;
        }
        
        // 从活跃对象集合中移除
        active_objects_.erase(it);
        
        // 调用析构函数
        object->~T();
        
        // 将对象放回空闲链表
        FreeObject* free_obj = static_cast<FreeObject*>(memory);
        free_obj->next = free_objects_;
        free_objects_ = free_obj;
    }

private:
    // 用于构建空闲对象链表的结构
    union FreeObject {
        FreeObject* next;
        char data[sizeof(T)]; // 确保足够的空间存储T类型对象
        
        // 对齐到缓存行边界
        alignas(CACHE_LINE_SIZE) char padding[0];
    };

    // 分配一个新的内存块
    void allocate_block() {
        // 计算块的大小
        size_t block_bytes = block_size_ * sizeof(FreeObject);
        
        // 分配内存
        FreeObject* block = static_cast<FreeObject*>(::operator new(block_bytes));
        memory_blocks_.push_back(block);
        
        // 将新分配的内存块划分为多个空闲对象，并链接到空闲链表
        for (size_t i = 0; i < block_size_ - 1; ++i) {
            block[i].next = &block[i + 1];
        }
        block[block_size_ - 1].next = nullptr;
        
        // 更新空闲对象链表头
        if (free_objects_ == nullptr) {
            free_objects_ = block;
        } else {
            // 找到当前空闲链表的末尾
            FreeObject* tail = free_objects_;
            while (tail->next != nullptr) {
                tail = tail->next;
            }
            // 将新块链接到末尾
            tail->next = block;
        }
    }

    size_t block_size_;                   // 每个内存块中的对象数量
    std::vector<FreeObject*> memory_blocks_; // 存储所有分配的内存块
    FreeObject* free_objects_;            // 空闲对象链表
    std::mutex mutex_;                    // 保护内存池的互斥锁
    std::unordered_set<void*> active_objects_; // 跟踪当前活跃的对象
}; 