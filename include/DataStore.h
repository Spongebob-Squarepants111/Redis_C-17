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

class DataStore {
public:
    struct Options {
        size_t shard_count;
        size_t cache_size;
        bool enable_compression;
        std::string persist_path;
        std::chrono::seconds sync_interval;

        // 默认配置值
        static constexpr size_t DEFAULT_SHARD_COUNT = 16;
        static constexpr size_t DEFAULT_CACHE_SIZE = 200000;
        static constexpr bool DEFAULT_ENABLE_COMPRESSION = false;
        static constexpr const char* DEFAULT_PERSIST_PATH = "./data/";
        static constexpr long DEFAULT_SYNC_INTERVAL_SEC = 600;

        Options()
            : 
             shard_count(DEFAULT_SHARD_COUNT)
            ,cache_size(DEFAULT_CACHE_SIZE)
            , enable_compression(DEFAULT_ENABLE_COMPRESSION)
            , persist_path(DEFAULT_PERSIST_PATH)
            , sync_interval(DEFAULT_SYNC_INTERVAL_SEC){}
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
        struct CacheEntry {
            std::string key;
            std::string value;
            std::chrono::steady_clock::time_point last_access;
            
            CacheEntry(std::string k, std::string v)
                : key(std::move(k)), value(std::move(v))
                , last_access(std::chrono::steady_clock::now()) {}
        };

    public:
        explicit LRUCache(size_t capacity) : capacity_(capacity) {}

        void put(const std::string& key, const std::string& value) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_map_.find(key);
            if (it != cache_map_.end()) {
                cache_list_.erase(it->second);
            }
            cache_list_.push_front(CacheEntry(key, value));
            cache_map_[key] = cache_list_.begin();
            if (cache_map_.size() > capacity_) {
                auto last = cache_list_.back();
                cache_map_.erase(last.key);
                cache_list_.pop_back();
            }
        }

        std::optional<std::string> get(const std::string& key) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_map_.find(key);
            if (it == cache_map_.end()) {
                return std::nullopt;
            }
            auto list_it = it->second;
            cache_list_.splice(cache_list_.begin(), cache_list_, list_it);
            return list_it->value;
        }

        void remove(const std::string& key) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_map_.find(key);
            if (it != cache_map_.end()) {
                cache_list_.erase(it->second);
                cache_map_.erase(it);
            }
        }

    private:
        size_t capacity_;
        std::list<CacheEntry> cache_list_;
        std::unordered_map<std::string, typename std::list<CacheEntry>::iterator> cache_map_;
        mutable std::mutex mutex_;
    };

    // 分片结构
    struct Shard {
        std::unordered_map<std::string, std::string> store;
        mutable std::shared_mutex mutex;
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
    std::atomic<bool> should_stop_{false};
};
