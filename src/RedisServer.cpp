#include "RedisServer.h"
#include "RESPParser.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

RedisServer::RedisServer(int port) 
    : port_(port)
    , readThreadPool(std::thread::hardware_concurrency())
    , writeThreadPool(std::thread::hardware_concurrency()) {}

RedisServer::~RedisServer() = default;

void RedisServer::add_client(int client_fd) {
    std::unique_lock lock(clients_mutex);
    clients.try_emplace(client_fd, std::make_shared<ClientContext>(client_fd));
}

void RedisServer::remove_client(int client_fd) {
    std::unique_lock lock(clients_mutex);
    if (auto it = clients.find(client_fd); it != clients.end()) {
        close(client_fd);
        clients.erase(it);
    }
}

std::optional<std::shared_ptr<RedisServer::ClientContext>> RedisServer::get_client(int client_fd) const {
    std::shared_lock lock(clients_mutex);
    if (auto it = clients.find(client_fd); it != clients.end()) {
        return it->second;
    }
    return std::nullopt;
}

void RedisServer::run() {
    if (server_fd_ = socket(AF_INET, SOCK_STREAM, 0); server_fd_ < 0) {
        throw std::runtime_error("socket failed");
    }

    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(server_fd_);
        throw std::runtime_error("setsockopt failed");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(server_fd_);
        throw std::runtime_error("bind failed");
    }

    if (listen(server_fd_, MAX_EVENTS) < 0) {
        close(server_fd_);
        throw std::runtime_error("listen failed");
    }

    if (int flags = fcntl(server_fd_, F_GETFL, 0); flags >= 0) {
        if (fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(server_fd_);
            throw std::runtime_error("fcntl failed");
        }
    } else {
        close(server_fd_);
        throw std::runtime_error("fcntl failed");
    }

    std::cout << "Redis Server running on port " << port_ << std::endl;
    epoll_loop();
}

void RedisServer::epoll_loop() {
    epfd = epoll_create1(0);
    epoll_event ev{EPOLLIN | EPOLLET, {.fd = server_fd_}};
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd_, &ev);

    std::vector<epoll_event> events(MAX_EVENTS);
    while (true) {
        int n = epoll_wait(epfd, events.data(), MAX_EVENTS, -1);
        for (int i = 0; i < n; ++i) {
            const auto& event = events[i];
            if (event.data.fd == server_fd_) {
                while (true) {
                    sockaddr_in cli{};
                    socklen_t len = sizeof(cli);
                    if (int client = accept(server_fd_, reinterpret_cast<sockaddr*>(&cli), &len); client >= 0) {
                        fcntl(client, F_SETFL, O_NONBLOCK);
                        epoll_event cev{EPOLLIN | EPOLLET, {.fd = client}};
                        epoll_ctl(epfd, EPOLL_CTL_ADD, client, &cev);
                        add_client(client);
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    } else {
                        std::cerr << "accept failed: " << strerror(errno) << std::endl;
                        break;
                    }
                }
            } else {
                const int fd = event.data.fd;
                if (event.events & EPOLLIN) {
                    readThreadPool.enqueue([this, fd] { handle_read(fd); });
                }
                if (event.events & EPOLLOUT) {
                    writeThreadPool.enqueue([this, fd] { handle_write_ready(fd); });
                }
            }
        }
    }
}

void RedisServer::handle_read(int client_fd) {
    auto client_opt = get_client(client_fd);
    if (!client_opt) return;

    auto& client = *client_opt;
    client->last_active = std::chrono::steady_clock::now();
    
    while (true) {
        if (client->read_pos >= client->read_buffer.size()) {
            client->read_buffer.resize(client->read_buffer.size() * 2);
        }
        
        if (ssize_t n = read(client_fd, 
                            client->read_buffer.data() + client->read_pos,
                            client->read_buffer.size() - client->read_pos); n > 0) {
            client->read_pos += n;
            
            if (try_parse_command(client)) {
                epoll_event ev{EPOLLOUT | EPOLLET, {.fd = client_fd}};
                epoll_ctl(epfd, EPOLL_CTL_MOD, client_fd, &ev);
                break;
            }
        } else if (n == 0) {
            remove_client(client_fd);
            return;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            remove_client(client_fd);
            return;
        }
    }
}

bool RedisServer::try_parse_command(std::shared_ptr<ClientContext> client) {
    std::string_view data{client->read_buffer.data(), client->read_pos};
    try {
        if (auto cmd = RESPParser::parse(data); !cmd.empty()) {
            auto resp = handler_.handle(cmd);
            client->write_buffer.assign(resp.begin(), resp.end());
            client->write_pos = 0;
            client->is_reading = false;
            reset_client_buffers(client);
            return true;
        }
    } catch (const std::exception& e) {
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
    auto client_opt = get_client(client_fd);
    if (!client_opt) return;

    auto& client = *client_opt;
    
    while (client->write_pos < client->write_buffer.size()) {
        if (ssize_t n = write(client_fd,
                            client->write_buffer.data() + client->write_pos,
                            client->write_buffer.size() - client->write_pos); n > 0) {
            client->write_pos += n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            remove_client(client_fd);
            return;
        }
    }
    
    reset_client_buffers(client);
    client->is_reading = true;
    
    epoll_event ev{EPOLLIN | EPOLLET, {.fd = client_fd}};
    epoll_ctl(epfd, EPOLL_CTL_MOD, client_fd, &ev);
}

void RedisServer::reset_client_buffers(std::shared_ptr<ClientContext> client) noexcept {
    client->read_pos = 0;
    client->write_pos = 0;
    
    if (client->read_buffer.size() > INITIAL_BUFFER_SIZE * 2) {
        try {
            client->read_buffer.shrink_to_fit();
            client->read_buffer.resize(INITIAL_BUFFER_SIZE);
        } catch (...) {
            // 忽略内存操作异常
        }
    }
    if (client->write_buffer.size() > INITIAL_BUFFER_SIZE * 2) {
        try {
            client->write_buffer.shrink_to_fit();
            client->write_buffer.resize(INITIAL_BUFFER_SIZE);
        } catch (...) {
            // 忽略内存操作异常
        }
    }
}