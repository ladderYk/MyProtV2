# 交叉编译工具链: x86_64 主机 → aarch64 (ARM64) Linux
#
# 用法:
#   cmake -S . -B build-linux-aarch64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-linux-aarch64 -j
#
# 依赖: g++-aarch64-linux-gnu (Ubuntu/Debian)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
