# Native Metal hardware ray tracing baseline

Measured September 9, 2026 on Apple M4, macOS 26.5.2 (25F84), release build,
offline Metal 3.1 shaders. This is a synthetic native GPU microbenchmark, not a
measurement of the renderer's complete shadow pipeline. The core source hashes,
binary hash and dirty-tree commit are in [summary.json](summary.json).

| Workload | Median of run p50, ms | Median of run p95, ms | Worst run p99, ms |
| --- | ---: | ---: | ---: |
| Trace prebuilt TLAS: 1k instances, 1M rays | 0.709 | 5.050 | 7.803 |
| Instance preparation + TLAS: 1k instances | 0.210 | 1.551 | 6.692 |
| Full warm native segment: 1k instances, 1M rays | 0.909 | 4.568 | 8.047 |
| Full warm native segment: 10k instances, 1M rays | 1.987 | 5.690 | 6.434 |
| Cold build: 1k unique cluster BLASes, 64k triangles | 12.424 | 19.879 | 25.254 |

Five runs per case, interleaved forward/reverse case order. Warm cases use 120
warmup frames plus 600 measured frames each. Cold BLAS cases use 20 measured
frames per run, with new geometry revisions for each batch. Every raw JSON file
retains its sample array, p50/p95/p99, CPU encoding time, cache counters, memory,
ray/geometry/result hashes and correctness result. Warm cases assert zero new
BLAS builds during measurement.

Each cluster is a static opaque 64-triangle grid. The 1k-instance cases share one
cluster BLAS; the 10k-instance case uses 1k unique cache identities with repeated
instances. Independent identities intentionally have the same local shape. Rays
are a deterministic half-hit/half-miss workload checked before measurement. These
are not representative imported scene complexity or a complete shadow image.

GPU time is Metal command-buffer end minus start; the benchmark serializes
completion for diagnostic sampling. It includes native dispatch/build overhead
and possible GPU scheduling gaps. Trace-only still includes ray-count/dispatch
preparation. The full native segment includes instance preparation, TLAS build
and trace, with warm cached BLASes. No hierarchy traversal, receiver generation,
light-volume generation, streaming demand, interop wait, material resolve or
display/vsync is timed. Isolated stage timings must not be summed as a measured
frame time. Power and thermal state were not instrumented.

Run-to-run and tail variation are substantial; these results do not establish a
2 ms p95 renderer budget or prove an optimal build/trace policy. Cold BLAS cost
reinforces the need for cached builds and a bounded installation queue outside
the critical frame path. The next performance gate is a complete renderer replay
with equal-quality shadow reference images and per-stage timestamps.

Reproduce from the repository root:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target VirtualGeometryHardwareRayTraceBenchmark -j4
python3 scripts/benchmark-vg-hardware-raytrace.py --executable build-release/tests/VirtualGeometryHardwareRayTraceBenchmark --output /tmp/vg-rt-benchmarks
```

Run GPU correctness tests separately before timing. Avoid simultaneous GPU tests
or captures during measurement. The driver refuses a non-release executable,
unverified results, or changes to benchmark/backend/shader source or the binary
during its run.
