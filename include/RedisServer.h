#pragma once
#include <unordered_map>
#include <memory>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <optional>
#include <chrono>
#include "DoubleBufferThreadPool.h"
#include "CommandHandler.h"

class RedisServer {
private:
    // 客户端上下文结构
    struct ClientContext {
        int fd;                          // 客户端socket文件描述符
        std::vector<char> read_buffer;   // 读缓冲区
        std::vector<char> write_buffer;  // 写缓冲区
        size_t read_pos;                 // 当前读取位置
        size_t write_pos;                // 当前写入位置
        bool is_reading;                 // 当前是否在读取状态
        std::chrono::steady_clock::time_point last_active; // 最后活跃时间

        explicit ClientContext(int client_fd, size_t initial_buffer_size = 4096) 
            : fd(client_fd)
            , read_buffer(initial_buffer_size)
            , write_buffer(initial_buffer_size)
            , read_pos(0)
            , write_pos(0)
            , is_reading(true)
            , last_active(std::chrono::steady_clock::now()) {}
    };

private:
    // 使用inline常量替代static constexpr
    static inline constexpr size_t MAX_EVENTS = 1024;
    static inline constexpr size_t INITIAL_BUFFER_SIZE = 4096*8;
    
    const int port_;
    int server_fd_;
    int epfd;
    
    // 连接管理
    std::unordered_map<int, std::shared_ptr<ClientContext>> clients;
    mutable std::shared_mutex clients_mutex;  // 使用shared_mutex替代mutex
    
    // 线程池
    DoubleBufferThreadPool readThreadPool;
    DoubleBufferThreadPool writeThreadPool;
    
    // 命令处理器
    CommandHandler handler_;

    // 方法声明
    void add_client(int client_fd);
    void remove_client(int client_fd);
    std::optional<std::shared_ptr<ClientContext>> get_client(int client_fd) const;  // 返回optional
    void handle_client_data(std::shared_ptr<ClientContext> client);
    bool try_parse_command(std::shared_ptr<ClientContext> client);
    void reset_client_buffers(std::shared_ptr<ClientContext> client) noexcept;  // 添加noexcept

public:
    explicit RedisServer(int port);
    ~RedisServer();
    RedisServer(const RedisServer&) = delete;  // 禁用拷贝
    RedisServer& operator=(const RedisServer&) = delete;
    RedisServer(RedisServer&&) noexcept = default;  // 启用移动
    RedisServer& operator=(RedisServer&&) noexcept = default;
    
    void run();

private:
    void epoll_loop();
    void handle_read(int client_fd);
    void handle_write_ready(int client_fd);
};