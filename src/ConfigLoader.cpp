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

std::string ConfigLoader::get(const std::string& section, const std::string& key,
                              const std::string& def) const {
    auto it = data_.find(section + "." + key);
    return it == data_.end() ? def : it->second;
}