#pragma once
#include "OptimizedRedisServer.h"
#include <string>

class SimpleConfig {
public:
    static OptimizedRedisServer::Config load_from_file(const std::string& filename);
    static OptimizedRedisServer::Config get_default_config();
    
private:
    static int parse_int(const std::string& str, int default_val);
    static size_t parse_size_t(const std::string& str, size_t default_val);
    static bool parse_bool(const std::string& str, bool default_val);
};
