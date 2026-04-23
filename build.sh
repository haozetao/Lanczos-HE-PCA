#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"
mkdir -p "$BUILD_DIR"

# macOS Apple Silicon: OpenFHE 需要 Homebrew 的 libomp
EXTRA_CMAKE_ARGS=()
if [[ "$(uname)" == "Darwin" ]]; then
    if [[ -d "/opt/homebrew/opt/libomp" ]]; then
        EXTRA_CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=/opt/homebrew/opt/libomp")
    else
        echo "⚠️  warning: libomp not found. Install via: brew install libomp"
    fi
fi

cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "${EXTRA_CMAKE_ARGS[@]}" \
    -Wno-dev

cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.logicalcpu 2>/dev/null || nproc)"

echo ""
echo "=== 构建完成, 运行: ./${BUILD_DIR}/he_pca ==="
