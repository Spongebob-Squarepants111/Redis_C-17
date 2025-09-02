#!/bin/bash

# 快速性能测试脚本 - 只测试几个关键配置

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}     Redis 快速性能测试                ${NC}"
echo -e "${BLUE}========================================${NC}"

best_qps=0
best_config=""

# 函数：创建配置文件
create_config() {
    local worker_threads=$1
    local io_threads=$2
    local cache_size=$3
    local buffer_size=$4
    local max_conn=$5
    
    cat > config_test.ini << EOF
# 测试配置
[server]
port = 6379
host = 127.0.0.1

[threading]
worker_threads = $worker_threads
io_threads = $io_threads
shard_count = 32

[performance]
max_connections = $max_conn
buffer_size = $buffer_size
batch_size = 128

[storage]
cache_size_mb = $cache_size
enable_persistence = false
sync_interval_sec = 600
EOF
}

# 函数：运行性能测试
run_test() {
    local config_name=$1
    local workers=$2
    local io=$3
    local cache=$4
    
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}测试配置: $config_name${NC}"
    echo -e "${BLUE}参数: Workers=$workers, IO=$io, Cache=${cache}MB${NC}"
    echo -e "${BLUE}========================================${NC}"
    
    # 停止现有服务器
    pkill -f simple_redis >/dev/null 2>&1
    sleep 2
    
    # 创建配置
    create_config $workers $io $cache 65536 50000
    
    # 启动服务器
    cd build
    ./simple_redis ../config_test.ini > server.log 2>&1 &
    local server_pid=$!
    
    # 等待服务器启动
    sleep 5
    
    # 检查服务器是否启动成功
    if ! kill -0 $server_pid 2>/dev/null; then
        echo -e "${RED}服务器启动失败${NC}"
        cat server.log
        cd ..
        return 1
    fi
    
    echo -e "${GREEN}服务器启动成功，开始性能测试...${NC}"
    
    # 运行测试
    echo -e "${YELLOW}>>> SET 测试 (1M 操作, 50 并发)${NC}"
    redis-benchmark -h 127.0.0.1 -p 6379 -t set -n 1000000 -c 50
    
    echo -e "${YELLOW}>>> GET 测试 (1M 操作, 50 并发)${NC}"
    redis-benchmark -h 127.0.0.1 -p 6379 -t get -n 1000000 -c 50
    
    echo -e "${YELLOW}>>> Pipeline SET 测试 (1M 操作, 50 并发, 16 Pipeline)${NC}"
    redis-benchmark -h 127.0.0.1 -p 6379 -t set -n 1000000 -c 50 -P 16
    
    # 停止服务器
    kill $server_pid >/dev/null 2>&1
    wait $server_pid 2>/dev/null
    
    cd ..
    
    echo -e "${GREEN}配置 $config_name 测试完成${NC}"
    echo "-------------------"
}

# 测试几个关键配置
echo -e "${YELLOW}开始测试关键配置...${NC}"

# 测试1: 当前配置
run_test "Current_64w_32io_16G" 64 32 16384

# 测试2: 更多工作线程
run_test "More_Workers_96w_32io_16G" 96 32 16384

# 测试3: 平衡配置
run_test "Balanced_80w_24io_8G" 80 24 8192

# 测试4: 高IO配置
run_test "High_IO_64w_48io_16G" 64 48 16384

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}           快速测试完成！              ${NC}"
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}请查看上述测试结果，选择性能最佳的配置${NC}"
