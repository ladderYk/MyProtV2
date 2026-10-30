#!/usr/bin/env bash
# scripts/ci-linux.sh — Linux CI 入口: 配置 → 编译 → 单元测试 → E2E
#
# 与 Windows 的 scripts/ci.ps1 对应, 共享同一份源码与 CMakeLists.txt。
# 用法:
#   ./scripts/ci-linux.sh                 # Release 构建 + 全量测试
#   BUILD_TYPE=Debug ./scripts/ci-linux.sh
#   BUILD_DIR=/tmp/myprot-build ./scripts/ci-linux.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-linux}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

echo "[ci-linux] configure ($BUILD_TYPE)"
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

echo "[ci-linux] build"
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"

BIN="$ROOT/bin/$BUILD_TYPE"
for t in Core Engine Service Transport Northbound Southbound; do
    echo "[ci-linux] unit: $t"
    "$BIN/MyProt.$t.Tests"
done

# E2E 以相对路径读写 configs*_test, 必须从仓库根目录运行
echo "[ci-linux] e2e"
cd "$ROOT"
"$BIN/MyProt.E2E"

echo "[ci-linux] all green"
