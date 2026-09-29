#!/usr/bin/env bash
# Replaces the wasm-base sysroot's JS-exception builds of boost, freetype and gmp with the wasm-exception
# builds from build-boost.sh / build-ft-gmp.sh. The originals go to /opt/fg-deps/sysroot-js-eh-backup.
set -euo pipefail
SR=/emsdk/upstream/emscripten/cache/sysroot
BACKUP=/opt/fg-deps/sysroot-js-eh-backup
mkdir -p "${BACKUP}/lib"
( cd "${SR}/lib" && cp -n libboost_*.a libfreetype.a libgmp.a libgmpxx.a "${BACKUP}/lib/" )
cp /opt/fg-deps/boost-weh/lib/libboost_*.a "${SR}/lib/"
cp /opt/fg-deps/weh/lib/libfreetype.a /opt/fg-deps/weh/lib/libgmp.a /opt/fg-deps/weh/lib/libgmpxx.a "${SR}/lib/"
for f in libboost_filesystem.a libboost_program_options.a libboost_regex.a libfreetype.a libgmpxx.a; do
  n=$(/emsdk/upstream/bin/llvm-nm -u "${SR}/lib/${f}" 2>/dev/null | grep -c -E 'invoke_|__cxa_find_matching_catch|emscripten_longjmp' || true)
  [ "${n}" -eq 0 ] || { echo "error: ${f} still has JS exception/longjmp symbols" >&2; exit 1; }
done
echo "sysroot: boost, freetype, gmp are wasm-exception builds"
