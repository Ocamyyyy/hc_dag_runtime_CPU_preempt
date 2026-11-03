cmake_minimum_required(VERSION 3.10)

project(hc_dag_runtime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)
include(CTest)

add_library(execution_backend
    src/mock_backend.cpp
    src/rp_state_manager.cpp
    src/fake_fpga_device_driver.cpp
    src/fpga_backend.cpp
)
target_include_directories(execution_backend PUBLIC include)
target_link_libraries(execution_backend PUBLIC Threads::Threads)

add_executable(day5_test
    test/day5_test.cpp
    src/task_model.cpp
    src/runtime_manager.cpp
    src/scheduler.cpp
    src/dispatcher.cpp
)
target_include_directories(day5_test PRIVATE include)
target_link_libraries(day5_test PRIVATE Threads::Threads)

if(BUILD_TESTING)
    add_executable(mock_backend_test
        test/mock_backend_test.cpp
    )
    target_link_libraries(mock_backend_test PRIVATE execution_backend)
    add_test(NAME mock_backend_test COMMAND mock_backend_test)

    add_executable(fpga_backend_test
        test/fpga_backend_test.cpp
    )
    target_link_libraries(fpga_backend_test PRIVATE execution_backend)
    add_test(NAME fpga_backend_test COMMAND fpga_backend_test)

    add_executable(fpga_runtime_integration_test
        test/fpga_runtime_integration_test.cpp
        src/task_model.cpp
        src/runtime_manager.cpp
        src/scheduler.cpp
        src/dispatcher.cpp
    )
    target_include_directories(fpga_runtime_integration_test PRIVATE include)
    target_link_libraries(fpga_runtime_integration_test
        PRIVATE execution_backend Threads::Threads)
    add_test(NAME fpga_runtime_integration_test
             COMMAND fpga_runtime_integration_test)
    add_test(NAME day5_test COMMAND day5_test)
endif()
