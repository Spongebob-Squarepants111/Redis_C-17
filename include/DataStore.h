#pragma once
#include <unordered_map>
#include <string>
#include <optional>
#include <mutex>
#include <vector>
#include <memory>
#include <functional>

class DataStore {
public:
    explicit DataStore(size_t shard_count = 128*2);  // 默认16个分片
    void set(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);

private:
    // 分片结构
    struct Shard {
        std::unordered_map<std::string, std::string> store;
        mutable std::mutex mutex;
    };

    // 获取key对应的分片
    size_t get_shard_index(const std::string& key) const;
    
    // 分片存储
    std::vector<std::unique_ptr<Shard>> shards_;
    const size_t shard_count_;
};
