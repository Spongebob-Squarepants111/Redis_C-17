#include "AdaptiveCache.h"
#include <xxhash.h>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <thread>

AdaptiveCache::AdaptiveCache(const Options& options)
    : shard_count_(options.shard_count)
    , shards_(options.shard_count)
    , capacity_(options.initial_capacity)
    , min_capacity_(options.min_capacity)
    , max_capacity_(options.max_capacity)
    , policy_(create_cache_policy(options.policy_type))
    , enable_adaptive_sizing_(options.enable_adaptive_sizing)
    , adjustment_interval_(options.adjustment_interval)
    , cleanup_threshold_(options.cleanup_threshold)
    , cleanup_target_(options.cleanup_target)
    , start_time_(std::chrono::steady_clock::now()) {
        
    // 初始化分片
    for (size_t i = 0; i < shard_count_; ++i) {
        shards_[i] = std::make_unique<Shard>(options.memory_pool_block_size);
    }
    
    // 启动自适应调整线程
    if (enable_adaptive_sizing_) {
        adjustment_thread_ = std::thread(&AdaptiveCache::adjustment_thread_func, this);
    }
}

AdaptiveCache::~AdaptiveCache() {
    // 停止自适应调整线程
    should_stop_ = true;
    if (adjustment_thread_.joinable()) {
        adjustment_thread_.join();
    }
    
    // 清空缓存
    clear();
}

void AdaptiveCache::put(const std::string& key, const std::string& value) {
    auto& shard = get_shard(key);
    
    {
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        
        // 检查是否已经存在
        auto it = shard.item_map.find(key);
        if (it != shard.item_map.end()) {
            // 更新现有项
            it->second->value = value;
            
            // 通知策略访问事件
            std::lock_guard<std::mutex> policy_lock(policy_mutex_);
            policy_->on_access(key, *it->second);
            
            // 根据策略移动项到合适的位置
            if (policy_->type() == CachePolicy::Type::LRU) {
                // 对于LRU，移到列表前端
                shard.items.splice(shard.items.begin(), shard.items, it->second);
            }
            
            return;
        }
        
        // 检查是否需要驱逐
        if (size() >= capacity_) {
            size_t items_to_evict = calculate_items_to_evict();
            evict_items(shard, items_to_evict);
        }
        
        // 从对象池分配新项
        CacheItem* new_item = shard.allocate_item(key, value);
        
        // 插入到列表和映射
        auto iter = shard.items.insert(shard.items.begin(), *new_item);
        shard.item_map[key] = iter;
        
        // 通知策略新项添加
        std::lock_guard<std::mutex> policy_lock(policy_mutex_);
        policy_->on_add(key, *new_item);
        
        // 更新缓存大小
        update_size_stats(1);
    }
    
    // 检查并清理过期项（在锁外执行以减少锁持有时间）
    if (static_cast<double>(size()) / capacity_ > cleanup_threshold_) {
        cleanup_expired(shard);
    }
}

std::optional<std::string> AdaptiveCache::get(const std::string& key) {
    auto& shard = get_shard(key);
    
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    
    auto it = shard.item_map.find(key);
    if (it == shard.item_map.end()) {
        // 缓存未命中
        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    
    auto& item = *it->second;
    
    // 检查是否应该驱逐
    {
        std::lock_guard<std::mutex> policy_lock(policy_mutex_);
        if (policy_->should_evict(key, item)) {
            // 项过期，移除并返回未命中
            lock.unlock(); // 需要先释放共享锁
            remove(key);
            expirations_.fetch_add(1, std::memory_order_relaxed);
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        
        // 通知策略访问事件
        policy_->on_access(key, item);
    }
    
    // 对于LRU策略，移动到列表前端
    if (policy_->type() == CachePolicy::Type::LRU) {
        // 需要转换为写锁
        lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(shard.mutex);
        
        // 重新检查，因为可能在释放共享锁后被修改
        auto it_recheck = shard.item_map.find(key);
        if (it_recheck != shard.item_map.end()) {
            shard.items.splice(shard.items.begin(), shard.items, it_recheck->second);
            hits_.fetch_add(1, std::memory_order_relaxed);
            return it_recheck->second->value;
        } else {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
    }
    
    // 对于其他策略，不移动位置
    hits_.fetch_add(1, std::memory_order_relaxed);
    return item.value;
}

bool AdaptiveCache::contains(const std::string& key) {
    auto& shard = get_shard(key);
    
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    return shard.item_map.find(key) != shard.item_map.end();
}

bool AdaptiveCache::remove(const std::string& key) {
    auto& shard = get_shard(key);
    
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    
    auto it = shard.item_map.find(key);
    if (it == shard.item_map.end()) {
        return false;
    }
    
    // 保存指针用于后续释放
    CacheItem* item_ptr = &(*it->second);
    
    // 通知策略项被驱逐
    {
        std::lock_guard<std::mutex> policy_lock(policy_mutex_);
        policy_->on_eviction(key, *item_ptr);
    }
    
    // 从列表移除
    shard.items.erase(it->second);
    
    // 从映射移除
    shard.item_map.erase(it);
    
    // 释放回对象池
    shard.deallocate_item(item_ptr);
    
    // 更新缓存大小
    update_size_stats(-1);
    
    return true;
}

void AdaptiveCache::clear() {
    for (size_t i = 0; i < shard_count_; ++i) {
        auto& shard = *shards_[i];
        
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        
        // 释放所有项回对象池
        for (auto& item : shard.items) {
            CacheItem* item_ptr = &item;
            shard.deallocate_item(item_ptr);
        }
        
        shard.items.clear();
        shard.item_map.clear();
    }
    
    // 重置大小统计
    size_.store(0, std::memory_order_relaxed);
}

size_t AdaptiveCache::size() const {
    return size_.load(std::memory_order_relaxed);
}

size_t AdaptiveCache::capacity() const {
    return capacity_.load(std::memory_order_relaxed);
}

double AdaptiveCache::hit_ratio() const {
    size_t hits = hits_.load(std::memory_order_relaxed);
    size_t misses = misses_.load(std::memory_order_relaxed);
    size_t total = hits + misses;
    
    if (total == 0) {
        return 0.0;
    }
    
    return static_cast<double>(hits) / total;
}

void AdaptiveCache::set_policy(CachePolicy::Type policy_type) {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    policy_ = create_cache_policy(policy_type);
}

CachePolicy::Type AdaptiveCache::get_policy_type() const {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    return policy_->type();
}

std::string AdaptiveCache::get_policy_name() const {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    return policy_->name();
}

void AdaptiveCache::enable_adaptive_sizing(bool enable) {
    if (enable == enable_adaptive_sizing_) {
        return;
    }
    
    enable_adaptive_sizing_ = enable;
    
    if (enable) {
        // 启动自适应调整线程
        if (!adjustment_thread_.joinable()) {
            should_stop_ = false;
            adjustment_thread_ = std::thread(&AdaptiveCache::adjustment_thread_func, this);
        }
    } else {
        // 停止自适应调整线程
        should_stop_ = true;
        if (adjustment_thread_.joinable()) {
            adjustment_thread_.join();
        }
    }
}

bool AdaptiveCache::is_adaptive_sizing_enabled() const {
    return enable_adaptive_sizing_;
}

void AdaptiveCache::set_capacity(size_t new_capacity) {
    // 确保容量在允许的范围内
    new_capacity = std::max(min_capacity_, std::min(max_capacity_, new_capacity));
    
    size_t old_capacity = capacity_.exchange(new_capacity);
    
    // 如果缩小了容量，可能需要驱逐一些项
    if (new_capacity < old_capacity && size() > new_capacity) {
        size_t items_to_evict = size() - new_capacity;
        
        // 平均分配到各个分片
        size_t per_shard = items_to_evict / shard_count_ + 1;
        
        for (size_t i = 0; i < shard_count_ && items_to_evict > 0; ++i) {
            size_t to_evict = std::min(per_shard, items_to_evict);
            evict_items(*shards_[i], to_evict);
            items_to_evict -= std::min(to_evict, items_to_evict);
        }
    }
}

AdaptiveCache::Stats AdaptiveCache::get_stats() const {
    Stats stats;
    
    stats.size = size();
    stats.capacity = capacity();
    stats.hits = hits_.load(std::memory_order_relaxed);
    stats.misses = misses_.load(std::memory_order_relaxed);
    stats.hit_ratio = hit_ratio();
    stats.policy_name = get_policy_name();
    stats.evictions = evictions_.load(std::memory_order_relaxed);
    stats.expirations = expirations_.load(std::memory_order_relaxed);
    
    // 估计内存使用量
    size_t approx_mem = 0;
    for (size_t i = 0; i < shard_count_; ++i) {
        std::shared_lock<std::shared_mutex> lock(shards_[i]->mutex);
        // 每个项大约需要：键大小 + 值大小 + CacheItem对象大小 + 映射开销
        for (const auto& item : shards_[i]->items) {
            approx_mem += item.key.size() + item.value.size() + sizeof(CacheItem) + 32; // 32为映射开销估计
        }
    }
    stats.memory_usage = approx_mem;
    
    // 计算运行时间
    auto now = std::chrono::steady_clock::now();
    stats.uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
    
    return stats;
}

size_t AdaptiveCache::get_shard_index(const std::string& key) const {
    // 使用xxHash计算哈希值
    uint32_t hash = XXH32(key.data(), key.size(), 0);
    return hash % shard_count_;
}

AdaptiveCache::Shard& AdaptiveCache::get_shard(const std::string& key) {
    size_t index = get_shard_index(key);
    return *shards_[index];
}

void AdaptiveCache::evict_items(Shard& shard, size_t count) {
    if (count == 0) return;
    
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    
    // 找出要驱逐的项
    std::vector<std::pair<std::string, double>> candidates;
    candidates.reserve(shard.items.size());
    
    // 计算每个项的优先级
    {
        std::lock_guard<std::mutex> policy_lock(policy_mutex_);
        for (const auto& item : shard.items) {
            // 检查是否应该过期
            if (policy_->should_evict(item.key, item)) {
                candidates.emplace_back(item.key, std::numeric_limits<double>::max()); // 最高优先级
            } else {
                double priority = policy_->get_priority(item.key, item);
                candidates.emplace_back(item.key, priority);
            }
        }
    }
    
    // 按优先级排序（优先级高的先驱逐）
    std::sort(candidates.begin(), candidates.end(), 
        [](const auto& a, const auto& b) { 
            return a.second > b.second; 
        });
    
    // 驱逐指定数量的项
    size_t to_evict = std::min(count, candidates.size());
    for (size_t i = 0; i < to_evict; ++i) {
        const auto& key = candidates[i].first;
        auto it = shard.item_map.find(key);
        if (it != shard.item_map.end()) {
            // 保存指针用于释放
            CacheItem* item_ptr = &(*it->second);
            
            // 通知策略
            {
                std::lock_guard<std::mutex> policy_lock(policy_mutex_);
                policy_->on_eviction(key, *item_ptr);
            }
            
            // 从列表和映射中移除
            shard.items.erase(it->second);
            shard.item_map.erase(it);
            
            // 释放回对象池
            shard.deallocate_item(item_ptr);
            
            // 更新统计
            update_size_stats(-1);
            evictions_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void AdaptiveCache::cleanup_expired(Shard& shard) {
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    
    // 找出过期的项
    std::vector<std::string> expired_keys;
    
    {
        std::lock_guard<std::mutex> policy_lock(policy_mutex_);
        for (const auto& item : shard.items) {
            if (policy_->should_evict(item.key, item)) {
                expired_keys.push_back(item.key);
            }
        }
    }
    
    // 移除过期项
    for (const auto& key : expired_keys) {
        auto it = shard.item_map.find(key);
        if (it != shard.item_map.end()) {
            // 保存指针用于释放
            CacheItem* item_ptr = &(*it->second);
            
            // 从列表和映射中移除
            shard.items.erase(it->second);
            shard.item_map.erase(it);
            
            // 释放回对象池
            shard.deallocate_item(item_ptr);
            
            // 更新统计
            update_size_stats(-1);
            expirations_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void AdaptiveCache::adjustment_thread_func() {
    while (!should_stop_) {
        // 休眠指定间隔
        for (int i = 0; i < adjustment_interval_.count() && !should_stop_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (should_stop_) break;
        
        // 检查是否需要调整大小
        if (!enable_adaptive_sizing_) continue;
        
        // 获取策略建议
        int adjustment = 0;
        {
            std::lock_guard<std::mutex> lock(policy_mutex_);
            adjustment = policy_->get_size_adjustment();
        }
        
        if (adjustment != 0) {
            // 根据当前大小和建议计算新容量
            size_t current = capacity();
            double factor = 1.0 + (adjustment / 100.0);
            size_t new_capacity = static_cast<size_t>(current * factor);
            
            // 限制在最小/最大范围内
            new_capacity = std::max(min_capacity_, std::min(max_capacity_, new_capacity));
            
            // 设置新容量
            if (new_capacity != current) {
                set_capacity(new_capacity);
            }
        }
    }
}

size_t AdaptiveCache::calculate_items_to_evict() const {
    size_t current_size = size();
    size_t current_capacity = capacity();
    
    // 如果超出容量，计算需要驱逐的数量
    if (current_size > current_capacity) {
        return current_size - current_capacity + 1; // 多驱逐一个作为缓冲
    }
    
    // 如果使用率高，按照目标比例驱逐
    double usage_ratio = static_cast<double>(current_size) / current_capacity;
    if (usage_ratio > cleanup_threshold_) {
        size_t target_size = static_cast<size_t>(current_capacity * cleanup_target_);
        if (current_size > target_size) {
            return current_size - target_size;
        }
    }
    
    return 1; // 至少驱逐一个
}

void AdaptiveCache::update_size_stats(int delta) {
    if (delta > 0) {
        size_.fetch_add(delta, std::memory_order_relaxed);
    } else if (delta < 0) {
        size_.fetch_sub(-delta, std::memory_order_relaxed);
    }
} 