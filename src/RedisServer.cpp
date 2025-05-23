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

RedisServer::RedisServer(int port, const std::string& host) 
    : port_(port)
    , host_(host)
    , client_pool_(100, 200) // 预分配128个客户端上下文，最多2000个
    , readThreadPool(std::thread::hardware_concurrency())
    , writeThreadPool(std::thread::hardware_concurrency()) {}

RedisServer::~RedisServer() {
    // 在析构函数中释放资源
    for (const auto& [fd, client] : clients) {
        close(fd);
    }
    
    if (server_fd_ >= 0) {
        close(server_fd_);
    }
    
    if (epfd >= 0) {
        close(epfd);
    }
}

void RedisServer::add_client(int client_fd) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    // 使用客户端池分配ClientContext
    clients[client_fd] = client_pool_.acquire(client_fd);
}

void RedisServer::remove_client(int client_fd) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    clients.erase(client_fd);
    close(client_fd);
}

RedisServer::ClientContextPtr RedisServer::get_client(int client_fd) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    auto it = clients.find(client_fd);
    return (it != clients.end()) ? it->second : nullptr;
}

void RedisServer::run() {
    // 创建服务器socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        perror("socket failed");
        return;
    }

    // TCP优化选项
    int opt = 1;
    // 允许地址重用
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        close(server_fd_);
        return;
    }

    // 允许端口重用
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEPORT failed");
        close(server_fd_);
        return;
    }

    // 启用TCP_NODELAY，禁用Nagle算法
    if (setsockopt(server_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        perror("setsockopt TCP_NODELAY failed");
        close(server_fd_);
        return;
    }

    // 设置TCP发送和接收缓冲区大小
    int buffer_size = INITIAL_BUFFER_SIZE; 
    if (setsockopt(server_fd_, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) < 0) {
        perror("setsockopt SO_SNDBUF failed");
        close(server_fd_);
        return;
    }
    if (setsockopt(server_fd_, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
        perror("setsockopt SO_RCVBUF failed");
        close(server_fd_);
        return;
    }

    // 启用TCP保活机制
    int keepalive = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
        perror("setsockopt SO_KEEPALIVE failed");
        close(server_fd_);
        return;
    }

    // 设置TCP保活参数
    int keepidle = 60;    // 空闲60秒后开始发送保活包
    int keepintvl = 10;   // 保活包发送间隔10秒
    int keepcnt = 3;      // 最多发送3次保活包

    if (setsockopt(server_fd_, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) < 0) {
        perror("setsockopt TCP_KEEPIDLE failed");
    }
    if (setsockopt(server_fd_, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) < 0) {
        perror("setsockopt TCP_KEEPINTVL failed");
    }
    if (setsockopt(server_fd_, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) < 0) {
        perror("setsockopt TCP_KEEPCNT failed");
    }

    // 绑定地址和端口
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(host_.c_str());
    addr.sin_port = htons(port_);

    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(server_fd_);
        return;
    }

    // 开始监听
    if (listen(server_fd_, MAX_EVENTS) < 0) {
        perror("listen failed");
        close(server_fd_);
        return;
    }

    // 设置服务器socket为非阻塞模式
    int flags = fcntl(server_fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl failed");
        close(server_fd_);
        return;
    }

    std::cout << "Redis Server running on port " << port_ << std::endl;

    // 启动epoll事件循环
    epoll_loop();
}

void RedisServer::epoll_loop() {
    epfd = epoll_create1(0);
    epoll_event ev{EPOLLIN | EPOLLET, {.fd = server_fd_}};
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd_, &ev);

    epoll_event events[MAX_EVENTS];
    while (true) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == server_fd_) {
                // 处理新连接
                while (true) {
                    sockaddr_in cli{};
                    socklen_t len = sizeof(cli);
                    int client = accept(server_fd_, (sockaddr*)&cli, &len);
                    if (client < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept failed");
                        break;
                    }
                    
                    // 设置客户端socket选项
                    int opt = 1;
                    // 启用TCP_NODELAY
                    if (setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
                        perror("client setsockopt TCP_NODELAY failed");
                    }

                    // 设置发送和接收缓冲区
                    int buffer_size = INITIAL_BUFFER_SIZE / 2;  // 32KB
                    if (setsockopt(client, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) < 0) {
                        perror("client setsockopt SO_SNDBUF failed");
                    }
                    if (setsockopt(client, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
                        perror("client setsockopt SO_RCVBUF failed");
                    }

                    // 启用TCP保活机制
                    int keepalive = 1;
                    if (setsockopt(client, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
                        perror("client setsockopt SO_KEEPALIVE failed");
                    }
                    
                    // 设置非阻塞
                    fcntl(client, F_SETFL, O_NONBLOCK);
                    
                    // 添加到epoll监听
                    epoll_event cev{EPOLLIN | EPOLLET, {.fd = client}};
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client, &cev);
                    
                    // 添加到客户端管理
                    add_client(client);
                }
            } else {
                // 处理客户端IO事件
                if (events[i].events & EPOLLIN) {
                    readThreadPool.enqueue([this, fd]() {
                        handle_read(fd);
                    });
                    // std::cout << "read:++++++++++++++++++++++++++++++++++++++++++" << std::endl;
                    // readThreadPool.print_stats(std::cout, true);
                }
                if (events[i].events & EPOLLOUT) {
                    writeThreadPool.enqueue([this, fd]() {
                        handle_write_ready(fd);
                    });
                    // std::cout << "write:======================================" << std::endl;
                    // writeThreadPool.print_stats(std::cout, true);
                }
            }
        }
    }
}

void RedisServer::handle_read(int client_fd) {
    auto client = get_client(client_fd);
    if (!client) {
        return;
    }

    char buffer[DEFAULT_BUFFER_SIZE];
    while (true) {
        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
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

        // 调整缓冲区大小
        if (client->read_pos + n > client->read_buffer.size()) {
            if (client->read_buffer.size() >= MAX_BUFFER_SIZE) {
                // 缓冲区已达最大大小，重置
                reset_client_buffers(client);
                continue;
            }
            // 扩大缓冲区
            client->read_buffer.resize(std::min(client->read_buffer.size() * 2, MAX_BUFFER_SIZE));
        }
        
        // 复制数据到缓冲区
        std::copy(buffer, buffer + n, client->read_buffer.begin() + client->read_pos);
        client->read_pos += n;
        
        // 尝试解析命令
        if (try_parse_command(client)) {
            // 设置写事件监听
            epoll_event ev{EPOLLOUT | EPOLLET, {.fd = client_fd}};
            epoll_ctl(epfd, EPOLL_CTL_MOD, client_fd, &ev);
            client->is_reading = false;
        }
    }
}

bool RedisServer::try_parse_command(ClientContextPtr client) {
    try {
        // 将读缓冲区内容转换为字符串
        std::string data(client->read_buffer.begin(), client->read_buffer.begin() + client->read_pos);
        
        // 尝试解析为Redis命令
        std::vector<std::vector<std::string>> cmds = RESPParser::parse_multi(data);
        
        // 如果有命令，处理它们
        if (!cmds.empty()) {
            // 处理多个命令（pipeline）
            auto results = handler_.handle_pipeline(cmds);
            
            // 将结果写入写缓冲区
            std::string response;
            for (const auto& result : results) {
                response += result;
            }
            
            // 调整写缓冲区大小
            if (client->write_pos + response.size() > client->write_buffer.size()) {
                if (client->write_buffer.size() >= MAX_BUFFER_SIZE) {
                    // 缓冲区已达最大大小，重置
                    reset_client_buffers(client);
                    return false;
                }
                // 扩大写缓冲区
                client->write_buffer.resize(std::min(client->write_buffer.size() * 2, MAX_BUFFER_SIZE));
            }
            
            // 复制响应到写缓冲区
            std::copy(response.begin(), response.end(), client->write_buffer.begin() + client->write_pos);
            client->write_pos += response.size();
            
            // 重置读缓冲区
            client->read_pos = 0;
            
            return true;  // 有数据需要写回
        }
    } catch (const std::exception& e) {
        // 解析错误
        std::string error = "-ERR " + std::string(e.what()) + "\r\n";
        
        // 调整写缓冲区大小
        if (client->write_pos + error.size() > client->write_buffer.size()) {
            client->write_buffer.resize(std::min(client->write_buffer.size() * 2, MAX_BUFFER_SIZE));
        }
        
        // 写入错误响应
        std::copy(error.begin(), error.end(), client->write_buffer.begin() + client->write_pos);
        client->write_pos += error.size();
        
        // 重置读缓冲区
        client->read_pos = 0;
        
        return true;  // 有错误需要写回
    }
    
    return false;  // 没有完整命令
}

void RedisServer::handle_write_ready(int client_fd) {
    auto client = get_client(client_fd);
    if (!client || client->write_pos == 0) {
        return;
    }

    // 更新最后活跃时间
    client->last_active = std::chrono::steady_clock::now();

    // 发送数据
    ssize_t n = send(client_fd, client->write_buffer.data(), client->write_pos, 0);
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

void RedisServer::reset_client_buffers(ClientContextPtr client) {
    client->read_pos = 0;
    client->write_pos = 0;
}
