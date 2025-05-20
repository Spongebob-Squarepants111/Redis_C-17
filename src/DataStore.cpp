#include "DataStore.h"
#include <functional>

DataStore::DataStore(size_t shard_count)
    : shard_count_(shard_count) {
    // 初始化分片
    shards_.reserve(shard_count_);
    for (size_t i = 0; i < shard_count_; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }
}

size_t DataStore::get_shard_index(const std::string& key) const {
    // 使用std::hash计算key的哈希值，然后对分片数取模
    return std::hash<std::string>{}(key) % shard_count_;
}

void DataStore::set(const std::string& key, const std::string& value) {
    size_t shard_idx = get_shard_index(key);
    auto& shard = shards_[shard_idx];
    
    // 只锁定对应的分片
    std::lock_guard<std::mutex> lock(shard->mutex);
    shard->store[key] = value;
}

std::optional<std::string> DataStore::get(const std::string& key) {
    size_t shard_idx = get_shard_index(key);
    auto& shard = shards_[shard_idx];
    
    // 只锁定对应的分片
    std::lock_guard<std::mutex> lock(shard->mutex);
    auto it = shard->store.find(key);
    if (it != shard->store.end()) {
        return it->second;
    }
    return std::nullopt;
}
