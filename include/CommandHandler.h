#pragma once
#include "DataStore.h"
#include <vector>
#include <string>

class CommandHandler {
public:
    std::string handle(const std::vector<std::string>& cmd);
private:
    DataStore store_;
};
