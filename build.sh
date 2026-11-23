#!/bin/sh
# Builds md.wasm from md.c: clang's wasm32 backend (LLVM 22) and wasm-ld, bulk memory on, no libc.
set -e
cd "$(dirname "$0")"
CLANG=${CLANG:-/usr/local/opt/llvm@22/bin/clang}
"$CLANG" --target=wasm32 -O2 -mbulk-memory -nostdlib \
  -fuse-ld="${WASM_LD:-/usr/local/opt/lld/bin/wasm-ld}" -Wl,--no-entry -Wl,--export-memory -Wl,--strip-all -Wl,-z,stack-size=262144 \
  -o md.wasm md.c
