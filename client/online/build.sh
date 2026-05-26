#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$ROOT_DIR/dist/online.asi"

mapfile -t SOURCES < <(find "$ROOT_DIR/src" -maxdepth 1 -name '*.cpp' | sort)

i686-w64-mingw32-g++ \
	-std=c++17 \
	-O2 \
	-s \
	-shared \
	-static \
	-static-libgcc \
	-static-libstdc++ \
	-o "$OUT" \
	"${SOURCES[@]}" \
	-lws2_32

echo "Built: $OUT"
