#!/usr/bin/env bash
# 构建 Linux Release 发布包: bin/MyProt.App + configs/ + webui/dist/
#
# 用法:
#   ./scripts/package-linux.sh
#   BUILD_TYPE=Debug ./scripts/package-linux.sh
#
# 产物:
#   dist/                       可直接运行的目录 (从 dist/ 启动)
#   build-linux-package/package/*.tar.gz
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-linux-package}"
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-}"
LINKER_FLAGS="${LINKER_FLAGS:-}"

# 安全护栏: 安装目录必须在项目内 (脚本会先清空它)
case "$DIST_DIR" in
    "$ROOT"/*) ;;
    *) echo "[package-linux] DIST_DIR 必须位于项目目录下: $ROOT" >&2; exit 1 ;;
esac

TOOLCHAIN_OPT=()
if [ -n "$TOOLCHAIN_FILE" ]; then
    TOOLCHAIN_OPT=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE")
fi
LINKER_OPT=()
if [ -n "$LINKER_FLAGS" ]; then
    LINKER_OPT=(-DCMAKE_EXE_LINKER_FLAGS="$LINKER_FLAGS")
fi

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DMYPROT_BUILD_TESTS=OFF \
    "${TOOLCHAIN_OPT[@]}" "${LINKER_OPT[@]}"
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"

rm -rf "$DIST_DIR"
cmake --install "$BUILD_DIR" --prefix "$DIST_DIR"
cpack --config "$BUILD_DIR/CPackConfig.cmake" -G TGZ

echo "[package-linux] 安装目录: $DIST_DIR"
echo "[package-linux] 发布包:   $BUILD_DIR/package"
