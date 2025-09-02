#pragma once
#include "ThreadPool.h"
#include "CommandHandler.h"
#include "DataStore.h"
#include <string>
#include <memory>
#include <atomic>
#include <thread>

class RedisServer {
public:
    struct Config {
        int port = 6379;
        std::string host = "127.0.0.1";
        size_t worker_threads = 32;
        size_t io_threads = 8;
        size_t shard_count = 16;
        size_t max_connections = 10000;
        size_t buffer_size = 32768;
        size_t cache_size_mb = 200;
        bool enable_persistence = true;
        int sync_interval_sec = 300;
    };

public:
    explicit RedisServer(const Config& config);
    ~RedisServer();
    
    void run();
    void stop();
    
    // 统计信息
    struct Stats {
        uint64_t total_connections;
        uint64_t total_commands;
        uint64_t current_connections;
        double commands_per_second;
        std::chrono::seconds uptime;
    };
    
    Stats get_stats() const;

private:
    void accept_loop();
    void print_stats_loop();
    void setup_server_socket();
    void optimize_socket(int sockfd);
    
    Config config_;
    int server_fd_;
    std::atomic<bool> running_{false};
    
    // 简化的组件
    std::shared_ptr<DataStore> datastore_;
    std::shared_ptr<CommandHandler> handler_;
    std::unique_ptr<ThreadPool> worker_pool_;
    
    // 只有一个accept线程
    std::thread accept_thread_;
    std::thread stats_thread_;
    
    // 统计信息
    std::atomic<uint64_t> total_connections_{0};
    std::atomic<uint64_t> current_connections_{0};
    std::chrono::steady_clock::time_point start_time_;
};
