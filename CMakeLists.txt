cmake_minimum_required(VERSION 3.10)
project(simple_redis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

# 包含头文件目录
include_directories(
    ${PROJECT_SOURCE_DIR}/include
)

# 源文件列表 - 只保留优化版本
set(SRCS
    src/AdaptiveCache.cpp
    src/CommandHandler.cpp
    src/DataStore.cpp
    src/RESPParser.cpp
    src/ThreadPool.cpp
    src/RedisServer.cpp
    src/Config.cpp
    src/main.cpp
)

# 生成可执行文件（重命名为 simple_redis）
add_executable(simple_redis ${SRCS})

# 优化选项
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")

# 默认使用Release构建类型
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif()

# 拷贝配置文件到构建目录
configure_file(
  ${CMAKE_SOURCE_DIR}/config.ini
  ${CMAKE_BINARY_DIR}/config.ini
  COPYONLY
)

# 链接线程库
find_package(Threads REQUIRED)

# 添加压缩库和哈希库依赖
find_package(ZLIB REQUIRED)

# 链接所有依赖库
target_link_libraries(simple_redis
    PRIVATE
    ${ZLIB_LIBRARIES}
    xxhash
    Threads::Threads
)

# 包含头文件目录
target_include_directories(simple_redis
    PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
