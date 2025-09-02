#include "ThreadPool.h"
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cerrno>

// WorkerThread实现
WorkerThread::WorkerThread(int worker_id, std::shared_ptr<CommandHandler> handler)
    : worker_id_(worker_id), handler_(handler) {
    
    // 创建epoll实例
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::runtime_error("Failed to create epoll instance for worker " + std::to_string(worker_id));
    }
}

WorkerThread::~WorkerThread() {
    stop();
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
}

void WorkerThread::start() {
    running_ = true;
    worker_thread_ = std::thread(&WorkerThread::worker_loop, this);
}

void WorkerThread::stop() {
    running_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    
    // 关闭所有客户端连接
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& [fd, client] : clients_) {
        close(fd);
    }
    clients_.clear();
}

void WorkerThread::add_client(int client_fd) {
    // 设置非阻塞
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    
    // 设置TCP_NODELAY
    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    // 添加到epoll
    epoll_event ev{EPOLLIN | EPOLLET, {.fd = client_fd}};
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
        close(client_fd);
        return;
    }
    
    // 创建客户端信息
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_[client_fd] = std::make_unique<ClientInfo>();
        client_count_++;
    }
}

void WorkerThread::remove_client(int client_fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
    
    std::lock_guard<std::mutex> lock(clients_mutex_);
    if (clients_.erase(client_fd) > 0) {
        client_count_--;
    }
}

void WorkerThread::worker_loop() {
    constexpr int MAX_EVENTS = 256;
    epoll_event events[MAX_EVENTS];
    
    while (running_) {
        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, 100); // 100ms超时
        
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        // 处理事件
        for (int i = 0; i < n; ++i) {
            handle_client_event(events[i].data.fd, events[i].events);
        }
    }
}

void WorkerThread::handle_client_event(int client_fd, uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        remove_client(client_fd);
        return;
    }
    
    if (events & EPOLLIN) {
        process_client_data(client_fd);
    }
    
    if (events & EPOLLOUT) {
        // 处理写事件（如果需要）
    }
}

void WorkerThread::process_client_data(int client_fd) {
    std::unique_ptr<ClientInfo>* client_ptr = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(client_fd);
        if (it == clients_.end()) return;
        client_ptr = &it->second;
    }
    
    auto& client = **client_ptr;
    
    // 读取数据
    char buffer[4096];
    while (true) {
        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            remove_client(client_fd);
            return;
        }
        if (n == 0) {
            remove_client(client_fd);
            return;
        }
        
        // 解析命令
        std::string_view data(buffer, n);
        auto commands = client.parser.parse(data);
        
        // 处理命令
        for (const auto& cmd : commands) {
            if (!cmd.empty()) {
                std::string response = handler_->handle(cmd);
                send_response(client_fd, response);
                processed_commands_++;
            }
        }
        
        client.last_active = std::chrono::steady_clock::now();
    }
}

void WorkerThread::send_response(int client_fd, const std::string& response) {
    ssize_t sent = send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        remove_client(client_fd);
    }
}

// WorkerThreadPool实现
ThreadPool::ThreadPool(size_t worker_count, std::shared_ptr<CommandHandler> handler)
    : handler_(handler) {
    
    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back(std::make_unique<WorkerThread>(i, handler));
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::start() {
    for (auto& worker : workers_) {
        worker->start();
    }
}

void ThreadPool::stop() {
    for (auto& worker : workers_) {
        worker->stop();
    }
}

void ThreadPool::assign_client(int client_fd) {
    // 负载均衡：选择客户端数量最少的Worker
    size_t best_worker = 0;
    size_t min_clients = workers_[0]->get_client_count();
    
    for (size_t i = 1; i < workers_.size(); ++i) {
        size_t count = workers_[i]->get_client_count();
        if (count < min_clients) {
            min_clients = count;
            best_worker = i;
        }
    }
    
    workers_[best_worker]->add_client(client_fd);
    
    // 记录映射关系
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    client_to_worker_[client_fd] = best_worker;
}

void ThreadPool::remove_client(int client_fd) {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    auto it = client_to_worker_.find(client_fd);
    if (it != client_to_worker_.end()) {
        workers_[it->second]->remove_client(client_fd);
        client_to_worker_.erase(it);
    }
}

ThreadPool::Stats ThreadPool::get_stats() const {
    Stats stats;
    stats.total_clients = 0;
    stats.total_commands = 0;
    stats.worker_clients.resize(workers_.size());
    stats.worker_commands.resize(workers_.size());
    
    for (size_t i = 0; i < workers_.size(); ++i) {
        stats.worker_clients[i] = workers_[i]->get_client_count();
        stats.worker_commands[i] = workers_[i]->get_processed_commands();
        stats.total_clients += stats.worker_clients[i];
        stats.total_commands += stats.worker_commands[i];
    }
    
    return stats;
}
