#pragma once
#include <vector>
#include <string>

class RESPParser {
public:
    static std::vector<std::string> parse(const std::string& input);
};
