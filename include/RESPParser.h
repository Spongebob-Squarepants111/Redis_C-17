#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <string_view>
#include <optional>
#include <deque>
#include <memory>

// RESP协议解析器，支持增量解析和零拷贝
class RESPParser {
public:
    // 用于表示一个RESP类型的值
    struct RESPValue {
        enum class Type {
            SimpleString,
            Error,
            Integer,
            BulkString,
            Array,
            Invalid
        };
        
        Type type = Type::Invalid;
        std::string_view value; // 原始值的视图
        std::vector<std::unique_ptr<RESPValue>> array; // 仅用于数组类型
        
        // 将RESPValue转换为命令格式（字符串数组）
        std::vector<std::string> to_command() const;
    };
    
    // 构造函数
    RESPParser() = default;
    
    // 增量解析，处理部分数据
    // 返回解析出的完整命令列表，未完成的保持在内部状态中
    std::vector<std::vector<std::string>> parse(std::string_view data);
    
    // 获取并清除当前已解析的所有命令
    std::vector<std::vector<std::string>> get_commands();

private:
    // 解析状态
    enum class ParseState {
        Initial,        // 初始状态
        ExpectingType,  // 期望类型标记(*,$,:,+,-)
        ReadingBulkLen, // 正在读取批量字符串长度
        ReadingBulk,    // 正在读取批量字符串内容
        ReadingArrayLen,// 正在读取数组长度
        ReadingArray,   // 正在读取数组元素
        Complete        // 完成解析
    };
    
    struct ParseContext {
        ParseState state = ParseState::Initial;
        std::string buffer; // 收集完整的数据缓冲区
        size_t position = 0; // 当前解析位置
        int remaining = 0; // 剩余元素数或剩余字节数
        std::unique_ptr<RESPValue> current_value; // 当前正在解析的值
        std::deque<std::unique_ptr<RESPValue>> completed_values; // 已完成的值
        std::vector<std::unique_ptr<RESPValue>> value_stack; // 解析栈
    };
    
    // 解析上下文
    ParseContext context_;
    
    // 内部解析方法
    std::optional<std::unique_ptr<RESPValue>> try_parse_next(std::string_view data);
    std::optional<std::unique_ptr<RESPValue>> parse_simple_string(std::string_view data, size_t& pos);
    std::optional<std::unique_ptr<RESPValue>> parse_error(std::string_view data, size_t& pos);
    std::optional<std::unique_ptr<RESPValue>> parse_integer(std::string_view data, size_t& pos);
    std::optional<std::unique_ptr<RESPValue>> parse_bulk_string(std::string_view data, size_t& pos);
    std::optional<std::unique_ptr<RESPValue>> parse_array(std::string_view data, size_t& pos);
    
    // 查找CRLF
    static size_t find_crlf(std::string_view data, size_t start);
    
    // 添加已完成的命令
    void add_completed_command(std::unique_ptr<RESPValue> value);
};
