#include "Config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <algorithm>

RedisServer::Config Config::load_from_file(const std::string& filename) {
    auto config = get_default_config();
    
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cout << "Warning: Could not open config file " << filename 
                  << ", using default configuration" << std::endl;
        return config;
    }
    
    std::string line, section;
    while (std::getline(file, line)) {
        // 去掉注释和空白
        auto comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        
        // 去掉首尾空白
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        if (line.empty()) continue;
        
        // 解析section
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        
        // 解析key=value
        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;
        
        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);
        
        // 去掉key和value的空白
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        // 解析配置
        if (section == "server") {
            if (key == "port") config.port = parse_int(value, config.port);
            else if (key == "host") config.host = value;
        }
        else if (section == "threading") {
            if (key == "worker_threads") config.worker_threads = parse_size_t(value, config.worker_threads);
            else if (key == "io_threads") config.io_threads = parse_size_t(value, config.io_threads);
            else if (key == "shard_count") config.shard_count = parse_size_t(value, config.shard_count);
        }
        else if (section == "performance") {
            if (key == "max_connections") config.max_connections = parse_size_t(value, config.max_connections);
            else if (key == "buffer_size") config.buffer_size = parse_size_t(value, config.buffer_size);
        }
        else if (section == "storage") {
            if (key == "cache_size_mb") config.cache_size_mb = parse_size_t(value, config.cache_size_mb);
            else if (key == "enable_persistence") config.enable_persistence = parse_bool(value, config.enable_persistence);
            else if (key == "sync_interval_sec") config.sync_interval_sec = parse_int(value, config.sync_interval_sec);
        }
    }
    
    return config;
}

RedisServer::Config Config::get_default_config() {
    RedisServer::Config config;
    
    // 智能默认值
    size_t hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) hw_threads = 4;
    
    config.worker_threads = std::min<size_t>(32, hw_threads * 2);  // 限制最大线程数
    config.io_threads = std::min<size_t>(8, hw_threads / 2);
    config.shard_count = 16;  // 固定为16，经验最优值
    
    return config;
}

int Config::parse_int(const std::string& str, int default_val) {
    try {
        return std::stoi(str);
    } catch (...) {
        return default_val;
    }
}

size_t Config::parse_size_t(const std::string& str, size_t default_val) {
    try {
        return static_cast<size_t>(std::stoull(str));
    } catch (...) {
        return default_val;
    }
}

bool Config::parse_bool(const std::string& str, bool default_val) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
        return true;
    } else if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
        return false;
    }
    return default_val;
}
