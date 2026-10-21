#!/usr/bin/env bash
# 交叉编译 armv7 (32-bit ARM hard-float) Linux 发布包 (x86_64 主机上执行)
#
# 依赖:
#   sudo apt install g++-arm-linux-gnueabihf
#
# 默认静态链接: 目标机无需匹配 glibc 版本, 可直接在 Debian 11/12、
# 树莓派 OS 等 armv7 系统运行。
# 需要动态链接时: LINKER_FLAGS= ./scripts/package-linux-armv7.sh
#
# 产物:
#   dist-armv7/
#   build-linux-armv7-package/package/MyProt-<version>-Linux-armv7l.tar.gz
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

export BUILD_DIR="${BUILD_DIR:-$ROOT/build-linux-armv7-package}"
export DIST_DIR="${DIST_DIR:-$ROOT/dist-armv7}"
export TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-$ROOT/cmake/toolchains/linux-armv7.cmake}"
if [ -z "${LINKER_FLAGS+x}" ]; then
    export LINKER_FLAGS="-static"
fi

exec bash "$ROOT/scripts/package-linux.sh"
