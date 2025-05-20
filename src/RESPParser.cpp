#include "RESPParser.h"
#include <sstream>

std::vector<std::string> RESPParser::parse(const std::string& input) {
    std::vector<std::string> result;
    size_t pos = 0;

    if (input[pos] != '*') return result;
    pos++;

    size_t num_args = std::stoul(input.substr(pos));
    pos = input.find("\r\n", pos) + 2;

    for (size_t i = 0; i < num_args; ++i) {
        if (input[pos] != '$') return result;
        pos++;
        size_t len = std::stoul(input.substr(pos));
        pos = input.find("\r\n", pos) + 2;
        result.push_back(input.substr(pos, len));
        pos += len + 2;
    }

    return result;
}
