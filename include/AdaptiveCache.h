#pragma once
#include <unordered_map>
#include <string>
#include <memory>
#include <list>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <algorithm>
#include <limits>
#include <utility>
#include <optional>
#include <atomic>
#include <thread>
#include "CachePolicy.h"
#include "MemoryPool.h"

// 可配置和自适应的缓存系统
class AdaptiveCache {
public:
    // 缓存配置选项
    struct Options {
        // 缓存分片数
        size_t shard_count;
        
        // 初始缓存大小
        size_t initial_capacity;
        
        // 最小缓存容量
        size_t min_capacity;
        
        // 最大缓存容量
        size_t max_capacity;
        
        // 缓存策略类型
        CachePolicy::Type policy_type;
        
        // 自适应大小调整间隔（秒）
        std::chrono::seconds adjustment_interval;
        
        // 内存池块大小
        size_t memory_pool_block_size;
        
        // 是否启用自适应大小调整
        bool enable_adaptive_sizing;
        
        // 缓存清理的阈值比例（当使用率超过此值时触发清理）
        double cleanup_threshold;
        
        // 缓存清理的目标比例（清理后的使用率）
        double cleanup_target;
        
        // 构造函数，设置默认值
        Options() 
            : shard_count(16)
            , initial_capacity(100000)
            , min_capacity(10000)
            , max_capacity(10000000)
            , policy_type(CachePolicy::Type::LRU)
            , adjustment_interval(std::chrono::minutes(5))
            , memory_pool_block_size(4096)
            , enable_adaptive_sizing(true)
            , cleanup_threshold(0.9)
            , cleanup_target(0.7) {}
    };
    
    // 缓存项，包含值和元数据
    struct CacheItem : public CacheItemMetrics {
        std::string key;
        std::string value;
        
        CacheItem(std::string k, std::string v)
            : key(std::move(k)), value(std::move(v)) {}
    };
    
    // 构造函数
    explicit AdaptiveCache(const Options& options = Options{});
    
    // 禁止拷贝和赋值
    AdaptiveCache(const AdaptiveCache&) = delete;
    AdaptiveCache& operator=(const AdaptiveCache&) = delete;
    
    // 析构函数
    ~AdaptiveCache();
    
    // 基本操作接口
    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);
    bool contains(const std::string& key);
    bool remove(const std::string& key);
    
    // 管理接口
    void clear();
    size_t size() const;
    size_t capacity() const;
    double hit_ratio() const;
    
    // 策略管理
    void set_policy(CachePolicy::Type policy_type);
    CachePolicy::Type get_policy_type() const;
    std::string get_policy_name() const;
    
    // 自适应大小调整相关
    void enable_adaptive_sizing(bool enable);
    bool is_adaptive_sizing_enabled() const;
    void set_capacity(size_t new_capacity);
    
    // 统计信息
    struct Stats {
        size_t size = 0;                // 当前项数
        size_t capacity = 0;            // 当前容量
        size_t hits = 0;                // 命中次数
        size_t misses = 0;              // 未命中次数
        double hit_ratio = 0.0;         // 命中率
        std::string policy_name;        // 策略名称
        size_t evictions = 0;           // 驱逐次数
        size_t expirations = 0;         // 过期次数
        size_t memory_usage = 0;        // 估计内存用量（字节）
        std::chrono::seconds uptime{0}; // 运行时间
    };
    
    // 获取缓存统计信息
    Stats get_stats() const;

private:
    // 缓存分片，每个分片有独立的锁
    struct Shard {
        // 使用list存储缓存项，支持快速移动
        using ItemList = std::list<CacheItem>;
        using ItemIterator = typename ItemList::iterator;
        using KeyToItemMap = std::unordered_map<std::string, ItemIterator>;
        
        ItemList items;                // 缓存项列表
        KeyToItemMap item_map;         // 键到缓存项的映射
        mutable std::shared_mutex mutex; // 分片锁
        MemoryPool<CacheItem> item_pool; // 缓存项对象池
        
        explicit Shard(size_t pool_block_size) : item_pool(pool_block_size) {}
        
        // 从内存池分配新的缓存项
        CacheItem* allocate_item(const std::string& key, const std::string& value) {
            return item_pool.allocate(key, value);
        }
        
        // 释放缓存项回内存池
        void deallocate_item(CacheItem* item) {
            item_pool.deallocate(item);
        }
    };
    
    // 决定key应该在哪个分片
    size_t get_shard_index(const std::string& key) const;
    
    // 获取特定分片
    Shard& get_shard(const std::string& key);
    
    // 驱逐过期或低优先级的项目
    void evict_items(Shard& shard, size_t count);
    
    // 检查并清理过期项
    void cleanup_expired(Shard& shard);
    
    // 定期自适应调整大小的线程函数
    void adjustment_thread_func();
    
    // 计算应该驱逐的项数
    size_t calculate_items_to_evict() const;
    
    // 更新缓存大小统计
    void update_size_stats(int delta);
    
    // 基本配置
    const size_t shard_count_;
    std::vector<std::unique_ptr<Shard>> shards_;
    
    // 缓存大小限制
    std::atomic<size_t> capacity_;
    const size_t min_capacity_;
    const size_t max_capacity_;
    
    // 策略和自适应调整
    std::unique_ptr<CachePolicy> policy_;
    mutable std::mutex policy_mutex_;
    bool enable_adaptive_sizing_;
    std::chrono::seconds adjustment_interval_;
    
    // 统计信息
    alignas(CACHE_LINE_SIZE) mutable std::mutex stats_mutex_;
    std::atomic<size_t> size_{0};
    std::atomic<size_t> hits_{0};
    std::atomic<size_t> misses_{0};
    std::atomic<size_t> evictions_{0};
    std::atomic<size_t> expirations_{0};
    
    // 清理阈值
    double cleanup_threshold_;
    double cleanup_target_;
    
    // 自适应调整线程
    std::thread adjustment_thread_;
    std::atomic<bool> should_stop_{false};
    
    // 启动时间
    std::chrono::steady_clock::time_point start_time_;
}; 