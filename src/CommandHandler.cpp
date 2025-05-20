#include "CommandHandler.h"
#include <chrono>
#include <sstream>
#include <iomanip>

CommandHandler::CommandHandler(std::shared_ptr<DataStore> store) 
    : store_(store ? store : std::make_shared<DataStore>()) {
    init_handlers();
}

void CommandHandler::init_handlers() {
    // 初始化命令处理函数映射
    cmd_handlers_["set"] = [this](const auto& args) { return handle_set(args); };
    cmd_handlers_["get"] = [this](const auto& args) { return handle_get(args); };
    cmd_handlers_["del"] = [this](const auto& args) { return handle_del(args); };
    cmd_handlers_["mset"] = [this](const auto& args) { return handle_mset(args); };
    cmd_handlers_["mget"] = [this](const auto& args) { return handle_mget(args); };
    cmd_handlers_["info"] = [this](const auto& args) { return handle_info(args); };
}

std::string CommandHandler::handle(const std::vector<std::string>& cmd) {
    if (cmd.empty()) {
        return "-ERR empty command\r\n";
    }

    // 获取命令名称并转换为小写
    std::string cmd_name = cmd[0];
    std::transform(cmd_name.begin(), cmd_name.end(), cmd_name.begin(), ::tolower);

    // 查找命令处理函数
    auto it = cmd_handlers_.find(cmd_name);
    if (it == cmd_handlers_.end()) {
        return "-ERR unknown command '" + cmd_name + "'\r\n";
    }

    // 记录开始时间
    auto start = std::chrono::high_resolution_clock::now();

    // 执行命令
    std::string result = it->second(cmd);

    // 计算执行时间并更新统计
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    update_command_stats(cmd_name, duration);

    return result;
}

std::vector<std::string> CommandHandler::handle_pipeline(const std::vector<std::vector<std::string>>& cmds) {
    std::vector<std::string> results;
    results.reserve(cmds.size());

    for (const auto& cmd : cmds) {
        results.push_back(handle(cmd));
    }

    return results;
}

std::vector<std::string> CommandHandler::handle_transaction(const std::vector<std::vector<std::string>>& cmds) {
    // 事务处理
    std::vector<std::string> results;
    results.reserve(cmds.size());

    // TODO: 实现事务的原子性，可以添加MULTI/EXEC/DISCARD等命令支持

    for (const auto& cmd : cmds) {
        results.push_back(handle(cmd));
    }

    return results;
}

void CommandHandler::update_command_stats(const std::string& cmd_name, uint64_t execution_time) {
    auto& stats = cmd_stats_[cmd_name];
    stats.calls++;
    stats.total_time += execution_time;
    stats.max_time = std::max(stats.max_time, execution_time);
    stats.min_time = std::min(stats.min_time, execution_time);
}

// 命令处理函数实现
std::string CommandHandler::handle_set(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        return "-ERR wrong number of arguments for 'set' command\r\n";
    }
    store_->set(args[1], args[2]);
    return "+OK\r\n";
}

std::string CommandHandler::handle_get(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        return "-ERR wrong number of arguments for 'get' command\r\n";
    }
    auto value = store_->get(args[1]);
    if (!value) {
        return "$-1\r\n";
    }
    return "$" + std::to_string(value->size()) + "\r\n" + *value + "\r\n";
}

std::string CommandHandler::handle_del(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        return "-ERR wrong number of arguments for 'del' command\r\n";
    }
    bool success = store_->del(args[1]);
    return ":" + std::to_string(success ? 1 : 0) + "\r\n";
}

std::string CommandHandler::handle_mset(const std::vector<std::string>& args) {
    if (args.size() < 3 || args.size() % 2 != 1) {
        return "-ERR wrong number of arguments for 'mset' command\r\n";
    }
    
    std::vector<std::pair<std::string, std::string>> kvs;
    for (size_t i = 1; i < args.size(); i += 2) {
        kvs.emplace_back(args[i], args[i + 1]);
    }
    
    for (const auto& [key, value] : kvs) {
        store_->set(key, value);
    }
    
    return "+OK\r\n";
}

std::string CommandHandler::handle_mget(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return "-ERR wrong number of arguments for 'mget' command\r\n";
    }
    
    std::vector<std::string> keys(args.begin() + 1, args.end());
    std::string response = "*" + std::to_string(keys.size()) + "\r\n";
    
    for (const auto& key : keys) {
        auto value = store_->get(key);
        if (!value) {
            response += "$-1\r\n";
        } else {
            response += "$" + std::to_string(value->size()) + "\r\n" + *value + "\r\n";
        }
    }
    
    return response;
}

std::string CommandHandler::handle_info(const std::vector<std::string>& args) {
    std::stringstream ss;
    ss << "$" << 1024 << "\r\n";  // 预估响应大小
    
    // 命令统计信息
    ss << "# Commands\r\n";
    for (const auto& [cmd, stats] : cmd_stats_) {
        ss << cmd << "_calls:" << stats.calls << "\r\n";
        if (stats.calls > 0) {
            double avg_time = static_cast<double>(stats.total_time) / stats.calls;
            ss << cmd << "_avg_time:" << std::fixed << std::setprecision(3) << avg_time << "us\r\n";
            ss << cmd << "_min_time:" << stats.min_time << "us\r\n";
            ss << cmd << "_max_time:" << stats.max_time << "us\r\n";
        }
    }
    
    ss << "\r\n";
    return ss.str();
}
