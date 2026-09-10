# =============================================================================
# scl.cmake —— 把 SCL 源码 + 注册表引入当前 CMake 工程
#
# 用法（例程的 CMakeLists.txt）：
#     include(${SCL_DIR}/scl.cmake)          # SCL_DIR 指向本文件所在目录（scl/）
#     scl_collect()                          # 产出 SCL_SRC_LIST / SCL_INCLUDE_DIRS
#     add_executable(app ${SCL_SRC_LIST} main.c)
#     target_include_directories(app PRIVATE ${SCL_INCLUDE_DIRS})
#     scl_apply_defs(app)                    # 加上必需宏（SCL_CFG_REG_LIST_EN=1）
#
# 产出：
#     SCL_SRC_LIST     核心 + scl/cmd/*.c（含已提交的 scl_cmd_list.c）
#     SCL_INCLUDE_DIRS scl/Inc
#
# 注册表（scl/cmd/scl_cmd_list.c）是**生成物但入库**：构建不需要 Python；
# 改了 scl/cmd/ 里的命令后，用 scl_regen() 重新生成并提交。
# =============================================================================

set(SCL_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "SCL 源码根目录")
set(SCL_REGISTRY_FILE "${SCL_DIR}/cmd/scl_cmd_list.c" CACHE INTERNAL "SCL 注册表文件")

# ---------------------------------------------------------------------------
# scl_collect()：产出源列表与头文件路径（在调用者作用域）
# ---------------------------------------------------------------------------
function(scl_collect)
    file(GLOB _scl_core "${SCL_DIR}/Src/*.c")
    file(GLOB _scl_cmd  "${SCL_DIR}/cmd/*.c")

    set(SCL_SRC_LIST     ${_scl_core} ${_scl_cmd} PARENT_SCOPE)
    set(SCL_INCLUDE_DIRS "${SCL_DIR}/Inc"        PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# scl_apply_defs(<target>)：必需的编译宏
#   SCL_CFG_REG_LIST_EN=1 —— 用生成的注册表（唯一注册方式，不再手写 Xxx_Register）
# ---------------------------------------------------------------------------
function(scl_apply_defs _target)
    target_compile_definitions(${_target} PRIVATE SCL_CFG_REG_LIST_EN=1)
endfunction()

# ---------------------------------------------------------------------------
# scl_regen()：跑生成器重造 scl/cmd/scl_cmd_list.c（生成物入库，改命令后手动跑）
# ---------------------------------------------------------------------------
function(scl_regen)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    file(GLOB _scl_cmd "${SCL_DIR}/cmd/*.c")
    list(FILTER _scl_cmd EXCLUDE REGEX "scl_cmd_list\\.c$")
    add_custom_target(scl_regen_registry
        COMMAND ${Python3_EXECUTABLE} "${SCL_DIR}/tool/scl_gen_list.py"
                ${_scl_cmd} -o "${SCL_REGISTRY_FILE}"
        DEPENDS ${_scl_cmd} "${SCL_DIR}/tool/scl_gen_list.py"
        COMMENT "重新生成 SCL 注册表 (scl/cmd/scl_cmd_list.c)"
        VERBATIM)
endfunction()
