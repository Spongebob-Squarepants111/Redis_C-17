# Simple Redis

Simple Redis 是一个用 C++17 编写的高性能、事件驱动的类 Redis 服务器，面向万级并发连接与高吞吐。项目聚焦于高效网络 I/O、细粒度并发、内存池与单层分片 LRU 缓存。

## 特性

- **高并发网络**：单独 accept 线程 + 多 worker 线程；每个 worker 独立 epoll(ET) 监听自身连接，避免惊群与共享状态竞争。
- **CPU 亲和性绑定**：通过 `ThreadAffinity` 将工作线程绑定到指定 CPU 核，减少线程在不同核心间迁移带来的缓存失效与调度抖动；。
- **分片 KV 存储**：一致性哈希定位分片，二次哈希定位桶，桶内 8 个子映射（各自 shared_mutex），仅对子映射加锁，降低争用。
- **单层缓存（LRU）**：多分片 LRU 缓存 `AdaptiveCache`（策略为 LRU），命中移动到分片链表前端；命中率/容量/逐出统计。
- **内存池优化**：专用对象池（MemoryPool<T> + MemoryBlockPool）。按块大小（默认 4096B）申请 chunk（约 16KB），等分为 block 并用空闲单链表管理，O(1) 分配/释放，显著降低 malloc/free 与碎片。
- **可选压缩**：基于 zlib 的按值压缩，通过 `config.ini` 的 `[storage] enable_compression` 开关启用。
- **后台持久化**：每分片独立二进制文件；后台线程按 `sync_interval_sec` 周期落盘；退出前 `flush()` 全量保存。
- **现代 C++/构建**：C++17、CMake、Release 优化（`-O3 -march=native -flto -fno-rtti`）。

## 架构

- **接入与分发**：
  - 主进程创建监听 socket；专用 accept 线程 `accept()` 新连接。
  - 新连接按“当前连接数最少”分配给某个 worker。
- **事件与处理**：
  - 每个 worker 拥有独立 epoll(ET) 与客户端表；循环 `epoll_wait` → `recv` 大块数据 → `RESPParser.parse`。
  - 命令由 `CommandHandler::handle` 分发到 `handle_set/get/del/...`。
- **数据路径（DataStore）**：
  - SET：可选压缩 → `cache_.put(key, 原文)` → 分片/分桶/子映射 → 仅对子映射加写锁写入。
  - GET：先查缓存 → 未命中按分片/分桶/子映射读取（读锁）→ 可选解压 → 回填缓存。
  - DEL：先删缓存 → 定位子映射 → 写锁擦除。
- **持久化**：
  - 启动：按分片 `load_shard(i)` 载入到子映射。
  - 运行：后台线程定期 `persist_shard(i)` 写盘。
  - 退出：析构中 `flush()` 全量落盘。

## 使用方法

1. 克隆项目：
   ```bash
   git clone https://github.com/Spongebob-Squarepants111/Redis_C-17.git
   ```

2. 编译项目：
   ```bash
   mkdir build
   cd build
   cmake ..
   make
   ```

3. 运行服务器：
   ```bash
   ./simple_redis
   ```
   
   或者使用构建脚本：
   ```bash
   ./build_run.sh
   ```

## 性能测试

使用 `redis-benchmark`（本地环回，Release 构建），示例结果：

- SET 50 并发，100 万次：约 118k QPS
  ```bash
  redis-benchmark -h 127.0.0.1 -p 6379 -t set -n 1000000 -c 50
  # 118021.96 requests per second
  ```

- GET 50 并发，100 万次：约 117k QPS
  ```bash
  redis-benchmark -h 127.0.0.1 -p 6379 -t get -n 1000000 -c 50
  # 117013.81 requests per second
  ```

- SET + Pipeline(P=16) 50 并发，100 万次：约 1.04M QPS
  ```bash
  redis-benchmark -h 127.0.0.1 -p 6379 -t set -n 1000000 -c 50 -P 16
  # 1,038,421.62 ~ 1,048,218.06 requests per second
  ```

- SET 10000 并发，100 万次：约 95.8k QPS
  ```bash
  redis-benchmark -h 127.0.0.1 -p 6379 -t set -n 1000000 -c 10000
  # 95,812.97 requests per second
  ```

注：不同环境/参数（CPU 核数、NUMA、网卡、优化开关）会影响结果，以上仅作参考。

## 贡献

欢迎贡献代码和提出建议！请提交Pull Request或在Issue中讨论。

## 许可证

该项目基于MIT许可证开源。