#!/usr/bin/env bash
# 每平台拉取 onnxruntime C++ 运行库到 third_party/onnxruntime/。
#
# 背景：GitHub releases 直链对本机限速，故优先走 ghproxy 镜像转发，失败再回退直连。
# 桌宠分发必需此库（仅几 MB）；模型文件见 tools/export_bge_onnx.py。
#
# 用法：
#   ./tools/fetch_onnxruntime.sh mac        # mac arm64（本机开发）
#   ./tools/fetch_onnxruntime.sh win        # Windows x64（打包交付）
#   ./tools/fetch_onnxruntime.sh all        # 两者都拉
set -euo pipefail

VERSION="1.28.0"
DEST="$(cd "$(dirname "$0")/.." && pwd)/third_party/onnxruntime"
MIRRORS=("https://ghproxy.net" "")  # 空=直连 GitHub

mac_url="https://github.com/microsoft/onnxruntime/releases/download/v${VERSION}/onnxruntime-osx-arm64-${VERSION}.tgz"
win_url="https://github.com/microsoft/onnxruntime/releases/download/v${VERSION}/onnxruntime-win-x64-${VERSION}.zip"

fetch() {
  local url="$1" outname="$2"
  mkdir -p "$DEST"
  local dst="$DEST/$outname"
  for m in "${MIRRORS[@]}"; do
    local full="${m}${url}"
    echo "  trying: ${m:-direct}"
    if curl -L --fail -o "$dst.tmp" --max-time 300 "$full"; then
      mv "$dst.tmp" "$dst"
      echo "  ok -> $dst"
      return 0
    fi
  done
  echo "  FAILED all mirrors for $url" >&2
  return 1
}

install_mac() {
  echo "[mac arm64] onnxruntime ${VERSION}"
  fetch "$mac_url" "ort.tgz" || return 1
  tar xzf "$DEST/ort.tgz" -C "$DEST"
  rm -f "$DEST/ort.tgz"
  ls "$DEST"/onnxruntime-osx-arm64-${VERSION}/lib
}

install_win() {
  echo "[win x64] onnxruntime ${VERSION}"
  fetch "$win_url" "ort.zip" || return 1
  unzip -o -q "$DEST/ort.zip" -d "$DEST"
  rm -f "$DEST/ort.zip"
  ls "$DEST"/onnxruntime-win-x64-${VERSION}/lib
}

target="${1:-}"
case "$target" in
  mac) install_mac ;;
  win) install_win ;;
  all) install_mac; install_win ;;
  *) echo "usage: $0 {mac|win|all}"; exit 1 ;;
esac