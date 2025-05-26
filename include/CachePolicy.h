#pragma once
#include <string>
#include <chrono>
#include <cstdint>

// 缓存项的基类，记录基本的缓存指标
class CacheItemMetrics {
public:
    // 访问时间
    std::chrono::steady_clock::time_point last_access_time;
    
    // 访问次数
    uint32_t access_count = 0;
    
    // 总累计访问次数（用于LFU）
    uint64_t total_access_count = 0;
    
    // 用于LFU的权重衰减
    double frequency_weight = 1.0;
    
    CacheItemMetrics() 
        : last_access_time(std::chrono::steady_clock::now()) {}
    
    // 记录访问，更新统计信息
    virtual void record_access() {
        last_access_time = std::chrono::steady_clock::now();
        access_count++;
        total_access_count++;
    }
    
    // 重置计数
    virtual void reset() {
        access_count = 0;
    }
    
    // 老化计数（用于LFU）
    virtual void age(double factor = 0.5) {
        frequency_weight *= factor;
    }
};

// 缓存策略接口
class CachePolicy {
public:
    // 策略类型枚举
    enum class Type {
        LRU,  // 最近最少使用
        LFU,  // 最不经常使用
        FIFO, // 先进先出
        TLRU, // 时间感知LRU (有TTL)
        ARC   // 自适应替换缓存
    };
    
    // 虚析构函数
    virtual ~CachePolicy() = default;
    
    // 获取策略类型
    virtual Type type() const = 0;
    
    // 获取策略名称
    virtual std::string name() const = 0;
    
    // 记录键访问
    virtual void on_access(const std::string& key, CacheItemMetrics& metrics) = 0;
    
    // 记录键驱逐
    virtual void on_eviction(const std::string& key, CacheItemMetrics& metrics) = 0;
    
    // 记录键添加
    virtual void on_add(const std::string& key, CacheItemMetrics& metrics) = 0;
    
    // 判断是否应该从缓存中移除（例如TTL过期）
    virtual bool should_evict(const std::string& key, const CacheItemMetrics& metrics) const = 0;
    
    // 获取优先级（用于排序要驱逐的项）
    virtual double get_priority(const std::string& key, const CacheItemMetrics& metrics) const = 0;
    
    // 获取推荐的缓存大小增量（用于自适应调整）
    virtual int get_size_adjustment() const = 0;
    
    // 重置策略状态
    virtual void reset() = 0;
};

// LRU (最近最少使用) 策略
class LRUPolicy : public CachePolicy {
public:
    Type type() const override { return Type::LRU; }
    std::string name() const override { return "LRU"; }
    
    void on_access(const std::string& key, CacheItemMetrics& metrics) override {
        metrics.record_access();
    }
    
    void on_eviction(const std::string& key, CacheItemMetrics& metrics) override {
        // LRU不需要特殊处理
    }
    
    void on_add(const std::string& key, CacheItemMetrics& metrics) override {
        metrics.record_access();
    }
    
    bool should_evict(const std::string& key, const CacheItemMetrics& metrics) const override {
        return false; // LRU不基于时间驱逐
    }
    
    double get_priority(const std::string& key, const CacheItemMetrics& metrics) const override {
        // 返回上次访问时间的时间戳（越旧优先级越高）
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            metrics.last_access_time.time_since_epoch()).count();
    }
    
    int get_size_adjustment() const override {
        return 0; // 基本LRU不调整大小
    }
    
    void reset() override {
        // 基本LRU不需要重置
    }
};

// LFU (最不经常使用) 策略
class LFUPolicy : public CachePolicy {
private:
    // 衰减因子，用于老化访问计数
    double decay_factor_ = 0.95;
    
    // 衰减间隔（分钟）
    int decay_interval_minutes_ = 60;
    
    // 上次衰减时间
    std::chrono::steady_clock::time_point last_decay_time_ = std::chrono::steady_clock::now();
    
    // 缓存命中率统计
    int total_accesses_ = 0;
    int cache_hits_ = 0;

public:
    LFUPolicy(double decay_factor = 0.95, int decay_interval_minutes = 60)
        : decay_factor_(decay_factor), decay_interval_minutes_(decay_interval_minutes) {}

    Type type() const override { return Type::LFU; }
    std::string name() const override { return "LFU"; }
    
    void on_access(const std::string& key, CacheItemMetrics& metrics) override {
        total_accesses_++;
        cache_hits_++;
        metrics.record_access();
        
        // 检查是否需要衰减
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - last_decay_time_).count();
        
        if (elapsed >= decay_interval_minutes_) {
            metrics.age(decay_factor_);
            last_decay_time_ = now;
        }
    }
    
    void on_eviction(const std::string& key, CacheItemMetrics& metrics) override {
        // LFU不需要特殊处理
    }
    
    void on_add(const std::string& key, CacheItemMetrics& metrics) override {
        metrics.record_access();
    }
    
    bool should_evict(const std::string& key, const CacheItemMetrics& metrics) const override {
        return false; // LFU不基于时间驱逐
    }
    
    double get_priority(const std::string& key, const CacheItemMetrics& metrics) const override {
        // 频率越低，优先级越高（应该被淘汰）
        if (metrics.access_count == 0) return std::numeric_limits<double>::max();
        
        // 使用加权频率
        double weighted_count = metrics.access_count * metrics.frequency_weight;
        return 1.0 / weighted_count;
    }
    
    int get_size_adjustment() const override {
        // 根据命中率调整大小
        if (total_accesses_ < 1000) return 0; // 样本太少，不调整
        
        double hit_ratio = static_cast<double>(cache_hits_) / total_accesses_;
        
        if (hit_ratio > 0.9) return 10; // 命中率高，可以增加缓存大小
        if (hit_ratio < 0.5) return -10; // 命中率低，减少缓存大小
        
        return 0;
    }
    
    void reset() override {
        total_accesses_ = 0;
        cache_hits_ = 0;
        last_decay_time_ = std::chrono::steady_clock::now();
    }
};

// FIFO (先进先出) 策略
class FIFOPolicy : public CachePolicy {
public:
    Type type() const override { return Type::FIFO; }
    std::string name() const override { return "FIFO"; }
    
    void on_access(const std::string& key, CacheItemMetrics& metrics) override {
        metrics.access_count++;
        // FIFO不更新访问时间
    }
    
    void on_eviction(const std::string& key, CacheItemMetrics& metrics) override {
        // FIFO不需要特殊处理
    }
    
    void on_add(const std::string& key, CacheItemMetrics& metrics) override {
        // FIFO使用插入时间作为排序依据
        // 不做任何操作，保持初始last_access_time不变
    }
    
    bool should_evict(const std::string& key, const CacheItemMetrics& metrics) const override {
        return false; // FIFO不基于时间驱逐
    }
    
    double get_priority(const std::string& key, const CacheItemMetrics& metrics) const override {
        // 直接使用插入时间作为优先级
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            metrics.last_access_time.time_since_epoch()).count();
    }
    
    int get_size_adjustment() const override {
        return 0; // 基本FIFO不调整大小
    }
    
    void reset() override {
        // FIFO不需要重置
    }
};

// TLRU (带超时的LRU) 策略
class TLRUPolicy : public LRUPolicy {
private:
    std::chrono::seconds ttl_; // 生存时间
    
public:
    explicit TLRUPolicy(std::chrono::seconds ttl = std::chrono::minutes(30))
        : ttl_(ttl) {}
    
    Type type() const override { return Type::TLRU; }
    std::string name() const override { return "TLRU"; }
    
    bool should_evict(const std::string& key, const CacheItemMetrics& metrics) const override {
        auto now = std::chrono::steady_clock::now();
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - metrics.last_access_time);
        
        return age > ttl_;
    }
    
    void set_ttl(std::chrono::seconds ttl) {
        ttl_ = ttl;
    }
    
    std::chrono::seconds get_ttl() const {
        return ttl_;
    }
};

// 自适应替换缓存 (ARC) 策略
class ARCPolicy : public CachePolicy {
private:
    // 记录T1 (最近使用)和T2 (经常使用)大小的目标比例
    double p_ = 0.5;
    
    // 记录缓存命中和未命中的计数
    int t1_hits_ = 0;
    int t2_hits_ = 0;
    int b1_hits_ = 0; // ghost list hits
    int b2_hits_ = 0; // ghost list hits
    
public:
    Type type() const override { return Type::ARC; }
    std::string name() const override { return "ARC"; }
    
    void on_access(const std::string& key, CacheItemMetrics& metrics) override {
        metrics.record_access();
        
        // 根据命中位置调整p
        if (metrics.access_count == 1) {
            t1_hits_++;
        } else {
            t2_hits_++;
        }
    }
    
    void on_ghost_hit(bool in_b1) {
        // 有幽灵列表命中，调整p值
        if (in_b1) {
            b1_hits_++;
            p_ = std::min(p_ + 0.05, 1.0); // 增加T1的目标大小
        } else {
            b2_hits_++;
            p_ = std::max(p_ - 0.05, 0.0); // 减少T1的目标大小
        }
    }
    
    void on_eviction(const std::string& key, CacheItemMetrics& metrics) override {
        // ARC特殊处理在上层实现
    }
    
    void on_add(const std::string& key, CacheItemMetrics& metrics) override {
        metrics.record_access();
    }
    
    bool should_evict(const std::string& key, const CacheItemMetrics& metrics) const override {
        return false; // ARC不基于时间驱逐
    }
    
    double get_priority(const std::string& key, const CacheItemMetrics& metrics) const override {
        // ARC有自己的逻辑，这里提供一个兼容接口
        if (metrics.access_count <= 1) {
            // 在T1中，按LRU排序
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                metrics.last_access_time.time_since_epoch()).count();
        } else {
            // 在T2中，优先级低于T1
            return std::numeric_limits<double>::lowest();
        }
    }
    
    double get_t1_ratio() const {
        return p_;
    }
    
    int get_size_adjustment() const override {
        // 根据总体命中率决定是否调整缓存大小
        int total_hits = t1_hits_ + t2_hits_ + b1_hits_ + b2_hits_;
        if (total_hits < 1000) return 0; // 样本太小不调整
        
        double ghost_hit_ratio = static_cast<double>(b1_hits_ + b2_hits_) / total_hits;
        
        if (ghost_hit_ratio > 0.2) {
            return 20; // 幽灵列表命中率高，说明缓存太小
        }
        
        double cache_hit_ratio = static_cast<double>(t1_hits_ + t2_hits_) / total_hits;
        if (cache_hit_ratio < 0.5) {
            return -10; // 缓存命中率低，减少大小
        }
        
        return 0;
    }
    
    void reset() override {
        p_ = 0.5;
        t1_hits_ = 0;
        t2_hits_ = 0;
        b1_hits_ = 0;
        b2_hits_ = 0;
    }
};

// 创建缓存策略的工厂函数
inline std::unique_ptr<CachePolicy> create_cache_policy(CachePolicy::Type type) {
    switch (type) {
        case CachePolicy::Type::LRU:
            return std::make_unique<LRUPolicy>();
        case CachePolicy::Type::LFU:
            return std::make_unique<LFUPolicy>();
        case CachePolicy::Type::FIFO:
            return std::make_unique<FIFOPolicy>();
        case CachePolicy::Type::TLRU:
            return std::make_unique<TLRUPolicy>();
        case CachePolicy::Type::ARC:
            return std::make_unique<ARCPolicy>();
        default:
            return std::make_unique<LRUPolicy>(); // 默认使用LRU
    }
} 