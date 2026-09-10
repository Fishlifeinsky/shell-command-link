# SCL PC 示例 + 全量测试工程
cmake_minimum_required(VERSION 3.16)
project(scl_test C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# SCL 库本体 + 库内命令（scl/cmd/）+ 生成的注册表
# 统一由 scl/scl.cmake 收集，产出 SCL_SRC_LIST / SCL_INCLUDE_DIRS
set(SCL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../scl)
include(${SCL_DIR}/scl.cmake)
scl_collect()

add_library(scl STATIC ${SCL_SRC_LIST})
target_include_directories(scl PUBLIC ${SCL_INCLUDE_DIRS}
                                        ${CMAKE_CURRENT_SOURCE_DIR}/../scl/Src)
scl_apply_defs(scl)

# PC 示例：移植输出 + 全量测试（命令来自 scl/cmd/，由注册表在 SCL_Init 内注册）
add_executable(scl_test
    scl_port.c
    main.c
)
target_link_libraries(scl_test PRIVATE scl)
target_include_directories(scl_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
