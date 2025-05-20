#include "CommandHandler.h"

std::string CommandHandler::handle(const std::vector<std::string>& cmd) {
    if (cmd.empty()) return "-Error: Empty command\r\n";

    std::string cmd_name = cmd[0];
    if (cmd_name == "SET" && cmd.size() == 3) {
        store_.set(cmd[1], cmd[2]);
        return "+OK\r\n";
    } else if (cmd_name == "GET" && cmd.size() == 2) {
        auto val = store_.get(cmd[1]);
        if (val.has_value())
            return "$" + std::to_string(val->size()) + "\r\n" + *val + "\r\n";
        else
            return "$-1\r\n";
    }

    return "-Error: Unknown or malformed command\r\n";
}
