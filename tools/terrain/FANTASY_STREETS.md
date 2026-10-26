# Fantasy dwellings and terraced streets

The supplied references informed stone ground floors with timber upper storeys,
projecting rooms, steep mixed-color roofs, dormers, shutters, covered shopfronts,
barrels and signs, and districts connected by long public stairs. These are
procedural rules rather than copied building plans.

## Native generation

Ordinary dwelling roofs vary in pitch and ridge height. Inns, pubs and merchant
houses request more dormers and yard balconies; inns and pubs enable the existing
timber framing system, including ceiling joists and roof tie beams. Terracotta,
slate and sage tile materials vary by building. Street details are enabled
independently of fences through `CityModelSettings::streetDetails`: canopies,
signs, benches, polygonal barrels with hoops, crates and yard props. Ground props
avoid public road widths, doorway clearances and entrance stair footprints.

`CityReliefNoise::terraceRiseMetres` creates broad level bands in the hill field.
`CityReliefSettings::stairStreets` lets steep road edges use a bounded stair grade.
The relief planner then reserves treads and intermediate/end landings before
buildings are generated. The native city mesh emits stone stair bodies and timber
handrails; terrain beneath the stairs is recessed to prevent triangles crossing
the treads. Links too short for the minimum going are counted as impossible and
remain graded routes rather than being emitted as compressed stair meshes.

The ordinary flat-city settings are retained. Enable terraced hills in the full
settlement preview with `--hills 24 --hill-wavelength 120 --terrace-rise 3.6`.
This is geometric planning; the viewer remains a free-flight inspection tool,
not a collision-based walking simulation.

## Reproducible district study

```sh
cmake --build build --target WorldSettlementPreview WorldCityCompletionTests -j6
build/tests/WorldSettlementPreview --terraced-street-demo build/renders/fantasy-street/city-model.json
python3 tools/terrain/render_city_neighborhood.py build/renders/fantasy-street/city-model.json build/renders/fantasy-street/exterior.png --whole-city --width 1500 --height 1100 --pitch 1.05 --yaw 2.65
python3 tools/terrain/export_city_viewer.py --input build/renders/fantasy-street/city-model.json --output build/fantasy-street-viewer
ctest --test-dir build -R '^WorldCityCompletionTests$' --output-on-failure
```

The study uses the same relief planner and dwelling/mesh generator as the city:
three adjoining blocks, twelve houses, two inns, two pubs, and two reserved
stair courts. It is a neighborhood fixture, not a regenerated world settlement.
Its JSON includes the stair bands for checking exact elevations and going.
The completion tests check stair rise, going, intermediate landings, endpoint
alignment and terrain clearance. Material tests cover the rendering library.

## Raised masonry blocks (separate from terrain)

`planRaisedCityBlocks` accepts explicitly selected block IDs, deck polygons and
levels, and access paths. It leaves `ground` and `openGround` unchanged. Each
block is emitted as a closed masonry solid with vertical sides and a flat paved
deck. The house/yard support sampler then uses that deck level. Edge parapets
leave the planned stair mouths open. Stairs and turning landings have masonry
supports down to their foundations; they do not require a slope underneath.

Access paths may include ascending flights and level turning landings. A concave
deck boundary can reserve a stair bay entering a side yard inside the block.
The planner rejects insufficient run, invalid levels, duplicate decks and lots
outside the deck boundary instead of compressing stairs or leaving unsupported
lots. Selection and access paths are explicit inputs, not automatic district-wide
placement. Callers must reserve these corridors clear of other blocks/buildings.

The `--terraced-street-demo` now demonstrates three independent decks at 0.8, 5.0
and 9.2 metres on flat base terrain, including a three-flight switchback and recessed stair bay. The two upper blocks
reserve former housing lots for circulation. An arched bridge reconnects the
upper street across its stair court, with a minimum 2.2m clearance check.
Use `build/renders/raised-blocks/city-model.json` for the updated output. The older
noise-terrace CLI option above remains a separate terrain feature.

The overview renderer now retains timber groups 17 and 45. These groups include
window frames, shutters and door leaves, and also fill the slots deliberately
cut out of half-timber wall panels. Filtering them out caused the apparent cracks
and missing joinery in the previous still images. The WebGL export already
included these groups. Exterior opening reveals also now use the visible wall
group rather than the hidden interior-wall group.
