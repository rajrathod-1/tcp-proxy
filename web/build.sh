#!/usr/bin/env bash
# Compiles the balancer to WebAssembly for the browser demo. Needs Emscripten
# (`brew install emscripten`). The output is committed, so the static site
# deploys without a C++ toolchain; rerun this after changing balancer.hpp.
set -euo pipefail
cd "$(dirname "$0")"

em++ -std=c++20 -O3 -DNDEBUG -I../include wasm.cpp -o balancer.js \
    -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=web \
    -sEXPORTED_FUNCTIONS=_create,_pick,_release,_connected,_failed,_active,_available

ls -lh balancer.js balancer.wasm
