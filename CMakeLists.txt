cmake_minimum_required(VERSION 3.10)

# 交叉编译目标平台配置
SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_SYSTEM_PROCESSOR aarch64)
SET(CMAKE_BUILD_TYPE Release)

# 交叉编译工具链
SET(CMAKE_C_COMPILER   /usr/bin/aarch64-linux-gnu-gcc)
SET(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

project(hc_dag_runtime LANGUAGES CXX)

# 编译选项
SET(CMAKE_C_FLAGS_DEBUG   "${CMAKE_C_FLAGS}   -O0 -g -Wall")
SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} -O0 -g -Wall")
SET(CMAKE_C_FLAGS_RELEASE   "${CMAKE_C_FLAGS}   -O2 -ffunction-sections -fdata-sections")
SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -O2 -ffunction-sections -fdata-sections")

include_directories(./include)

add_executable(day5_test
    test/day5_test.cpp
    src/task_model.cpp
    src/runtime_manager.cpp
    src/scheduler.cpp
    src/dispatcher.cpp
)
target_include_directories(day5_test PRIVATE include)
target_link_libraries(day5_test PRIVATE pthread)