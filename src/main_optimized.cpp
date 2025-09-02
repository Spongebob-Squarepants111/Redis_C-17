#include "SimpleConfig.h"
#include "OptimizedRedisServer.h"
#include "Logo.h"
#include <iostream>
#include <signal.h>

// 全局服务器指针，用于信号处理
std::unique_ptr<OptimizedRedisServer> g_server;

void signal_handler(int signal) {
    if (g_server) {
        std::cout << "\nReceived signal " << signal << ", shutting down gracefully..." << std::endl;
        g_server->stop();
    }
}

int main(int argc, char* argv[]) {
    Logo::print();
    
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 确定配置文件
    std::string config_file = "config_optimized.ini";
    if (argc > 1) {
        config_file = argv[1];
    }
    
    try {
        // 加载配置
        auto config = SimpleConfig::load_from_file(config_file);
        
        std::cout << "Loaded configuration from: " << config_file << std::endl;
        std::cout << "Server configuration:" << std::endl;
        std::cout << "  Host: " << config.host << std::endl;
        std::cout << "  Port: " << config.port << std::endl;
        std::cout << "  Worker threads: " << config.worker_threads << std::endl;
        std::cout << "  IO threads: " << config.io_threads << std::endl;
        std::cout << "  Shard count: " << config.shard_count << std::endl;
        std::cout << "  Max connections: " << config.max_connections << std::endl;
        std::cout << std::endl;
        
        // 创建并启动服务器
        g_server = std::make_unique<OptimizedRedisServer>(config);
        g_server->run();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "===== Optimized Redis Server Stopped =====" << std::endl;
    return 0;
}
