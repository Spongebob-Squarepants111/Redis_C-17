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
#include "ThreadAffinity.h"

// 统一分片常量
constexpr size_t OPTIMAL_SHARD_COUNT = 16;

class WorkerThread {
public:
    WorkerThread(int worker_id, std::shared_ptr<CommandHandler> handler, int cpu_id = -1);
    ~WorkerThread();
    
    void start();
    void stop();
    void add_client(int client_fd);
    void remove_client(int client_fd);
    
    // 线程亲和性相关
    void set_cpu_affinity(int cpu_id);
    int get_cpu_affinity() const { return cpu_id_; }
    bool is_affinity_enabled() const { return cpu_id_ >= 0; }
    
    // 统计信息
    size_t get_client_count() const { return client_count_.load(); }
    uint64_t get_processed_commands() const { return processed_commands_.load(); }

private:
    void worker_loop();
    void handle_client_event(int client_fd, uint32_t events);
    void process_client_data(int client_fd);
    void send_response(int client_fd, const std::string& response);
    
    int worker_id_;
    int cpu_id_;  // CPU亲和性绑定的核心ID (-1表示未绑定)
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

class ThreadPool {
public:
    // 构造函数选项
    struct Options {
        bool enable_cpu_affinity = true;    // 是否启用CPU亲和性
        bool auto_detect_topology = true;   // 是否自动检测CPU拓扑
        std::vector<int> custom_cpu_assignment; // 自定义CPU分配
    };
    
    ThreadPool(size_t worker_count, std::shared_ptr<CommandHandler> handler);
    ThreadPool(size_t worker_count, std::shared_ptr<CommandHandler> handler, const Options& options);
    ~ThreadPool();
    
    void start();
    void stop();
    
    // 负载均衡：将客户端分配给负载最轻的Worker
    void assign_client(int client_fd);
    void remove_client(int client_fd);
    
    // CPU亲和性管理
    void enable_cpu_affinity(bool enable);
    bool is_cpu_affinity_enabled() const { return options_.enable_cpu_affinity; }
    void print_cpu_assignment() const;
    
    // 统计信息
    struct Stats {
        size_t total_clients;
        uint64_t total_commands;
        std::vector<size_t> worker_clients;
        std::vector<uint64_t> worker_commands;
        std::vector<int> worker_cpu_assignments; // 工作线程CPU分配
    };
    
    Stats get_stats() const;
    
private:
    std::vector<std::unique_ptr<WorkerThread>> workers_;
    std::atomic<size_t> current_worker_{0};  // 轮询分配
    std::shared_ptr<CommandHandler> handler_;
    Options options_;  // 线程池选项
    std::vector<int> cpu_assignments_;  // CPU分配方案
    
    // 客户端到Worker的映射
    std::unordered_map<int, int> client_to_worker_;
    std::mutex mapping_mutex_;
    
    // 初始化CPU分配
    void initialize_cpu_assignment(size_t worker_count);
};
