#include "DataStore.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <xxhash.h>
#include <cstring>

DataStore::DataStore(const Options& options)
    : shards_(options.shard_count)
    , shard_count_(options.shard_count)
    , cache_(options.cache_size, options.memory_pool_block_size)
    , enable_compression_(options.enable_compression)
    , persist_path_(options.persist_path)
    , sync_interval_(options.sync_interval) {
    
    // 创建持久化目录
    std::filesystem::create_directories(persist_path_);
    
    // 初始化分片
    for (size_t i = 0; i < shard_count_; ++i) {
        shards_[i] = std::make_unique<Shard>();
        shards_[i]->persist_file = persist_path_ + "shard_" + std::to_string(i) + ".dat";
        load_shard(i);
    }
    
    // 启动同步线程
    start_sync_thread();
}

DataStore::~DataStore() {
    // 停止同步线程
    should_stop_ = true;
    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }
    
    // 保存所有分片
    flush();
}

void DataStore::set(const std::string& key, const std::string& value) {
    std::string stored_value = enable_compression_ ? compress(value) : value;
    
    // 更新缓存
    cache_.put(key, value);
    
    // 更新存储
    size_t shard_idx = get_shard_index(key);
    auto& shard = shards_[shard_idx];
    {
        std::unique_lock<std::shared_mutex> lock(shard->mutex);
        shard->store[key] = std::move(stored_value);
    }
}

std::optional<std::string> DataStore::get(const std::string& key) {
    // 先查询缓存
    if (auto cached = cache_.get(key)) {
        return cached;
    }
    
    // 缓存未命中，查询存储
    size_t shard_idx = get_shard_index(key);
    auto& shard = shards_[shard_idx];
    {
        std::shared_lock<std::shared_mutex> lock(shard->mutex);
        auto it = shard->store.find(key);
        if (it == shard->store.end()) {
            return std::nullopt;
        }
        
        std::string value = enable_compression_ ? decompress(it->second) : it->second;
        // 更新缓存
        cache_.put(key, value);
        return value;
    }
}

bool DataStore::del(const std::string& key) {
    // 从缓存中删除
    cache_.remove(key);
    
    // 从存储中删除
    size_t shard_idx = get_shard_index(key);
    auto& shard = shards_[shard_idx];
    {
        std::unique_lock<std::shared_mutex> lock(shard->mutex);
        return shard->store.erase(key) > 0;
    }
}

void DataStore::flush() {
    for (size_t i = 0; i < shard_count_; ++i) {
        persist_shard(i);
    }
}

// 一致性哈希实现
uint32_t DataStore::hash(const std::string& key) const {
    return XXH32(key.data(), key.size(), 0);
}

size_t DataStore::get_shard_index(const std::string& key) const {
    return hash(key) % shard_count_;
}

// 压缩功能实现
std::string DataStore::compress(const std::string& data) {
    std::vector<unsigned char> compressed(data.size() + 128);

    z_stream zs{};

    if (deflateInit(&zs, Z_BEST_COMPRESSION) != Z_OK) {
        throw std::runtime_error("deflateInit failed");
    }

    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());
    zs.next_out = compressed.data();
    zs.avail_out = static_cast<uInt>(compressed.size());

    int ret = deflate(&zs, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&zs);
        throw std::runtime_error("deflate failed");
    }

    deflateEnd(&zs);

    compressed.resize(zs.total_out);

    return std::string(reinterpret_cast<char*>(compressed.data()), compressed.size());
}

std::string DataStore::decompress(const std::string& data) {
    std::vector<unsigned char> decompressed;
    decompressed.resize(data.size() * 2); // 初始缓冲区大小

    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        throw std::runtime_error("inflateInit failed");
    }

    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    int ret;
    do {
        if (zs.total_out >= decompressed.size()) {
            // 缓冲区不够，扩大一倍
            decompressed.resize(decompressed.size() * 2);
        }

        zs.next_out = decompressed.data() + zs.total_out;
        zs.avail_out = decompressed.size() - zs.total_out;

        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&zs);
            throw std::runtime_error("inflate failed");
        }
    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);

    decompressed.resize(zs.total_out);

    return std::string(reinterpret_cast<char*>(decompressed.data()), decompressed.size());
}

// 持久化功能实现
void DataStore::persist_shard(size_t shard_index) {
    auto& shard = shards_[shard_index];
    std::ofstream file(shard->persist_file, std::ios::binary);
    
    std::unique_lock<std::shared_mutex> lock(shard->mutex);
    for (const auto& [key, value] : shard->store) {
        // 写入key长度和value长度
        uint32_t key_size = key.size();
        uint32_t value_size = value.size();
        file.write(reinterpret_cast<char*>(&key_size), sizeof(key_size));
        file.write(reinterpret_cast<char*>(&value_size), sizeof(value_size));
        
        // 写入key和value
        file.write(key.data(), key_size);
        file.write(value.data(), value_size);
    }
}

void DataStore::load_shard(size_t shard_index) {
    auto& shard = shards_[shard_index];
    std::ifstream file(shard->persist_file, std::ios::binary);
    if (!file) return;
    
    std::unique_lock<std::shared_mutex> lock(shard->mutex);
    while (file) {
        uint32_t key_size, value_size;
        
        // 读取大小
        file.read(reinterpret_cast<char*>(&key_size), sizeof(key_size));
        if (!file) break;
        file.read(reinterpret_cast<char*>(&value_size), sizeof(value_size));
        if (!file) break;
        
        // 读取数据
        std::string key(key_size, '\0');
        std::string value(value_size, '\0');
        file.read(&key[0], key_size);
        file.read(&value[0], value_size);
        
        shard->store[key] = value;
    }
}

void DataStore::start_sync_thread() {
    sync_thread_ = std::thread([this] { sync_routine(); });
}

void DataStore::sync_routine() {
    while (!should_stop_) {
        std::this_thread::sleep_for(sync_interval_);
        flush();
    }
}
