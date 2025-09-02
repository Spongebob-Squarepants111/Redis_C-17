#include "RedisServer.h"
#include "RESPParser.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <sstream>
#include <thread>
#include <array>

RedisServer::RedisServer(const Config& config)
    : config_(config)
    , server_fd_(-1)
    , client_pool_(500, 5000, config.server().client_pool_shards)
    , clients_(config.server().client_shard_count)
    , clients_mutex_(config.server().client_shard_count)
    , parsers_(config.server().client_parser_shard_count)
    , parsers_mutex_(config.server().client_parser_shard_count)
    , readThreadPool(config.thread_pool().read_threads)
    , writeThreadPool(config.thread_pool().write_threads)
    , acceptThreadPool(config.thread_pool().accept_threads)
    , commandThreadPool(config.thread_pool().command_threads)
{
    // 预分配连接映射表
    for (auto& shard : clients_) {
        shard.reserve(2000);  // 增加预分配空间
    }
    
    for (auto& shard : parsers_) {
        shard.reserve(2000);  // 增加预分配空间
    }
    
    // 记录启动时间
    start_time_ = std::chrono::steady_clock::now();
}

RedisServer::~RedisServer() {
    // 关闭所有连接
    for (size_t i = 0; i < clients_.size(); ++i) {
        std::lock_guard<std::mutex> lock(clients_mutex_[i]);
        for (const auto& [fd, client] : clients_[i]) {
            close(fd);
        }
        clients_[i].clear();
    }

    // 关闭服务器socket
    if (server_fd_ != -1) {
        close(server_fd_);
    }
    
    if (epfd >= 0) {
        close(epfd);
    }
}

void RedisServer::add_client(int client_fd) {
    // 创建客户端上下文
    auto client = client_pool_.acquire(client_fd);
    
    // 计算分片索引
    size_t shard_idx = client_fd % config_.server().client_shard_count;
    
    // 加锁对应分片
    std::lock_guard<std::mutex> lock(clients_mutex_[shard_idx]);
    clients_[shard_idx][client_fd] = std::move(client);
}

void RedisServer::remove_client(int client_fd) {
    // 计算分片索引
    size_t shard_idx = client_fd % config_.server().client_shard_count;
    
    // 锁定对应分片
    {
        std::lock_guard<std::mutex> lock(clients_mutex_[shard_idx]);
        clients_[shard_idx].erase(client_fd);
    }
    
    // 清理解析器状态
    reset_client_parser(client_fd);
    
    // 从epoll中移除
    epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd, nullptr);
    
    // 关闭连接
    close(client_fd);
}

RedisServer::ClientContextPtr RedisServer::get_client(int client_fd) {
    // 计算分片索引
    size_t shard_idx = client_fd % config_.server().client_shard_count;
    
    // 锁定对应分片
    std::lock_guard<std::mutex> lock(clients_mutex_[shard_idx]);
    auto it = clients_[shard_idx].find(client_fd);
    if (it != clients_[shard_idx].end()) {
        return it->second;
    }
    
    return nullptr;
}

// 优化socket参数
void RedisServer::optimize_socket(int sockfd) {
    int opt = 1;
    // 允许地址重用
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
    }

    // 允许端口重用
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEPORT failed");
    }

    // 启用TCP_NODELAY，禁用Nagle算法
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        perror("setsockopt TCP_NODELAY failed");
    }
    
    // 增加TCP的接收和发送缓冲区大小
    int buf_size = config_.server().initial_buffer_size * 2;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size)) < 0) {
        perror("setsockopt SO_RCVBUF failed");
    }
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size)) < 0) {
        perror("setsockopt SO_SNDBUF failed");
    }

    // 设置TCP_QUICKACK选项
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_QUICKACK, &opt, sizeof(opt)) < 0) {
        perror("setsockopt TCP_QUICKACK failed");
    }
    
    // 使用TCP_FASTOPEN (如果内核支持)
    int qlen = 5;  // 队列长度
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen)) < 0) {
        // 忽略错误，不是所有内核都支持
    }

    // 启用TCP保活机制
    if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_KEEPALIVE failed");
    }

    // 设置TCP保活参数
    int keepidle = 60;    // 空闲60秒后开始发送保活包
    int keepintvl = 10;   // 保活包发送间隔10秒
    int keepcnt = 3;      // 最多发送3次保活包

    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) < 0) {
        perror("setsockopt TCP_KEEPIDLE failed");
    }
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) < 0) {
        perror("setsockopt TCP_KEEPINTVL failed");
    }
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) < 0) {
        perror("setsockopt TCP_KEEPCNT failed");
    }
    
    // 设置为非阻塞模式
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL failed");
        return;
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK failed");
    }
}

// 服务器启动方法
void RedisServer::run() {
    // 创建服务器socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        perror("socket failed");
        return;
    }

    // 应用socket优化
    optimize_socket(server_fd_);

    // 绑定地址和端口
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(config_.server().host.c_str());
    addr.sin_port = htons(config_.server().port);

    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(server_fd_);
        return;
    }

    // 开始监听，增加监听队列长度
    if (listen(server_fd_, config_.server().max_events) < 0) {
        perror("listen failed");
        close(server_fd_);
        return;
    }

    std::cout << "Redis Server running on " << config_.server().host << ":" << config_.server().port << std::endl;
    std::cout << "Thread configuration: " 
              << "read=" << config_.thread_pool().read_threads 
              << ", write=" << config_.thread_pool().write_threads 
              << ", accept=" << config_.thread_pool().accept_threads 
              << ", command=" << config_.thread_pool().command_threads << std::endl;

    // 启动epoll事件循环
    epoll_loop();
}

void RedisServer::epoll_loop() {
    // 创建epoll实例
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        perror("epoll_create1 failed");
        return;
    }
    
    // 注册服务器socket到epoll，使用边缘触发模式
    epoll_event ev{EPOLLIN | EPOLLET, {.fd = server_fd_}};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd_, &ev) < 0) {
        perror("epoll_ctl failed for server socket");
        close(epfd);
        return;
    }

    // 预分配事件数组，使用栈内存避免堆分配
    std::vector<epoll_event> events(config_.server().max_events);
    
    // 定时统计信息
    auto last_stats_time = std::chrono::steady_clock::now();
    
    // 准备多个接受连接事件，避免热点
    const int ACCEPT_BATCH_SIZE = 4;
    std::array<bool, ACCEPT_BATCH_SIZE> accept_in_progress{};
    
    std::cout << "Event loop started with " << config_.server().max_events << " max events\n";
    
    while (true) {
        // 等待事件，-1表示无超时
        int n = epoll_wait(epfd, events.data(), config_.server().max_events, 1000); // 1秒超时，用于定时处理
        
        if (n < 0) {
            if (errno == EINTR) continue; // 被信号中断，继续
            perror("epoll_wait failed");
            break;
        }
        
        // 批量处理事件
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            
            // 服务器socket事件 - 有新连接
            if (fd == server_fd_) {
                // 找到一个空闲的accept槽位
                int slot = -1;
                for (int j = 0; j < ACCEPT_BATCH_SIZE; ++j) {
                    if (!accept_in_progress[j]) {
                        slot = j;
                        break;
                    }
                }
                
                if (slot >= 0) {
                    accept_in_progress[slot] = true;
                    acceptThreadPool.enqueue([this, slot, &accept_in_progress]() {
                        accept_new_connections();
                        accept_in_progress[slot] = false;
                    });
                } else {
                    // 所有槽位都忙，直接在当前线程处理
                    accept_new_connections();
                }
            } else {
                // 客户端事件
                uint32_t ev_flags = events[i].events;
                
                // 可读事件
                if (ev_flags & EPOLLIN) {
                    // 读取数据的任务提交给读线程池
                    readThreadPool.enqueue([this, fd]() {
                        handle_read(fd);
                    });
                }
                
                // 可写事件
                if (ev_flags & EPOLLOUT) {
                    // 写数据的任务提交给写线程池
                    writeThreadPool.enqueue([this, fd]() {
                        handle_write_ready(fd);
                    });
                }
                
                // 错误或挂起事件
                if ((ev_flags & EPOLLERR) || (ev_flags & EPOLLHUP)) {
                    // 处理错误或连接关闭
                    remove_client(fd);
                }
            }
        }
        
        // 每30秒输出一次统计信息
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stats_time).count() >= 30) {
            print_stats();
            last_stats_time = now;
        }
    }
}

// 批量接受新连接
void RedisServer::accept_new_connections() {
    // 使用栈上缓冲区存储新接受的连接
    std::vector<int> new_clients(config_.server().max_accept_per_round);
    std::vector<sockaddr_in> client_addrs(config_.server().max_accept_per_round);
    std::vector<socklen_t> addr_lens(config_.server().max_accept_per_round);
    
    int accepted_count = 0;
    
    // 预填充地址长度数组
    std::fill(addr_lens.begin(), addr_lens.end(), sizeof(sockaddr_in));
    
    // 批量accept循环
    while (accepted_count < config_.server().max_accept_per_round) {
        sockaddr_in& client_addr = client_addrs[accepted_count];
        socklen_t& addr_len = addr_lens[accepted_count];
        
        // 接受新连接
        int client_fd = accept4(server_fd_, (sockaddr*)&client_addr, &addr_len, SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept4 failed");
            break;
        }
        
        // 应用socket优化
        optimize_socket(client_fd);
        
        // 存储新连接
        new_clients[accepted_count++] = client_fd;
        
        // 更新连接计数
        total_connections_++;
    }
    
    if (accepted_count == 0) return;
    
    // 批量添加到epoll和客户端映射表
    for (int i = 0; i < accepted_count; ++i) {
        int client_fd = new_clients[i];
        
        // 添加到epoll监听
        epoll_event cev{EPOLLIN | EPOLLET, {.fd = client_fd}};
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev) < 0) {
            perror("epoll_ctl failed for new client");
            close(client_fd);
            continue;
        }
        
        // 添加到客户端管理
        add_client(client_fd);
    }
}

// 处理客户端读取事件
void RedisServer::handle_read(int client_fd) {
    auto client = get_client(client_fd);
    if (!client) {
        return;
    }

    // 使用较大的栈上缓冲区，减少堆分配
    std::vector<char> local_buffer(config_.server().default_buffer_size * 2);
    ssize_t total_bytes_read = 0;
    bool complete_command_processed = false;
    
    while (true) {
        ssize_t n = recv(client_fd, local_buffer.data(), local_buffer.size(), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // 没有更多数据可读
            }
            perror("recv failed");
            remove_client(client_fd);
            return;
        }
        
        if (n == 0) {  // 客户端关闭连接
            remove_client(client_fd);
            return;
        }
        
        // 更新最后活跃时间
        client->last_active = std::chrono::steady_clock::now();
        total_bytes_read += n;

        // 使用字符串视图直接解析数据，避免复制到客户端缓冲区
        std::string_view direct_view(local_buffer.data(), n);
        
        // 获取该客户端对应的解析器
        auto& parser = get_parser(client_fd);
        
        // 尝试直接解析接收到的数据
        auto direct_cmds = parser.parse(direct_view);
        
        // 如果有完整命令，通过命令线程池处理并返回
        if (!direct_cmds.empty()) {
            complete_command_processed = true;
            
            // 使用命令线程池异步处理命令
            commandThreadPool.enqueue([this, cmds = std::move(direct_cmds), fd = client_fd]() {
                process_commands(cmds, fd);
            });
        } else {
            // 没有完整命令，数据需要追加到读缓冲区
            client->ensure_read_capacity(n);
            std::copy(local_buffer.data(), local_buffer.data() + n, client->read_buffer.begin() + client->read_pos);
            client->read_pos += n;
            
            // 尝试处理可能已完成的命令
            if (try_parse_command(client, client_fd)) {
                complete_command_processed = true;
            }
        }
        
        // 如果接收缓冲区已满，停止读取
        if (total_bytes_read >= config_.server().max_buffer_size) {
            break;
        }
    }
    
    // 如果处理过完整命令但仍有未处理的数据，尝试压缩读缓冲区
    if (complete_command_processed && client->read_pos > 0) {
        client->compact_read_buffer();
    }
}

// 处理解析出的命令
void RedisServer::process_commands(const std::vector<std::vector<std::string>>& cmds, int client_fd) {
    auto client = get_client(client_fd);
    if (!client) return;
    
    // 处理命令并增加计数
    auto results = handler_.handle_pipeline(cmds);
    total_commands_ += cmds.size();
    
    // 准备响应
    size_t total_size = 0;
    for (const auto& result : results) {
        total_size += result.size();
    }
    
    // 确保写缓冲区有足够空间
    client->ensure_write_capacity(total_size);
    
    // 一次性构建响应并复制到写缓冲区
    {
        std::lock_guard<std::mutex> lock(client->write_mutex);
        for (const auto& result : results) {
            std::copy(result.begin(), result.end(), client->write_buffer.begin() + client->write_pos);
            client->write_pos += result.size();
        }
    }
    
    // 注册EPOLLOUT事件，表示有数据可写
    epoll_event ev{EPOLLIN | EPOLLOUT | EPOLLET, {.fd = client_fd}};
    epoll_ctl(epfd, EPOLL_CTL_MOD, client_fd, &ev);
}

bool RedisServer::try_parse_command(ClientContextPtr client, int client_fd) {
    if (client->read_pos == 0) return false;
    
    // 使用字符串视图，避免复制数据
    std::string_view buffer_view(client->read_buffer.data(), client->read_pos);
    
    // 获取解析器
    auto& parser = get_parser(client_fd);
    
    // 解析命令
    auto cmds = parser.parse(buffer_view);
    if (cmds.empty()) return false;
    
    // 处理命令
    auto results = handler_.handle_pipeline(cmds);
    
    // 预计算总响应大小
    size_t total_size = 0;
    for (const auto& result : results) {
        total_size += result.size();
    }
    
    // 准备响应
    client->ensure_write_capacity(total_size);
    for (const auto& result : results) {
        std::copy(result.begin(), result.end(), client->write_buffer.begin() + client->write_pos);
        client->write_pos += result.size();
    }
    
    // 重置读取位置（由于已经使用字符串视图处理了全部内容）
    client->read_pos = 0;
    
    // 设置EPOLLOUT事件
    epoll_event ev{EPOLLIN | EPOLLOUT | EPOLLET, {.fd = client_fd}};
    epoll_ctl(epfd, EPOLL_CTL_MOD, client_fd, &ev);
    
    return true;
}

void RedisServer::handle_write_ready(int client_fd) {
    auto client = get_client(client_fd);
    if (!client || client->write_pos == 0) {
        return;
    }

    // 更新最后活跃时间
    client->last_active = std::chrono::steady_clock::now();

    // 使用局部变量存储写入位置，减少锁持有时间
    size_t write_pos;
    const char* write_data;

    {
        std::lock_guard<std::mutex> lock(client->write_mutex);
        write_pos = client->write_pos;
        write_data = client->write_buffer.data();
    }

    // 发送数据
    ssize_t n = send(client_fd, write_data, write_pos, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  // 缓冲区满，稍后重试
        }
        perror("send failed");
        remove_client(client_fd);
        return;
    }
    
    if (n == 0) {  // 客户端关闭连接
        remove_client(client_fd);
        return;
    }
    
    // 更新写位置
    {
        std::lock_guard<std::mutex> lock(client->write_mutex);
        if (static_cast<size_t>(n) < client->write_pos) {
            // 还有数据未发送完，移动剩余数据
            std::copy(client->write_buffer.begin() + n, client->write_buffer.begin() + client->write_pos, client->write_buffer.begin());
            client->write_pos -= n;
        } else {
            // 所有数据已发送
            client->write_pos = 0;
            
            // 切换回读取模式
            client->is_reading = true;
            epoll_event ev{EPOLLIN | EPOLLET, {.fd = client_fd}};
            epoll_ctl(epfd, EPOLL_CTL_MOD, client_fd, &ev);
        }
    }
}

void RedisServer::reset_client_buffers(ClientContextPtr client) {
    client->read_pos = 0;
    client->write_pos = 0;
}

void RedisServer::reset_client_parser(int client_fd) {
    // 计算分片索引
    size_t shard_idx = client_fd % config_.server().client_parser_shard_count;
    
    // 锁定对应分片
    std::lock_guard<std::mutex> lock(parsers_mutex_[shard_idx]);
    parsers_[shard_idx].erase(client_fd);
}

RESPParser& RedisServer::get_parser(int client_fd) {
    // 计算分片索引
    size_t shard_idx = client_fd % config_.server().client_parser_shard_count;
    
    // 锁定对应分片
    std::lock_guard<std::mutex> lock(parsers_mutex_[shard_idx]);
    auto it = parsers_[shard_idx].find(client_fd);
    if (it == parsers_[shard_idx].end()) {
        // 创建新解析器并绑定到连接
        auto [new_it, inserted] = parsers_[shard_idx].emplace(client_fd, RESPParser());
        return new_it->second;
    }
    return it->second;
}

// 输出服务器统计信息
void RedisServer::print_stats() {
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    
    std::cout << "\n=== Server Stats ===\n";
    std::cout << "Uptime: " << uptime << " seconds\n";
    std::cout << "Total connections: " << total_connections_.load() << "\n";
    std::cout << "Total commands: " << total_commands_.load() << "\n";
    
    // 计算QPS
    double qps = static_cast<double>(total_commands_) / uptime;
    std::cout << "Commands per second: " << qps << "\n";
    
    // 当前连接数
    size_t current_connections = 0;
    for (size_t i = 0; i < clients_.size(); ++i) {
        std::lock_guard<std::mutex> lock(clients_mutex_[i]);
        current_connections += clients_[i].size();
    }
    std::cout << "Current connections: " << current_connections << "\n";
    
    // 缓存命中率等更多信息可以在这里添加
    std::cout << "=====================\n";
}
