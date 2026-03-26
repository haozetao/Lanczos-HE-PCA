#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"
mkdir -p "$BUILD_DIR"

cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DSEAL_USE_ZLIB=OFF \
    -DSEAL_USE_ZSTD=OFF \
    -Wno-dev

cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.logicalcpu 2>/dev/null || nproc)"

echo ""
echo "=== 构建完成, 运行: ./${BUILD_DIR}/he_pca ==="
