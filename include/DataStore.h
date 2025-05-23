#pragma once
#include <unordered_map>
#include <string>
#include <optional>
#include <mutex>
#include <vector>
#include <memory>
#include <functional>
#include <list>
#include <chrono>
#include <fstream>
#include <zlib.h>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include "MemoryPool.h"

// 定义缓存行大小为64字节，通常CPU缓存行大小
#define CACHE_LINE_SIZE 64

class DataStore {
public:
    struct Options {
        size_t shard_count;
        size_t cache_size;
        bool enable_compression;
        std::string persist_path;
        std::chrono::seconds sync_interval;
        size_t memory_pool_block_size;  // 内存池块大小

        // 默认配置值
        static constexpr size_t DEFAULT_SHARD_COUNT = 16;
        static constexpr size_t DEFAULT_CACHE_SIZE = 200000;
        static constexpr bool DEFAULT_ENABLE_COMPRESSION = false;
        static constexpr const char* DEFAULT_PERSIST_PATH = "./data/";
        static constexpr long DEFAULT_SYNC_INTERVAL_SEC = 600;
        static constexpr size_t DEFAULT_MEMORY_POOL_BLOCK_SIZE = 1024;

        Options()
            : 
             shard_count(DEFAULT_SHARD_COUNT)
            ,cache_size(DEFAULT_CACHE_SIZE)
            , enable_compression(DEFAULT_ENABLE_COMPRESSION)
            , persist_path(DEFAULT_PERSIST_PATH)
            , sync_interval(DEFAULT_SYNC_INTERVAL_SEC)
            , memory_pool_block_size(DEFAULT_MEMORY_POOL_BLOCK_SIZE) {}
    };

    explicit DataStore(const Options& options = Options{});
    ~DataStore();

    void set(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    void flush(); // 强制持久化

private:
    // LRU缓存实现
    class LRUCache {
        // 使用对齐属性确保CacheEntry位于缓存行边界
        struct alignas(CACHE_LINE_SIZE) CacheEntry {
            std::string key;
            std::string value;
            std::chrono::steady_clock::time_point last_access;
            
            CacheEntry(std::string k, std::string v)
                : key(std::move(k)), value(std::move(v))
                , last_access(std::chrono::steady_clock::now()) {}
                
            // 不再需要next指针，内存池的FreeObject中已经有了
        };

        // 内存池类型定义
        using CacheEntryPool = MemoryPool<CacheEntry>;

    public:
        explicit LRUCache(size_t capacity, size_t pool_block_size = 1024) 
            : capacity_(capacity), entry_pool_(pool_block_size) {}

        void put(const std::string& key, const std::string& value) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_map_.find(key);
            if (it != cache_map_.end()) {
                // 获取旧对象指针并从列表中移除
                CacheEntry* old_entry = &(*it->second);
                cache_list_.erase(it->second);
                // 释放旧对象到内存池
                entry_pool_.deallocate(old_entry);
                // 从map中移除
                cache_map_.erase(it);
            }
            
            // 从内存池分配新对象
            CacheEntry* new_entry = entry_pool_.allocate(key, value);
            auto insert_iter = cache_list_.insert(cache_list_.begin(), *new_entry);
            cache_map_[key] = insert_iter;
            
            // 检查容量并移除多余项
            if (cache_map_.size() > capacity_) {
                // 获取最后一项的引用
                CacheEntry* last_entry = &cache_list_.back();
                // 从map中移除
                cache_map_.erase(last_entry->key);
                // 从列表中移除前先保存引用
                CacheEntry last_copy = cache_list_.back();
                cache_list_.pop_back();
                // 释放对象到内存池
                entry_pool_.deallocate(&last_copy);
            }
        }

        std::optional<std::string> get(const std::string& key) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_map_.find(key);
            if (it == cache_map_.end()) {
                return std::nullopt;
            }
            
            // 获取节点引用并更新访问时间
            CacheEntry& entry = *it->second;
            entry.last_access = std::chrono::steady_clock::now();
            
            // 移动到链表前端
            auto old_pos = it->second;
            CacheEntry entry_copy = *old_pos; // 复制对象内容
            cache_list_.erase(old_pos);
            auto new_pos = cache_list_.insert(cache_list_.begin(), entry_copy);
            it->second = new_pos; // 更新迭代器
            
            return entry_copy.value;
        }

        void remove(const std::string& key) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_map_.find(key);
            if (it != cache_map_.end()) {
                // 保存对象引用以释放内存
                CacheEntry* entry = &(*it->second);
                cache_list_.erase(it->second);
                cache_map_.erase(it);
                // 释放对象到内存池
                entry_pool_.deallocate(entry);
            }
        }

    private:
        size_t capacity_;
        std::list<CacheEntry> cache_list_;
        std::unordered_map<std::string, typename std::list<CacheEntry>::iterator> cache_map_;
        alignas(CACHE_LINE_SIZE) mutable std::mutex mutex_; // 对齐互斥锁以避免伪共享
        CacheEntryPool entry_pool_;  // CacheEntry对象的内存池
    };

    // 分片结构，对齐到缓存行以减少伪共享
    struct alignas(CACHE_LINE_SIZE) Shard {
        std::unordered_map<std::string, std::string> store;
        alignas(CACHE_LINE_SIZE) mutable std::shared_mutex mutex; // 对齐互斥锁
        std::string persist_file;
    };

    // 压缩功能
    static std::string compress(const std::string& data);
    static std::string decompress(const std::string& data);

    // 持久化功能
    void persist_shard(size_t shard_index);
    void load_shard(size_t shard_index);
    void start_sync_thread();
    void sync_routine();

    // 一致性哈希
    size_t get_shard_index(const std::string& key) const;
    uint32_t hash(const std::string& key) const;

    std::vector<std::unique_ptr<Shard>> shards_;
    const size_t shard_count_;
    LRUCache cache_;
    const bool enable_compression_;
    const std::string persist_path_;
    std::chrono::seconds sync_interval_;
    
    // 同步线程
    std::thread sync_thread_;
    alignas(CACHE_LINE_SIZE) std::atomic<bool> should_stop_{false}; // 对齐原子变量
};
