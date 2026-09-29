#!/bin/bash
set -ex
export EMSDK=/emsdk PATH=/emsdk:/emsdk/upstream/emscripten:/emsdk/node/22.16.0_64bit/bin:$PATH
cd /opt/fg-deps
[ -f freetype-2.13.3.tar.xz ] || curl -sSL -o freetype-2.13.3.tar.xz https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz
[ -d freetype-2.13.3 ] || tar xJf freetype-2.13.3.tar.xz
rm -rf ft-build && mkdir ft-build && cd ft-build
emcmake cmake ../freetype-2.13.3 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/fg-deps/weh \
  -DCMAKE_C_FLAGS="-O3 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm" \
  -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_REQUIRE_ZLIB=ON
ninja && ninja install
cd /opt/fg-deps
[ -f gmp-6.3.0.tar.xz ] || curl -sSL -o gmp-6.3.0.tar.xz https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz
rm -rf gmp-6.3.0 && tar xJf gmp-6.3.0.tar.xz && cd gmp-6.3.0
emconfigure ./configure --host=none --disable-assembly --enable-cxx --disable-shared --enable-static --prefix=/opt/fg-deps/weh \
  CFLAGS="-O3" CXXFLAGS="-O3 -fwasm-exceptions" CC_FOR_BUILD=gcc
emmake make -j12
emmake make install
echo FT-GMP-DONE
