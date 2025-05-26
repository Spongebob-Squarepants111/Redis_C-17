#include "RESPParser.h"
#include <sstream>
#include <cassert>
#include <cstring>

// RESPValue方法实现
std::vector<std::string> RESPParser::RESPValue::to_command() const {
    if (type != Type::Array) {
        return {}; // 命令必须是数组
    }
    
    std::vector<std::string> result;
    for (const auto& element : array) {
        if (element->type == Type::BulkString) {
            result.push_back(std::string(element->value));
        } else {
            // 命令的所有元素都必须是批量字符串
            return {};
        }
    }
    
    return result;
}

// 静态方法，兼容旧接口
std::vector<std::string> RESPParser::parse_command(const std::string& input) {
    RESPParser parser;
    auto commands = parser.parse(input);
    if (commands.empty()) {
        return {};
    }
    return commands[0];
}

std::vector<std::vector<std::string>> RESPParser::parse_multi(const std::string& input) {
    RESPParser parser;
    return parser.parse(input);
}

// 实例方法
void RESPParser::reset() {
    context_ = ParseContext{};
}

std::vector<std::vector<std::string>> RESPParser::parse(std::string_view data) {
    // 将新数据追加到缓冲区
    size_t old_size = context_.buffer.size();
    context_.buffer.append(data.data(), data.size());
    
    // 创建整个缓冲区的视图
    std::string_view buffer_view(context_.buffer);
    
    // 尝试解析尽可能多的完整命令
    while (true) {
        auto resp_value = try_parse_next(buffer_view);
        if (!resp_value) {
            break; // 没有更多完整的命令
        }
        
        // 添加到已完成命令列表
        add_completed_command(std::move(*resp_value));
    }
    
    // 压缩缓冲区 - 移除已处理的数据
    if (context_.position > 0) {
        context_.buffer.erase(0, context_.position);
        context_.position = 0;
    }
    
    // 返回所有完整的命令
    return get_commands();
}

bool RESPParser::has_complete_command() const {
    return !context_.completed_values.empty();
}

std::vector<std::vector<std::string>> RESPParser::get_commands() {
    std::vector<std::vector<std::string>> commands;
    
    while (!context_.completed_values.empty()) {
        auto value = std::move(context_.completed_values.front());
        context_.completed_values.pop_front();
        
        auto cmd = value->to_command();
        if (!cmd.empty()) {
            commands.push_back(std::move(cmd));
        }
    }
    
    return commands;
}

std::optional<std::unique_ptr<RESPParser::RESPValue>> RESPParser::try_parse_next(std::string_view data) {
    // 检查是否有足够的数据
    if (context_.position >= data.size()) {
        return std::nullopt;
    }
    
    // 根据当前字符决定解析方法
    char type = data[context_.position];
    std::optional<std::unique_ptr<RESPValue>> result;
    
    switch (type) {
        case '+': // 简单字符串
            result = parse_simple_string(data, context_.position);
            break;
        case '-': // 错误
            result = parse_error(data, context_.position);
            break;
        case ':': // 整数
            result = parse_integer(data, context_.position);
            break;
        case '$': // 批量字符串
            result = parse_bulk_string(data, context_.position);
            break;
        case '*': // 数组
            result = parse_array(data, context_.position);
            break;
        default: // 不支持的类型
            // 尝试跳过无效数据直到找到有效的类型标记
            while (context_.position < data.size() && 
                   data[context_.position] != '+' && 
                   data[context_.position] != '-' && 
                   data[context_.position] != ':' && 
                   data[context_.position] != '$' && 
                   data[context_.position] != '*') {
                context_.position++;
            }
            return std::nullopt;
    }
    
    return result;
}

std::optional<std::unique_ptr<RESPParser::RESPValue>> RESPParser::parse_simple_string(std::string_view data, size_t& pos) {
    pos++; // 跳过'+'
    
    size_t crlf = find_crlf(data, pos);
    if (crlf == std::string::npos) {
        // 数据不完整
        return std::nullopt;
    }
    
    auto value = std::make_unique<RESPValue>();
    value->type = RESPValue::Type::SimpleString;
    value->value = data.substr(pos, crlf - pos);
    
    pos = crlf + 2; // 跳过\r\n
    return value;
}

std::optional<std::unique_ptr<RESPParser::RESPValue>> RESPParser::parse_error(std::string_view data, size_t& pos) {
    pos++; // 跳过'-'
    
    size_t crlf = find_crlf(data, pos);
    if (crlf == std::string::npos) {
        // 数据不完整
        return std::nullopt;
    }
    
    auto value = std::make_unique<RESPValue>();
    value->type = RESPValue::Type::Error;
    value->value = data.substr(pos, crlf - pos);
    
    pos = crlf + 2; // 跳过\r\n
    return value;
}

std::optional<std::unique_ptr<RESPParser::RESPValue>> RESPParser::parse_integer(std::string_view data, size_t& pos) {
    pos++; // 跳过':'
    
    size_t crlf = find_crlf(data, pos);
    if (crlf == std::string::npos) {
        // 数据不完整
        return std::nullopt;
    }
    
    auto value = std::make_unique<RESPValue>();
    value->type = RESPValue::Type::Integer;
    value->value = data.substr(pos, crlf - pos);
    
    pos = crlf + 2; // 跳过\r\n
    return value;
}

std::optional<std::unique_ptr<RESPParser::RESPValue>> RESPParser::parse_bulk_string(std::string_view data, size_t& pos) {
    pos++; // 跳过'$'
    
    size_t crlf = find_crlf(data, pos);
    if (crlf == std::string::npos) {
        // 数据不完整
        return std::nullopt;
    }
    
    // 解析长度
    std::string len_str(data.substr(pos, crlf - pos));
    int len = std::stoi(len_str);
    
    pos = crlf + 2; // 跳过\r\n
    
    if (len < 0) {
        // 处理NULL批量字符串
        auto value = std::make_unique<RESPValue>();
        value->type = RESPValue::Type::BulkString;
        value->value = std::string_view();
        return value;
    }
    
    // 检查是否有足够的数据
    if (pos + len + 2 > data.size()) {
        // 数据不完整
        return std::nullopt;
    }
    
    auto value = std::make_unique<RESPValue>();
    value->type = RESPValue::Type::BulkString;
    value->value = data.substr(pos, len);
    
    pos += len + 2; // 跳过字符串内容和\r\n
    return value;
}

std::optional<std::unique_ptr<RESPParser::RESPValue>> RESPParser::parse_array(std::string_view data, size_t& pos) {
    pos++; // 跳过'*'
    
    size_t crlf = find_crlf(data, pos);
    if (crlf == std::string::npos) {
        // 数据不完整
        return std::nullopt;
    }
    
    // 解析数组长度
    std::string len_str(data.substr(pos, crlf - pos));
    int count = std::stoi(len_str);
    
    pos = crlf + 2; // 跳过\r\n
    
    auto array = std::make_unique<RESPValue>();
    array->type = RESPValue::Type::Array;
    
    if (count < 0) {
        // NULL数组
        return array;
    }
    
    // 解析数组元素
    for (int i = 0; i < count; ++i) {
        auto element = try_parse_next(data);
        if (!element) {
            // 数据不完整
            return std::nullopt;
        }
        
        array->array.push_back(std::move(*element));
    }
    
    return array;
}

void RESPParser::add_completed_command(std::unique_ptr<RESPValue> value) {
    // 只有数组类型才被视为命令
    if (value && value->type == RESPValue::Type::Array) {
        context_.completed_values.push_back(std::move(value));
    }
}

size_t RESPParser::find_crlf(std::string_view data, size_t start) {
    // 查找\r\n序列
    for (size_t i = start; i < data.size() - 1; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            return i;
        }
    }
    return std::string::npos;
}
