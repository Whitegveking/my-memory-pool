# 设置CMake最低版本
cmake_minimum_required(VERSION 3.10)

# 定义项目
project(my-memory-pool VERSION 1.0 LANGUAGES CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 添加编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -Wall -Wextra)
else()
    add_compile_options(-O2)
endif()

# 包含目录
include_directories(${PROJECT_SOURCE_DIR}/include)

# 查找必要的包
find_package(Threads REQUIRED)

# 添加库
add_library(memory_pool STATIC src/MemoryPool.cpp)

# 添加测试可执行文件(如果有测试文件)
file(GLOB TEST_FILES "tests/*.cpp")
if(TEST_FILES)
    add_executable(unit_tests ${TEST_FILES})
    # 使用统一风格的链接库语法
    target_link_libraries(unit_tests memory_pool pthread)
    # 不要在其他地方再次为unit_tests调用target_link_libraries
endif()

# 添加主程序(如果需要)
file(GLOB MAIN_FILES "src/main.cpp")
if(MAIN_FILES)
    add_executable(${PROJECT_NAME} ${MAIN_FILES})
    target_link_libraries(${PROJECT_NAME} memory_pool pthread)
endif()

# 添加安装规则
install(TARGETS memory_pool DESTINATION lib)
install(FILES include/MemoryPool.hpp DESTINATION include)