# Landmark compounds

The native compound planner supports three large landmark layouts through
`CompoundBrief::fantasyGothic` and `landmarkLayout`:

- `Citadel`: an asymmetric clipped-corner curtain, polygonal inhabited towers,
  a tall keep, great hall, residential ranges, chapel, planted forecourt and dungeon.
- `GardenManor`: a three-wing house around a garden court, lower enclosure,
  paired parterres, entrance avenue and two fountains.
- `Palace`: a five-wing composition with reception and ballroom pavilions,
  residential range, library/banqueting galleries and formal gardens.

The supplied reference images informed these layout rules rather than serving
as literal plans. Their useful recurring features were a hierarchy of volumes,
connected courtyard circulation, differentiated service and ceremonial wings,
corner and stair towers, planted entrance courts, and material changes between
base, wall, trim and roof. This implementation uses Gothic architectural details
for all three layouts; it does not reproduce a classical palace's domes or orders.

## Native generation and rendering

```sh
cmake --build build --target WorldSettlementPreview GothicCastleTests -j 6
build/tests/WorldSettlementPreview --castle --style citadel --seed 41 --width 140 --depth 120 --dungeon-levels 3 --output build/renders/citadel-estate
build/tests/WorldSettlementPreview --castle --style manor --output build/renders/manor-estate
build/tests/WorldSettlementPreview --castle --style palace --output build/renders/palace-estate
python3 tools/terrain/render_city_neighborhood.py build/renders/citadel-estate/city-model-c10.json build/renders/citadel-estate/exterior.png --whole-city --width 1600 --height 1200 --pitch .8
python3 tools/terrain/render_city_neighborhood.py build/renders/citadel-estate/dungeon-0.json build/renders/citadel-estate/dungeon.png --whole-city --pitch .6
python3 tools/terrain/build_dungeon_explorer.py build/renders/citadel-estate/city-model-c10.json build/renders/citadel-estate/explore.html
python3 tools/terrain/export_city_viewer.py --input build/renders/citadel-estate/city-model-c10.json --output build/castle-viewer
ctest --test-dir build -R '^(GothicCastleTests|WorldCityCastleTests|CityBlockPlannerTests)$' --output-on-failure
python3 -m unittest discover -s tools/terrain -p test_dwelling_materials.py
```

Large city castle precincts request this profile automatically at 14,000 square
metres. Manor precincts request the garden manor at that size, and the palace
at 24,000 square metres. The inscribed usable rectangle must still fit at least
110 by 100 metres; a failed fit falls back to the existing smaller compound.

## Geometry contract

Curtains are 3.8m wide with two 0.5m parapets, leaving a 2.8m promenade.
Their tops meet the third or fourth 4.2m tower storey at 12.6m or 16.8m.
Towers at height transitions connect the two walks through their return stairs. Tower volumes are subtracted
from curtain bodies and parapets; these are convex pieces, not intersecting
filled boxes. Elevated tower openings are cut through both wall skins. Tower
floors have explicit stairwell holes, return stairs have treads and risers,
and landings and floor edges have guards with openings at stair arrivals.

Inhabited tower floors require a clear inner radius of at least 3.6m. Doorway,
landing, stair and guard geometry is checked by the existing access validator.
The return stair is solid: each flight has treads and risers, a stringer down
either side and a soffit, so the flight above is the ceiling of the flight
below; a spine wall closes the slot between the two flights from the ground to
a rail's height above the lookout, and the open edge of every flight carries a
rail that climbs with it (`Opening::along`, `FortressPieceKind::Wall`).

Tower footprints are turned half a face, so an axis-aligned curtain meets a
face centre, and a wall-walk doorway is cut where the curtain's axis crosses
the tower face rather than on the face's middle: every point of the doorway is
within 1.35m of the axis, which is what keeps it on the 2.8m promenade when the
curtain is a bevel and meets the tower on a corner.

A city-wall tower's spiral stair lays a two-face landing wherever a wall-walk
door comes in and shares the storey's rise over the remaining faces; the
revolution starts at the first door's landing, so that door meets the walk
exactly. A second door on the far side meets its landing part of a storey
higher and gets a short flight of steps on the wall top up to its sill; the
city raises a wall tower until that door's head clears the lookout, and a
tower too short for it keeps the near door only. The corner kites between
flights are slabs, every flight is a solid, and the well side of every flight
is railed. The access model's clear width on a tower stair follows the stair
(`0.45 W`), which is what the corner kites can carry.

A wing-to-curtain gangway is a deck slab on piers with a masonry parapet down
each side; its route is rejected when it would pass through another wing or a
tower, not only through its own wing's walls.
Narrow lights and larger upper lights are actual shell openings. Stone crown
bands sit below the wall walk and under the lookout rather than across doors.

Wing connections are selected from actual ground-storey exterior walls after
cell realisation, before skeleton meshing. A short spanning set of unobstructed
connections receives paired arch openings and covered colonnades. These can be
open to the court; they are not sealed corridors. Candidate links avoid tower,
wing and planted-bed interiors, and the result is exported as `buildingLinks`.
Arbitrary precincts can still yield disconnected wings when no valid straight
connection fits; inspect this metadata rather than assuming complete connectivity.

Gardens, edging, hedges, stone paths and tiered fountain basins are native mesh
geometry. Water is a static material, without fluid simulation. Room roles are
programmatic layout categories, not a guarantee of specialised furniture.

## Materials

The masonry record declares a three-layer weathered blend: original stone,
warm plaster repairs and tinted damp stone. Smooth metre-scale masks vary across
UV space independently of triangle boundaries and the underlying 3.2m tile.
The Python preview blends these layers in linear light. The WebGL viewer uses
the same masks with a gamma approximation. Exported texture tints are baked
once. The raw native mesh carries surface IDs and UVs; another runtime renderer
must implement this manifest blend to match these previews.

## Dungeon prototype

Dungeon levels contain nine named chambers, shared-wall doors, cross passages
and a linked central stair. The exporter writes separate native cutaway meshes
and a graph used by the small HTML exploration prototype. The graph reaches all
rooms from the keep entrance. These diagnostic cutaways are not yet a complete
underground runtime shell with ceilings, keep-floor excavation, collision,
enemies or inventory. The exploration prototype simulates graph movement only.

## Validation evidence

The seed-41 140m × 120m examples produce:

| Layout | Wings | Towers | Covered wing links | Dungeon rooms |
|---|---:|---:|---:|---:|
| Citadel | 5 | 17 | 4 | 27 |
| Garden manor | 3 | 0 | 2 | 0 |
| Palace | 5 | 0 | 4 | 0 |

`GothicCastleTests` checks gate orientation, disjoint wing reservations, precinct
containment, all dungeon graph connections, shared-wall door positions, native
cutaway arrays, the garden manor's realised gallery links, three inhabited tower
sizes, floor/stair/guard access geometry, and roof winding. Existing access,
city skeleton, completion, castle and block planner tests are also exercised.
The material tests cover normalized masks, tile independence and linear-light
blending; exporter tests cover native attributes.

For browser verification, serve `build` locally and run:

```sh
python3 -m http.server 8788 --bind 127.0.0.1 --directory build
# In another terminal with Playwright on NODE_PATH:
node tools/terrain/city_viewer/landmark_smoke_test.cjs http://127.0.0.1:8788
```

Set `BROWSER_EXECUTABLE` if using an installed Chrome instead of Playwright's
bundled Chromium. The test compiles the material shader, uploads the native
mesh, enters a tower, reaches the deepest dungeon vault through graph links,
returns to the keep, and checks for browser errors. Fly-through movement is
unrestricted; it is a geometry inspection tool, not a collision simulation.

## Yard and wall-attached keep variants

`--keep attached|detached|auto` selects keep placement. Automatic placement uses
an attached keep for one third of seeds. The attached keep replaces part of the
rear curtain, with paired upper doorways and stone connections into the two wall
ends. Residential ranges also receive raised bridges to real exterior door cuts
at occupied storeys. These routes are exported as `wallAccessLinks`. Branch rails
start beyond the inner parapet to preserve the through promenade.

Keeps have seven or eight storeys; residential ranges have five or six.
`--extra-storeys 0..4` increases these and service-annex heights. Available free
space is scanned on a 4m grid after wings, towers, paths, fountains and planted
beds are reserved. Up to four substantial yards receive stone perimeter paths;
larger yards also receive planted beds while retaining open space. Service
annexes are added only where their clearance fits.

```sh
build/tests/WorldSettlementPreview --castle --keep attached --output build/renders/castle-attached-yards
build/tests/WorldSettlementPreview --castle --keep detached --width 180 --depth 150 --extra-storeys 1 --output build/renders/castle-grand-yards
```

The seed-41 attached example has six wings, fourteen towers, four raised wall
connections and two yards totaling 2,238 square metres. The larger detached
example has six wings, seventeen towers and four yards totaling 3,264 square
metres. Yard area includes planting and perimeter paths. These are native meshes;
full collision-based traversal remains outside the preview's validation.

## Passage dimensions

Landmark wings use a 2.4m capacity grid instead of the urban 1m pitch. Their
ordinary door leaves are 1.6m wide; single arches retain at least 1.58m on this
grid. Raised and ground gallery connections use 1.6m apertures and require at
least 1.8m of wall span. They are cut at those dimensions before meshing, rather
than enlarged by stretching an existing doorway. Castle corridor cell counts include
interior wall thickness when converting the requested clear width into cells.

Door-clearance clipping for Gothic roof ribs uses a snapshot of opening volumes,
so adding decoration does not repeatedly scan the growing decorative mesh.
`GothicCastleTests` measures realised internal doors as well as exported gallery
thresholds. The wider-grid preview is in `build/renders/castle-wide-passages`.

## Masonry facade and arcade variation

Masonry facades now opt into deterministic per-opening variation through
`masonryVariation` and `variationSeed`. Castle windows mix pointed, round and
square heads, open apertures and glazing, and selected paired lights with a
small circular tracery head. Both wall skins use the same segment-based choice;
open windows omit glazing on both sides rather than drawing a dark panel.
Ordinary city stone buildings use the same policy, with Romanesque or Gothic
families, while timber facades retain their existing treatment.

Covered courtyard links now have stone bases, slender piers, capitals and closed
arch-ring meshes along both sides. Round and pointed bays vary by wing seed.
The arches meet the canopy and remain outside the central walking aisle. The
existing 1.6m entrance cuts are retained. Facade tests check glazing omission,
profile diversity, paired-light detail and agreement between both wall skins.

## Local support and gallery clearance

Exterior buttresses now measure the continuous wall stack across their actual
attachment width. Low wings below 5m omit monumental supports. Other supports
stop below their local wall top, nearby openings and roof planes over their
projecting footprint; the tallest storey elsewhere in the wing is irrelevant.

Gallery candidates reserve their canopy width against buildings and towers,
while allowing terminal canopies to bond into the two connected facades. Door
positions can slide within their wall bay, preserving the 1.6m opening and jamb
clearance, to avoid clipping a narrow courtyard return.
Selected galleries must remain separated from other gallery envelopes. Planting
may sit below canopy overhangs, but never in the walking aisle.

Internal corridor ribs use an arch-only atlas asset. Their columns meet the
round or pointed spring line without duplicating the vertical legs of a complete
portal frame. Targeted tests cover local low/tall walls, roof clearance, broken
wall stacks, crossing/parallel galleries and arch-to-column separation.

### Tower wall-walk junctions

Curtain clipping and the realised tower use the same polygon rotation and side
count. Door width is measured on the inner skin, so the thickness of the shell
does not squeeze the passage. Shell apertures include sill and soffit faces;
ground entrances have separate threshold slabs. Elevated entrances in inhabited
towers must coincide with a complete storey floor or planning fails.

Inhabited tower floors, including the lookout, are 18 cm slabs with an underside
and exposed stairwell edges. Their openings include the entire return landing,
so the next storey cannot cover the landing halfway up the stair. The central
stair remains open while the surrounding floor connects wall-walk doors.

`GothicCastleTests` checks footprint rotation, sill geometry, top-floor thickness,
stairwell extent and rejection of between-floor doors, alongside access reachability.

### Terraced citadel

`WorldSettlementPreview --castle --terraces 3 --dungeon-levels 0 --output build/renders/terraced-castle`
reserves three broad height bands before buildings. `--terraces` accepts 3–5;
provide at least 128 metres of width and 32 metres of depth per band plus the
14 metre precinct margin. The width reserves clearance between side ranges and
the longitudinal climbing route.
The template rotates toward the entrance. Each tier rises one 4.2 metre storey.
A 4.8 metre staircase occupies a notch in the retaining platform, with 24 risers
of 17.5 cm and 35 cm treads. Successive stairs alternate sides; paved cross-courts
and longitudinal paths connect their landings. Side and rear edges have guards.

The bands own their foundations, towers, walls, courts and planting heights.
A summit keep, chapel and hall terminate the climb; lower bands contain service,
guard and residential ranges. Wings use actual cell dwelling blueprints (including
rooms, doors and stairs), rather than the legacy three-storey rectangular planner.
Galleries only connect wings on the same terrace. Native preview JSON includes
`terraces`, `terraceLinks` and `terraceRoutes` for inspection and downstream tools.

This is a citadel template, not a terrain erosion simulation. Large city castle
districts try it for one third of site seeds and fall back to the single-court
layout when it does not fit. The standalone preview selects it explicitly. It currently
requires zero dungeon levels and uses a summit keep; wall-attached keeps remain
available in the single-court template. Spiral bands and arbitrary height steps
are not implemented. The standalone preview retains the single-court default.

Generate the labelled site blueprint with `python3 tools/terrain/render_castle_blueprint.py INPUT.json OUTPUT.svg`.
