#include "DataStore.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <xxhash.h>
#include <cstring>
#include <unordered_map>

DataStore::DataStore(const Options& options)
    : shards_(options.shard_count)
    , shard_count_(options.shard_count)
    , bucket_per_shard_(options.bucket_per_shard)
    , enable_compression_(options.enable_compression)
    , persist_path_(options.persist_path)
    , sync_interval_(options.sync_interval)
    , cache_([&options]() {
        AdaptiveCache::Options cache_options;
        cache_options.shard_count = options.cache_shards;
        cache_options.initial_capacity = options.cache_size;
        cache_options.policy_type = options.cache_policy;
        cache_options.memory_pool_block_size = options.memory_pool_block_size;
        cache_options.enable_adaptive_sizing = options.adaptive_cache_sizing;
        return AdaptiveCache(cache_options);
      }())
{
    // 创建持久化目录
    std::filesystem::create_directories(persist_path_);
    
    // 初始化分片
    for (size_t i = 0; i < shard_count_; ++i) {
        shards_[i] = std::make_unique<Shard>(bucket_per_shard_);
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
    set(std::string_view(key), std::string_view(value));
}

std::optional<std::string> DataStore::get(const std::string& key) {
    return get(std::string_view(key));
}

bool DataStore::del(const std::string& key) {
    return del(std::string_view(key));
}

void DataStore::set(std::string_view key, std::string_view value) {
    // 优化：仅在必要时进行string转换
    std::string key_str;
    std::string value_str;
    std::string stored_value;
    
    // 只有在需要压缩时才转换value
    if (enable_compression_) {
        value_str = std::string(value);
        stored_value = compress(value_str);
        // 更新缓存使用未压缩的值
        key_str = std::string(key);
        cache_.put(key_str, value_str);
    } else {
        // 不压缩时直接使用string_view的数据
        key_str = std::string(key);
        value_str = std::string(value);
        stored_value = value_str;
        cache_.put(key_str, value_str);
    }
    
    // 更新存储
    size_t shard_idx = get_shard_index(key_str);
    auto& shard = shards_[shard_idx];
    
    // 计算桶索引并获取对应的桶
    size_t bucket_idx = get_bucket_index(key_str, bucket_per_shard_);
    auto& bucket = shard->buckets[bucket_idx];
    
    // 获取子map索引
    size_t submap_idx = bucket->get_submap_index(key_str);
    auto& submap = bucket->sub_maps[submap_idx];
    
    // 锁定单个子map
    {
        std::unique_lock<std::shared_mutex> lock(submap.mutex);
        submap.store[key_str] = std::move(stored_value);
    }
}

std::optional<std::string> DataStore::get(std::string_view key) {
    // 转换为std::string
    std::string key_str(key);
    
    // 先查询缓存
    if (auto cached = cache_.get(key_str)) {
        return cached;
    }
    
    // 缓存未命中，查询存储
    size_t shard_idx = get_shard_index(key_str);
    auto& shard = shards_[shard_idx];
    
    // 计算桶索引并获取对应的桶
    size_t bucket_idx = get_bucket_index(key_str, bucket_per_shard_);
    auto& bucket = shard->buckets[bucket_idx];
    
    // 获取子map索引
    size_t submap_idx = bucket->get_submap_index(key_str);
    auto& submap = bucket->sub_maps[submap_idx];
    
    // 只锁定单个子map
    {
        std::shared_lock<std::shared_mutex> lock(submap.mutex);
        auto it = submap.store.find(key_str);
        if (it == submap.store.end()) {
            return std::nullopt;
        }
        
        std::string value = enable_compression_ ? decompress(it->second) : it->second;
        // 更新缓存
        cache_.put(key_str, value);
        return value;
    }
}

bool DataStore::del(std::string_view key) {
    // 转换为std::string
    std::string key_str(key);
    
    // 从缓存中删除
    cache_.remove(key_str);
    
    // 从存储中删除
    size_t shard_idx = get_shard_index(key_str);
    auto& shard = shards_[shard_idx];
    
    // 计算桶索引并获取对应的桶
    size_t bucket_idx = get_bucket_index(key_str, bucket_per_shard_);
    auto& bucket = shard->buckets[bucket_idx];
    
    // 获取子map索引
    size_t submap_idx = bucket->get_submap_index(key_str);
    auto& submap = bucket->sub_maps[submap_idx];
    
    // 只锁定单个子map
    {
        std::unique_lock<std::shared_mutex> lock(submap.mutex);
        return submap.store.erase(key_str) > 0;
    }
}

void DataStore::flush() {
    for (size_t i = 0; i < shard_count_; ++i) {
        persist_shard(i);
    }
}

// 缓存管理接口实现
void DataStore::set_cache_policy(CachePolicy::Type policy_type) {
    cache_.set_policy(policy_type);
}

CachePolicy::Type DataStore::get_cache_policy() const {
    return cache_.get_policy_type();
}

std::string DataStore::get_cache_policy_name() const {
    return cache_.get_policy_name();
}

void DataStore::enable_adaptive_cache(bool enable) {
    cache_.enable_adaptive_sizing(enable);
}

bool DataStore::is_adaptive_cache_enabled() const {
    return cache_.is_adaptive_sizing_enabled();
}

void DataStore::set_cache_capacity(size_t capacity) {
    cache_.set_capacity(capacity);
}

size_t DataStore::get_cache_capacity() const {
    return cache_.capacity();
}

double DataStore::get_cache_hit_ratio() const {
    return cache_.hit_ratio();
}

AdaptiveCache::Stats DataStore::get_cache_stats() const {
    return cache_.get_stats();
}

// 一致性哈希实现
uint32_t DataStore::hash(const std::string& key) const {
    return XXH32(key.data(), key.size(), 0);
}

size_t DataStore::get_shard_index(const std::string& key) const {
    return hash(key) % shard_count_;
}

size_t DataStore::get_bucket_index(const std::string& key, size_t bucket_count) const {
    // 使用二次哈希来确定桶索引，避免分片和桶使用相同的哈希值
    return XXH32(key.data(), key.size(), 0x42) % bucket_count;
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
    
    // 遍历所有桶
    for (size_t bucket_idx = 0; bucket_idx < bucket_per_shard_; ++bucket_idx) {
        auto& bucket = shard->buckets[bucket_idx];
        
        // 遍历所有子map
        for (size_t submap_idx = 0; submap_idx < Bucket::SUB_MAPS_COUNT; ++submap_idx) {
            auto& submap = bucket->sub_maps[submap_idx];
            std::shared_lock<std::shared_mutex> lock(submap.mutex);
        
            for (const auto& [key, value] : submap.store) {
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
    }
}

void DataStore::load_shard(size_t shard_index) {
    auto& shard = shards_[shard_index];
    std::ifstream file(shard->persist_file, std::ios::binary);
    if (!file) return;
    
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
        
        // 确定数据应该存储在哪个桶中
        size_t bucket_idx = get_bucket_index(key, bucket_per_shard_);
        auto& bucket = shard->buckets[bucket_idx];
        
        // 确定子map索引
        size_t submap_idx = bucket->get_submap_index(key);
        auto& submap = bucket->sub_maps[submap_idx];
        
        // 将数据存入子map中
        std::unique_lock<std::shared_mutex> lock(submap.mutex);
        submap.store[key] = value;
    }
}

void DataStore::start_sync_thread() {
    sync_thread_ = std::thread([this] { sync_routine(); });
}

void DataStore::sync_routine() {
    while (!should_stop_) {
        // 每隔sync_interval_秒进行一次同步
        std::this_thread::sleep_for(sync_interval_);
        
        if (should_stop_) break;
        
        // 同步所有分片
        for (size_t i = 0; i < shard_count_; ++i) {
            persist_shard(i);
        }
    }
}

// 批量设置多个键值对 (string_view版本)
void DataStore::multi_set(const std::vector<std::pair<std::string_view, std::string_view>>& kvs) {
    // 按照shard和bucket对键值对进行分组
    std::unordered_map<size_t, std::unordered_map<size_t, std::unordered_map<size_t, std::vector<std::pair<std::string, std::string>>>>> grouped_kvs;
    
    // 第一阶段：分组
    for (const auto& [key, value] : kvs) {
        std::string key_str(key);
        std::string value_str(value);
        
        size_t shard_idx = get_shard_index(key_str);
        size_t bucket_idx = get_bucket_index(key_str, bucket_per_shard_);
        auto& bucket = shards_[shard_idx]->buckets[bucket_idx];
        size_t submap_idx = bucket->get_submap_index(key_str);
        
        grouped_kvs[shard_idx][bucket_idx][submap_idx].emplace_back(key_str, value_str);
        
        // 更新缓存
        cache_.put(key_str, value_str);
    }
    
    // 第二阶段：批量设置
    for (const auto& [shard_idx, shard_data] : grouped_kvs) {
        for (const auto& [bucket_idx, bucket_data] : shard_data) {
            auto& bucket = shards_[shard_idx]->buckets[bucket_idx];
            
            for (const auto& [submap_idx, submap_data] : bucket_data) {
                auto& submap = bucket->sub_maps[submap_idx];
                
                // 一次性获取锁，设置多个值
                std::unique_lock<std::shared_mutex> lock(submap.mutex);
                for (const auto& [key, value] : submap_data) {
                    std::string stored_value = enable_compression_ ? compress(value) : value;
                    submap.store[key] = std::move(stored_value);
                }
            }
        }
    }
}

// 批量获取多个键的值 (string_view版本)
std::vector<std::optional<std::string>> DataStore::multi_get(const std::vector<std::string_view>& keys) {
    std::vector<std::optional<std::string>> results(keys.size());
    
    // 按照shard和bucket对键进行分组
    std::unordered_map<size_t, std::unordered_map<size_t, std::unordered_map<size_t, std::vector<size_t>>>> grouped_indices;
    
    // 第一阶段：查询缓存并分组未命中的键
    for (size_t i = 0; i < keys.size(); ++i) {
        std::string key_str(keys[i]);
        
        // 先查询缓存
        if (auto cached = cache_.get(key_str)) {
            results[i] = std::move(cached);
            continue;
        }
        
        // 缓存未命中，准备从存储中查询
        size_t shard_idx = get_shard_index(key_str);
        size_t bucket_idx = get_bucket_index(key_str, bucket_per_shard_);
        auto& bucket = shards_[shard_idx]->buckets[bucket_idx];
        size_t submap_idx = bucket->get_submap_index(key_str);
        
        grouped_indices[shard_idx][bucket_idx][submap_idx].push_back(i);
    }
    
    // 第二阶段：批量获取未命中的键
    for (const auto& [shard_idx, shard_data] : grouped_indices) {
        for (const auto& [bucket_idx, bucket_data] : shard_data) {
            auto& bucket = shards_[shard_idx]->buckets[bucket_idx];
            
            for (const auto& [submap_idx, indices] : bucket_data) {
                auto& submap = bucket->sub_maps[submap_idx];
                
                // 一次性获取锁，读取多个值
                std::shared_lock<std::shared_mutex> lock(submap.mutex);
                for (size_t i : indices) {
                    std::string key_str(keys[i]);
                    auto it = submap.store.find(key_str);
                    if (it != submap.store.end()) {
                        std::string value = enable_compression_ ? decompress(it->second) : it->second;
                        results[i] = value;
                        // 更新缓存
                        cache_.put(key_str, value);
                    } else {
                        results[i] = std::nullopt;
                    }
                }
            }
        }
    }
    
    return results;
}

// 批量删除多个键 (string_view版本)
size_t DataStore::multi_del(const std::vector<std::string_view>& keys) {
    size_t deleted_count = 0;
    
    // 按照shard和bucket对键进行分组
    std::unordered_map<size_t, std::unordered_map<size_t, std::unordered_map<size_t, std::vector<std::string>>>> grouped_keys;
    
    // 第一阶段：分组并从缓存中删除
    for (const auto& key : keys) {
        std::string key_str(key);
        
        // 从缓存中删除
        cache_.remove(key_str);
        
        size_t shard_idx = get_shard_index(key_str);
        size_t bucket_idx = get_bucket_index(key_str, bucket_per_shard_);
        auto& bucket = shards_[shard_idx]->buckets[bucket_idx];
        size_t submap_idx = bucket->get_submap_index(key_str);
        
        grouped_keys[shard_idx][bucket_idx][submap_idx].push_back(key_str);
    }
    
    // 第二阶段：批量删除
    for (const auto& [shard_idx, shard_data] : grouped_keys) {
        for (const auto& [bucket_idx, bucket_data] : shard_data) {
            auto& bucket = shards_[shard_idx]->buckets[bucket_idx];
            
            for (const auto& [submap_idx, submap_keys] : bucket_data) {
                auto& submap = bucket->sub_maps[submap_idx];
                
                // 一次性获取锁，删除多个键
                std::unique_lock<std::shared_mutex> lock(submap.mutex);
                for (const auto& key : submap_keys) {
                    deleted_count += submap.store.erase(key);
                }
            }
        }
    }
    
    return deleted_count;
}

// 预取多个键的值到缓存 (string_view版本)
void DataStore::prefetch(const std::vector<std::string_view>& keys) {
    // 按照shard和bucket对键进行分组
    std::unordered_map<size_t, std::unordered_map<size_t, std::unordered_map<size_t, std::vector<std::string>>>> grouped_keys;
    
    // 第一阶段：查询缓存并分组未命中的键
    for (const auto& key : keys) {
        std::string key_str(key);
        
        // 跳过已在缓存中的键
        if (cache_.get(key_str)) {
            continue;
        }
        
        // 缓存未命中，准备从存储中查询
        size_t shard_idx = get_shard_index(key_str);
        size_t bucket_idx = get_bucket_index(key_str, bucket_per_shard_);
        auto& bucket = shards_[shard_idx]->buckets[bucket_idx];
        size_t submap_idx = bucket->get_submap_index(key_str);
        
        grouped_keys[shard_idx][bucket_idx][submap_idx].push_back(key_str);
    }
    
    // 第二阶段：批量获取并更新缓存
    for (const auto& [shard_idx, shard_data] : grouped_keys) {
        for (const auto& [bucket_idx, bucket_data] : shard_data) {
            auto& bucket = shards_[shard_idx]->buckets[bucket_idx];
            
            for (const auto& [submap_idx, submap_keys] : bucket_data) {
                auto& submap = bucket->sub_maps[submap_idx];
                
                // 一次性获取锁，读取多个值
                std::shared_lock<std::shared_mutex> lock(submap.mutex);
                for (const auto& key_str : submap_keys) {
                    auto it = submap.store.find(key_str);
                    if (it != submap.store.end()) {
                        std::string value = enable_compression_ ? decompress(it->second) : it->second;
                        // 更新缓存
                        cache_.put(key_str, value);
                    }
                }
            }
        }
    }
}

// 批量设置多个键值对 (string版本)
void DataStore::multi_set(const std::vector<std::pair<std::string, std::string>>& kvs) {
    // 转换为string_view类型
    std::vector<std::pair<std::string_view, std::string_view>> view_kvs;
    view_kvs.reserve(kvs.size());
    
    for (const auto& [key, value] : kvs) {
        view_kvs.emplace_back(key, value);
    }
    
    multi_set(view_kvs);
}

// 批量获取多个键的值 (string版本)
std::vector<std::optional<std::string>> DataStore::multi_get(const std::vector<std::string>& keys) {
    // 转换为string_view类型
    std::vector<std::string_view> view_keys;
    view_keys.reserve(keys.size());
    
    for (const auto& key : keys) {
        view_keys.emplace_back(key);
    }
    
    return multi_get(view_keys);
}

// 批量删除多个键 (string版本)
size_t DataStore::multi_del(const std::vector<std::string>& keys) {
    // 转换为string_view类型
    std::vector<std::string_view> view_keys;
    view_keys.reserve(keys.size());
    
    for (const auto& key : keys) {
        view_keys.emplace_back(key);
    }
    
    return multi_del(view_keys);
}

// 预取多个键的值到缓存 (string版本)
void DataStore::prefetch(const std::vector<std::string>& keys) {
    // 转换为string_view类型
    std::vector<std::string_view> view_keys;
    view_keys.reserve(keys.size());
    
    for (const auto& key : keys) {
        view_keys.emplace_back(key);
    }
    
    prefetch(view_keys);
}
