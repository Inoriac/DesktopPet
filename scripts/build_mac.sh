#!/usr/bin/env bash
# macOS 构建脚本(arm64)
# 依赖由本脚本自带的 venv + aqt 安装的 Qt6,无需 Homebrew/sudo。
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

QT_BUILD_ENV="$HOME/.qt-build-env"
CMAKE_BIN="$QT_BUILD_ENV/bin/cmake"
QT6_DIR="$HOME/Qt/6.8.3/macos/lib/cmake/Qt6"

if [ ! -x "$CMAKE_BIN" ]; then
    echo "❌ cmake 未找到于 $CMAKE_BIN" >&2
    echo "   先装依赖:python3 -m venv $QT_BUILD_ENV && $QT_BUILD_ENV/bin/pip install cmake aqtinstall" >&2
    exit 1
fi
if [ ! -d "$QT6_DIR" ]; then
    echo "❌ Qt6 未找到于 $QT6_DIR" >&2
    echo "   先装 Qt6:$QT_BUILD_ENV/bin/aqt install-qt mac desktop 6.8.3 clang_64 -O $HOME/Qt" >&2
    exit 1
fi

BUILD_TYPE="${BUILD_TYPE:-Debug}"

if [ ! -f build/CMakeCache.txt ]; then
    echo "==> 配置项目 (configure)"
    "$CMAKE_BIN" -S . -B build -DQt6_DIR="$QT6_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi

echo "==> 编译 (build, -j8)"
"$CMAKE_BIN" --build build -j8

echo "✅ 构建完成 → build/Desktop_Pet"
