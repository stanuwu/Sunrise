#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="$HOME/.cargo/bin:$PATH"
XWIN="${XWIN_DIR:-$HOME/.xwin}"

cmake -G Ninja -B ../build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-msvc.cmake \
  -DXWIN_DIR="$XWIN"

cmake --build ../build -j"$(nproc)"

echo "=> $(cd .. && pwd)/build/steam_api64.dll"
