#pragma once
#include <unordered_map>
#include <memory>
#include <vector>
#include <mutex>
#include <array>
#include "ThreadPool.h"
#include "CommandHandler.h"
#include "ClientContextPool.h"
#include "RESPParser.h"
#include "Config.h"

class RedisServer {
private:
    // 使用ClientContextPool中定义的ClientContext和ClientContextPtr
    using ClientContext = ClientContextPool::ClientContext;
    using ClientContextPtr = ClientContextPool::ClientContextPtr;

private:
    Config config_;
    int server_fd_;
    int epfd;

    // 客户端上下文池
    ClientContextPool client_pool_;
    
    // 分片连接管理 - 动态大小基于配置
    std::vector<std::unordered_map<int, ClientContextPtr>> clients_;
    std::vector<std::mutex> clients_mutex_;
    
    // 分片解析器映射表 - 动态大小基于配置
    std::vector<std::unordered_map<int, RESPParser>> parsers_;
    std::vector<std::mutex> parsers_mutex_;
    
    // 线程池
    ThreadPool readThreadPool;
    ThreadPool writeThreadPool;
    ThreadPool acceptThreadPool;
    ThreadPool commandThreadPool;  // 专门的命令处理线程池
    
    // 命令处理器
    CommandHandler handler_;

    // 性能统计变量
    std::atomic<uint64_t> total_commands_{0};
    std::atomic<uint64_t> total_connections_{0};
    std::chrono::steady_clock::time_point start_time_;

    void add_client(int client_fd);
    void remove_client(int client_fd);
    ClientContextPtr get_client(int client_fd);
    void handle_client_data(ClientContextPtr client);
    bool try_parse_command(ClientContextPtr client, int client_fd);
    void reset_client_buffers(ClientContextPtr client);
    void reset_client_parser(int client_fd);
    void accept_new_connections();  // 批量接受连接
    RESPParser& get_parser(int client_fd); // 获取解析器
    void process_commands(const std::vector<std::vector<std::string>>& cmds, int client_fd); // 处理命令

public:
    explicit RedisServer(const Config& config);
    ~RedisServer();
    void run();
    void print_stats(); // 输出服务器统计信息

private:
    void epoll_loop();
    void handle_read(int client_fd);
    void handle_write_ready(int client_fd);
    void optimize_socket(int sockfd); // 新方法：优化socket参数
};