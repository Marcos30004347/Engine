# Conformance to Merrell, Schkufza and Koltun

*Computer-Generated Residential Building Layouts*, SIGGRAPH Asia 2010.
<https://vladlen.info/papers/architecture.pdf>

Written by reading the paper, not by remembering it. Every algorithm below is
transcribed from the PDF; where we depart from it the departure is named and
argued rather than left to be discovered.

The short verdict, **revised after §5 was implemented**: §4 is substantially
implemented bar structure learning, **§5 is now implemented**, §6 is half
implemented.

The first draft of this document recorded that §5 was not implemented at all -
what stood in for it was a weighted power diagram, which delivered 77% of the
programme's doors and could not be improved by tuning because a Voronoi cell
cannot be a rectilinear room. `FloorPlanOptimiser` is now the paper's
algorithm: rectilinear rooms, the canonical grid start, the slide and swap
moves with snapping, the four cost terms, and Metropolis with annealed beta. It
delivers **94% of doors**, every room rectilinear, and a dimension cost **0.92
of what the programme's own draw would cost** - that is, the realised rooms are
more likely under the network than the sample they came from, which is what
§5.2 asks for and what a target value cannot express.

`ProgramDwellingPlanner`'s power diagram is kept: it is fast, it has no failure
mode, and it is what the massing path uses. The optimiser is what to reach for
when the plan has to be right.

---

## §4 Data-driven architectural programming

### What the paper does

A Bayesian network per storey count (single, two, three storey; 40, 50 and 30
training programmes). Nodes: `Total Square Footage`, `Footprint`, one
`<Room> Exists` per room *instance*, one `<Room> Area` per instance, an
`Aspect Ratio` hanging off each `Area`, and a pair of nodes per room pair -
`Adjacency Exists` and `Adjacency Type` ∈ {open wall, door}. Continuous
attributes are quantised before fitting.

Structure learning (§4.2): local search over structures - add, remove or flip
one edge - hill-climbing the Bayesian score

    log p(D, S^h) = log p(D | S^h) + log p(S^h)

with `p(S^h)` uniform and `p(D | S^h)` approximated by BIC. Aspect-ratio
variables are excluded from the search and attached to their own `Area` node
afterwards. Sampling clamps any subset as the brief and draws the rest by the
junction tree algorithm.

### What we do

| Paper | Ours | Status |
|---|---|---|
| one network per **storey count** | one per **building class** (16) | **departure, argued below** |
| 42 / 82 / 120 nodes | 79 to 413 nodes per class | comparable |
| quantised areas and aspect ratios | 17 area bins, 5 aspect states | ✓ |
| per-instance Exists/Area nodes | per **type**, instances drawn in the decoder | **departure** |
| pair of nodes per room pair | ✓, plus a `closedBy` conditioning we added | ✓ + extension |
| aspect ratio outside the structure search | ✓, wired to its own Area afterwards | ✓ |
| retain full distributions, not a sample | ✓ `areaBinWeights`, `areaLogLikelihood` | ✓ |
| exact inference (junction tree) | variable elimination + local-Markov fast path | ✓ equivalent |
| **structure learning, BIC, local search** | **not implemented** | **MISSING** |
| parameters from 120 programmes | MAP from 59, prior = hand-authored tables | departure of necessity |

**The storey-count departure.** The paper's reason for three networks is that
"the structure of residential architectural programs varies significantly with
the number of floors". The same argument applies with more force across
building types, and our corpus spans taverns, churches, castles and warehouses
where the paper's spans only houses. We split by class and keep storeys as a
variable inside each. If the corpus ever reaches the paper's density for
houses alone, splitting the dwelling class by storey count as well is the
faithful thing to do.

**The per-instance departure.** The paper has a node per room instance - three
bedrooms are three `Exists` nodes and three `Area` nodes. We have one per type
plus a count node, and draw the instances in the decoder from the type's
distribution. This costs us the ability to say that *this* bedroom is the large
one; it saves a network that would be several times the size. It is a real
departure and it is why the decoder has to redraw areas per instance.

**Structure learning is the gap.** Everything above is parameters. The paper's
§4.2 searches over *structures* and its Figure 5 shows why: at 10 iterations
the network produces a low-likelihood clique, at 100 it produces a house with
no kitchen, at 1000 the privacy gradient has appeared on its own. We have
never run that search, so our structure is entirely hand-authored and the
Figure 5 reproduction - the plan's stated gate - has never been attempted.

---

## §5 Floor plan optimization

### What the paper does

**State.** A floor plan `x` is a set of `n` rooms. Each room is a **rectilinear
polygon** enclosed by wall segments. A maximal contiguous set of collinear wall
segments is a *wall*.

**Initialisation.** "The optimization begins from a canonical high-cost
configuration, in which equally-sized rectangular rooms are packed on a grid on
each floor."

**Objective.** `f(x) = exp(-βC(x))`, explored by the Metropolis algorithm:
a proposed layout `x*` is accepted with probability

    α(x*|x) = min(1, exp(β(C(x) - C(x*))))

β annealed over time. Floors are optimised *together*, for mutual coherence.

**Move 1, sliding a wall.** Choose a wall and a split point along it. Split it
into two collinear walls; one stays, the other moves forward or backward by
`d ~ N(0, σ²)`. The split point has strictly positive probability of being an
endpoint, in which case the whole wall moves. Splitting prioritises existing
vertices but may split a segment - "such splits are essential since they
introduce concave corners". After a split move, **snap** the moved wall to an
existing one within ε, and align with walls on neighbouring floors.
`ε = 1 ft`, `σ = 2ε`.

**Move 2, swapping rooms.** Two rooms chosen at random, their labels
interchanged. "This generally results in a drastic change in the objective
function value."

**Cost.** `C(x) = ka·Ca + kd·Cd + kf·Cf + ks·Cs`

- `Ca` **accessibility** = number of missing connections + number of enclosed
  patios, entrances and garages. A connection is only possible if the two rooms
  "share a sufficiently long wall segment to accommodate a door".
- `Cd` **dimensions** = `-Σᵢ (ℓ_aⁱ(x) + ℓ_asⁱ(x))`, the log-likelihoods of the
  observed area and aspect ratio of room `i` under the distributions the network
  produced. Explicitly *not* a target value: "we have found it advantageous to
  retain the complete probability distribution".
- `Cf` **floors** = `Σᵢ A(Fˣᵢ − Fˣⁱ⁻¹) / Σᵢ A(Fˣᵢ)`, the unsupported area. `Fˣ⁰`
  is the building lot, so a lot constraint falls out of the same term.
- `Cs` **shapes** = `kr·Σ(1−hᵢ)·Mc(Rᵢ) + kg·Σ Mc(Gᵢ) + ko·Σ e(Fᵢ)` where
  `Mc(S) = (A(H(S)) − A(S))/A(S) + e(S)`, `H` the convex hull, `e` the edge
  count; `hᵢ` marks hallways and stairways, which are not penalised for
  concavity; `Gᵢ` are groups of rooms joined by **open walls**, which are
  experienced as one space; and the last term penalises an irregular floor
  outline.

### What we do

`FloorPlanOptimiser`, which is the paper's algorithm.

| Paper | Ours | Status |
|---|---|---|
| rectilinear rooms | cells on a refining grid; every room a union of cells | ✓ |
| grid of equal rectangles to start | ✓, boustrophedon so neighbours are neighbours | ✓ |
| Metropolis, annealed β | ✓, β from 0.35 to 9 geometrically | ✓ |
| slide a wall, split, `d ~ N(0,σ²)` | ✓ | ✓ |
| snap at ε=1 ft, σ=2ε | ✓ | ✓ |
| align with walls on neighbouring floors | ✓, the snap also considers other floors' lines | ✓ |
| swap two room labels | ✓ | ✓ |
| `Ca` missing connections, sealed entrances | ✓ | ✓ |
| `Cd` = −Σ(ℓ_area + ℓ_aspect) from the network's own distributions | ✓, and the aspect distribution is now retained too | ✓ |
| `Cf` unsupported area, lot as `Fx⁰` | ✓ | ✓ |
| `Cs` = kr·Σ(1−h)Mc(R) + kg·Σ Mc(G) + ko·Σ e(F) | ✓ | ✓ |
| floors optimised together | ✓, a move picks a floor at random | ✓ |

**Departures, named.** The state is a labelled cell complex rather than explicit
polygons. The paper's words are "rectilinear polygon enclosed by wall
segments", and the paper uses **no grid** except to start - "equally-sized
rectangular rooms are packed on a grid on each floor" is the initial
configuration and nothing else.

A cell complex over the arrangement of the *actual wall lines* is the same set
of shapes: every rectilinear layout is the arrangement of its own walls. What
it must not become is a mesh, and without care it does - a slide inserts a line
spanning the whole floor, so every accepted move leaves behind a line that one
wall used once. Measured before this was fixed: **95 lines and 1039 cells to
hold a layout with 86 corners in it.** Redundant lines - the same label on both
sides all the way along - are now dropped after every move, and the same
layouts come out at **28 lines and 74 cells for 74 corners**. The arrangement is
the walls, which is what the paper's state is.

Coalescing then exposed a second thing the words imply and the representation
did not enforce: two cells of one room meeting only at a corner make a shape
that is connected but is not a *polygon* - its boundary passes through that
vertex twice, and an outline tracer keyed on the vertex takes the wrong edge.
Pinched rooms are rejected rather than traced more cleverly. Three things the paper leaves implicit are enforced explicitly, because the
representation allows what the words do not: a room must stay above a minimum
area, a room must stay **connected**, and a room must not **pinch**. The second
was found by the outline tracer disagreeing with the cell area it was tracing.

One refinement: the area likelihood is interpolated between bin centres rather
than taken as the bin's constant. A piecewise-constant density is flat inside a
bin, so minimising its negative log gives the search no reason to prefer any
area within one - and the bins are 25 sq ft wide at the bottom, which is most
of a small room.

**Figure 8 reproduced.** Ablating each term degrades what it holds and nothing
else improves to compensate:

| | doors | Ca | Cd | Cs | acceptance |
|---|---|---|---|---|---|
| all terms | 95% | 1.50 | 57.1 | 46.6 | 27% |
| no `Ca` | **57%** | **9.75** | 54.3 | 44.5 | 29% |
| no `Cd` | 97% | 1.00 | **126.5** | 40.0 | 37% |
| no `Cs` | 92% | 1.92 | 52.6 | **163.9** | **68%** |

The acceptance rate under no-`Cs` is the tell: with nothing constraining shape
the search accepts almost anything.

---

## §6 Generating 3D models

| Paper | Ours | Status |
|---|---|---|
| roofs by straight skeleton | `geom2::roofFromSkeleton` | ✓ |
| gables by projecting roof-face vertices onto the façade plane; a face with horizontal top edges is not converted | `buildRoofs` produces gables by style and at joins | ✓ in spirit, not by projection |
| doors placed to minimise the sum of distances to the nearest corner of each adjacent room (Alexander: a door in the middle of a wall "breaks the room in two") | ✓, 33 samples along the run, constrained so the door fits | ✓ — 428 doors at 0.52 off-centre |
| passageway type by privacy: ordinary door in private rooms, French doors in semi-private, wide passage between public rooms, banister at a change of elevation | ✓ `passageFor` on a 0/1/2 privacy scale | ✓ — 374 doors, 13 french, 41 wide passages (the banister is not modelled) |
| windows at regular intervals on exterior segments, smaller or omitted where a lower roof obstructs; entrance room's central window becomes the front door | the **front door** is placed: the longest exterior run of the entrance room, preferring the street side, off-centre by the same rule as every other door. Windows themselves are still missing. | partial — 52 of 53 buildings on real lots have a way in |
| staircase form from the shape of its space: narrow rectangle → linear, wide → U, L-shaped → 90° turn; ≥36 in wide, tread 9–11 in, riser ≤ 8¼ in | one straight flight per storey | **MISSING** |
| style templates listing geometry and materials per element | `RoofStyle` only | **MISSING** |

---

## Fitting a real lot

Everything above is measured on open ground. A lot allocated by the settlement
generator is a different problem, and the numbers that matter there are its
own. Measured over 53 buildings on lots taken straight out of `SettlementLayout`
— a town of 900 and a village of 260, 470 lots between them, 20 m² to 424 m²,
aspect up to 3.8 (`ProgrammeLotTests`):

| | |
| --- | --- |
| built | 53 of 53 offered, none refused |
| programme doors delivered | 99% |
| dimension cost against the programme's own draw | 1.05 |
| floor area standing off the lot | 0.08% mean, 1.3% worst |
| the storey plates against the plan's own floors | 0.00% |
| rooms standing on another room | none |
| buildings with a way in | 52 of 53 |
| of the lot allocated (building + its outdoor ground) | 99% |

Six things this needed that open ground does not, and one that was worth more
than all of them:

- **The lot is a polygon and a hard constraint.** The paper's outline is given
  by the user, so a soft `Cf` suffices; ours decides its own outline, and a
  plan that pays a fine and stands in the street is still in the street.
- **The plan must start over the whole plate**, with what is off it cut away.
  An L's largest inscribed rectangle is half of it, and a plan started there
  can never reach an arm — every route out crosses ground that is not yet its
  own.
- **The programme is conditioned on the massing, not on the lot**, by rejection:
  a wedge carries a much smaller rectangle than its area suggests.
- **The exterior is re-massed onto the finished plan** (§6's order), because the
  scaffolding massing left half its plate empty.
- **The building's own total area is scored**, as the squared log ratio to the
  programme's total. A departure: the paper's distributions are tight enough
  that per-room costs hold the size, and ours — fitted to 59 designs — are not.
- **A room too small to place leaves no floor behind it**, and a floor's outline
  is the largest loop of its boundary, not the first found.

And the one that mattered most: **the frame is the lot's, not the world's**.
A rectilinear plan is rectilinear in *some* frame; squared to north on a wedge
it wastes a triangle along every boundary and stands askew to its own frontage.
Taking the axis whose bounding box holds the lot most tightly — which on a
burgage strip is along the street — took the dimension cost from 1.50 to 1.05
and door delivery from 96% to 100%, on its own.

## Beyond the book: the medieval settlement

The corpus is Victorian, and a medieval town is not. Four things were added for
it, none of which the paper or the book describes:

| | |
| --- | --- |
| `PeasantCottage` | the longhouse: house-place, byre at the lower end, cross passage between, croft and pen and midden outside. No kitchen — the house-place *is* the kitchen. |
| `Dormitory` | lodging for people housed in numbers: rooms of beds, a refectory, one kitchen, a keeper. |
| outdoor ground | yard, croft, garden, orchard, pen, midden. Rooms of the programme like any other, allocated over the lot the building leaves by a power diagram seeded at the rooms whose doors open onto them. |
| the house-place rule | a dwelling with no room of the living band has its largest service room become one. A house consisting of a kitchen is not a small house; it is a mis-labelled one. |

Also: storeys come from the programme rather than from the massing's width rule
(a burgage-plot house is tall *because* it is narrow), upper floors are jettied,
and on a crowded lot the programme draw is biased toward more storeys rather
than a wider footprint — which is the whole reason a town is shaped the way it
is. A village is exempt: land is the one thing there is.

## Multiple floors

The paper's own account of where this goes wrong is worth quoting, because ours
went wrong in the same place: multi-story spaces "couple the optimization on
multiple floors" and are "the chief cause for these unsuccessful optimizations",
which arise "in roughly 1 out of 20 optimization instances for two-story
buildings, and in 1 out of 5 three-story optimizations".

| | |
| --- | --- |
| Storeys the network can express | **four** (was three) — the corpus has four-storey designs in it, and a brief asking for one came back "probability zero" |
| Bedrooms it can express | **ten** (was six) — the first class town mansion has seven on its first floor alone |
| Every upper floor has a stair or landing | **always** — added where the draw left one out, because a floor you cannot reach is not a floor |
| A landing covers the flight below it | **78%** of the flight, and 43 of 51 buildings clear 50% |
| Storey heights | per floor: ground 3.4–3.6 m, plain 2.9 m, attic 2.3–2.4 m, basement 2.4 m, scaled by standing |
| Jetty | upper floors oversail by 28 cm |
| Roofs | every storey is roofed over whatever the storey above does not cover |

Three ways of asking the search to line the stair up were tried, and the note in
`FloorPlanOptimiser.cpp` records what each was worth: as a cost term alone, 55%;
as a random relocation move, accepted three times in a hundred; imposed on every
candidate, a third of the dimension likelihood. What works is cutting the shaft
once through the finished plan, and only where the search did not manage it —
which is a departure from optimising it, recorded as one.

## The roof, rebuilt

§6 builds the roof by straight skeleton. Ours did too, by its lower-envelope
form - the height at a point is its distance from the nearest wall that can
reach it - and the rule it used for "can reach it" was wrong in a way that only
showed on the plans this generator actually makes.

A wall competed for a cell only if the **whole cell** lay on its inward side.
On a stepped plan that strikes out the wall that is genuinely nearest over part
of the cell, and the survivors' regions then do not tile it. Measured: a cell of
73 m² came back with 48 m² claimed, a farmstead roofed 268 m² of 504 m², and a
quarter of every stepped plan was open to the sky.

The rule is now asked of the *point*: inside a cell the roof is the lowest of
the planes of the walls that face that point and can reach it. The cell is cut
where the answer changes, and the pieces that fall out have one winner each.

Three ways of cutting were tried and the third is the one that works:

| | |
| --- | --- |
| by every line at once (each wall's line, each pair's bisector) | 25 lines fragmented a 30 m² cell into 149 **overlapping** regions |
| triangulate, then split every triangle by every line | correct, but the slivers fall below the micrometre grid: a fifth of the plan lost |
| **split only where the corners disagree** | tiles exactly, stops as soon as a piece is uniform, 0.03 s for 41 plates |

Three further losses, each silent:

- **A wall's reach was unbounded.** The plane of one half of a T's top ran
  straight across the stem's opening, the ridge collapsed to a point, and a plan
  asked for three gables came back hipped. A wall now reaches only within 45° of
  its own ends, which is exact for a rectilinear plan.
- **The per-wall union dropped area.** The cross's five pieces tiled exactly and
  their union came back 18 m² short. Checked now; the convex pieces stand when
  it fails.
- **A merged facet the triangulator refused was skipped**, dropping a whole side
  of the roof — the missing 16% of every L-shaped plan.

And two things above the skeleton:

- **A roof spans a range, not a building.** The skeleton will pitch one hipped
  roof over five hundred square metres and what comes back is a tent. A piece
  deeper than a range (nine metres, wall to ridge to wall) is cut into strips
  across its short axis and each is roofed on its own, which is what puts
  valleys between the ranges of a large house.
- **No patches.** A flat lid over what the skeleton would not build, a fill for
  what it lost, a threshold below which a piece of roof was not worth roofing -
  each of those is a hole with a dressing on it. They are gone, and the last
  hole in the set went with the threshold: a first floor oversailing its ground
  floor by half a metre leaves a strip of 0.4 m², and 0.4 was below the cut-off.

Measured over the 41 storey plates the generator produces: every one roofed to
within 0.5% of its own area with no sampled hole, and the skeleton's own 124
checks pass.

## Into the town

`PlotPlanner` decides what a lot does with its ground - the share the house
takes, where the yard, the garden, the fence and the path go, which edge is the
street - and composes the house's exterior. Its own comment says the interior
"is the next layer's problem", and until now nothing was that layer: the town's
houses were cut out of their footprints by the recursive planner, and every
programme building stood on ground invented for it.

`planProgrammeInside` joins them. It takes the footprint the plot planner chose
and plans the programme inside it, exterior and all, with the massing coverage
at one - because the standing's own share of the lot has already been applied,
and applying it twice puts a cottage in the middle of a house-sized hole.

Measured over a town of 700 and a village of 240 (`ProgrammeTownTests`):

| | |
| --- | --- |
| lots offered / planned / built | 27 / 27 / 27 |
| the house's share of its lot | 68% |
| rooms each | 9.6 |
| programme doors delivered | 93% |
| the street houses | 136 people, employing 23 |

## Watabou

`watabou/TownGeneratorOS` is published as source. Dwellings and Procgen Mansion
are not - but they are Haxe compiled to JavaScript and served on the itch.io
page, and **the compiled bundle keeps its Haxe class paths**: 106 of them for
Dwellings, `com.watabou.dwellings.model.rooms.Bedroom` and the rest, with the
`validate` and `fill` functions intact and `.hx` file names and line numbers
surviving in trace calls. That is compiled output rather than published source
and is read here to understand an approach, not to copy code.

### From the published source

`Ward.createOrthoBuilding(poly, minBlockSq, fill)` is the recursive cut we
already claim to follow. Transcribed:

1. Take the longest edge; `c1` is its direction and `c2` is `c1` rotated 90°.
   **Two fixed axes for the whole recursion**, not per cut.
2. Choose a ratio in `[0.4, 0.6]` along the longest edge; that is the cut point.
3. Cut along whichever of `c1`, `c2` is *more perpendicular* to the longest
   edge - the smaller absolute dot product.
4. Recurse on each half until `half.square < minBlockSq · 2^(N(0,1)·2 − 1)`,
   a **log-normal** area threshold rather than a fixed one.
5. A piece that stops recursing becomes a building only with probability
   `fill`; otherwise it is left empty. That is where the gaps in a block come
   from.

`Ward.getCityBlock` insets a patch by the street it fronts: `MAIN_STREET 2.0`,
`REGULAR_STREET 1.0`, `ALLEY 0.6`, each halved, and it **shrinks a convex patch
but buffers a concave one**.

`Cutter` offers four primitives: `bisect` (cut at a ratio along an edge at an
angle, with a gap), `radial`, `semiRadial` and `ring` (peel slices inward,
shortest side first).

### From the compiled bundle

Dwellings assigns room types by **constraint satisfaction**, not by scoring.
Each room type declares a `validate` predicate over a candidate space and the
assigner matches spaces to types. Read off the bundle:

    Bedroom.validate  = aboveGround && size in [4,8] && narrow/size <= 0.5
                        && doors <= 1
    Cellar.validate   = size >= 5 && narrow != size && floor <= -1
    Chapel.validate   = checkBasic(minSize 4, maxSize 6, narrow 0.2, maxDoors 1)

`narrow` is the part of the room's contour that is too close to the opposite
wall, so `narrow.length / size` is a thinness measure - a bedroom may not be
more than half thin, a cellar may not be entirely thin. `Chapel.populate` puts
the altar on the long edge **furthest from every door**, summing distances from
each door to each candidate edge and taking the maximum.

That is a different answer to the same question our `Cd`/`Cs` terms answer: a
hard predicate per type instead of a soft cost. It is worth knowing that the
predicates are about **size, thinness, door count and floor** - not adjacency -
and that the paper's cost function is strictly more expressive.

### From the itch.io descriptions

**Procgen Mansion**: a wing is a rectangular block with its own roof; wings
group into clusters whose roofs merge; at most one door per wing; storeys
follow the ground plan; "there is a one-one match between its exterior and
floor plan" - the exterior is designed and the rooms are a byproduct.

**Dwellings**: floor plan first, labels second. It generates "house-like
dungeons" and says so: loops and unconventional layouts are wanted because they
are explorable. That is a different goal from ours and is not worth copying.
Restrictive rerolls - walls only, or labels only - are worth copying.

---

## The three orderings

|  | orders | exterior | interior |
|---|---|---|---|
| Watabou, Dwellings | geometry → labels | falls out | cut first, named after |
| Watabou, Procgen Mansion | exterior → interior | composed of wings | byproduct |
| Merrell/Koltun, and ours | programme → geometry | follows the rooms | asked for first |

`ComplexDwellingPlanner` is the first. `ProgramDwellingPlanner` is meant to be
the third and is currently a placement heuristic wearing its clothes.
