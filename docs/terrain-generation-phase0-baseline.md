# GPU terrain generation Phase 0 baseline

The Phase 0 baseline is reproducibly generated rather than checked in with
machine-specific timing values.

Build and run the complete CPU fixture and semantic-contract suite:

```sh
cmake --build build-release --target SparseVoxelTerrainPhase0Tests -j 8
./build-release/tests/SparseVoxelTerrainPhase0Tests
```

Generate the full JSON benchmark:

```sh
cmake --build build-release --target SparseVoxelTerrainPhase0Benchmark -j 8
./build-release/tests/SparseVoxelTerrainPhase0Benchmark \
  --output build-release/terrain-generation-phase0.json
```

The report contains one record for every flat, mountain, cliff, canyon, river,
road-smoothed, coastline, snow, and cross-LOD fixture at every admitted LOD.
Each record separates map sampling, shell extraction, raster hierarchy, and
Tree64 wall time and records samples, runs/voxels, hierarchy levels/nodes,
payload bytes, and a semantic hash.

The current schema is `gpu-terrain-generation-phase0-v1`. A full run contains
27 page records (nine fixtures times three LODs) and seven streaming-churn
events. Timing values are intentionally generated locally instead of committed
because they are machine- and load-dependent.

The streaming section executes a deterministic bootstrap, camera walk,
teleport, opposite teleport, and return. It records update count, generated,
reused and evicted pages, request-to-visible wall time, pending work, and
physical upload bytes. GPU upload time remains the existing render-graph
`TerrainUpload` timer because this CPU benchmark does not create a graphics
device. Capture that timer alongside this report for a GPU baseline.

The smoke test generates a shorter report and validates the schema:

```sh
ctest --test-dir build-release -R SparseVoxelTerrainPhase0BenchmarkSmoke \
  --output-on-failure
```

The semantic decoder reports the first differing canonical cell for occupancy,
material, or context, and the first differing canonical brick node for LOD
error. Raster/Tree64 checks decode both representations onto the same finest
cell lattice and require exact occupancy agreement.
