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

class RedisServer {
private:
    static constexpr size_t MAX_EVENTS = 8192;                 // 增加事件处理容量
    static constexpr size_t INITIAL_BUFFER_SIZE = 64 * 1024;   // 64KB
    static constexpr size_t MAX_BUFFER_SIZE = INITIAL_BUFFER_SIZE * 4;  // 256KB
    static constexpr size_t DEFAULT_BUFFER_SIZE = 32 * 1024;   // 32KB
    static constexpr size_t CLIENT_POOL_SHARDS = 32;           // 增加客户端池分片数
    static constexpr size_t CLIENT_SHARD_COUNT = 128;          // 增加客户端连接分片数
    static constexpr size_t CLIENT_PARSER_SHARD_COUNT = 64;    // 增加解析器分片数
    static constexpr size_t SERVER_CACHE_LINE_SIZE = 64;       // 缓存行大小，用于对齐
    static constexpr int MAX_ACCEPT_PER_ROUND = 128;           // 每轮最大接受连接数
    static constexpr int MAX_BATCH_PROCESS = 64;               // 每次批量处理的命令数
    static constexpr int IO_URING_QUEUE_DEPTH = 1024;          // io_uring队列深度（如果系统支持）
    
    // 使用ClientContextPool中定义的ClientContext和ClientContextPtr
    using ClientContext = ClientContextPool::ClientContext;
    using ClientContextPtr = ClientContextPool::ClientContextPtr;

    // 线程池配置结构体
    struct ThreadPoolConfig {
        size_t read_threads;
        size_t write_threads;
        size_t accept_threads;
        size_t command_threads;
        
        ThreadPoolConfig(size_t hardware_threads = std::thread::hardware_concurrency()) 
            : read_threads(hardware_threads * 2)
            , write_threads(hardware_threads)
            , accept_threads(4) 
            , command_threads(hardware_threads * 2) {}
    };

private:
    const int port_;
    const std::string host_;
    int server_fd_;
    int epfd;
    ThreadPoolConfig thread_config_;

    // 客户端上下文池
    ClientContextPool client_pool_;
    
    // 分片连接管理
    std::array<std::unordered_map<int, ClientContextPtr>, CLIENT_SHARD_COUNT> clients_;
    std::array<std::mutex, CLIENT_SHARD_COUNT> clients_mutex_;
    
    // 分片解析器映射表
    std::array<std::unordered_map<int, RESPParser>, CLIENT_PARSER_SHARD_COUNT> parsers_;
    std::array<std::mutex, CLIENT_PARSER_SHARD_COUNT> parsers_mutex_;
    
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
    explicit RedisServer(int port, const std::string& host = "0.0.0.0", const ThreadPoolConfig& config = ThreadPoolConfig());
    ~RedisServer();
    void run();
    void print_stats(); // 输出服务器统计信息

private:
    void epoll_loop();
    void handle_read(int client_fd);
    void handle_write_ready(int client_fd);
    void optimize_socket(int sockfd); // 新方法：优化socket参数
};