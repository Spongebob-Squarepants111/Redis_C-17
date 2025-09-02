#pragma once
#include <string>
#include <string_view>
#include <fstream>
#include <sstream>
#include <unordered_map>

class ConfigLoader {
public:
    bool load(const std::string& path);
    
    // 获取字符串值
    std::string get(std::string_view section, std::string_view key, 
                    std::string_view def = "") const;
    
    // 获取整数值
    int get_int(std::string_view section, std::string_view key, int def = 0) const;
    
    // 获取size_t值
    size_t get_size_t(std::string_view section, std::string_view key, size_t def = 0) const;
    
    // 获取浮点数值
    float get_float(std::string_view section, std::string_view key, float def = 0.0f) const;
    
    // 获取布尔值
    bool get_bool(std::string_view section, std::string_view key, bool def = false) const;
    
private:
    std::unordered_map<std::string, std::string> data_;
};