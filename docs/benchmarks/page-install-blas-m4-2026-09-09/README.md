# Page-install BLAS microbenchmarks — Apple M4

September 9, 2026. Release build, three interleaved repetitions per case,
10 warmup and 60 measured cold-build submissions per repetition. Each cluster
contains 64 opaque triangles. The encoded cases include native GPU position/index
expansion followed by BLAS construction. The decoded cases use existing float3
inputs. All cases verify baseline visibility before timing.

| Input | Clusters/submission | Median run GPU p50 (ms) | Median run GPU p95 (ms) | Run p50 range (ms) |
|---|---:|---:|---:|---:|
| decoded | 64 | 0.932 | 9.276 | 0.920–1.685 |
| encoded | 64 | 1.194 | 9.073 | 1.188–1.263 |
| decoded | 256 | 8.240 | 12.353 | 1.497–11.680 |
| encoded | 256 | 6.544 | 13.005 | 5.020–11.560 |

**The observed variation is too large to establish a throughput improvement.**
These are measurements on an active desktop, not an isolated performance lab.
GPU timestamps cover native command-buffer elapsed time, including scheduling
gaps. The benchmark serializes diagnostic waits and excludes raster decode, VG
traversal, scene publication, handoff, page resolve and temporal accumulation.
It measures cold installation, not warm shadow-frame cost. Cached reuse and zero
warm rebuilds are correctness-tested separately.

Use these results to justify bounded asynchronous installs, not a claim that
256 cold BLASes fit a frame budget. Tune count/AS-byte quotas on the target
device and measure complete shadow frames before enabling a default quality mode.

Raw JSON includes every GPU sample, CPU preparation/encoding percentiles, cache
statistics and visibility hashes. [Provenance](provenance.json) records exact
commands, source hashes, platform and the base revision of the uncommitted build.

Reproduce a single case:

```sh
cmake --build build-release --target VirtualGeometryHardwareRayTraceBenchmark -j4
build-release/tests/VirtualGeometryHardwareRayTraceBenchmark --suite blas --geometry encoded --clusters 64 --triangles 64 --instances 64 --rays 1024 --warmup 10 --frames 60 --output /tmp/page-blas.json
```
