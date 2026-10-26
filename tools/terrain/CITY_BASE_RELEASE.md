# Full-world city release audit

**Status: release blocked.** This harness exposes integration failures; it does
not switch the default world generator to the accepted phase-3–6 blueprint path.
That automatic candidate-selection integration still has to be implemented.

Build the native tools and run the deterministic history fixture:

```sh
cmake --build build --target CityEvolutionPreview WorldSettlementPreview -j4
build/tests/CityEvolutionPreview build/history-release 2000 41 --no-mesh
```

This produces years 20, 100, 1000 and 2000. The simulation is explicitly labelled
`CitySocietyDriver scale fixture; not graph matcher`. Schema 2 exports dated
funded wall circuits and building membership, without old physical enclosure
polygons. It does not establish production graph-matcher performance.

Run a fresh full-world matrix:

```sh
python3 tools/terrain/run_city_release.py \
  --history build/history-release --output build/city-release
```

Defaults are all four years and world seeds 41, 42 and 43. The history seed stays
fixed so the world-site variations consume the same historical demand. Failed
cases keep their logs and native reports. The runner refuses existing output
folders and records each snapshot's SHA-256. Use `--years 20 --seeds 41` for a
single diagnostic. Nonzero exit is expected until all gates pass.

For native screenshots, add `--render`, with Node/Playwright available. Set
`BROWSER_EXECUTABLE` if using an installed Chrome; use `--node /path/to/node` and
`NODE_PATH` when using a bundled runtime. NumPy and Pillow are needed to pack the
native meshes. Each case captures top, front, back, left, right, perspective and
street views and writes camera metadata. Capturing an image does not approve it.

Individual native generation:

```sh
build/tests/WorldSettlementPreview \
  --history-snapshot build/history-release/year-20/settlement-100/history.json \
  --seed 41 --city-region 1 --release-check 1 --output build/city-release-single
```

Exit 3 means territory/model failure, 4 means incomplete runtime terrain, and 5
means the geometry release audit failed. Diagnostic files are retained. The
normal preview now also rejects partially built runtime terrain, regardless of
whether special landform generation was enabled.

`settlement.json.release` checks accepted snapshot identity, unique historical
bindings (including institutions), requested storeys and special programmes,
unrequested housing and all runtime tile results. A generated legacy castle is
not proof that a particular funded historical keep was projected correctly.
Missing or unprojected defence ledgers are explicit failures. Visual approval and
hill/ridge/valley coverage remain independent gates; seed names do not certify
terrain shape.

The current viewer displays the native city surface/structure export. It does
**not** prove that failed runtime terrain tiles built. Inspect both the runtime
job report and the actual screenshots. Sparse frontages, blocked doors, clipped
buildings or terrain artifacts keep the visual gate closed even if unit tests
pass.
