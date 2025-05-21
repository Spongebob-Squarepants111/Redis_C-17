#include "ConfigLoader.h"

bool ConfigLoader::load(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line, curSec;
    while (std::getline(in, line)) {
        // 去掉注释和空白
        auto pos = line.find('#');
        if (pos != std::string::npos) line.resize(pos);
        std::istringstream is(line);
        std::string tok;
        if (!(is >> tok)) continue;
        if (tok.front() == '[' && tok.back() == ']') {
            curSec = tok.substr(1, tok.size() - 2);
        } else if (auto eq = tok.find('='); eq != std::string::npos) {
            std::string key = tok.substr(0, eq);
            std::string val = tok.substr(eq + 1);
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