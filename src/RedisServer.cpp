#include "RedisServer.h"
#include "RESPParser.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

RedisServer::RedisServer(int port) 
    : port_(port)
    , readThreadPool(std::thread::hardware_concurrency())
    , writeThreadPool(std::thread::hardware_concurrency()) {}

void RedisServer::add_client(int client_fd) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    clients[client_fd] = std::make_shared<ClientContext>(client_fd);
}

void RedisServer::remove_client(int client_fd) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    clients.erase(client_fd);
    close(client_fd);
}

std::shared_ptr<RedisServer::ClientContext> RedisServer::get_client(int client_fd) {
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
    int buffer_size = 64 * 1024;  // 64KB
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
    addr.sin_addr.s_addr = INADDR_ANY;
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
                    int buffer_size = 32 * 1024;  // 32KB
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
                }
                if (events[i].events & EPOLLOUT) {
                    writeThreadPool.enqueue([this, fd]() {
                        handle_write_ready(fd);
                    });
                }
            }
        }
    }
}

void RedisServer::handle_read(int client_fd) {
    auto client = get_client(client_fd);
    if (!client) return;

    client->last_active = std::chrono::steady_clock::now();
    
    while (true) {
        // 确保缓冲区有足够空间
        if (client->read_pos >= client->read_buffer.size()) {
            client->read_buffer.resize(client->read_buffer.size() * 2);
        }
        
        // 读取数据
        ssize_t n = read(client_fd, 
                        client->read_buffer.data() + client->read_pos,
                        client->read_buffer.size() - client->read_pos);
                        
        if (n > 0) {
            client->read_pos += n;
            
            // 尝试解析命令
            if (try_parse_command(client)) {
                // 修改epoll事件为写
                epoll_event ev{EPOLLOUT | EPOLLET, {.fd = client_fd}};
                epoll_ctl(epfd, EPOLL_CTL_MOD, client_fd, &ev);
                break;
            }
        } else if (n == 0) {
            // 客户端断开连接
            remove_client(client_fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            // 发生错误
            remove_client(client_fd);
            return;
        }
    }
}

bool RedisServer::try_parse_command(std::shared_ptr<ClientContext> client) {
    // 将string_view转换为string
    std::string data(client->read_buffer.data(), client->read_pos);
    try {
        auto cmds = RESPParser::parse_multi(data);  // 修改为支持多命令解析
        if (!cmds.empty()) {
            std::string response;
            
            if (cmds.size() == 1) {
                // 单个命令处理
                response = handler_.handle(cmds[0]);
            } else {
                // 批量命令处理
                auto responses = handler_.handle_pipeline(cmds);
                // 合并所有响应
                for (const auto& resp : responses) {
                    response += resp;
                }
            }
            
            // 将响应放入写缓冲区
            client->write_buffer.assign(response.begin(), response.end());
            client->write_pos = 0;
            client->is_reading = false;
            
            // 重置读缓冲区
            reset_client_buffers(client);
            return true;
        }
    } catch (const std::exception& e) {
        // 解析错误处理
        std::string error = "-ERR " + std::string(e.what()) + "\r\n";
        client->write_buffer.assign(error.begin(), error.end());
        client->write_pos = 0;
        client->is_reading = false;
        reset_client_buffers(client);
        return true;
    }
    return false;
}

void RedisServer::handle_write_ready(int client_fd) {
    auto client = get_client(client_fd);
    if (!client) return;

    // client->last_active = std::chrono::steady_clock::now();
    
    while (client->write_pos < client->write_buffer.size()) {
        ssize_t n = write(client_fd,
                         client->write_buffer.data() + client->write_pos,
                         client->write_buffer.size() - client->write_pos);
                         
        if (n > 0) {
            client->write_pos += n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            // 发生错误
            remove_client(client_fd);
            return;
        }
    }
    
    // 写入完成，重置状态并切换回读模式
    reset_client_buffers(client);
    client->is_reading = true;
    
    // 修改epoll事件为读
    epoll_event ev{EPOLLIN | EPOLLET, {.fd = client_fd}};
    epoll_ctl(epfd, EPOLL_CTL_MOD, client_fd, &ev);
}

void RedisServer::reset_client_buffers(std::shared_ptr<ClientContext> client) {
    // 重置读写位置
    client->read_pos = 0;
    client->write_pos = 0;
    
    // 如果缓冲区过大，则收缩到初始大小
    if (client->read_buffer.size() > INITIAL_BUFFER_SIZE * 2) {
        client->read_buffer.shrink_to_fit();
        client->read_buffer.resize(INITIAL_BUFFER_SIZE);
    }
    if (client->write_buffer.size() > INITIAL_BUFFER_SIZE * 2) {
        client->write_buffer.shrink_to_fit();
        client->write_buffer.resize(INITIAL_BUFFER_SIZE);
    }
}

RedisServer::~RedisServer() {
    // 清理资源
    for (const auto& [fd, client] : clients) {
        close(fd);
    }
    close(server_fd_);
    close(epfd);
}