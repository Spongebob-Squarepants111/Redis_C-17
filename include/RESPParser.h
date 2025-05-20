#pragma once
#include <string>
#include <vector>
#include <stdexcept>

class RESPParser {
public:
    // 解析单个命令
    static std::vector<std::string> parse(const std::string& input);
    
    // 解析多个命令（pipeline模式）
    static std::vector<std::vector<std::string>> parse_multi(const std::string& input);

private:
    // 辅助函数
    static std::pair<std::vector<std::string>, size_t> parse_array(const std::string& input, size_t start);
    static std::pair<std::string, size_t> parse_bulk_string(const std::string& input, size_t start);
    static size_t find_next_crlf(const std::string& input, size_t start);
};
