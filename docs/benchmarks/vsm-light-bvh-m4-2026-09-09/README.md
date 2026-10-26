# VSM tile light BVH — Apple M4, September 9, 2026

The parallel full-light loop remains the default. In this workload, dividing BVH
traversal across 64 lanes did not pay for its additional traversal and mask costs.

| Enumeration | Median of four run medians |
|---|---:|
| Parallel full-light loop | 0.229 ms |
| Parallel BVH traversal | 0.274 ms |

BVH enumeration was about 20% slower. This result is specific to this light
distribution and GPU; it does not establish that a BVH cannot help other scenes.
The option remains available through `enableLightBVH` for further measurement.

The fixture uses a 1280 × 720 screen, 16 × 16 tiles, 202 finite lights and 32
directional lights. Four interleaved repeats alternate ordering, each with 20
warmup frames and 80 measured frames. GPU timestamps measure only the graph's
`_LightTiles.pipeline` node. Page scheduling, shadow tracing, lighting resolve,
CPU tree construction and uploads are excluded from the reported measurement.
The test uses a small page configuration to fit the existing packed layer limit.

Raw samples and per-run p50/p95 are in [runs.json](runs.json). There are scheduling
outliers, including unusually short samples and long tails; this is a local
microbenchmark, not a full-renderer frame-time claim. Build identity and command
are in [provenance.json](provenance.json).

Reproduce from the repository root:

```sh
cmake --build build --target VirtualShadowTileTests -j4
cd build/tests
./VirtualShadowTileTests --benchmark
```

Correctness tests separately compare the exact tile bitsets and page demand
against the full-light loop, including moving lights and tree revision reuse.
