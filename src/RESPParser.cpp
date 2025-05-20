#include "RESPParser.h"
#include <sstream>

std::vector<std::string> RESPParser::parse(const std::string& input) {
    if (input.empty()) {
        throw std::runtime_error("Empty input");
    }
    
    auto [result, _] = parse_array(input, 0);
    return result;
}

std::vector<std::vector<std::string>> RESPParser::parse_multi(const std::string& input) {
    if (input.empty()) {
        throw std::runtime_error("Empty input");
    }
    
    std::vector<std::vector<std::string>> commands;
    size_t pos = 0;
    
    while (pos < input.size()) {
        // 跳过前导空白字符
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\r' || input[pos] == '\n')) {
            ++pos;
        }
        
        if (pos >= input.size()) {
            break;
        }
        
        // 解析一个命令
        auto [cmd, next_pos] = parse_array(input, pos);
        if (!cmd.empty()) {
            commands.push_back(std::move(cmd));
        }
        pos = next_pos;
    }
    
    return commands;
}

std::pair<std::vector<std::string>, size_t> RESPParser::parse_array(const std::string& input, size_t start) {
    if (input[start] != '*') {
        throw std::runtime_error("Invalid array format");
    }
    
    size_t pos = start + 1;
    size_t crlf = find_next_crlf(input, pos);
    int count = std::stoi(input.substr(pos, crlf - pos));
    pos = crlf + 2;  // 跳过\r\n
    
    std::vector<std::string> result;
    for (int i = 0; i < count; ++i) {
        auto [str, next_pos] = parse_bulk_string(input, pos);
        result.push_back(std::move(str));
        pos = next_pos;
    }
    
    return {result, pos};
}

std::pair<std::string, size_t> RESPParser::parse_bulk_string(const std::string& input, size_t start) {
    if (input[start] != '$') {
        throw std::runtime_error("Invalid bulk string format");
    }
    
    size_t pos = start + 1;
    size_t crlf = find_next_crlf(input, pos);
    int len = std::stoi(input.substr(pos, crlf - pos));
    
    if (len < 0) {
        return {"", crlf + 2};  // NULL bulk string
    }
    
    pos = crlf + 2;  // 跳过\r\n
    std::string result = input.substr(pos, len);
    pos += len + 2;  // 跳过字符串和\r\n
    
    return {result, pos};
}

size_t RESPParser::find_next_crlf(const std::string& input, size_t start) {
    size_t pos = input.find("\r\n", start);
    if (pos == std::string::npos) {
        throw std::runtime_error("Missing CRLF");
    }
    return pos;
}
