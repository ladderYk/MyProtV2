#!/usr/bin/env bash
# 交叉编译 aarch64 (ARM64) Linux 发布包 (x86_64 主机上执行)
#
# 依赖:
#   sudo apt install g++-aarch64-linux-gnu
#
# 默认静态链接: 目标机无需匹配 glibc 版本。
# 需要动态链接时: LINKER_FLAGS= ./scripts/package-linux-arm64.sh
#
# 产物:
#   dist-aarch64/
#   build-linux-aarch64-package/package/MyProt-<version>-Linux-aarch64.tar.gz
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

export BUILD_DIR="${BUILD_DIR:-$ROOT/build-linux-aarch64-package}"
export DIST_DIR="${DIST_DIR:-$ROOT/dist-aarch64}"
export TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-$ROOT/cmake/toolchains/linux-aarch64.cmake}"
if [ -z "${LINKER_FLAGS+x}" ]; then
    export LINKER_FLAGS="-static"
fi

exec bash "$ROOT/scripts/package-linux.sh"
