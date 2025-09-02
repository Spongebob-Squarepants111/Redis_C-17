#pragma once
#include "ConfigLoader.h"
#include "CachePolicy.h"
#include <string>
#include <chrono>

class Config {
public:
    // 服务器配置
    struct ServerConfig {
        int port;
        std::string host;
        size_t max_events;
        size_t initial_buffer_size;
        size_t max_buffer_size;
        size_t default_buffer_size;
        size_t client_pool_shards;
        size_t client_shard_count;
        size_t client_parser_shard_count;
        size_t server_cache_line_size;
        int max_accept_per_round;
        int max_batch_process;
    };

    // 线程池配置
    struct ThreadPoolConfig {
        size_t read_threads;
        size_t write_threads;
        size_t accept_threads;
        size_t command_threads;
    };

    // 数据存储配置
    struct DataStoreConfig {
        size_t shard_count;
        size_t cache_size;
        bool enable_compression;
        std::string persist_path;
        std::chrono::seconds sync_interval;
        size_t memory_pool_block_size;
        size_t bucket_per_shard;
        size_t cache_shards;
        CachePolicy::Type cache_policy;
        bool adaptive_cache_sizing;
    };

    // 内存池配置
    struct MemoryPoolConfig {
        size_t num_pools;
        size_t max_pool_size;
    };

    // 客户端上下文配置
    struct ClientContextConfig {
        size_t max_pool_size;
        size_t initial_buffer_size;
        size_t max_buffer_size;
        float buffer_grow_factor;
    };

    // 自适应缓存配置
    struct AdaptiveCacheConfig {
        size_t min_capacity;
        size_t max_capacity;
        std::chrono::seconds adjustment_interval;
        double cleanup_threshold;
        double cleanup_target;
    };

public:
    Config() = default;
    bool load(const std::string& config_file);
    
    const ServerConfig& server() const { return server_config_; }
    const ThreadPoolConfig& thread_pool() const { return thread_pool_config_; }
    const DataStoreConfig& datastore() const { return datastore_config_; }
    const MemoryPoolConfig& memory_pool() const { return memory_pool_config_; }
    const ClientContextConfig& client_context() const { return client_context_config_; }
    const AdaptiveCacheConfig& adaptive_cache() const { return adaptive_cache_config_; }

private:
    CachePolicy::Type parse_cache_policy(const std::string& policy_str) const;
    
    ServerConfig server_config_;
    ThreadPoolConfig thread_pool_config_;
    DataStoreConfig datastore_config_;
    MemoryPoolConfig memory_pool_config_;
    ClientContextConfig client_context_config_;
    AdaptiveCacheConfig adaptive_cache_config_;
};
