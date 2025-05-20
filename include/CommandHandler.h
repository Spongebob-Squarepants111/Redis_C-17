#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include "DataStore.h"

class CommandHandler {
public:
    explicit CommandHandler(std::shared_ptr<DataStore> store = nullptr);

    // 单个命令处理
    std::string handle(const std::vector<std::string>& cmd);
    
    // 批量命令处理
    std::vector<std::string> handle_pipeline(const std::vector<std::vector<std::string>>& cmds);
    
    // 事务命令处理
    std::vector<std::string> handle_transaction(const std::vector<std::vector<std::string>>& cmds);

private:
    // 命令处理函数类型
    using CommandFunc = std::function<std::string(const std::vector<std::string>&)>;
    
    // 命令处理函数缓存
    std::unordered_map<std::string, CommandFunc> cmd_handlers_;
    
    // 命令统计
    struct CommandStats {
        uint64_t calls{0};        // 调用次数
        uint64_t total_time{0};   // 总执行时间(微秒)
        uint64_t max_time{0};     // 最长执行时间
        uint64_t min_time{~0ull}; // 最短执行时间
    };
    std::unordered_map<std::string, CommandStats> cmd_stats_;

    // 数据存储
    std::shared_ptr<DataStore> store_;

    // 初始化命令处理函数
    void init_handlers();
    
    // 更新命令统计
    void update_command_stats(const std::string& cmd_name, uint64_t execution_time);
    
    // 常用命令的处理函数
    std::string handle_set(const std::vector<std::string>& args);
    std::string handle_get(const std::vector<std::string>& args);
    std::string handle_del(const std::vector<std::string>& args);
    std::string handle_mset(const std::vector<std::string>& args);
    std::string handle_mget(const std::vector<std::string>& args);
    std::string handle_info(const std::vector<std::string>& args);
};
