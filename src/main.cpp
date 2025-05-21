#include "ConfigLoader.h"
#include "RedisServer.h"
#include "Logo.h"
#include <iostream>

int main() {
    Logo::print();

    ConfigLoader cfg;
    if (!cfg.load("config.ini")) {
        std::cerr << "Failed to load config.ini\n";
        return 1;
    }

    int port = std::stoi(cfg.get("server", "port", "6379"));
    std::string host = cfg.get("server", "host", "0.0.0.0");
    std::cout << "Config loaded successfully." << std::endl;

    RedisServer server(port, host);
    server.run();

    std::cout << "===== REDISC++17 Server Stopped =====" << std::endl;
    return 0;
}
