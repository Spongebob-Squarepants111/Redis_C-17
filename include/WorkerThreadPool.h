#pragma once
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <sys/epoll.h>
#include "RESPParser.h"
#include "CommandHandler.h"

// 统一分片常量
constexpr size_t OPTIMAL_SHARD_COUNT = 16;

class WorkerThread {
public:
    WorkerThread(int worker_id, std::shared_ptr<CommandHandler> handler);
    ~WorkerThread();
    
    void start();
    void stop();
    void add_client(int client_fd);
    void remove_client(int client_fd);
    
    // 统计信息
    size_t get_client_count() const { return client_count_.load(); }
    uint64_t get_processed_commands() const { return processed_commands_.load(); }

private:
    void worker_loop();
    void handle_client_event(int client_fd, uint32_t events);
    void process_client_data(int client_fd);
    void send_response(int client_fd, const std::string& response);
    
    int worker_id_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    
    // 每个Worker有自己的epoll实例
    int epoll_fd_;
    
    // 客户端管理
    struct ClientInfo {
        std::vector<char> read_buffer;
        std::vector<char> write_buffer;
        size_t read_pos = 0;
        size_t write_pos = 0;
        RESPParser parser;
        std::chrono::steady_clock::time_point last_active;
        
        ClientInfo() : read_buffer(8192), write_buffer(8192) {}
    };
    
    std::unordered_map<int, std::unique_ptr<ClientInfo>> clients_;
    std::mutex clients_mutex_;
    std::atomic<size_t> client_count_{0};
    
    // 命令处理器
    std::shared_ptr<CommandHandler> handler_;
    
    // 统计
    std::atomic<uint64_t> processed_commands_{0};
};

class WorkerThreadPool {
public:
    WorkerThreadPool(size_t worker_count, std::shared_ptr<CommandHandler> handler);
    ~WorkerThreadPool();
    
    void start();
    void stop();
    
    // 负载均衡：将客户端分配给负载最轻的Worker
    void assign_client(int client_fd);
    void remove_client(int client_fd);
    
    // 统计信息
    struct Stats {
        size_t total_clients;
        uint64_t total_commands;
        std::vector<size_t> worker_clients;
        std::vector<uint64_t> worker_commands;
    };
    
    Stats get_stats() const;
    
private:
    std::vector<std::unique_ptr<WorkerThread>> workers_;
    std::atomic<size_t> current_worker_{0};  // 轮询分配
    std::shared_ptr<CommandHandler> handler_;
    
    // 客户端到Worker的映射
    std::unordered_map<int, int> client_to_worker_;
    std::mutex mapping_mutex_;
};
