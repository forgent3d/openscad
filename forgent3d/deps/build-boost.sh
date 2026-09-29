#!/bin/bash
set -e
export EMSDK=/emsdk PATH=/emsdk:/emsdk/upstream/emscripten:/emsdk/node/22.16.0_64bit/bin:$PATH
cd /opt/fg-deps/boost_1_87_0
./b2 -j12 toolset=emscripten link=static variant=release threading=single runtime-link=static \
  --with-filesystem --with-program_options --with-regex --with-system --with-atomic \
  cxxflags="-fwasm-exceptions -O3" cflags="-fwasm-exceptions -O3" linkflags="-fwasm-exceptions" \
  --prefix=/opt/fg-deps/boost-weh --build-dir=/opt/fg-deps/boost-build install
echo BOOST-DONE
