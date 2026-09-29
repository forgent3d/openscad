# OpenSCAD, Forgent3D fork

https://github.com/forgent3d/openscad, branch `forgent3d`: OpenSCAD, based on upstream `dc1998e4f` (2025-07-17 — the snapshot openscad-wasm 0.0.4
shipped as 2025.07.18). Forgent3D runs OpenSCAD only to **evaluate** `.scad` files: OpenSCAD executes the language,
and Forgent3D's own CAD kernel builds exact geometry from the evaluated node tree. The consumer is
`packages/cloud/lib/scad-transpile/` in the forgent3d-platform repo; the artifacts land in
`packages/openscad-wasm/dist/` there (`forgent3d/sync_dist.sh`). Why, and the road map: that repo's
`docs/scad-endgame.md`.

OpenSCAD is GPL-2.0-or-later (COPYING, with the CGAL linking exception). This fork's changes are under the same
license and **must be published with every deployed build**: push the branch before deploying a build of it
(dist/BUILD.txt links the commit).

## What the fork changes

| Change | Where | Why |
| --- | --- | --- |
| `-o out.fgjson`: the evaluated node tree as JSON — every node's arguments by the .csg's names, numbers at full precision, `at` = file:line:col of the call that made it, `module` / `call` = the user module / builtin behind it, `%`/`#`; the log (every message, echo included, with its location); the Customizer parameters; optionally (`-O fgjson/csg=true`) the .csg text of the same evaluation | `src/io/export_fgjson.cc` (format in its header) | the .csg prints 6 significant digits, no locations, unescaped strings, bare `inf`/`nan` |
| `text()` nodes carry `contours`: each glyph outline as FreeType has it — lines, quadratic and cubic Béziers — laid out by OpenSCAD, before `$fn` flattening | `FreetypeRenderer::outlines()` (`src/core/FreetypeRenderer.{h,cc}`) | exact lettering in the consumer, verifiable against OpenSCAD's own render |
| `-O fgjson/mesh=true` (full build): `import()`, `surface()`, `roof()`, `projection(cut = false)`, `offset(chamfer = true)` carry `rendered` — OpenSCAD's render of that node from the same evaluation (3D: points + faces; 2D: points + paths) | `export_fgjson.cc` `rendered()` | the mesh fallback: what has no exact counterpart still builds, as facets, with a warning |
| `.csg` export at full precision | `src/utils/full_precision.{h,cc}` (a `num_put` facet installed in `main`, switched on by `FullPrecisionScope`); used by the CSG and fgjson exports | the oracle renders the very tree the consumer built |
| every message reaches a tap; `echo()` carries its location to it | `src/utils/printutils.{h,cc}` (`message_tap`, `message_origin`), `src/core/Expression.cc`, `src/core/control.cc` | the fgjson log; the console text is unchanged |
| repeated commas accepted again — `[a,,b]`, `f(a,,b,)`, `module m(a,,b)` — read as OpenSCAD 2021.01 read them | `src/core/parser.y` (`commas`) | a tenth of the Thingiverse files that no longer parse; no file the stricter grammar accepts parses differently |
| `WASM_BUILD_TYPE=forgent`: one ES module for browsers, workers and node (MEMFS everywhere), `-sSTACK_SIZE=${STACKSIZE}`, native Wasm exceptions and longjmp | `CMakeLists.txt` | emscripten's 64 KB default stack crashed recursion at depth ~150; JS-emulated exceptions cost ~10 KB of V8 stack per module level (recursion died at ~100) — now ~2600 function / ~600 module levels in V8's default stack |
| builds without CGAL and Manifold (the slim build) | `src/geometry/GeometryEvaluator.cc`, `src/geometry/Polygon2d.cc` | upstream's no-backend configuration had bit-rotted |

## Builds

- **native** (`build-native/`): Release, `EXPERIMENTAL`, `HEADLESS`/`NULLGL`, CGAL 6.0.1 from `/home/openscad-deps`. Used as a
  fast oracle (`OPENSCAD_BIN=/home/openscad/build-native/openscad` for the platform's corpus scripts) and to try
  exporter changes: `openscad in.scad -o out.fgjson [-O fgjson/csg=true] [-O fgjson/mesh=true]`.
- **slim wasm** (`build-wasm-slim/`, 4.9 MB): no CGAL / Manifold — evaluate + export. The platform's default.
- **full wasm** (`build-wasm-full/`, 8.1 MB): CGAL + Manifold — renders. The corpus oracle's default renderer, and the
  browser's mesh fallback (downloaded only when a model needs it).

`forgent3d/sync_dist.sh [PLATFORM_ROOT]` configures (first run) and builds both wasm variants, then copies them, COPYING,
the Liberation fonts (text()'s, OFL — mounted at `/fonts` by the platform's runners) and BUILD.txt into
`packages/openscad-wasm/dist/`. Commit the fork before syncing: BUILD.txt flags uncommitted changes.

## Toolchain (no Docker needed)

The wasm builds use the `openscad/wasm-base` image's emsdk (Emscripten 4.0.10 with OpenSCAD's dependencies prebuilt
in its sysroot), unpacked without a Docker daemon:

1. `python3 forgent3d/deps/pull_image.py openscad/wasm-base latest /opt/wasm-base` — pulls the layers from Docker
   Hub and unpacks them (keeps the layer tarballs in `/opt/wasm-base.layers`).
2. `mv /opt/wasm-base/emsdk /emsdk` — **a real directory, not a symlink**: emscripten compares its sanity file with
   the resolved path and, finding it changed, wipes `cache/` — which is where the prebuilt dependencies live. (If
   that happened: re-extract `emsdk/upstream/emscripten/cache/` from the layer tarballs.)
3. Native Wasm exceptions need every C++ library built the same way. The image's boost, freetype (setjmp) and
   gmpxx are JS-exception builds: `forgent3d/deps/build-boost.sh` (boost 1.87: atomic, filesystem,
   program_options, regex, system), `forgent3d/deps/build-ft-gmp.sh` (freetype 2.13.3, gmp 6.3.0), then
   `forgent3d/deps/install-into-sysroot.sh` (backs the originals up to `/opt/fg-deps/sysroot-js-eh-backup`).
   lib3mf is left out of the build (`CMAKE_DISABLE_FIND_PACKAGE_Lib3MF`): the platform never imports or exports 3MF.
4. `forgent3d/sync_dist.sh`.

## Rebasing on a newer upstream

Upstream moves; the patches above are small and local. After a rebase: rebuild native, run
`openscad x.scad -o x.fgjson` on a few files, rebuild the wasm (`sync_dist.sh`), then in the platform repo
`pnpm test:scad`, `pnpm test:scad-diff` and `pnpm test:scad-thingiverse`. If the image's Emscripten changes, the
dependencies in step 3 must be rebuilt with it.
