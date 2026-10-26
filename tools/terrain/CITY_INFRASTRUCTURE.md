# Textured city infrastructure

Build `WorldSettlementPreview`, then run:

```sh
build/tests/WorldSettlementPreview --seed 17 --population 120 --city-infrastructure 1 --output build/renders/world-city
```

This runs the world/site/territory/fabric/completion generator chain. Infrastructure
mode enables the fabric's lot-allocation path and skips dwelling and compound
meshing, retaining walls, towers and gates. It is not the hand-authored demo.

The city model fills unallocated ground within a 12-metre margin of the city plots,
subtracting the existing street/block/lot surfaces before triangulation. Heights
come from the settlement terrain sampler. This is a city terrain patch, not an
export of the entire island. Long approach roads remain in the mesh.
Infrastructure previews default to six-metre maximum ground edges; add
`--ground-spacing 2` after `--city-infrastructure 1` for a much denser mesh.

`city-model-c10.json` includes metric triangle UVs, material IDs, and normalized
grass/earth/stone weights per triangle corner. Shared ground vertices reuse one
weight calculation. Vertical fortification faces use elevation-aware UVs.

Serve the repository and open:
`tools/terrain/ground_material_preview.html?mesh=/build/renders/world-city/city-model-c10.json`

The WebGL preview focuses on the city in infrastructure mode. It uses the
simplified mesh renderer, not virtual geometry. The Weights checkbox visualizes
the three terrain blend channels.

## Street-first blocks and connected houses

The block planner reserves sidewalks, a central common (when there is room),
and two opposing approaches before it divides private lots. Four-sided convex
blocks use interpolated cuts between opposite edges, so trapezoids have shared,
non-orthogonal parcel boundaries. Shallow blocks remain all frontage; crofts
retain their separate rural layout. `commonCourt` and `conformingQuadLots` on
`CityBlockPlanInput` can independently disable these choices.

Public-space polygons now survive into `CitySkeleton::publicSpaces` and the
surface ownership pass. Streets take precedence, then sidewalks/courts and
alleys, then private lots. Courts have a 0.54 m stone base with 0.18 m risers
and 0.38 m step bands. These are meshed surfaces and masonry risers, including
inner boundaries; they follow the city's graded ground. They are not a new
city-wide elevation solver. Buildings are generated after these reservations.

The house fitter avoids applying a second frontage setback behind a planned
sidewalk. Compatible houses on opposite sides of a narrow block approach can
become a gateway house, with upper rooms over a reserved ground-floor passage.
A skewed party-wall extension that crosses its lot is reduced before rejecting
the house. Room grids remain orthogonal; the new non-orthogonal grid is the
parcel subdivision, not unrestricted deformation of doors and rooms.

Reproduce the block inspection (native engine geometry, no image synthesis):

```sh
cmake -S . -B build
cmake --build build --target WorldSettlementPreview CityBlockPlannerTests -j 6
build/tests/WorldSettlementPreview --block --width 46 --depth 38 --skew 8 --infrastructure 1 --output build/renders/block-infrastructure
build/tests/WorldSettlementPreview --block --width 46 --depth 38 --skew 8 --output build/renders/block-after
python3 tools/terrain/render_city_model.py build/renders/block-after/city-model-c10.json build/renders/block-after/city
ctest --test-dir build -R '^(CityBlockPlannerTests|WorldCitySkeletonTests|WorldCityCompletionTests)$' --output-on-failure
```

`--infrastructure 1` on the block fixture emits only the base. Omitting it uses
the same plan and adds dwellings. The block JSON reports gateway houses and
rejection reasons. `CityBlockPlannerTests` checks public/private separation,
area conservation, skewed lot containment, stepped meshing and gateway housing.

For matching diagnostic views, use `render_city_block.py INPUT OUTPUT.png`;
`--clay` exposes the mesh without textures, and `--gateway` frames the passage
in the 46 x 38 m fixture. The tested skewed fixture now realizes 18 buildings
(one gateway joining two parcels), compared with 16 before these changes.
The rectangular fixture realizes 18 buildings including two gateways.

Integration observation: the seed-41/population-1600/relief-500 world fixture
exported a city mesh with 1,922 buildings. Its separate terrain job report
still contains two failures: `tile-0-0` reports a source-chain gap/overlap and
`tile-1-0` reports a constrained-triangulation failure. This city/block work
does not resolve those terrain-tile failures.

## Production policy

Towns and cities default to `SettlementFabricSettings::streetFirstBlocks`.
The fabric emits blocks/lots; completion and the city model place dwellings
after reserving public ground. Villages retain the population director path.
The setting can be disabled explicitly for legacy callers.

Every valid block records a strategy: frontage, courtyard, crofts, infill, or
common. Triangles and complex boundaries are eligible for frontage planning;
existing allocations provide infill when a new frontage plan cannot fit.
Empty unallocated blocks become commons. Block lot roles and attachment policy
survive into completion, rather than being replaced by random house roles.
Croft yards/gardens and internal gardens retain their own ground ownership.

Attached frontage rows reserve an approach into their common before lots are
cut. Compatible lots on either side become one gateway house with a physically
open street-level passage and occupied rooms above. If fitting that connection
fails, the reserved alley stays open. The passage mesh is checked by an
unobstructed eye-height ray in the block regression test.

Paving retains semantic material IDs (18 for walks/treads, 19 for courts),
but now shares normalized grass/earth/cobble weights with streets and lot
approaches. All top surfaces sample the same world-space field and metric UVs.
Stone reaches the full walking boundary, then feathers 1.25 m into adjacent
ground, with subtle continuous wear. This removes wall-texture stripes and
parcel-edge material jumps. Road curbs default to 9 cm. House-front paving
influences also apply behind planned sidewalks; they do not add overlapping
geometry. Unallocated urban interior ground gets paving; rural ground stays green.

Full-city JSON includes `blockStrategies`, `publicSpaceCount`, and
`gatewayHouses` for inspection. The default path can be exercised without
`--lots`:

```sh
build/tests/WorldSettlementPreview --seed 41 --population 1600 --city-region 1 --relief 500 --output build/renders/city-integrated
python3 tools/terrain/render_city_neighborhood.py build/renders/city-integrated/city-model-c10.json build/renders/city-integrated/courtyard.png --crop 1310 290 1365 342
```

The earlier production baseline exported 1,639 buildings and 4,946,860 triangles.
All 308 blocks have strategies: 194 frontage, 100 crofts, 9 courtyard and 5
infill. It contains 2,149 public-space reservations and 3 realised gateway
houses. Thirty-eight attempted lot buildings were rejected by the fitter;
the export records diagnostics rather than forcing invalid geometry. The two
terrain-tile errors described above remain in the separate runtime job report.
All exported positions, UVs and weights are finite, triangle indices are valid,
and the earlier export used direct paving materials (superseded by the shared blend).

The neighborhood renderer keeps a 30 m geometry margin outside the camera crop
so nearby roofs and walls still occlude correctly. Its output uses the exported
engine triangles and texture manifest.

Validation: `CityBlockPlannerTests`, `WorldCitySkeletonTests`, and
`WorldCityCompletionTests` pass, including open gateway geometry, public/private
area conservation, material separation and a common-garden path regression.
The six Python material tests pass. The broader `WorldGeneratorIntegrationTests`
exceeds its 180 s CTest timeout; a direct diagnostic run also reports failing
top-storey gable-closure and small poor-home assertions, and was stopped during
its later runtime-chain checks. Broad integration is not green.

## Natural paving and land admission

Undersized or narrow frontage pieces merge with a contiguous neighbour before
lot admission. A union must remain a single polygon without holes; reserved
public approaches cannot be merged away. Unusable pieces remain shared ground.
Admitted parcels need 45 m² by default and a usable inset; the dwelling fitter
also rejects occupied ground footprints below 24 m² instead of emitting tiny
fallback houses. Reserved ground is available for later furnishing; this pass
does not automatically place a bench, well or shed on every remainder.

Deep urban interiors are planned recursively as lane-fronted housing around
a smaller common. The outer entrances terminate at the inner perimeter lane,
which connects its houses and inner approaches. `maximumCourtMetres2` (240 m²)
triggers subdivision when four additional minimum-sized lots can fit. Explicit
market squares retain their civic space. Districts labeled suburb use urban
planning inside a wall circuit; agricultural crofts remain outside.

The 100 × 80 m regression checks over 60% private-lot coverage, a court below
500 m², minimum lot admission and no private/public overlaps. Its skewed native
render realizes 98 buildings, with a minimum footprint above 30 m². Paving tests
check continuity, normalization and agreement between indexed/reference fields.

```sh
build/tests/WorldSettlementPreview --block --width 100 --depth 80 --skew 8 --output build/renders/block-deep-natural
build/tests/WorldSettlementPreview --seed 41 --population 1600 --city-region 1 --relief 500 --output build/renders/city-natural
```

Full-city validation (`city-natural`, seed 41 / population 1600): 1,464
buildings, 4 gateway houses, 4,975,747 triangles. The house/trade footprint
minimum is 24 m² (within floating-point tolerance), versus 271 sub-24 m²
buildings in the prior export. Median footprint increased from 39.07 to
44.31 m²; total footprint increased from 63,618.94 to 65,346.70 m² (2.72%).
All exported positions/UVs are finite and indices valid. Public paving weights
sum to one within 1.2e-7, the precision of their float32 binary export.

The three focused C++ suites and six Python material tests pass. The full run
still reports 290 rejected/reserved lot fits (including intentional small-house
rejections), 20 inaccessible plots left empty, 5 dropped building faces and the
same two separate runtime terrain-tile failures. It is not a watertight-building
or fully successful terrain publication certification. Full statistics are in
`build/renders/city-natural/audit.json`.

```sh
python3 tools/terrain/render_city_neighborhood.py build/renders/city-natural/city-model-c10.json build/renders/city-natural/whole-city.png --whole-city --width 2000 --height 1500 --pitch 1.02
python3 tools/terrain/render_city_neighborhood.py build/renders/city-natural/city-model-c10.json build/renders/city-natural/neighborhood.png --crop 1310 290 1365 342
```
