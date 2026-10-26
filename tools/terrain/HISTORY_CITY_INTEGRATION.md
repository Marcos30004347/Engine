# History-driven city generation

The historical record is authoritative. Geometry is a disposable projection of a
single dated snapshot, not the input to the next simulation. Existing persistent
fabric remains the construction/economy implementation while this boundary is
introduced; its geometric constraints must eventually move out of weekly history.

## 1. Semantic snapshot (implementation first)
Export living residents, real domestic households, ancestry, occupations, workplaces,
completed building programmes and completed defensive eras. Use typed semantic keys
(settlement + building/household/workplace), never mesh indices, in biographies.
Keep previous residence/migration events in the history ledger. Homeless households
are explicit unmet demand, not secretly housed by the renderer.
Gate: residents counted once, dead ancestors retained but excluded from land demand,
closed workplaces excluded, no future projects built, stable output on repeated reads.

## 2. Fresh spatial realization
Calculate residential and employment area from the snapshot. Plan connected terrain
cells, routes, block access and lots before fitting dwelling programmes. Assign old
institutions/core households first and place later growth in outer districts. Return
semantic-key -> lot -> actual building mappings, plus capacity/rejection diagnostics.
Never reuse previous blueprints as a prerequisite. Fixed dwelling module sizes remain.
Gate: every accepted requirement has one lot, no occupied lot overlap, all doors
reachable, explicit deficits on failed fitting, identical snapshot/seed gives same plan.

## 3. Historical defences and civic districts
After blocks and roads exist, reconstruct only funded/completed wall eras around
historical core membership, with gates at every crossing. New suburbs may remain
outside. Wall age alone must not manufacture an enclosure. Commercial streets follow
real workplaces; a royal seat needs a completed manor/keep/castle programme.
Gate: route-wall crossings have openings, full circuits close, partial works remain
partial, younger suburbs are not automatically enclosed by an older wall.

## 4. Court, military and daily life
Retain economic occupation separately from duty: guards can serve as soldiers/watch,
administrators as stewards, domestic staff as household servants; future explicit
cook, stablehand, armourer and captain jobs need funded slots and skill/wage policies.
Optional schedules are queries, not new people simulations: rest/home, work/apprentice,
meal/home. Bind to a generated building and a valid room only after geometry exists.
Missing housing, workplaces or rooms must remain unresolved; do not invent locations.
Gate: one activity per citizen/time, minors only apprentice when ledger assigned,
no dead/foreign actors, every resolved building/room reference exists. Movement and
travel-time simulation are a later phase, not implied by schedule lookup.

## 5. Century-scale validation and renders
One deterministic timeline, seed 41, snapshots at elapsed years 20, 100 and 1000.
Use the graph matcher for production social behavior. The existing CitySocietyDriver
is a separately labelled scale/balancing fixture, NOT proof of graph-rule equivalence.
Disable weekly snapshot retention for long runs; preserve events and dated milestones.
Capture top/front/back/side/perspective of actual native meshes, and report population,
households, jobs, homes, wall stage, unmet demand, runtime and invariant failures.
Gate: shorter same-seed prefix equals corresponding long-run snapshot; no forced
monotonic growth; finite mesh data/valid indices; screenshot review; all relevant
history/economy/defence/regeneration tests pass. A stagnant or failed millennium must
be reported honestly, never replaced by an age-scaled decorative city.

## 6. Full world generator integration
Pass semantic demand into world-cell selection/master planning (instead of synthetic
population), reserve court/castle and farm support, grow connected cells until capacity
is satisfied, then solve historical wall envelopes. Keep this opt-in until shared
landform polygon and runtime terrain failures are resolved. Test flat, hillside,
ridge, river and multi-settlement kingdom seeds, including decline and extinction.

## Current implementation and reproduction

Implemented: `WorldCityHistoricalBrief.hpp` projects validated household and employment
records without mutating the ledgers. It keeps extended families intact (the old
8-person synthetic limit is removed, while the 100,000-person bound remains).
The director and world-site selector accept explicit historical households. The
world CLI's `--history-snapshot history.json` consumes `householdRecords` and completed
`buildings`, including manor/keep eligibility. It does not yet impose every historical
workplace or wall era on the full world layout.

`CityEvolutionPreview OUTPUT 1000 41` runs the existing scale fixture once, without
retaining every weekly snapshot, and writes years 20/100/1000 for settlements 100 and
200. It retains the fixture's 2,000 living-person safety cap. The focal settlement
100 receives native meshes. Base/parcels are regenerated
from founding parameters; historical parcel IDs select active buildings. This still
uses the existing bounded base and historical wall coordinates, so it is not the
unrestricted cell-growth implementation in phases 2/3/6. Mesh bindings report failures
rather than inventing buildings. `--no-mesh`, `--no-activities` and
`--activity-minute 0..1439` provide inspection options. Room bindings remain null.

The preview compiles the same simulation sources with optimization even when the
engine is a Debug build. Derived completed-project and completed-site indexes avoid
repeated archive scans; they must preserve exact state hashes. Production graph-rule replay is
validated separately with `CityHistoryIntegration`, and `CityHistoryPreview ... 20 41
--renders DIR` renders the actual story scenario. Do not call the scale fixture a
Graph Pattern Matching millennium.

Tools:
- `audit_city_evolution.py OUTPUT` checks population reconciliation, ancestry dates,
  current identities, native indices/coordinates/UVs and geometry bindings.
- `export_city_viewer.py --input .../city-model.json --output build/VIEWER`, followed
  by `city_viewer/render_views.cjs`, captures native top/front/back/side/perspective.
- `export_city_history_review.py --history-root OUTPUT --views-root VIEWS
  --graph-root GRAPH_OUTPUT --output build/history-evolution-review` creates a local
  comparison and family inspector. Large semantic IDs stay exact in the browser.

The first regression gate comprises CityHistoricalBriefTests, WorldSettlementDirectorTests,
CityHistoryTests, CityHouseholdTests, CityEconomyTests, CityRealmTests, CityDefenceTests,
CityChronicleTests and the actual graph integration test. Year-20 independent runs and
Debug/optimized year-20/100 snapshots must agree on people, families, buildings and moves.

The full-city completion stage now also consumes completed construction records via
`allocateHistoricalCityLots`: deterministic age/ID order, central lots first, ordinary
buildings limited to the supplied record count, historical floors and household room
demand preserved, and explicit zero-lot bindings when space runs out. Access and stair
reservations precede allocation. Compounds with no completed historical request are
omitted. Native model exports resolve these logical building IDs to actual generated
lot/building IDs, retaining null on failed fitting.

Still gated: re-sizing/coalescing the planned urban envelope to avoid surplus empty
blocks, matching every workplace slot and measured bedroom capacity, multiple same-use
institutions, room-level daily binding, and reconstruction of wall circuits from
historical membership after a fresh layout. The current history construction ledger
still caps growth at founding parcel capacity; this must become a semantic land budget
before claiming unrestricted village-to-capital growth. Full-city terrain tile failures
are independent failures and must not be reported as successful world generation.

## Next implementation gates, in dependency order

1. Replace the founding parcel ceiling with a semantic land account. Historical
   buildings retain IDs, construction/replacement dates, use, occupants, capacity,
   district membership and access requirements. A household waiting for housing
   remains a demand record. Construction authorization consumes funds, materials,
   workforce and land capacity; it must not call the dwelling mesher. Test growth
   beyond the original founding area, contraction, abandonment and reoccupation.
2. Plan an entire dated city from that account. Seed the oldest civic cluster on
   suitable connected world cells. Expand over adjacent cells using slope, water,
   road access and land cost; stop only when measured usable modular area covers
   the requested programme. Reserve through-roads, stair landings and river
   crossings before subdividing blocks. Keep connected unoccupied cells as future
   expansion, not paved empty blocks. Test five terrain families and population
   decline, with exact capacity and connectivity diagnostics.
3. Fit buildings with bounded backtracking. A failed envelope or entrance releases
   its tentative lot and tries the next compatible lot; after local exhaustion,
   merge/split blocks or request another connected cell. Preserve minimum module
   dimensions. No identifier is marked realized until its actual door and native
   mesh exist. Gate: every fulfilled request has a unique native binding; all
   remaining deficits have a specific reason and are visible in the viewer.
4. Reconstruct each historical defence era after final block assignment. Store
   enclosed semantic district/building membership and funded component state in
   history. Derive the new envelope from those members, then intersect it with
   streets, stairs and rivers to place gates/culverts. Never reuse former wall
   coordinates. Incomplete historical circuits remain incomplete; later suburbs
   stay outside unless a later completed enclosure includes them. Test shuffled
   geometry seeds against identical history, with no blocked crossings.
5. Match interior capacity and workplaces to realized rooms. Keep bedroom, hall,
   kitchen, workshop, guardroom and stable requirements distinct from occupation.
   Resolve daily activities through semantic building IDs into room IDs only
   after checking use and capacity. Add travel only after walkable connections
   exist. Test missing homes/jobs/rooms and interrupted access explicitly.
6. Run the authored graph matcher on the same long-run harness, with bounded
   checkpoint retention and profiling. Keep the scale fixture as a stress test;
   do not substitute it for production behavior. Then repeat 20/100/1000 native
   renders and invariant audits across multiple seeds, including a declining
   kingdom. Acceptance does not require monotonic population or perpetual growth.

## Full-world millennium review — 2026-09-27

Implemented historical population/programme projection through the world → site →
territory → fabric → completion → native mesh pipeline. Landmark district requests
now reserve larger historical precincts, retain preferred summit anchors, and may
extend a castle into connected lower defensible terraces. Manor pieces are joined
before fitting. Compact Gothic manors use their own minimum dimensions. Elevated
castle wall access projects onto usable facade spans, adds turning landings where
needed, and meshes continuous decks with side rails. The real millennium castle
precinct is retained as a regression fixture.

Seed 41 completed 1,000 simulated years in the demographic scale fixture. This is
not the production graph matcher; that has a separate 20-year replay/reload check.
The focal snapshot contains 756 living residents, 216 households and 356 completed
semantic building records. Full-world year 100/1,000 meshes contain 269/358 physical
structures, including compound wings and defensive towers. Ordinary historical
bindings resolve 183/249 and 274/354 respectively. Institutions need separate room
and wing binding. A physical structure count is not a fulfilled historical demand
count.

The current Gothic castle is realized. The manor still falls back because its
actual district rectangle is too narrow despite adequate gross area. Next gate:
reserve a feasible two-dimensional landmark envelope during territory allocation,
not merely an area target. Preserve module dimensions; do not stretch the building.
Year 20 remains blocked by castle district eligibility/connectivity. Do not replace
its missing world render with a standalone fixture image.

Evidence: `build/renders/history-evolution/world/audit.json`, dated `views/`
directories, `build/history-final-regressions.log` (11/11), and
`build/history-shelf-fallback-tests.log`. Binary arrays are finite, indices valid,
and resolved ordinary bindings unique. Screenshots include five city views and
three views of each landmark. The viewer comparison explicitly selects years 100
and 1,000 and reports omitted/unfinished work.

Remaining release gates include native lot fitting deficits, terrain publishing
(6/8 tiles), historical wall reconstruction, unbounded semantic land expansion,
room binding, and a production graph-driven millennium. A disk-full interruption
left part of the secondary settlement's old residence-event archive incomplete;
its snapshot records that warning. The focal settlement archive is complete.

## Lot support and low-poly terrain follow-up

The city mesher now evaluates support after footprint warping and samples the
whole lot, not only the original house corners. Successful lots receive a flat
stone cap; entrance flights are cut out of that cap before emission. Previously
`wayToDoor` was planned and counted but never emitted. Thin nondegenerate ground
triangles are retained, and historical unoccupied blocks no longer get automatic
stone paving or raised aprons. Historical allocation fills blocks in order rather
than distributing each house independently by radius. Raised-district candidates
are filtered against a preliminary historical occupancy plan, before final access
reservations and final allocation.

Broad eight-metre terrain facets now precede road grading and stair reservations.
They interpolate the same sampled height at every shared grid vertex, so hills
are simple continuous ramps while dwelling pads remain horizontal. This is a
terrain-shape simplification, not yet a polygon-budget optimization. A zero facet
pitch preserves the original source. Platform headroom above the highest natural
point is reduced from 0.65m to 0.15m; large drops still require retained structures.
The final native render and diagnostics must be checked after this change; this
entry does not declare all access and fitting failures resolved.

### Coarse terrain and historical land budgeting

Terrain tessellation now tests height deviation along shared edges rather than
subdividing every long edge. Flat areas and straight ramps retain large faces;
interior crest/valley probes refine where a ramp transition is needed. Kerbs use
the same edge criterion. The native 100×100m planar-ramp regression stays below
250 triangles. This is in addition to the eight-metre source terrain facets.

`historicalLandBudget` projects completed records into modular footprint areas,
with separate use counts and circulation allowance. The world site adapter now
uses these demands to size residential and commercial districts before selecting
urban cells. A strict inscribed-rectangle admission experiment was rejected: it reduced realized
buildings from 264 to 156 by excluding viable irregular footprints. Native modular
fitting remains authoritative; a future retry pass must test complete footprints.

This is a first spatial budget, not a completed kingdom allocator: production
history still needs independent snapshots for the other kingdom settlements,
multiple same-use institutions need explicit compound bindings, farm production
land needs its own semantic area account, and failed native placements need a
bounded retry/expansion pass. History-era walls and full walking connectivity
remain release gates. Do not interpret a successful terrain audit as proof that
every requested building or every kingdom pathway is realized.

### Normal streets as the default

Raised district platforms are now opt-in in the full-world fabric settings and
preview CLI. Normal generation keeps graded streets on coarse terrain without
raised-block stair connectors. Full-lot raised aprons are also disabled by default;
level foundations remain beneath dwelling footprints to prevent terrain clipping.
The optional raised mode is retained for explicit experiments.

### Voronoi polygon terrain

With world landform cells present, the site surface is now constructed from their
shared corners after site-envelope selection and before road and lot construction. A cell gets a centre vertex only
when its height differs from its corner planes by more than 0.75 m. This replaces
the eight-metre sampling grid for this path. The native city model intersects its
road/sidewalk/lot ownership polygons with these terrain faces, nodes the resulting
boundaries, and runs CDT on the pieces. Recursive heightmap refinement is disabled
for that cover. Houses sample the same coarse facet field. Cell-free legacy/test
callers retain the earlier adaptive path; this does not replace world runtime tile
meshing. Explicit raised-platform generation remains a separate optional mode.

Tests gate planar face counts, crest preservation, shared-cell height continuity,
and a road crossing multiple terrain facets through the native model path. Render
validation uses the same millennium history fixture as the normal-streets review.

### Occupancy-driven street cover

After historical lot allocation, frontage routes serving developed lots, compound
wings, gates and waterways become terminals. A shortest-path forest over the
existing route graph keeps their connecting chains; unused branches, cycles and
empty disconnected components are removed from the city construction routes.
Historical lots without allocation no longer emit separate lot paving, and empty
blocks no longer receive public paving or alley meshes. Agricultural fields remain
land uses rather than being interpreted as empty residential lots.

The pruning gate checks preservation of inhabited frontage connectivity, removal
of an empty spur and disconnected island, and repeatability. This pass follows
planned historical occupancy; native building fitting can still reject an allocated
lot, so realized-only cleanup and retry/expansion remain separate work.

Terrain style is authored before CDT: `--terrain-crown 0` selects sparse angular
peaks; the default `0.28` adds polygon crown/basin contours and planar shoulders;
values up to `0.6` broaden the centres. Flat cells receive no extra rings. The
shared outer cell vertices remain unchanged, so changing a crown's width cannot
open a crack between neighbours. Tests cover broad level crowns and angular mode.

### Density repair and texture-only streets

Architectural lot ownership is now captured before terrain facet cuts. The fitter
previously selected the largest *terrain fragment* of a lot, rejecting houses that
would fit its complete legal polygon. Terrain faces must never define buildable
lot capacity. A regression test builds a house across facet cuts.

Non-farm historical homes use attached-house planning; detached setbacks are
0.15 m. Historical block remainder ground stays grass instead of blanket paving.
In the Voronoi terrain path, texture-street mode unions the ground cover before
facet clipping and emits no separate road/curb mesh. Roads still reserve legal
building space and remain in the access graph. Native material influence polygons
are exported as `groundPaint`; the viewer exporter bakes them into a 4096-square
three-channel terrain mask and samples it in world space. Other native renderers
must consume this paint product to display the same street textures.

Terrain peak/basin prominence is increased 1.6× relative to its corner plane,
while shared boundary heights remain fixed. The model does not apply this boost
again. Flat cells remain planar. The reviewed crown fraction is 0.4.

### Merged street paint

Street ribbons, alleys, public paving and frontage strips now form one final
circulation paint pass. A 1.5 m morphological closing fills narrow cosmetic gaps;
new paint is clipped against complete allocated lots, while original circulation
coverage is preserved. Material regions are unioned, and the circulation layer is
painted after grass and yards so later masks cannot reopen seams. Separate streets
beyond the gap threshold remain separate. Native regression tests cover both a
close parallel pair and a protected lot between two routes. This changes paint,
not road connectivity or building placement.

### Elevation contours before CDT

The city model derives explicit constant-height lines at 2 m intervals from the
coarse Voronoi landform planes before constructing render surfaces. The resulting
height bands and ridge boundaries are the terrain polygon constraints consumed by
CDT. Intersections use canonical endpoint ordering; coincident contour segments
are deduplicated. Flat planes remain unsliced. Streets remain a texture mask.
`city-model-c10.json` publishes `elevationLines` for inspection. Tests verify exact
slope-area coverage, constant contour height and no redundant flat-plane contours.
Road paint merging uses 64 m sections with overlapping halos to bound boolean
work; a regression compares its coverage across a section boundary with the
unsplit operation.

### Connected level contours (replaces facet slicing)

The previous contour stage sliced pre-existing terrain triangles. It has been
replaced in the full-city path: Voronoi cells are classified by elevation, their
regions are unioned and rounded, and higher regions are inset/intersected inside
lower regions to produce non-touching nested contour loops. Every point on a loop
receives its single exact absolute elevation. Only then does CDT triangulate all
loops together; triangle heights come from contour vertices, never a heightmap
resampling. The site publishes these same loops to the city model, so they are not
regenerated from its triangles. A regression verifies complete loops have one
height and that the CDT sampler preserves that height at every contour vertex.

### Organic contour detail

Contour outlines now receive two corner-cutting passes before level assignment,
followed by polygon regularization. Rounding tolerance is 0.2 m instead of 1 m;
post-rounding simplification retains detail down to 0.05 m. Higher contours are
still clipped inside inset lower contours, so smoothing cannot make levels cross.
Only XY shape changes; every final ring still receives one exact elevation before
CDT. The diagnostic contour map now exports at 2800×2200. Tests cover valid curved
polygon topology and unchanged contour heights after triangulation.

### Preserve street grading over contour terrain

The contour plan is now sampled from the natural fabric terrain, and the city
mesh retains the relief planner's street/access field afterward. Texture-only
streets keep their polygon edges as triangulation constraints; the texture no
longer implies deleting the geometry constraints that hold a road level across
its width. Only non-planar grading banks are adaptively refined (2 m minimum
edge length, existing 2 cm height-error criterion); planar terrain stays sparse.
Road ribbons take precedence over neighboring earthwork banks, with tread-only
blending at junctions and continuous transitions outside the road edge.

CityBaseTests includes two neighboring roads on a 40% longitudinal / 30%
cross-slope hillside. It verifies an 8% profile limit, level cross-sections,
continuous cut/fill edges, and the actual city mesh's heights after contour CDT.
The production grade limit remains configurable through maximumStreetGrade
(default 12%). Roads connecting different heights are ramps; they are not all
forced onto a single global elevation. This fixes grading preservation, not
horizontal route optimization or automatic switchback construction.

Validation: CityBaseTests and WorldCityCompletionTests pass. Seed 41 / year
1000 full-city export produced 361 structures, resolving 278/354 ordinary
historical requests (previously 287); some entrances no longer fit the graded
terrain. The support audit found no sampled ground-centroid penetration.
Five of eight runtime terrain jobs build; the same two source-chain failures
and one frozen-boundary station error remain. Views are under
`build/renders/history-evolution/graded-streets-year-1000/views`.

### Actual street excavation

Street relief now defaults to cut-only road beds (`excavateStreets=true`,
`streetExcavationMetres=0.3`). The grade-constrained profile is capped against
terrain sampled at no more than 1 m spacing along and across the carriageway,
including interior valleys between junctions. The caps include the 35 mm mesh
skin offset. A second one-sided graph solve restores shared junction heights
and the grade limit while only lowering the excavation caps. Banks terminate
inside their spatial query reach so the field returns continuously to terrain.
The normal full-city pipeline replans relief against retained route edges after
occupancy pruning; unused road branches no longer keep excavation authority.

Regression checks cover minimum excavation across the road width, an interior
valley, horizontal sections, bank continuity, and actual rendered triangle
interiors below the original terrain. Cut-only grading can require deep cuts
on steep fixed alignments; route relocation/switchback design remains separate.

Full-city excavation validation (seed 41, year 1000): 329 physical structures,
246/354 ordinary historical placements; 32 fewer placements than the previous
graded run. Deep cuts on fixed alignments expose additional entrance-access
limitations. No sampled ground-centroid penetration; finite vertices and valid
indices. The same three runtime tile errors remain (two source-chain failures,
one frozen-boundary station error). Native mesh renders are in
`build/renders/history-evolution/excavated-streets-year-1000/views`.

### City-base builder: local street profiles

The city-base builder plan is `docs/plans/city-base-builder.md`. Its first
implemented foundation is `WorldCityStreetProfiles.hpp`: a shared-junction
station graph with excavation ceilings across each carriageway. It solves the
highest profile under those ceilings that respects the longitudinal grade.
Interior rises and dips no longer force a complete road to the same low chord.
Exactly collinear solved stations are removed before mesh consumers see them.
The full city relief planner uses this blueprint for ordinary excavated streets;
explicit stepped-street mode keeps its separate stair planner.

Regression gates include a local valley which preserves distant flat junctions,
shared junction identity, edge-order determinism, sampled excavation, slope
bounds, sparse straight runs and substantially less cut than whole-road lowering.
This is the road-profile foundation, not completion of the semantic public-space,
land-patch and plot-contract phases described in the plan.


## Occupations, rooms, farms and the kingdom (2026-10-01)

Five additions close the gap between the ledgers and the geometry.

1. **A trade is a room.** A building's workplace occupation crosses as a
   programme word (`cityTradeOf`: smithy, carpentry, masonry, farmstead,
   guardhouse, hall) on `HistoricalBuildingBrief::trade`,
   `SettlementHistoricalBuilding::trade` and `CityLotDevelopment::trade`,
   and the fabric ledger keeps `CityBuilding::occupation` so a regenerated
   lot keeps its trade. The cell dwelling programme reads the word as a tag
   and requires the room on the ground floor: a forge, an oven room, a
   workshop, a byre with a barn and granary, a guardroom, a counting house
   (`RoomKind::Forge`, `Bakehouse`, `Granary` are new). Planned block roles
   (bakehouse, smithy, workshop, farmstead, civic-hall) map to the same words.
2. **A day is bound to a room.** `CityModelBuilding::roomList` carries every
   realised room with its use and kind; `bindActivityRoom` resolves work,
   sleep, meals and home to a room by kind and otherwise leaves it null. The
   timeline preview meshes first and then exports activities with rooms.
3. **Farmers on holdings.** `CityTerritorySettings::farmsteadsWanted` makes
   as many farmsteads as the record's farms; every holding publishes its
   farmhouse site as a lot with the `farmstead` programme inside its yard, the
   city builds a real farmhouse there, farm records bind to those lots first,
   and the farmhouse box massing is no longer emitted (the barn and cottage
   still are).
4. **The kingdom as one history.** `runKingdomHistory` founds every site of a
   `KingdomPlan` in one week on one chronicle (`CityScaleSettings::settlements`,
   `routes`, `seat`), runs it, and projects a dated brief, households,
   buildings and enclosures per settlement. `KingdomHistoryTests` runs five
   settlements for ten years in about thirteen seconds.
5. **On by default.** `WorldSettlementPreview` runs the kingdom history for
   thirty years before selecting the focal site (sixty cost 664 s on the
   seed-41 plan of 24 settlements, 5,187 people; thirty costs 201 s, 4,251
   people), composes the focal city from
   its own settlement's records (the one at `--cell`, else the capital), gives
   every `--kingdom N` settlement its own records, and writes
   `history/settlement-<id>.json` per settlement. `--kingdom-history 0`
   restores the synthetic population; `--history-snapshot` stays authoritative.

A district the historical budget marks required and the ground cannot carry
no longer refuses the settlement: `generateCityTerritory` replans with every
district optional and records the shortfall per unplaced district in
`CityTerritory::rejections` (the thirty-year capital, 92 homes and no
institutions yet, was refused outright before this).

Still open: the social source is the CitySocietyDriver fixture, not the graph
matcher; room-level furniture for the trade rooms; farmsteads depend on the
territory finding a broad, level yard, which several seeds never do.
