#!/usr/bin/env bash
# Builds the benchmark scene bench/littlest_tokyo.vab (WSL): fetches Draco 1.5.7 and Littlest Tokyo (pinned, hash-checked) into
# $WORK, builds Draco's static library and the converter, converts. Usage: tools/bench_scene/build.sh <work dir> <output.vab>
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${1:?work dir}; OUTVAB=${2:?output .vab}
mkdir -p "$WORK/dl" "$WORK/src" "$WORK/draco_build" "$WORK/bin"
fetch() { [ -f "$WORK/dl/$2" ] || curl -sS -L -o "$WORK/dl/$2" "$1"; echo "$3  $WORK/dl/$2" | sha256sum -c --quiet; }
fetch https://github.com/google/draco/archive/refs/tags/1.5.7.tar.gz draco-1.5.7.tar.gz bf6b105b79223eab2b86795363dfe5e5356050006a96521477973aba8f036fe1
fetch https://raw.githubusercontent.com/mrdoob/three.js/r169/examples/models/gltf/LittlestTokyo.glb LittlestTokyo.glb 8375c2aa6808e28ad1cb9c0d43e1513f57ffd7865244bf66bbbd5bb0e907e06d
[ -d "$WORK/src/draco-1.5.7" ] || tar -xzf "$WORK/dl/draco-1.5.7.tar.gz" -C "$WORK/src"
if [ ! -f "$WORK/draco_build/libdraco.a" ]; then
  (cd "$WORK/draco_build" && cmake "$WORK/src/draco-1.5.7" -DCMAKE_BUILD_TYPE=Release -DDRACO_JS_GLUE=OFF -DDRACO_TESTS=OFF -DBUILD_SHARED_LIBS=OFF >/dev/null && make -j"$(nproc)" draco_static >/dev/null)
fi
g++ -O2 -std=c++17 -I"$ROOT/third_party/cgltf" -I"$WORK/src/draco-1.5.7/src" -I"$WORK/draco_build" "$ROOT/tools/bench_scene/convert.cpp" "$WORK/draco_build/libdraco.a" -o "$WORK/bin/convert"
"$WORK/bin/convert" "$WORK/dl/LittlestTokyo.glb" "$OUTVAB"
