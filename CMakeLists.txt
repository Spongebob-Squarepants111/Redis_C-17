cmake_minimum_required(VERSION 3.10)
project(simple_redis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

# —— 查找 liburing 依赖（通过 pkg-config） ——
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

# 包含头文件目录
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${LIBURING_INCLUDE_DIRS}    # liburing 的头文件路径
)

# 源文件列表
file(GLOB SRCS src/*.cpp)

# 生成可执行文件
add_executable(simple_redis ${SRCS})
add_compile_options(-O3)

# 拷贝配置文件（如 config.ini）到构建目录
configure_file(
  ${CMAKE_SOURCE_DIR}/config.ini
  ${CMAKE_BINARY_DIR}/config.ini
  COPYONLY
)

# 链接线程库和 liburing
find_package(Threads REQUIRED)
target_link_libraries(simple_redis
    PRIVATE
      Threads::Threads         # pthread
      ${LIBURING_LIBRARIES}    # -luring
)

# 链接器搜索路径（可选，仅当找不到库时使用）
link_directories(${LIBURING_LIBRARY_DIRS})
