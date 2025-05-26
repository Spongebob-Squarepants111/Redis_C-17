#pragma once
#include <unordered_map>
#include <string>
#include <string_view>
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
#include "AdaptiveCache.h"
#include "CachePolicy.h"
#include <array>

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
        size_t bucket_per_shard;        // 每个分片的桶数量
        size_t cache_shards;            // 缓存的分片数量
        CachePolicy::Type cache_policy; // 缓存策略类型
        bool adaptive_cache_sizing;     // 是否启用自适应缓存大小调整

        // 默认配置值
        static constexpr size_t DEFAULT_SHARD_COUNT = 128;
        static constexpr size_t DEFAULT_CACHE_SIZE = 200000;
        static constexpr bool DEFAULT_ENABLE_COMPRESSION = false;
        static constexpr const char* DEFAULT_PERSIST_PATH = "./data/";
        static constexpr long DEFAULT_SYNC_INTERVAL_SEC = 600;
        static constexpr size_t DEFAULT_MEMORY_POOL_BLOCK_SIZE = 4096;
        static constexpr size_t DEFAULT_BUCKET_PER_SHARD = 16;  // 每个分片默认16个桶
        static constexpr size_t DEFAULT_CACHE_SHARDS = 32;      // 缓存默认32个分片

        Options()
            : 
             shard_count(DEFAULT_SHARD_COUNT)
            ,cache_size(DEFAULT_CACHE_SIZE)
            , enable_compression(DEFAULT_ENABLE_COMPRESSION)
            , persist_path(DEFAULT_PERSIST_PATH)
            , sync_interval(DEFAULT_SYNC_INTERVAL_SEC)
            , memory_pool_block_size(DEFAULT_MEMORY_POOL_BLOCK_SIZE)
            , bucket_per_shard(DEFAULT_BUCKET_PER_SHARD)
            , cache_shards(DEFAULT_CACHE_SHARDS)
            , cache_policy(CachePolicy::Type::LRU)
            , adaptive_cache_sizing(true) {}
    };

    explicit DataStore(const Options& options = Options{});
    ~DataStore();

    // 使用string_view版本接口，减少字符串拷贝
    void set(std::string_view key, std::string_view value);
    std::optional<std::string> get(std::string_view key);
    bool del(std::string_view key);
    void multi_set(const std::vector<std::pair<std::string_view, std::string_view>>& kvs);
    std::vector<std::optional<std::string>> multi_get(const std::vector<std::string_view>& keys);
    size_t multi_del(const std::vector<std::string_view>& keys);
    void prefetch(const std::vector<std::string_view>& keys);
    
    // 保留原有的std::string接口以保持兼容性
    void set(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    void multi_set(const std::vector<std::pair<std::string, std::string>>& kvs);
    std::vector<std::optional<std::string>> multi_get(const std::vector<std::string>& keys);
    size_t multi_del(const std::vector<std::string>& keys);
    void prefetch(const std::vector<std::string>& keys);
    
    void flush(); // 强制持久化
    
    // 缓存管理接口
    void set_cache_policy(CachePolicy::Type policy_type);
    CachePolicy::Type get_cache_policy() const;
    std::string get_cache_policy_name() const;
    void enable_adaptive_cache(bool enable);
    bool is_adaptive_cache_enabled() const;
    void set_cache_capacity(size_t capacity);
    size_t get_cache_capacity() const;
    double get_cache_hit_ratio() const;
    AdaptiveCache::Stats get_cache_stats() const;

private:
    // 存储桶结构，每个桶有自己的锁
    struct alignas(CACHE_LINE_SIZE) Bucket {
        // 将map替换为多个子map，每个子map有自己的锁，进一步减少锁竞争
        static constexpr size_t SUB_MAPS_COUNT = 8;
        
        struct SubMap {
            std::unordered_map<std::string, std::string> store;
            alignas(CACHE_LINE_SIZE) mutable std::shared_mutex mutex; // 对齐互斥锁
        };
        
        std::array<SubMap, SUB_MAPS_COUNT> sub_maps;
        
        // 计算子map索引
        size_t get_submap_index(const std::string& key) const {
            // 使用简单哈希来确定子map
            return std::hash<std::string>{}(key) % SUB_MAPS_COUNT;
        }
    };

    // 分片结构，包含多个桶
    struct Shard {
        std::vector<std::unique_ptr<Bucket>> buckets;
        std::string persist_file;
        
        explicit Shard(size_t bucket_count) : buckets(bucket_count) {
            for (size_t i = 0; i < bucket_count; ++i) {
                buckets[i] = std::make_unique<Bucket>();
            }
        }
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
    size_t get_bucket_index(const std::string& key, size_t bucket_count) const;
    uint32_t hash(const std::string& key) const;

    std::vector<std::unique_ptr<Shard>> shards_;
    const size_t shard_count_;
    const size_t bucket_per_shard_;
    
    // 使用自适应缓存替代原来的LRUCache
    AdaptiveCache cache_;
    
    const bool enable_compression_;
    const std::string persist_path_;
    std::chrono::seconds sync_interval_;
    
    // 同步线程
    std::thread sync_thread_;
    alignas(CACHE_LINE_SIZE) std::atomic<bool> should_stop_{false}; // 对齐原子变量
};
