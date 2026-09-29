#!/usr/bin/env bash
# Builds the Forgent3D OpenSCAD wasm (slim + full) and copies it into the platform repo's
# packages/openscad-wasm/dist/. See forgent3d/README.md for the toolchain (emsdk at /emsdk with the
# wasm-exception dependencies installed into its sysroot).
#
#   forgent3d/sync_dist.sh [PLATFORM_REPO_ROOT]     (default: ../forgent3d-platform, a sibling of this repo)
#
#   slim  build-wasm-slim  no CGAL / Manifold: evaluate + export (-o x.fgjson). The default artifact.
#   full  build-wasm-full  CGAL + Manifold: also renders (the corpus oracle; the mesh fallback, lazily).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PLATFORM_ROOT="${1:-${REPO_ROOT}/../forgent3d-platform}"
DIST_DIR="${PLATFORM_ROOT}/packages/openscad-wasm/dist"
MAX_BYTES=$((25 * 1024 * 1024)) # Cloudflare static-asset limit (copy-kernel-assets.mjs checks it too)

[ -d "${PLATFORM_ROOT}/packages/openscad-wasm" ] || { echo "error: platform repo not found at ${PLATFORM_ROOT}" >&2; exit 1; }
export EMSDK=/emsdk PATH=/emsdk:/emsdk/upstream/emscripten:/emsdk/node/22.16.0_64bit/bin:$PATH
command -v emcmake >/dev/null || { echo "error: no emsdk at /emsdk (forgent3d/README.md)" >&2; exit 1; }
SYSROOT=/emsdk/upstream/emscripten/cache/sysroot
if /emsdk/upstream/bin/llvm-nm -u "${SYSROOT}/lib/libboost_program_options.a" 2>/dev/null | grep -q "invoke_"; then
  echo "error: the sysroot's boost is built with JS exceptions; run forgent3d/deps/*.sh first (README.md)" >&2
  exit 1
fi

build() { # dir, extra cmake args…
  local dir="$1"; shift
  if [ ! -f "${REPO_ROOT}/${dir}/CMakeCache.txt" ]; then
    ( cd "${REPO_ROOT}" && emcmake cmake -B "${dir}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DEXPERIMENTAL=ON \
        -DWASM_BUILD_TYPE=forgent -DENABLE_TESTS=OFF -DUSE_CCACHE=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Lib3MF=TRUE \
        -DCMAKE_CXX_FLAGS="-fwasm-exceptions" -DCMAKE_C_FLAGS="-fwasm-exceptions -sSUPPORT_LONGJMP=wasm" "$@" )
  fi
  ninja -C "${REPO_ROOT}/${dir}" -j "${JOBS:-8}"
  local size; size=$(stat -c %s "${REPO_ROOT}/${dir}/openscad.wasm")
  [ "${size}" -le "${MAX_BYTES}" ] || { echo "error: ${dir}/openscad.wasm is ${size} bytes, over ${MAX_BYTES}" >&2; exit 1; }
}

build build-wasm-slim -DENABLE_CGAL=OFF -DENABLE_MANIFOLD=OFF
build build-wasm-full

mkdir -p "${DIST_DIR}"
cp "${REPO_ROOT}/build-wasm-slim/openscad.js" "${DIST_DIR}/openscad.mjs"
cp "${REPO_ROOT}/build-wasm-slim/openscad.wasm" "${DIST_DIR}/openscad.wasm"
# both glues look for "openscad.wasm" next to themselves; the full one gets its own name
sed 's/"openscad\.wasm"/"openscad-full.wasm"/g' "${REPO_ROOT}/build-wasm-full/openscad.js" > "${DIST_DIR}/openscad-full.mjs"
grep -q '"openscad-full.wasm"' "${DIST_DIR}/openscad-full.mjs" || { echo "error: the full glue no longer names openscad.wasm as expected" >&2; exit 1; }
cp "${REPO_ROOT}/build-wasm-full/openscad.wasm" "${DIST_DIR}/openscad-full.wasm"
cp "${REPO_ROOT}/COPYING" "${DIST_DIR}/COPYING"
# text()'s fonts: OpenSCAD's own bundled set (Liberation 2.00.1, SIL OFL), mounted at /fonts by the runners when a
# file draws text — resourcePath("fonts") is /fonts in the wasm, and fontconfig reads 10-liberation.conf there
mkdir -p "${DIST_DIR}/fonts"
cp "${REPO_ROOT}"/fonts/Liberation-2.00.1/ttf/*.ttf "${DIST_DIR}/fonts/"
cp "${REPO_ROOT}/fonts/Liberation-2.00.1/LICENSE" "${DIST_DIR}/fonts/LICENSE"
cp "${REPO_ROOT}/fonts/10-liberation.conf" "${DIST_DIR}/fonts/10-liberation.conf"
# what the browser's worker fetches (packages/cloud/scripts/openscad/worker.js)
( cd "${DIST_DIR}/fonts" && ls | grep -E '\.(ttf|conf)$' | python3 -c 'import json, sys; print(json.dumps([l.strip() for l in sys.stdin if l.strip()]))' > index.json )

commit="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
dirty="$(git -C "${REPO_ROOT}" status --porcelain --untracked-files=no -- src CMakeLists.txt | wc -l)"
{
  echo "OpenSCAD, Forgent3D fork: https://github.com/forgent3d/openscad/tree/${commit} (branch forgent3d)$([ "${dirty}" -gt 0 ] && echo " + ${dirty} uncommitted file(s) — COMMIT AND PUSH THE FORK")"
  echo "emscripten: $(emcc --version | head -1)"
  echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "${DIST_DIR}/BUILD.txt"
cat "${DIST_DIR}/BUILD.txt"
ls -la "${DIST_DIR}"
