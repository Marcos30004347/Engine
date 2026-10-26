# Base-first mountain cities

The new native `WorldCityBase` planner runs in two stages:

1. `planCityBase(settings, base)` creates a continuous hillside, irregular cliff terraces, rock outcrops, contour lanes, cross alleys, switchback stair courts, landings and bridges. It reserves buildable envelopes but creates no occupied lots.
2. `populateCityBase(base, city, relief)` divides only those envelopes into useful lots, assigns their terrace elevations and dwelling programmes, and preserves circulation. Pass both products to `buildCityModel` for the existing dwelling generator.

The default city has four levels, 16.2 m of terrace rise, 46 lots, ten stair flights and 27 alley segments. The platform solids remain independent of the underlying terrain. Alley junctions are unioned and clipped to terrace boundaries before meshing; they do not cap stair courts. Stair dimensions, landing lengths, deck containment and bridge headroom use the existing raised-block validator. No terrain noise is applied to individual house floors.

```sh
build/tests/WorldSettlementPreview --mountain-base build/renders/mountain-city/base.json
build/tests/WorldSettlementPreview --mountain-city build/renders/mountain-city/city.json
# Same base seed and settings in both stages:
build/tests/WorldSettlementPreview --mountain-city build/renders/mountain-city/variant.json --seed 71 --tiers 5 --width 100
python3 tools/terrain/export_city_viewer.py --input build/renders/mountain-city/city.json --output build/mountain-city-viewer
```

Inspect the base before populating it. JSON exports include terrace polygons, stair bands, rock bounds, alley widths and lot elevations. The raw-bedrock material reuses the earth texture with a neutral tint, while retaining walls and paving retain their own materials.

This is an explicit mountain-city mode, not the default world-settlement recipe. The present layout uses linked contour terraces with rectangular lot envelopes; it does not yet trace arbitrary mountain contours or produce caves. The browser viewer supports visual navigation rather than collision-based walking.

Validation: `CityBaseTests` covers four seeds at two, four and six levels, minimum lot area, full-width alley clearance, preserved stair courts, deck containment, bridge headroom, deterministic output and transactional rejection of invalid settings.

## Stair clearance and layout variations

Raised block caps now triangulate their actual concave boundary, preserving stair
notches. The planner checks the full tread width and turn-landing area against
all platform solids and occupied lots before accepting a layout. Guard openings
follow the whole flight and landing footprint, including stairs running parallel
to a lower deck edge.

`accessGround` is a separate local excavation field below stair surfaces; the
original `ground` and `openGround` fields remain available unchanged. This prevents
uneven ground from penetrating the edge of a wide stair. The initial approach is
lengthened when the height difference requires more treads and landings.

Examples:

```
build/tests/WorldSettlementPreview --mountain-city build/renders/stair-clearance/ridge.json --tiers 3 --seed 17 --layout staggered --terrain ridge
build/tests/WorldSettlementPreview --mountain-city build/renders/stair-clearance/valley.json --tiers 3 --seed 103 --layout broad --terrain valley
```

Layouts: `tapered`, `staggered`, `broad`. Terrain profiles: `hillside`, `ridge`,
`valley`. These are deterministic variations of the terraced hillside layout,
not an unrestricted terrain solver. Incompatible stair courts are rejected rather
than meshed through their supporting walls.

Stair handrails are clipped at level connections to decks and turning landings.
The renderer splits rail spans at tread boundaries and neighboring polygon edges,
then keeps guards only beside exposed drops; end posts stay inside those spans.
CityBaseTests checks both sideways exits and retained protection on the opposite side.

### Voronoi landforms

`--layout voronoi` builds a shared nearest-site diagram before reserving streets,
stair courts and lots. Inhabited sites follow the hillside contours; seeded exterior
sites produce different angled cliff boundaries for each terrace. The usable core
expands to fit each cell, and rock outcrops are placed beyond its outer edge.
The raw cells and generator sites are exported as `landCells` and `landSites`.

```sh
./build/tests/WorldSettlementPreview --mountain-city build/voronoi-city.json --layout voronoi --terrain ridge --seed 17 --tiers 3
./build/tests/WorldSettlementPreview --mountain-base build/voronoi-base.json --layout voronoi --terrain valley --seed 41 --tiers 4
```

This mode retains contour-aligned height bands and rectangular dwelling lots inside
irregular land cells. It does not yet distribute arbitrary heights across an
unconstrained two-dimensional district graph. Stair connections and clearances use
the same validated infrastructure planner as the other layouts.

### Raised districts

`--layout districts` replaces full-width height bands with two or three adjacent
columns of separate raised blocks (three for sites at least 105 m wide). Each
column has independently offset subdivision lines, producing T-junctions. Blocks
mix rectangular footprints and clipped corners. Elevation increases along a
serpentine route through neighboring blocks, so blocks beside one another also
have different heights. `--tiers` controls rows, not the final platform count.

The planner cuts a stair court into each destination block before placing lots.
Stair approaches, landings and full-width access reserves are subtracted from
building envelopes; usable rectangular remnants can still receive buildings.
The shared relief planner validates every flight against platform solids and
occupied lots. Public lanes and square-grid dwelling lots are planned separately
from the underlying terrain. This mode is deterministic and opt-in; it currently
uses straight flights and a rectangular overall site, not arbitrary mountain
contours or a free-form Voronoi subdivision.

```sh
./build/tests/WorldSettlementPreview --mountain-city build/district-city.json --layout districts --terrain ridge --seed 17 --tiers 3
./build/tests/WorldSettlementPreview --mountain-base build/district-base.json --layout districts --terrain valley --seed 41 --tiers 4
```

`CityBaseTests` checks lateral elevation differences, offset subdivision lines,
lot containment, populated stair clearance and determinism across four seeds and
four site widths, plus native mesh clearance. Preview review uses top, front,
back, side and perspective renders of both populated and bare bases.

### Full world-city integration

The normal settlement generator now plans relief during city completion, before
native buildings are fitted. `city.complete` and `city.planned-relief` are published
together; `city.relief` forwards that validated relief to the model and runtime.
The completed skeleton is the authority for occupied lots after stair reservations.

`SettlementFabricSettings::raisedDistricts` defaults to true. It preserves the
site heightmap instead of flattening the entire settlement. Existing road grading,
sloped terrain, horizontal dwelling foundations and doorstep planning remain active.
Gentle blocks retain hills. Blocks with at least 1.2 m of local terrain variation
may receive retaining platforms if their entrance rise is 1.8–4.4 m and a full
stair court fits. Deck elevations come from the local heightmap, not random lifts.
At most twelve suitable blocks are upgraded. Candidates that cannot preserve two
lots or pass solid/tread clearance checks remain on their original terrain.

Stair reservations remove intersecting lot developments and their region contacts
atomically with the validated platform. Existing streets, outer walls, castle
programmes and agricultural allocations remain in the normal city pipeline.
This is a mixed hillside/retaining-block integration; it does not yet insert broad
new earthen ramps between arbitrary previously generated flat decks.

```sh
build/tests/WorldSettlementPreview --seed 41 --population 700 --city-region 1 --relief 500 --hills 12 --output build/renders/full-city-districts
# Historical whole-town policy, for comparison:
build/tests/WorldSettlementPreview --seed 41 --population 700 --city-region 1 --raised-districts 0 --output build/renders/full-city-flat
```

### Density on slopes

The native house fitter now evaluates nearby frontage elevation as well as street
distance. Before rooms are placed, it reserves a frontage approach sized from the
terrain rise and stair going/rise ratio, including space for the entrance plinth.
The reservation is capped at 6.5 m and preserves at least 5 m of lot depth. Existing
stair feasibility rejection remains enabled. This avoids fitting a full house
first and subsequently deleting it because its downhill entrance has no stair run.
Regression rows at 8% and 14% grades retain all four houses with level footprints.

Same-seed density check (seed 41, population target 700, hills 12 m): moving
entrance planning ahead of fitting reduces unreachable plots from 235 to 66.
Total native buildings increase from 562 to 708, and total building footprint
area from 24,402.69 to 29,634.58 m². Both runs retain twelve raised districts.
The former 700-person integration preview was not comparable to the earlier
1,600-person city; the final full-city preview restores that target.

Final full-generator validation: seed 41, population target 1,600, hills 12 m
produces 1,255 buildings, twelve raised districts and 4,755,801 triangles.
242 base/density checks and 37 completion checks pass. Exported mesh indices,
material counts, vertices and UVs validate. There are still 126 plots without a
valid entrance. Runtime terrain remains 6/8 tiles successful, with the same two
source-chain/CDT failures recorded in earlier city-natural/city-integrated runs.

### Compact stairs and bridge placement

Switchback courts no longer emit bridges across their staircase routes. Parallel
2.1 m flights now have 10 cm between them (previously 70 cm), and upper courts
are 5.6 m deep rather than 12 m. Straight district courts are 2.8 m wide, including
in the full-city upgrade, leaving 20–25 cm beside the stair flight.

Explicit bridges must connect distinct raised platforms at the same elevation.
Planning rejects crossings over staircase routes, including stairs from later
platform requests. Both bridge endpoints open the corresponding deck parapet.
Automatic bridge selection above occupied lower neighborhoods is not yet provided;
these changes remove inappropriate stair-court bridges rather than placing new ones.

Validation: 254 CityBaseTests checks and 39 WorldCityCompletionTests checks pass.
Native top, front, back, side and perspective renders are in
`build/renders/compact-stairs/views`. These isolate the base geometry without houses.

### Engineered waterways

`stoneWaterways` (enabled in the full generator's fabric settings) runs after
raised district planning. `planCityWaterway` selects one nearly straight, broad,
level street reach, including contiguous portions of longer roads. An explicit
route ID can constrain selection. A candidate requires at least 30 m length,
5.2 m total reservation and at most 0.6 m terrain variation. It retains source
road edge IDs. Unsafe candidates leave both city and relief unchanged.

The native mesh subtracts the channel footprint from ground surfaces, adds a
stone bed, retaining walls and coping, and retains 1.2 m public quays. Dwelling
frontages target the quay instead of the water centreline. Street intersections
get clear-span stone decks; bridge sides have parapets with open approaches.
Wall crossings get a masonry portal and overhead wall section. Tower/gatehouse,
lot and staircase conflicts disqualify a candidate. Independent platforms are
conservatively excluded. Runtime terrain is excavated below the structural bed.

This first version is one contained canal reach, with end sills and end crossings.
It does not yet join regional rivers, model flow, bends, tributaries, locks, or
publish a replacement pedestrian/navigation graph for the two quays. Unsuitable
cities retain their street network without a canal. Waterway geometry and source
edge ownership are exported in the full native model JSON.

```sh
build/tests/WorldSettlementPreview --canal-demo build/renders/waterways/canal.json
python3 tools/terrain/export_city_viewer.py --input build/renders/waterways/canal.json --output build/waterways-viewer
```

Waterway validation: 50 completion checks and 254 base checks pass. The native
canal fixture contains two buildings (the same count as its current ground-demo
baseline), one street bridge, two end crossings, and two wall portals. Mesh ray
checks verify water clearance below the bridge, both wall passages, quay access,
and bridge pedestrian access. Top/front/back/side/perspective screenshots are in
`build/renders/waterways/views`. The 700-population full pipeline smoke run produced
818 buildings and safely declined canal placement with the earlier candidate
sampling. Interior subreach sampling was subsequently added and regression-tested.
That full run still has the two existing runtime terrain tile failures (6/8 built).

### Raised paving and enclosed stair slots

Straight stairs now fill their 2.8 m reserved courts in both district previews
and full-city upgrades. Railing spans are suppressed where an adjacent platform
solid contains the side probe at or above tread level; exposed runs retain rails.

Street pavement offsets use connected neighboring road polygons, then clip back
to the original ownership faces. This prevents each ownership face from acquiring
its own inset curb. The default keeps the entire merged road ribbon flush (no additional street
inset); block/lot paving supplies the raised apron. Optional inset sidewalks use
the connected-neighbor algorithm. The historical `streetCarveMetres` setting now controls upward urban
slab thickness: roadway elevations stay unchanged while block, lot and sidewalk
skins rise. Shared boundaries cancel before exposed slab sides are emitted.
House finished floors include this rise. Castle/compound floor datums retain their
original 5 cm allowance so occupied floors still match authored wall walks.

Ordinary house requests now allow 128 m² footprints (90 m² in suburbs), with
three floors and five rooms on suitable lots of at least 90 m². This is capacity,
not permission to overlap neighbors or skip entrance feasibility. Gothic castle
and manor requests begin at 3,200 / 1,800 m²; compact fallback layouts retain
Gothic facades, while sufficiently large precincts use the existing monumental
Gothic layout. Mansion and manor lot facades use Gothic stonework and glazing.

Authored block programmes also apply the density policy: row/shop-house lots of
at least 75 m² request three occupied floors; those of at least 150 m² allow four.
Their room demand is five instead of three on suitable lots. Cottages and workshop
programmes keep their separate low-rise rules. Entrance landings sample the raised
lot apron, so slab thickness is not mistaken for an additional doorstep climb.
Validation includes 257 base checks, 50 completion checks and 77 Gothic castle
checks, including a realized attached keep with valid wall access.

Final same-seed density comparison (seed 41, population 700, hills 0): buildings
818 → 819, rooms 4,261 → 4,623 (+8.50%), footprint 36,155.81 → 36,181.74 m².
This is increased vertical occupancy, not a claim of narrower city blocks.
Unreachable plots remain nine, equal to baseline after correcting apron landing
heights. The two pre-existing terrain tile failures remain (6/8 built). Final
native output: `build/renders/city-joins/full-city-final/city-model-c10.json`.


### Block hill planning and modular dwellings

The full-city district upgrade now first plans a single `LandBlock` for each
existing block. Its boundary, member lots, hill fit and retaining access belong
to that record. Smooth inclined ground belongs to the **city base**; each
house still builds its own horizontal **stone foundation** above that ground.
Retained city districts remain available where the sampled incline is too steep.

`planCityHillBlocks` samples the complete block interior as well as its vertices,
fits a plane, and blends back to the original surface at the boundary. Street
edges retain their original elevations. The candidate is accepted only when its
sampled grade fits the walking budget; a long gentle rise no longer becomes a
flat platform merely because its total height difference exceeds 1.2 metres.
The terrain modifier is applied once. This is a sampled grade check, not a
continuous mathematical guarantee for arbitrary user-supplied height functions.

City dwelling capacity uses fixed 1.2 m construction cells, forming 2.4 m
two-cell bays. Frontages no longer alter the pitch to consume their remainder.
Shared street grids and covered-passage reservations use the same pitch.
Rectangular lots use their exact inscribed bounds. Optional footprint notches
must preserve 85% of requested cell capacity, or the complete available rectangle
when whole-cell fitting already leaves less space. Circulation minima are
converted from metres without a three-cell cap; smaller lots simplify the
programme instead of shrinking the physical dimensions.

Windows reserve metric jambs instead of a percentage of every cell. Corner
conflicts move an aperture before reducing its width. Under a low roof, the
window sill moves downward first (minimum 0.7 m) to preserve the model's aspect.
Vault supports are clipped against the final openings. Domestic door openings
are 1.1 m wide and up to 2.3 m high, with rectangular lintels below monumental
arch height; the frame leaves about 0.92 m width and 2.12 m headroom when the
full opening fits. Existing small/historic authored profiles can differ.

Reproduce the native hill neighborhood with:

```sh
./build/tests/WorldSettlementPreview --hill-block-demo build/renders/hill-blocks/final-neighbourhood.json
./build/tests/CellDwellingTests --city-dimensions
./build/tests/CityBaseTests
./build/tests/FacadeAtlasTests
```

The preview contains two planned inclined blocks and sixteen inhabited lots,
with separate level dwelling bases and sixteen doorway approaches. JSON exports
include each block's membership, sampled grade, elevation range and kind
(0 natural, 1 hillside, 2 retained). View screenshots from top, front, back,
side and perspective under `build/renders/hill-blocks/final-views`.

Domestic interior arches also retain metric door width and use a shallow crown.
Street signs reference the elevated doorway datum and leave 2.4 m clear below them.

Validation artifacts for this pass are in `build/renders/hill-blocks/REVIEW.md`.
The final same-seed hill-city run has 709 buildings and 3,529 rooms; the earlier
one-metre-grid run had 713 buildings and 3,858 rooms. Ground footprint coverage
changes from 29,791 to 29,356 m². This preserves nearly all buildings/coverage,
but not the previous room count. The same two runtime terrain tile failures
remain; six of eight tiles complete.
