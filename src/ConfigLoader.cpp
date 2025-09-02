#include "ConfigLoader.h"
#include <algorithm>
#include <cctype>

bool ConfigLoader::load(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line, curSec;
    while (std::getline(in, line)) {
        // 去掉注释和空白
        auto pos = line.find('#');
        if (pos != std::string::npos) line.resize(pos);
        
        // 去掉首尾空白
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        line.erase(std::find_if(line.rbegin(), line.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), line.end());
        
        if (line.empty()) continue;
        
        if (line.front() == '[' && line.back() == ']') {
            curSec = line.substr(1, line.size() - 2);
        } else if (auto eq = line.find('='); eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            
            // 去掉key和value的空白
            key.erase(key.begin(), std::find_if(key.begin(), key.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
            key.erase(std::find_if(key.rbegin(), key.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(), key.end());
            
            val.erase(val.begin(), std::find_if(val.begin(), val.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
            val.erase(std::find_if(val.rbegin(), val.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(), val.end());
            
            data_[curSec + "." + key] = val;
        }
    }
    return true;
}

std::string ConfigLoader::get(std::string_view section, std::string_view key,
                            std::string_view def) const {
    std::string lookup_key;
    lookup_key.reserve(section.size() + 1 + key.size());
    lookup_key.append(section).append(".").append(key);
    
    auto it = data_.find(lookup_key);
    return it == data_.end() ? std::string(def) : it->second;
}

int ConfigLoader::get_int(std::string_view section, std::string_view key, int def) const {
    auto str = get(section, key);
    if (str.empty()) return def;
    try {
        return std::stoi(str);
    } catch (...) {
        return def;
    }
}

size_t ConfigLoader::get_size_t(std::string_view section, std::string_view key, size_t def) const {
    auto str = get(section, key);
    if (str.empty()) return def;
    try {
        return static_cast<size_t>(std::stoull(str));
    } catch (...) {
        return def;
    }
}

float ConfigLoader::get_float(std::string_view section, std::string_view key, float def) const {
    auto str = get(section, key);
    if (str.empty()) return def;
    try {
        return std::stof(str);
    } catch (...) {
        return def;
    }
}

bool ConfigLoader::get_bool(std::string_view section, std::string_view key, bool def) const {
    auto str = get(section, key);
    if (str.empty()) return def;
    
    // 转换为小写
    std::string lower_str = str;
    std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), ::tolower);
    
    if (lower_str == "true" || lower_str == "1" || lower_str == "yes" || lower_str == "on") {
        return true;
    } else if (lower_str == "false" || lower_str == "0" || lower_str == "no" || lower_str == "off") {
        return false;
    } else {
        return def;
    }
}