#pragma once
#include <string>
#include <chrono>
#include <cstdint>
#include <memory>

// 缓存项的基类，记录基本的缓存指标
class CacheItemMetrics {
public:
    // 访问时间
    std::chrono::steady_clock::time_point last_access_time;
    
    // 访问次数
    uint32_t access_count = 0;
    
    CacheItemMetrics() 
        : last_access_time(std::chrono::steady_clock::now()) {}
    
    // 记录访问，更新统计信息
    virtual void record_access() {
        last_access_time = std::chrono::steady_clock::now();
        access_count++;
    }
    
    // 重置计数
    virtual void reset() {
        access_count = 0;
    }
};

// 缓存策略接口
class CachePolicy {
public:
    // 策略类型枚举
    enum class Type {
        LRU  // 最近最少使用
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

// 创建缓存策略的工厂函数
inline std::unique_ptr<CachePolicy> create_cache_policy(CachePolicy::Type type) {
    // 只支持LRU策略
    return std::make_unique<LRUPolicy>();
}