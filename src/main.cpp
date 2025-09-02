#include "Config.h"
#include "RedisServer.h"
#include "Logo.h"
#include <iostream>

int main(int argc, char* argv[]) {
    Logo::print();

    // 默认配置文件，可通过命令行参数覆盖
    std::string config_file = "config.ini";
    if (argc > 1) {
        config_file = argv[1];
    }

    Config config;
    if (!config.load(config_file)) {
        std::cerr << "Failed to load " << config_file << "\n";
        return 1;
    }

    std::cout << "Config loaded from: " << config_file << std::endl;

    std::cout << "Config loaded successfully." << std::endl;
    std::cout << "Server will listen on " << config.server().host << ":" << config.server().port << std::endl;

    RedisServer server(config);
    server.run();

    std::cout << "===== REDISC++17 Server Stopped =====" << std::endl;
    return 0;
}
