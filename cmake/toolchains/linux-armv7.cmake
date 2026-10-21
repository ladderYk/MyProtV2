# 交叉编译工具链: x86_64 主机 → armv7 (32-bit ARM hard-float) Linux
#
# 用法:
#   cmake -S . -B build-linux-armv7 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-armv7.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-linux-armv7 -j
#
# 依赖: g++-arm-linux-gnueabihf (Ubuntu/Debian)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# 目标指令集: ARMv7-A + VFPv3-D16 + hard-float (不含 NEON, 兼容性更好)
set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard")

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
