# SCL PC 示例 + 全量测试工程
cmake_minimum_required(VERSION 3.16)
project(scl_test C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# SCL 库本体（静态库，可直接被外部 target 引用）
add_library(scl STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../scl/Src/scl.c
)
target_include_directories(scl PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../scl/Inc
)
target_compile_definitions(scl PRIVATE
    # 可用 -DSCL_CFG_MSG_EN=0 等裁剪（见 scl_cfg.h）
)

# PC 示例：移植输出 + 演示命令 + 全量测试
add_executable(scl_test
    scl_port.c
    demo_cmds.c
    main.c
)
target_link_libraries(scl_test PRIVATE scl)
target_include_directories(scl_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 演示命令开关：OFF 可整组裁掉演示命令
option(SCL_EX_CMDS_EN "演示命令开关" ON)
if(NOT SCL_EX_CMDS_EN)
    target_compile_definitions(scl_test PRIVATE SCL_EX_CMDS_EN=0)
endif()
