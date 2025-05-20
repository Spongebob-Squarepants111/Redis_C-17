#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>

class ConfigLoader {
public:
    bool load(const std::string& path);
    std::string get(const std::string& section, const std::string& key, 
                    const std::string& def = "") const;
private:
    std::unordered_map<std::string, std::string> data_;
};