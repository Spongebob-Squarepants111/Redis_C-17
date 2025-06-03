#!/bin/bash
# 构建和运行Redis服务器的脚本

# 设置终端颜色
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # 无颜色

# 确保构建目录存在
mkdir -p build

# 进入构建目录
cd build

echo -e "${BLUE}正在构建项目...${NC}"
# 使用CMake生成构建文件
cmake .. -DCMAKE_BUILD_TYPE=Release

# 使用多线程编译
make -j$(nproc)

# 检查编译是否成功
if [ $? -ne 0 ]; then
    echo "编译失败，请检查错误信息"
    exit 1
fi

echo -e "${GREEN}编译成功!${NC}"

# 运行Redis服务器
echo -e "${BLUE}启动Redis服务器...${NC}"

# 先停止已经运行的实例
pkill -f simple_redis >/dev/null 2>&1

# 等待1秒确保进程已经停止
sleep 1

# 在后台启动服务器
./simple_redis &

# 打印服务器PID
echo -e "${GREEN}服务器已启动，PID: $!${NC}"

# 等待服务器初始化
sleep 2

echo -e "${GREEN}服务器就绪，可以使用redis-cli连接（端口6379）${NC}"
echo -e "示例命令："
echo -e " - redis-cli -p 6379 SET key value"
echo -e " - redis-cli -p 6379 GET key"

exit 0
