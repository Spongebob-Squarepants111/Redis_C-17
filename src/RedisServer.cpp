#include "RedisServer.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

RedisServer::RedisServer(const Config& config)
    : config_(config), server_fd_(-1) {
    
    // 创建简化的DataStore配置
    DataStore::Options ds_options;
    ds_options.shard_count = config.shard_count;
    ds_options.cache_size = config.cache_size_mb * 1000; // 转换为条目数
    ds_options.enable_compression = config.enable_compression;
    ds_options.persist_path = "./data/";
    ds_options.sync_interval = std::chrono::seconds(config.sync_interval_sec);
    ds_options.memory_pool_block_size = 4096;
    ds_options.bucket_per_shard = 16;
    ds_options.cache_shards = config.shard_count;
    ds_options.cache_policy = CachePolicy::Type::LRU;
    ds_options.adaptive_cache_sizing = false; // 简化：关闭自适应
    
    datastore_ = std::make_shared<DataStore>(ds_options);
    handler_ = std::make_shared<CommandHandler>(datastore_);
    
    // 配置线程池选项，启用CPU亲和性
    ThreadPool::Options pool_options;
    pool_options.enable_cpu_affinity = true;
    pool_options.auto_detect_topology = true;
    // 可以根据需要自定义CPU分配
    // pool_options.custom_cpu_assignment = {0, 1, 2, 3, ...};
    
    worker_pool_ = std::make_unique<ThreadPool>(config.worker_threads, handler_, pool_options);
    
    start_time_ = std::chrono::steady_clock::now();
}

RedisServer::~RedisServer() {
    stop();
}

void RedisServer::run() {
    try {
        setup_server_socket();
        std::cout << "Socket setup successful, server_fd = " << server_fd_ << std::endl;
        
        std::cout << "Optimized Redis Server starting on " << config_.host << ":" << config_.port << std::endl;
        std::cout << "Configuration: " << config_.worker_threads << " workers, " 
                  << config_.shard_count << " shards" << std::endl;
        
        running_ = true;
        
        // 启动Worker线程池
        worker_pool_->start();
        std::cout << "Worker pool started" << std::endl;
        
        // 启动accept线程
        accept_thread_ = std::thread(&RedisServer::accept_loop, this);
        std::cout << "Accept thread started" << std::endl;
        
        // 启动统计线程
        stats_thread_ = std::thread(&RedisServer::print_stats_loop, this);
        std::cout << "Stats thread started" << std::endl;
        
        // 等待accept线程
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error during server startup: " << e.what() << std::endl;
        running_ = false;
        throw;
    }
}

void RedisServer::stop() {
    running_ = false;
    
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    
    if (stats_thread_.joinable()) {
        stats_thread_.join();
    }
    
    if (worker_pool_) {
        worker_pool_->stop();
    }
    
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
}

void RedisServer::setup_server_socket() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        throw std::runtime_error("Failed to create server socket");
    }

    optimize_socket(server_fd_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(config_.host.c_str());
    addr.sin_port = htons(config_.port);

    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server_fd_);
        throw std::runtime_error("Failed to bind server socket");
    }

    if (listen(server_fd_, 1024) < 0) {
        close(server_fd_);
        throw std::runtime_error("Failed to listen on server socket");
    }
}

void RedisServer::optimize_socket(int sockfd) {
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    int buf_size = config_.buffer_size * 2;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
}

void RedisServer::accept_loop() {
    std::cout << "Accept loop started, listening on " << config_.host << ":" << config_.port << std::endl;
    std::cout << "Server fd: " << server_fd_ << std::endl;
    
    if (server_fd_ < 0) {
        std::cerr << "Error: Invalid server socket fd in accept loop!" << std::endl;
        return;
    }

    while (running_) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (running_) {
                perror("accept failed");
                std::cout << "Accept failed with errno: " << errno << std::endl;
            }
            break;
        }
        
        std::cout << "Accepted connection: fd=" << client_fd << std::endl;
        
        // 检查连接数限制
        auto current_conns = worker_pool_->get_stats().total_clients;
        if (current_conns >= config_.max_connections) {
            std::cout << "Max connections reached, rejecting client" << std::endl;
            close(client_fd);
            continue;
        }
        
        optimize_socket(client_fd);
        
        // 分配给Worker
        worker_pool_->assign_client(client_fd);
        
        total_connections_++;
        std::cout << "Client assigned to worker, total connections: " << total_connections_.load() << std::endl;
    }
    
    std::cout << "Accept loop ended" << std::endl;
}

void RedisServer::print_stats_loop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (!running_) break;
        
        auto stats = get_stats();
        auto pool_stats = worker_pool_->get_stats();
        
        std::cout << "\n=== Optimized Server Stats ===" << std::endl;
        std::cout << "Uptime: " << stats.uptime.count() << " seconds" << std::endl;
        std::cout << "Total connections: " << stats.total_connections << std::endl;
        std::cout << "Current connections: " << pool_stats.total_clients << std::endl;
        std::cout << "Total commands: " << pool_stats.total_commands << std::endl;
        std::cout << "Commands per second: " << stats.commands_per_second << std::endl;
        
        // Worker负载分布
        std::cout << "Worker load distribution: ";
        for (size_t i = 0; i < pool_stats.worker_clients.size(); ++i) {
            std::cout << "[" << i << "]:" << pool_stats.worker_clients[i] << " ";
        }
        std::cout << std::endl;
        std::cout << "==============================" << std::endl;
    }
}

RedisServer::Stats RedisServer::get_stats() const {
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
    
    auto pool_stats = worker_pool_->get_stats();
    double qps = uptime.count() > 0 ? 
        static_cast<double>(pool_stats.total_commands) / uptime.count() : 0.0;
    
    return Stats{
        total_connections_.load(),
        pool_stats.total_commands,
        pool_stats.total_clients,
        qps,
        uptime
    };
}
