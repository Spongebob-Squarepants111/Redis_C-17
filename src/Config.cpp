#include "Config.h"
#include <algorithm>
#include <cctype>
#include <thread>

bool Config::load(const std::string& config_file) {
    ConfigLoader loader;
    if (!loader.load(config_file)) {
        return false;
    }

    // 加载服务器配置
    server_config_.port = loader.get_int("server", "port", 6379);
    server_config_.host = loader.get("server", "host", "127.0.0.1");
    server_config_.max_events = loader.get_size_t("server", "max_events", 4096);
    server_config_.initial_buffer_size = loader.get_size_t("server", "initial_buffer_size", 65536);
    server_config_.max_buffer_size = loader.get_size_t("server", "max_buffer_size", 262144);
    server_config_.default_buffer_size = loader.get_size_t("server", "default_buffer_size", 32768);
    server_config_.client_pool_shards = loader.get_size_t("server", "client_pool_shards", 32);
    server_config_.client_shard_count = loader.get_size_t("server", "client_shard_count", 128);
    server_config_.client_parser_shard_count = loader.get_size_t("server", "client_parser_shard_count", 64);
    server_config_.server_cache_line_size = loader.get_size_t("server", "server_cache_line_size", 64);
    server_config_.max_accept_per_round = loader.get_int("server", "max_accept_per_round", 128);
    server_config_.max_batch_process = loader.get_int("server", "max_batch_process", 64);

    // 加载线程池配置
    size_t hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0) hardware_threads = 4; // 备用值
    
    size_t read_threads = loader.get_size_t("thread_pool", "read_threads", 0);
    size_t write_threads = loader.get_size_t("thread_pool", "write_threads", 0);
    size_t command_threads = loader.get_size_t("thread_pool", "command_threads", 0);
    
    thread_pool_config_.read_threads = (read_threads == 0) ? hardware_threads * 2 : read_threads;
    thread_pool_config_.write_threads = (write_threads == 0) ? hardware_threads : write_threads;
    thread_pool_config_.accept_threads = loader.get_size_t("thread_pool", "accept_threads", 4);
    thread_pool_config_.command_threads = (command_threads == 0) ? hardware_threads * 2 : command_threads;

    // 加载数据存储配置
    datastore_config_.shard_count = loader.get_size_t("datastore", "shard_count", 128);
    datastore_config_.cache_size = loader.get_size_t("datastore", "cache_size", 200000);
    datastore_config_.enable_compression = loader.get_bool("datastore", "enable_compression", false);
    datastore_config_.persist_path = loader.get("datastore", "persist_path", "./data/");
    datastore_config_.sync_interval = std::chrono::seconds(loader.get_int("datastore", "sync_interval_sec", 600));
    datastore_config_.memory_pool_block_size = loader.get_size_t("datastore", "memory_pool_block_size", 4096);
    datastore_config_.bucket_per_shard = loader.get_size_t("datastore", "bucket_per_shard", 16);
    datastore_config_.cache_shards = loader.get_size_t("datastore", "cache_shards", 32);
    datastore_config_.cache_policy = parse_cache_policy(loader.get("datastore", "cache_policy", "LRU"));
    datastore_config_.adaptive_cache_sizing = loader.get_bool("datastore", "adaptive_cache_sizing", true);

    // 加载内存池配置
    memory_pool_config_.num_pools = loader.get_size_t("memory_pool", "num_pools", 10);
    memory_pool_config_.max_pool_size = loader.get_size_t("memory_pool", "max_pool_size", 10000);

    // 加载客户端上下文配置
    client_context_config_.max_pool_size = loader.get_size_t("client_context", "max_pool_size", 100);
    client_context_config_.initial_buffer_size = loader.get_size_t("client_context", "initial_buffer_size", 8192);
    client_context_config_.max_buffer_size = loader.get_size_t("client_context", "max_buffer_size", 524288);
    client_context_config_.buffer_grow_factor = loader.get_float("client_context", "buffer_grow_factor", 1.5f);

    // 加载自适应缓存配置
    adaptive_cache_config_.min_capacity = loader.get_size_t("adaptive_cache", "min_capacity", 1000);
    adaptive_cache_config_.max_capacity = loader.get_size_t("adaptive_cache", "max_capacity", 1000000);
    adaptive_cache_config_.adjustment_interval = std::chrono::seconds(loader.get_int("adaptive_cache", "adjustment_interval_sec", 60));
    adaptive_cache_config_.cleanup_threshold = static_cast<double>(loader.get_float("adaptive_cache", "cleanup_threshold", 0.9f));
    adaptive_cache_config_.cleanup_target = static_cast<double>(loader.get_float("adaptive_cache", "cleanup_target", 0.8f));

    return true;
}

CachePolicy::Type Config::parse_cache_policy(const std::string& policy_str) const {
    std::string lower_policy = policy_str;
    std::transform(lower_policy.begin(), lower_policy.end(), lower_policy.begin(), ::tolower);
    
    if (lower_policy == "lru") {
        return CachePolicy::Type::LRU;
    } else if (lower_policy == "lfu") {
        return CachePolicy::Type::LFU;
    } else if (lower_policy == "tlru") {
        return CachePolicy::Type::TLRU;
    } else if (lower_policy == "fifo") {
        return CachePolicy::Type::FIFO;
    } else if (lower_policy == "arc") {
        return CachePolicy::Type::ARC;
    } else {
        return CachePolicy::Type::LRU; // 默认策略
    }
}
