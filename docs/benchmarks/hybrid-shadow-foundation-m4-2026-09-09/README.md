# Metal any-hit and closest-hit baseline

September 9, 2026, Apple M4. Release build, synthetic opaque cluster grid,
1,000 instances, one cached 64-triangle BLAS, 1,000,000 rays (half hits),
120 warmup + 600 measured frames, five interleaved repeats per query.
Numbers below are medians of the five per-run percentiles, in milliseconds.

| Query | GPU p50 | GPU p95 | GPU p99 | CPU encode p50 |
| --- | ---: | ---: | ---: | ---: |
| Any-hit | 0.749 | 4.828 | 6.344 | 0.054 |
| Closest-hit | 0.855 | 5.376 | 6.424 | 0.047 |

All runs validate hit/miss results; closest-hit also validates distance. Warm
frames build no BLAS. GPU timings cover instance preparation, TLAS and tracing.
These serialized native microbenchmarks exclude VSM demand, page ray generation,
receiver accumulation, material resolve and graph interop waits. They do not
establish a complete shadow-renderer frame budget or a throughput guarantee.
The large tails remain relevant; do not derive presets from the median alone.

The ten JSON files contain raw frame samples. [Provenance](provenance.json)
records the binary hash, base commit, uncommitted-tree status and command.

```sh
cmake --build build-release --target VirtualGeometryHardwareRayTraceBenchmark -j4
build-release/tests/VirtualGeometryHardwareRayTraceBenchmark --suite full --query closest --instances 1000 --rays 1000000 --clusters 1 --warmup 120 --frames 600 --output /tmp/closest.json
```

Use `--query any` for the any-hit comparison. This baseline is intentionally
separate from the previous 25-run benchmark report.
