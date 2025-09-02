# Simple Redis

Simple Redis是一个用C++17编写的高性能Redis实现，旨在提供高效的键值存储服务。该项目的主要特点包括：

## 特性

- **高并发性能**：利用多线程和Epoll事件驱动模型，支持大量并发连接。
- **现代C++**：使用C++17标准，充分利用现代C++的特性和库。
- **内存管理**：采用自定义内存池和缓存策略，优化内存使用和访问速度。
- **网络编程**：基于Linux Socket API和Epoll事件驱动模型，实现高效的网络通信。
- **持久化支持**：支持数据持久化，确保数据的可靠性和一致性。

## 架构

- **多线程设计**：使用线程池处理客户端请求，减少线程创建和销毁的开销。
- **分片存储**：数据分片存储，减少锁竞争，提高访问效率。
- **自适应缓存**：根据访问模式动态调整缓存大小，提高缓存命中率。

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

## 性能测试

`test_eff.sh`脚本用于测试Redis服务器的性能。测试结果如下：

- **SET和GET操作**：在50个并发连接下，性能可达到10万+ QPS。
  ```bash
  redis-benchmark -h 127.0.0.1 -p 6379 -t set -n 1000000 -c 50
  redis-benchmark -h 127.0.0.1 -p 6379 -t get -n 1000000 -c 50
  ```

- **Pipeline测试**：使用请求管道，性能可达到100万+ QPS。
  ```bash
  redis-benchmark -h 127.0.0.1 -p 6379 -t set -n 1000000 -c 50 -P 16
  ```

- **高并发测试**：在10000个并发连接下，性能可达到7万+ QPS。
  ```bash
  redis-benchmark -h 127.0.0.1 -p 6379 -t set -n 1000000 -c 10000
  ```

这些测试结果展示了Simple Redis在不同并发和请求条件下的卓越性能。

## 贡献

欢迎贡献代码和提出建议！请提交Pull Request或在Issue中讨论。

## 许可证

该项目基于MIT许可证开源。