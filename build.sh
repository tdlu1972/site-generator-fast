#!/bin/sh
# Builds site.wasm from site.c (and md.wasm from md.c): clang's wasm32 backend (LLVM 22) and wasm-ld, bulk memory on,
# no libc.
set -e
cd "$(dirname "$0")"
CLANG=${CLANG:-/usr/local/opt/llvm@22/bin/clang}
for m in site md; do
  "$CLANG" --target=wasm32 -O2 -msimd128 -mbulk-memory -nostdlib -fno-builtin-strlen \
    -fuse-ld="${WASM_LD:-/usr/local/opt/lld/bin/wasm-ld}" -Wl,--no-entry -Wl,--export-memory -Wl,--strip-all -Wl,-z,stack-size=1048576 \
    -o $m.wasm $m.c
done
