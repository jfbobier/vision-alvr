# Third-party code and data

| What | Version | License | Used by | sha256 |
|---|---|---|---|---|
| cgltf (`cgltf/cgltf.h`) | v1.14 | MIT (header) | offline scene converter `tools/bench_scene/convert.cpp` | 19db3bdf85f6aa991a3b9d056641bcb39a2ef11cd5211e7a9f6b6c307d9ebc64 |
| stb_image (`stb/stb_image.h`) | v2.30 (commit f58f558c120e9b32c217290b80bad1a0729fbb2c) | public domain / MIT (header) | host benchmark renderer (scene textures) | 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3 |
| Draco | 1.5.7 (github.com/google/draco tag) | Apache-2.0 | offline scene converter only (fetched and built by `tools/bench_scene/build.sh`, not shipped) | tarball bf6b105b79223eab2b86795363dfe5e5356050006a96521477973aba8f036fe1 |
| *Littlest Tokyo* (`LittlestTokyo.glb`, three.js examples) | - | CC-BY-4.0, by Glen Fox (glenatron, https://sketchfab.com/glenatron) | benchmark scene, converted to `bench/littlest_tokyo.vab` and shipped | 8375c2aa6808e28ad1cb9c0d43e1513f57ffd7865244bf66bbbd5bb0e907e06d (three.js r169) |
