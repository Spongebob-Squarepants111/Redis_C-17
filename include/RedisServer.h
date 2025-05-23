#pragma once
#include <unordered_map>
#include <memory>
#include <vector>
#include <mutex>
#include "ThreadPool.h"
#include "CommandHandler.h"
#include "ClientContextPool.h"

class RedisServer {
private:
    static constexpr size_t MAX_EVENTS = 4096;               // 事件处理容量
    static constexpr size_t INITIAL_BUFFER_SIZE = 64 * 1024;  // 256KB
    static constexpr size_t MAX_BUFFER_SIZE = INITIAL_BUFFER_SIZE * 4;      // 1MB
    static constexpr size_t DEFAULT_BUFFER_SIZE = 32 * 1024;   // 128KB
    
    // 使用ClientContextPool中定义的ClientContext和ClientContextPtr
    using ClientContext = ClientContextPool::ClientContext;
    using ClientContextPtr = ClientContextPool::ClientContextPtr;

private:
    const int port_;
    const std::string host_;
    int server_fd_;
    int epfd;

    // 客户端上下文池
    ClientContextPool client_pool_;
    
    // 连接管理
    std::unordered_map<int, ClientContextPtr> clients;
    alignas(CACHE_LINE_SIZE) mutable std::mutex clients_mutex;
    
    // 线程池
    ThreadPool readThreadPool;
    ThreadPool writeThreadPool;
    
    // 命令处理器
    CommandHandler handler_;

    void add_client(int client_fd);
    void remove_client(int client_fd);
    ClientContextPtr get_client(int client_fd);
    void handle_client_data(ClientContextPtr client);
    bool try_parse_command(ClientContextPtr client);
    void reset_client_buffers(ClientContextPtr client);

public:
    explicit RedisServer(int port, const std::string& host = "0.0.0.0");
    ~RedisServer();
    void run();

private:
    void epoll_loop();
    void handle_read(int client_fd);
    void handle_write_ready(int client_fd);
};