#include "ConfigLoader.h"
#include "RedisServer.h"
#include <iostream>

int main() {
    ConfigLoader cfg;
    if (!cfg.load("config.ini")) {
        std::cerr << "Failed to load config.ini\n";
        return 1;
    }
    int port = std::stoi(cfg.get("server", "port", "6379"));
    RedisServer server(port);
    server.run();
    return 0;
}
