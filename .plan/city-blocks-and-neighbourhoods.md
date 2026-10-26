# City blocks and neighbourhoods: porting Watabou to the hierarchical generator

Written 2026-09-14 from the **source** of Watabou's Medieval Fantasy City
Generator — [TownGeneratorOS](https://github.com/watabou/TownGeneratorOS), the
open-source predecessor of the itch build — read directly rather than inferred
from screenshots. Every constant and formula below is quoted from that code.

Companion plan: [`buildings-and-dwellings.md`](buildings-and-dwellings.md).

---

## 1. What Watabou actually does

### 1.1 The pipeline

```
  Voronoi over relaxed points      -> patches
  patches inside the wall          -> "inner", the city
  plaza patch                      -> Market ward
  patches touching a gate          -> GateWard  (p = 0.5 walled, 0.2 unwalled)
  remaining inner patches          -> a ward class, chosen by rateLocation
  patches outside, compact, p=0.2  -> Farm; otherwise countryside
  every ward                       -> createGeometry()
```

The decisive structural point: **a ward is a patch, and the patch's own edges
are the streets.** Watabou never "draws streets" inside a block. Streets are
the shared edges of the Voronoi cells, and a block is the patch inset from
them. Our `GridStreets` / `RadialStreets` impose a road network on a region,
which is a different — and less coherent — idea.

### 1.2 `getCityBlock()` — the block is the patch, inset per edge

```haxe
MAIN_STREET = 2.0;  REGULAR_STREET = 1.0;  ALLEY = 0.6;   // Watabou units
```

Per edge of the patch:

| edge borders | inset |
|---|---|
| the wall | `MAIN_STREET / 2` |
| the plaza, or an artery | `MAIN_STREET / 2` |
| inside the walls, otherwise | `REGULAR_STREET / 2` |
| outside the walls | `ALLEY / 2` |

```haxe
return patch.shape.isConvex() ? patch.shape.shrink( insetDist )
                              : patch.shape.buffer( insetDist );
```

Note the branch: a **per-edge** inset on a convex shape, a general buffer
otherwise. Our `geom2::offset` already does both cases with one code path, so
we need the per-edge variant and nothing else.

At 1 unit = 2.5 m (the repo's existing convention) that is a 5 m main street,
2.5 m regular street and 1.5 m alley.

### 1.3 `createAlleys()` — the lot algorithm

This is the part worth porting exactly.

```haxe
static function createAlleys( p, minSq, gridChaos, sizeChaos, emptyProb=0.04, split=true ) {
  // 1. longest edge, keep its START vertex
  var v = argmax over edges of length;

  // 2. where along that edge to cut
  var spread = 0.8 * gridChaos;
  var ratio  = (1 - spread) / 2 + Random.float() * spread;

  // 3. how far off perpendicular - ZERO for small parcels
  var angleSpread = Math.PI / 6 * gridChaos * (p.square < minSq * 4 ? 0.0 : 1);
  var b = (Random.float() - 0.5) * angleSpread;

  // 4. cut, with an alley gap only while splitting
  var halves = Cutter.bisect( p, v, ratio, b, split ? ALLEY : 0.0 );

  for (half in halves)
    if (half.square < minSq * Math.pow( 2, 4 * sizeChaos * (Random.float() - 0.5) )) {
      if (!Random.bool( emptyProb )) buildings.push( half );      // a lot
    } else {
      createAlleys( half, ..., split = half.square > minSq / (Random.float() * Random.float()) );
    }
}
```

Five rules, each doing real work:

1. **Cut the longest edge, perpendicular to it.** Lots therefore always get
   their frontage on the short side, which is what a lot is.
2. **Ratio is centred on 0.5** and spreads by `0.8 * gridChaos`. At
   `gridChaos = 0` every cut halves exactly; at 1 the cut can land anywhere in
   the middle 80 %.
3. **Angle jitter switches off below `minSq * 4`.** Big blocks get skewed cuts
   and look organic; the last two levels are perpendicular, so the final lots
   are rectangles. *This is the single rule that keeps an organic town from
   dissolving into triangles, and the one our generator most obviously lacks.*
4. **The stop size is log-uniform**: `minSq * 2^(4 * sizeChaos * (r - 0.5))`.
   At `sizeChaos = 0.7` a lot can be anywhere from `minSq/6.9` to `minSq*6.9`.
   A block therefore holds a *mix* of sizes, not one size with jitter.
5. **The alley gap is conditional**: `split` for the next level is
   `half.square > minSq / (r1 * r2)`. Since `r1*r2` is usually small, the
   threshold is usually large, so **alleys appear between big groups and
   disappear between neighbours**. Buildings end up terraced, with alleys
   between terraces — which is exactly what a medieval town looks like.

`Cutter.bisect` itself:

```haxe
p1 = interpolate( vertex, next, ratio );
d  = next - vertex;
v' = rotate( d, angle );
p2 = p1 + perpendicular( v' );     // (p1.x - v'.y, p1.y + v'.x)
return poly.cut( p1, p2, gap );
```

`Polygon.cut(p1, p2, gap)` finds the **two** edges the infinite line crosses,
splits there, and pushes the two halves apart by `gap` along the cut normal.

### 1.4 Per-ward parameters — the whole table

`CommonWard(minSq, gridChaos, sizeChaos, emptyProb)`:

| Ward | minSq | gridChaos | sizeChaos | emptyProb |
|---|---|---|---|---|
| Craftsmen | `10 + 80·r·r` (small→large) | `0.5 + 0.2r` | 0.6 | 0.04 |
| Merchant | `50 + 60·r·r` (medium→large) | `0.5 + 0.3r` | 0.7 | 0.15 |
| Patriciate | `80 + 30·r·r` (large) | `0.5 + 0.3r` | 0.8 | 0.2 |
| Administration | `80 + 30·r·r` (large) | `0.1 + 0.3r` (**regular**) | 0.3 | 0.04 |
| Gate | `10 + 50·r·r` | `0.5 + 0.3r` | 0.7 | 0.04 |
| Slum | `10 + 30·r·r` (small) | `0.6 + 0.4r` (**chaotic**) | 0.8 | 0.03 |

`r·r` rather than `r` biases every size toward the small end. Note how little
separates a slum from a patriciate ward: a smaller `minSq`, more chaos, and a
lower empty probability. **The character of a quarter is four numbers.**

### 1.5 The other ward geometries

- **Park** — `Cutter.radial` when `compactness >= 0.7`, else `semiRadial`,
  with an `ALLEY` gap. `compactness = 4πA / P²`.
- **Cathedral** — 40 % `Cutter.ring(block, 2 + 4r)`, else
  `createOrthoBuilding(block, 50, 0.8)`.
- **Castle** — `patch.shrinkEq(MAIN_STREET * 2)` then
  `createOrthoBuilding(block, sqrt(block.square) * 4, 0.6)`.
- **Market** — a plaza with a fountain or statue (60 % statue), offset toward
  the longest edge.
- **Farm** — a 4×4 rect, rotated randomly, placed at
  `interpolate(randomVertex, centroid, 0.3 + 0.4r)`, then
  `createOrthoBuilding(housing, 8, 0.5)`.

`Cutter.ring(poly, thickness)` peels the block: offset each edge inward by
`thickness`, **sort the slices short-edge first**, and cut them off one at a
time. Short sides first is what stops a long thin remainder.

`createOrthoBuilding(poly, minBlockSq, fill)` fixes **two axes** from the
longest edge (`c1`, `c1.rotate90()`), and at each step cuts with whichever axis
is *more perpendicular* to the current longest edge, at ratio `0.4 + 0.2r`,
keeping each finished piece with probability `fill`. No fans, ever.

### 1.6 `filterOutskirts()` — thinning a ward outside the wall

Each patch edge gets a population weight: 1 on an artery, 1 beside an enclosed
city patch, 0.4 beside an open one. Each vertex gets a density: 1 at a gate,
`2r` if every patch at that vertex is in the city, else 0. A building survives
when `Random.fuzzy(1) > minDist / interpolatedDensity`. The effect: buildings
crowd the roads and the gates and thin out into the fields.

### 1.7 The Village Generator — a different shape of settlement

[Village Generator](https://watabou.itch.io/village-generator) is not the town
algorithm at a smaller size. The author states it uses the **Parish-Müller**
road approach — the same paper our `generator::RoadGrowth` already ports —
and the rules that follow from it are different in kind:

- **Roads first, houses second.** There are no patches and no wards. A road
  network is grown, then buildings are hung off it.
- **Houses sit perpendicular to their road**, with **secondary driveways**
  connecting individual structures back to the primary road. A village house
  faces the road it is on; a town house faces the block it is in.
- **Farmland is a tag**, not a consequence: `farmland` / `uncultivated`, and
  `grove` concentrates trees at the centre rather than scattering them.
- Density is a tag too: `sparse`, `dense`, `district`.
- The author's own scale note, worth quoting because our population model has
  the same problem: *"Settlements measuring 1500x1500 have populations over 2K
  which is too many for a village."*

This maps onto what we already have. `generator::SettlementLayout` is the
roads-first path (`--roads 1`), `generator::RoadGrowth` is the Parish-Müller
L-system, and `WorldRuralPatches` allocates the farmland. What is missing is
the **perpendicular house rule and the driveway**: our village lots come off
the road graph's faces rather than being placed against the road with their
own access.

Watabou's own farm allocation in the town generator (`Farm.hx`) is worth
copying directly for farmsteads — it is three lines:

```haxe
var housing = Polygon.rect( 4, 4 );
var pos = GeomUtils.interpolate( patch.shape.random(), patch.shape.centroid, 0.3 + Random.float() * 0.4 );
housing.rotate( Random.float() * Math.PI );
housing.offset( pos );
geometry = Ward.createOrthoBuilding( housing, 8, 0.5 );
```

A 4×4-unit (10 m) square, randomly rotated, placed between a random vertex of
the field and its centroid — so a farmstead sits *off-centre in its field,
toward an edge*, which is where a farmstead sits. A farm patch is chosen as
`Random.bool(0.2) && compactness >= 0.7`: only compact fields, and only one in
five.

---

## 2. What we do now, and where it differs

| | Watabou | Ours |
|---|---|---|
| Streets | shared edges of Voronoi patches | `GridStreets` / `RadialStreets` impose a network |
| Block | the patch, inset per edge | a face of the imposed road network |
| Lots | recursive longest-edge bisection | `StripLots`: a regular grid at one spacing |
| Lot size | log-uniform around `minSq` | one target with ±jitter |
| Angle | skewed while big, perpendicular when small | always axis-aligned |
| Alleys | gap that appears between big groups only | none |
| Empty lots | `emptyProb` per finished lot | none |
| Ward character | four numbers per ward class | one settings struct |
| Outskirts | density-weighted thinning | none |

Our production `generator::LotPlanner` **already ports `createAlleys`
faithfully** — longest edge, ratio, angle spread, size chaos, empty
probability, alley gap. The gap is that it is not reachable from the
hierarchical generator, and the hierarchy's own `StripLots` is a much cruder
thing that the gallery render exposed as producing slivers.

So this is mostly a *wiring* job, not an algorithm job.

---

## 3. Plan

Ordered so each step is independently visible in the gallery.

| Step | Work | Gate |
|---|---|---|
| **B1** `BisectionLots` | **DONE 2026-09-14.** `createAlleys` ported as an `EdgeGenerator`: recursive longest-edge bisection emitting each cut chord as an `Edge` — zero width for a lot line, banded with `ALLEY` when `split`. All five rules, including the angle cutoff below `minSq * 4` and the `minSq / (r*r)` alley condition. The five ward parameter sets are presets. | Gallery panels `ward-craftsmen`, `ward-patriciate`, `ward-administration`, `ward-slum`: the same block, four parameter sets, four visibly different quarters |
| **B2** Per-edge block inset | `getCityBlock`: inset each region edge by half the width of whatever it borders — wall, artery, plaza, interior, exterior. Needs an offset that takes a distance per edge. | A block pulls back further from an artery than from an alley |
| **B3** Ward classes | The six parameter sets above as named bindings on `Role::Lots` — `lots.craftsmen`, `lots.merchant`, `lots.patriciate`, `lots.administration`, `lots.gate`, `lots.slum` — selected per region by a policy rule on the region's use. | One settlement shows visibly different quarters |
| **B4** `Cutter` family | `radial`, `semiRadial` (parks), `ring` (cathedral precincts, and our existing ring-lot idea done Watabou's way: short sides first). | Park and temple panels |
| **B5** Patches as blocks | The bigger structural change: derive blocks from a relaxed-Voronoi patch layer whose **shared edges are the streets**, instead of imposing a road grid. This is what makes a Watabou town look like a town. | A settlement whose street network is the patch graph |
| **B6** `filterOutskirts` | Density-weighted thinning for regions outside the wall. | Buildings crowd the gate roads and thin into the fields |
| **V1** Village house rule | Roads-first settlements place each house **perpendicular to its road** with a driveway back to it, rather than subdividing road-graph faces. | A village reads as houses along a lane, not as a small town |
| **V2** Farmsteads | Port `Farm.hx` exactly: a 10 m square, randomly rotated, placed between a random field vertex and the centroid, then `createOrthoBuilding(_, 8, 0.5)`. Only compact fields (`compactness >= 0.7`), one in five. | Farmsteads sit off-centre toward a field edge |

**Not in scope here**: what goes *inside* a lot. That is the buildings plan.

### What B1 cost, for the next step's benefit

Two bugs, both found by the render rather than by the assertions:

- **A cut must be a chord of the piece being bisected, not a line across the
  block.** The first version emitted the full-width line, so a cut made deep in
  the recursion sliced every other lot it crossed. A ward with a small
  `minSq` came out as a web. Watabou's `Polygon.cut` finds the two edges the
  line crosses; so must ours.
- The chord solve had a **sign error in `u`**, which the assertions caught only
  as "the block was subdivided" failing — the render was what made it obvious
  the cuts had vanished entirely rather than gone wrong.

### Notes for the implementer

- **Units.** Watabou's unit is 2.5 m in this repo. `minSq` is in units², so
  `minSq = 10` is 62.5 m² and `minSq = 110` is 687 m². Multiply by 6.25.
- **`r·r` is not a typo.** Squaring a uniform draw biases small. Every ward
  size range uses it.
- **Keep the angle cutoff.** `p.square < minSq * 4` disabling angle jitter is
  the rule that makes the output read as a town rather than as shattered glass.
- **The `split` recursion argument is the alley rule.** Passing `true`
  unconditionally gives every building its own alley and looks wrong.
- Our `geom2::cutPolygon` returns N pieces and takes a line; `Polygon.cut`
  takes a line and a gap. Add the gap by cutting with a thin ribbon instead of
  a line — we already have `offsetPolyline` for exactly that.

---

## 6. Read from the source (2026-09-15): corrections, and the missing pieces

The sections above were written from the generator's *output* and from memory
of the code. This one is from `TownGeneratorOS/Source/com/watabou/...` read
line by line - `Model.hx`, `Cutter.hx`, `Topology.hx`, `Ward.hx`, `Polygon.hx`
and every ward class. Four things above were wrong or missing, and three more
were never described.

### 6.1 `build()` is an order, and the order is the design

```haxe
buildPatches();       // sites on a spiral, Voronoi, relax the centre only
optimizeJunctions();  // merge corners closer than 8 units
buildWalls();         // the wall is the boundary of the inner patches; gates
buildStreets();       // A* over patch corners, gate -> plaza; then SMOOTH IN PLACE
createWards();        // a ward class per patch, each rating every patch
buildGeometry();      // each ward cuts its block into buildings
```

Streets come *before* wards, and they deform the patches. Everything the wards
do happens on patches that the streets have already bent.

### 6.2 `buildPatches` — the sites are a spiral, not blue noise

```haxe
var a = sa + Math.sqrt( i ) * 5;
var r = (i == 0 ? 0 : 10 + i * (2 + Random.float()));
```

Radius linear in `i`, angle in `sqrt(i)`: dense at the middle, opening toward
the edge. Only the three central patches and the citadel's are relaxed, three
times. `nPatches` (default 15) of the sorted-by-distance regions are the town;
the rest are countryside. **A town that is even everywhere has no middle.**

### 6.3 `optimizeJunctions` — short edges are merged

Any two consecutive corners closer than 8 units become one, at their midpoint,
in every patch that uses either. A raw Voronoi has three-cells-meet-a-hair-apart
junctions everywhere; each is a corner every later cut has to respect.

### 6.4 `buildStreets` + `Topology` — routes over *corners*, then smoothed in place

`Topology` builds a graph whose nodes are patch **vertices** and whose edges are
patch edges weighted by length (wall and shore vertices are blocked, gates are
not). For each gate: A* to the nearest plaza corner, excluding outer nodes;
outside the wall, a road from the far edge to the gate excluding inner nodes.
`tidyUpRoads` chains the segments into `arteries`. Then:

```haxe
function smoothStreet( street ) {
    var smoothed = street.smoothVertexEq( 3 );      // (v0 + 3 v1 + v2) / 5
    for (i in 1...street.length-1) street[i].set( smoothed[i] );
}
```

`set` writes into the **shared Point objects**. The street's points *are* the
patches' corners, so the patches bend to fit the street. That is the whole
reason a Watabou main street reads as one continuous road: the junctions move.
Our first port walked the graph of *cells* and widened the edge crossed between
them - a row of segments each perpendicular to the direction of travel. Not a
street.

### 6.5 `Cutter.ring` — sequential, and no cross-cuts

```haxe
slices.sort( shortest first );
var p = poly;
for (slice) { var halves = p.cut( slice ); p = halves[0]; peel.push( halves[1] ); }
```

Each slice cuts what the previous slices *left*, so a later slice is bounded by
the earlier ones. Emitting every slice as a full line into one arrangement makes
every slice cross every other, and the crossings pile up at the court's corners
- the fans in the render. Each peeled strip is one piece; the court is what
remains and is emitted as nothing.

### 6.6 `createWards` — a class per patch, each rating every patch

```haxe
WARDS = [Craftsmen, Craftsmen, Merchant, Craftsmen, Craftsmen, Cathedral,
         Craftsmen ×5, Craftsmen ×3, Administration, Craftsmen,
         Slum, Craftsmen, Slum, Patriciate, Market, Slum, ... ]   // 36 entries
```

Taken in order (lightly shuffled) until every inner patch has one, so a
fifteen-patch town is **eleven craftsmen, one merchant, one cathedral, one
administration, one slum**. Each class picks *where* it goes by rating patches:

| ward | `rateLocation` |
|---|---|
| Market (plaza) | patch 0, the middle; a second market must not touch the first |
| Merchant | nearest the plaza centre |
| Administration | borders the plaza, else nearest it |
| Cathedral | borders the plaza (weighted to the largest), else nearest × area |
| Slum | **farthest** from the plaza |
| Patriciate | borders a park, does not border a slum |
| Military | borders the citadel or the wall |
| Craftsmen, Gate | anywhere |

Countryside patches: `Random.bool(0.2) && compactness >= 0.7 ? Farm : Ward`.

### 6.7 What each ward does with its block

| ward | geometry |
|---|---|
| CommonWard (craftsmen, merchant, patriciate, administration, slum, gate) | `createAlleys(getCityBlock(), minSq, gridChaos, sizeChaos, emptyProb)`; `filterOutskirts()` if not enclosed |
| Cathedral | 40 %: `Cutter.ring(block, 2 + 4r)`; else `createOrthoBuilding(block, 50, 0.8)` |
| Park | compact: `Cutter.radial(block, ALLEY)`; else `semiRadial` |
| Market | a statue (rect, rotated to the longest edge) or a fountain (circle), offset toward the longest edge |
| Farm | 4×4 rect, random rotation, between a random vertex and the centroid; `createOrthoBuilding(_, 8, 0.5)` |
| Military | `createAlleys(block, sqrt(area)·(1+r), 0.1+0.3r, 0.3, 0.25)` |

`createOrthoBuilding` fixes `c1 = longest edge`, `c2 = c1.rotate90()` once and
cuts each piece along whichever is more perpendicular to its longest edge at
`0.4 + 0.2r`; stop size `minBlockSq · 2^(normal·2 − 1)`; keep with probability
`fill`. **No fans, ever.**

### 6.8 Scale, which was most of the problem

| | Watabou (1 unit ≈ 2.5 m) | ours before | ours now |
|---|---|---|---|
| patches per town | ~15 | 9 → 32 | 28 |
| buildings per patch | 10–20 | 120 → 30 | ~25 |
| regular street | 1 unit = 2.5 m | 4.4 m | 2.5 m |
| main street | 2 units = 5 m | 8.8 m | 5 m |
| alley | 0.6 unit = 1.5 m | 1.5 m | 1.5 m |
| courtyard blocks | ≤ 1 (40 % of one cathedral) | 1 in 4 | 1 |

### 6.9 Comparison, and what is still missing

| piece | Watabou | ours | status |
|---|---|---|---|
| sites | spiral, centre relaxed | blue noise | **ported** |
| junction merge | `optimizeJunctions` | none | **ported** |
| arteries | A* over corners, gate → plaza | walked cells | **ported** (corners) |
| street smoothing | in place, patches bend | none | **ported** |
| street hierarchy | main / regular / alley by edge role | uniform | **ported** (main/regular) |
| ring | sequential peel | simultaneous lines | **ported** |
| ward variety | `WARDS` list + `rateLocation` | one preset | **policy rules by distance and id** — a `FieldGenerator` writing a ward attribute is the proper home |
| plaza | patch 0, open, statue | none | open block at the centre; no statue |
| wall + gates | boundary of inner patches; gates are wall vertices | none | **missing** — gates are the furthest corner per sector |
| outskirts | `filterOutskirts` thins by road/gate density | none | **missing** (B6) |
| park | `radial` / `semiRadial` | none | **missing** (B4) |
| farm | `Farm.hx` | none | **missing** (V2) |
| lot outlines | not drawn | drawn | render choice; his buildings-only drawing hides the patch partition |

Measured on the gallery town after 6.2–6.8: wedges 101 of 1,023 → 7 of 930;
triangles 110 → 13; the orthogonal wards produce zero wedges, every remaining
one is the slum's `createAlleys` skew, which is the slum's character.
