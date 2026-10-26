# A Bayesian network for the architectural programme

**Status:** steps 1 and 3 done; step 2 (the transcription) is the open work.

- **Step 1, triage — done.** `tools/procedural/plate_triage.py`, queue checked
  in at `assets/procedural/programmes/candidates.json`. 182 candidates, 78 of
  them drawings naming three or more dwelling rooms. The co-occurrence table it
  prints reproduces §2 below, so the filter is reading the book correctly.
- **Step 2, Stage A — not started.** 120+ plates to transcribe by eye. The
  schema and the rules are in §3; the work queue is the file above.
- **Step 3, B0 — done.** `src/runtime/procedural/learn/BayesianNetwork.{hpp,cpp}`
  (discrete network, exact inference by variable elimination, conditional
  ancestral sampling) and
  `src/runtime/procedural/world/generator/ArchitecturalProgram.{hpp,cpp}`
  (the hand-authored network, 198 nodes / 264 edges over 29 room uses and 62
  adjacency pairs, and `sampleProgramme`). Gated by
  `tests/procedural/ArchitecturalProgramTests.cpp`.
- **Step 4, Stage D — not started.** `ProgramDwellingPlanner` does not exist
  yet; nothing consumes a `Programme`.

Three things B0 needed that this plan did not anticipate, recorded because they
are decisions and not drift:

1. **Rooms only open into rooms on their own floor**, the stair excepted. The
   adjacency nodes know which rooms exist and nothing about where they are, so
   without this the network puts a door between an upstairs bedroom and the
   ground-floor passage. Floors are decided in the decoder, so the rule is
   applied there.
2. **A pair can be closed by a third room.** A chamber opens off the living
   room in a cottage because there is nowhere else; give the house a hall and a
   landing and that door is gone. Authored as extra parents on the adjacency
   node — the conditional dependence is the whole reason for a network.
3. **The programme is closed for connectivity after sampling**, because every
   door is drawn independently and a thirty-room house will strand one. How
   many doors the repair has to add is a reported number (modest .106, grand
   .057, rural .120 per room) and the gate on it is a regression fence.

Everything needed to begin is here, including what was measured rather than
assumed.

**Goal.** A *second* dwelling generator, standing beside the existing one, which
samples an **architectural programme** from a Bayesian network trained on a real
corpus, and builds both the interior partition and the exterior massing from it.
The existing generator stays: the two should be selectable per plot so a town
carries both.

---

## 1. The paper

> Paul Merrell, Eric Schkufza, Vladlen Koltun.
> **"Computer-Generated Residential Building Layouts."**
> *ACM Transactions on Graphics* 29(6), SIGGRAPH Asia 2010.
> PDF: <https://vladlen.info/papers/architecture.pdf>
> Project page: <https://vladlen.info/publications/computer-generated-residential-building-layouts/>

Section headings, for navigation: 1 Introduction · 2 Related Work ·
3 Building Layout Design · **4 Data-driven Architectural Programming**
(4.1 Bayesian Networks, 4.2 Structure Learning) · 5 Floor Plan Optimization
(5.1 Proposal Moves, 5.2 Cost Function) · 6 Generating 3D Models · 7 Results ·
8 Discussion.

This plan covers **§4 only**. §5 (floor-plan optimisation) and §6 (the 3D model)
are in `.plan/merrell-koltun-building-layouts.md`, which also records the roof
work — now done, see `src/runtime/procedural/geom2/Skeleton.cpp`.

### What §4 actually says

Read off the PDF, not remembered. Quote the paper rather than this file if the
two ever disagree.

**The feature table (Figure 3, top).** These are the variable *kinds*; the
network has one node per instance.

| Feature | Domain |
|---|---|
| Total Square Footage | ℤ |
| Footprint | ℤ × ℤ |
| Room | {bed, bath, …} |
| Per-room Area | ℤ |
| Per-room Aspect Ratio | ℤ × ℤ |
| Room to Room Adjacency | {true, false} |
| Room to Room Adjacency Type | {open-wall, door} |

**Node naming (Figure 3, bottom, a didactic network over mountain cabins).**
`Total Square Footage`, `Footprint`, `Kitchen Exists`, `Living Room Exists`,
`Kitchen Area`, `Kitchen Aspect Ratio`, `Living Room Area`,
`Living Room Aspect Ratio`, `Living Room–Kitchen Adjacency Exists`,
`Living Room–Kitchen Adjacency Type`. Note the shape of it: an `Exists` node per
room *instance* (several for bedrooms), an `Area` per instance, an
`Aspect Ratio` hanging off the `Area`, and a pair of nodes per room *pair*.

**Training data.** "For the purpose of this work, we manually encoded **120
architectural programs** from an extensive catalogue of residential layouts
[Wood 2007]." Per instance they recorded: globally, total square footage and
bounding footprint; per room, type, square footage and aspect ratio of its
bounding rectangle; per pair, whether adjacent and whether the adjacency is
open-wall or a door. **Continuous attributes were quantised** to simplify
parameter estimation.

**Three networks, not one.** "The structure of residential architectural
programs varies significantly with the number of floors." Separate networks for
single-, two- and three-storey houses, trained on **40, 50 and 30** programmes
respectively.

| Network | Nodes | Edges | Training time |
|---|---|---|---|
| Single-storey | 42 | 33 | 1452 s |
| Two-storey | 82 | 61 | 4407 s |
| Three-storey | 120 | 92 | 8293 s |

**Structure learning (§4.2).** Standard local search over structures — add,
remove or flip one edge at a time — maximising the Bayesian score

```
log p(D, S^h) = log p(D | S^h) + log p(S^h)
```

with the structure prior `p(S^h)` **uniform** and the marginal likelihood
`p(D | S^h)` approximated by the **Bayesian Information Criterion**. They point
at Heckerman 1999 ("A tutorial on learning with Bayesian networks") for the
procedure, and used the open-source Probabilistic Networks Library
[Eruhimov et al. 2003].

**One trick worth copying.** Aspect-ratio variables are **left out of structure
learning entirely** and attached afterwards in an automatic post-process, each
`Aspect Ratio` node wired to its own `Area` node. It halves the variable count
for free, because the dependence is known a priori.

**Sampling.** Clamp any subset of variables as the brief — "number of bedrooms
and bathrooms, total square footage, areas of specific rooms" — and sample the
rest conditionally. Distributions over unobserved variables come from the
**junction tree algorithm** [Lauritzen & Spiegelhalter 1988].

**Why a network at all, in their words.** Encoding each room's existence
probability and each adjacency independently "does not take into account
conditional dependencies between multiple rooms and adjacencies, and can
generate unrealistic architectural programs. For example, the frequent
occurrence in the data of a bedroom-bathroom adjacency and a kitchen-bathroom
adjacency could lead to architectural programs being generated with
kitchen-bathroom-bedroom paths, which have very low likelihood."

**Figure 5** is the acceptance test to imitate: bubble diagrams from the network
after 10, 100 and 1000 structure-learning iterations, against one an architect
drew. At 10 iterations there is a low-likelihood clique (dining room + living
room + bedroom + hallway all mutually adjacent); at 100 the kitchen is missing
entirely; at 1000 the connectivity is plausible and the privacy gradient has
appeared on its own. **Draw this figure for our corpus.** If our network at
convergence still produces a house with no kitchen, it is not trained.

---

## 2. The corpus

`~/Desktop/building-book-markdown-and-images (1)/`

*The Builder's Practical Director* — 784 PDF pages, one 150 DPI JPEG scan per
page in `images/page-NNN.jpg`, plus `building-book.md` carrying, per page: an
image description, the ABBYY OCR prose, and a **`#### Supplemental drawing
labels (OCR)`** block holding the text that sits *on the plate*.

Everything below was measured in this repo, not estimated.

| what | count |
|---|---|
| page scans | 784 |
| pages the descriptions call "Architectural drawing page" | 84 |
| pages whose drawing-label block names **≥ 2 rooms** | **223** |
| room labels per plate (of those) | median 4, max 13 |
| usable `N ft. by N ft.` dimensions in the OCR prose, whole book | 14 |

**223 candidate plates against the paper's 120 programmes.** That is enough, with
room to throw away the unusable half.

The page numbers are worth keeping; regenerate them with the filter above rather
than pasting a list, but the first few are 27, 35, 37, 42, 51, 53, 54, 55, 57,
58, 61, 62, 65, 66, 69, 70, 71, 73, 79, 81 …

**Room vocabulary over those 223 plates** — this is the node set, handed to us:

```
hall 115   closet 92   kitchen 84   cellar 69   bed room 58   passage 45
scullery 40   parlour 36   chamber 35   dressing 33   dining room 33
wash-house 32   office 32   basement 30   yard 30   landing 28
water closet 27   shop 24   staircase 23   pantry 23   larder 22
breakfast room 20
```

**Co-occurrence is already meaningful** (over the 84 strongest plates):
bed room+bedroom 25, landing+staircase 23, kitchen+scullery 22, hall+kitchen 19,
bedroom+closet 15, kitchen+parlour 14, cellar+scullery 12. These are the edges
the network should recover; if structure learning produces something that
contradicts them, the fit is wrong.

**Building types, from the prose** (whole-book mention counts, for conditioning
and for the standing table): cottage 478, inn 305, shop 206, villa 143,
detached 81, labourer's 69, school 55, semi-detached 45, lodge 42, mansion 37,
tenement 15, farm house 14.

### The dimensions are on the plates, not in the text

This is the finding that changes the plan. The OCR yields **14** usable
dimensions in 784 pages, because the feet-and-inches marks are mangled. But the
plates carry them, in small italic under each room label, plus a
`SCALE OF FEET` bar. Verified on `page-027.jpg`:

```
SCULLERY 13' × 13'      KITCHEN 18' × 13'
BEER CELLAR 9' × 7'     WINE CELLAR 12' × 5'
LARDER AND DAIRY 16' × 12' 6"
COAL CELLAR · W.C. · AREA · Coal Shoot · Dresser
```

— a semi-detached pair, basement plan and elevation on one plate, with the
storey count readable off the elevation. So **the plates must be read as
images**. There is no shortcut through the OCR.

**Beware the triage.** The 84 "Architectural drawing page" descriptions include
engineering plates: `page-139.jpg` is *Sustaining & Wing Walls* — a retaining
wall section with no rooms at all. The ≥2-room-label filter is the better one,
and even it will pass specifications and details. Triage is part of the work,
not a preliminary to it.

---

## 3. Stage A — extract programmes from the plates

**Output:** one JSON per plate, checked into the repo (they are small and they
are the training data; regenerating them costs a vision pass over 223 images).

Suggested home: `assets/procedural/programmes/plate-NNN.json`, with an index.

```jsonc
{
  "page": 27,
  "source": "The Builder's Practical Director, PDF page 27",
  "kind": "semi-detached villa",      // from the plate title or nearby prose
  "storeys": 3,                        // basement + 2, read off the elevation
  "scaleBar": { "pixels": 742, "feet": 40 },
  "floors": [
    { "level": -1, "name": "basement",
      "rooms": [
        { "use": "scullery",  "feet": [13, 13] },
        { "use": "kitchen",   "feet": [18, 13] },
        { "use": "cellar",    "qualifier": "beer", "feet": [9, 7] },
        { "use": "cellar",    "qualifier": "wine", "feet": [12, 5] },
        { "use": "larder",    "feet": [16, 12.5] },
        { "use": "wc" }                       // no dimension given: allowed
      ],
      "adjacencies": [
        { "a": "kitchen", "b": "scullery", "link": "door" },
        { "a": "kitchen", "b": "larder",   "link": "door" }
      ]
    }
  ],
  "confidence": { "rooms": "high", "dimensions": "high", "adjacency": "medium" }
}
```

Rules that keep the data honest:

- **Only what is printed.** A room with no dimension on the plate gets no
  dimension; the network handles missing values, a guess corrupts it.
- **Adjacency is the paper's definition**: direct access — a door or an open
  wall. Not "shares a wall". Mark `"link": "door" | "open"`. The book's plates
  draw door swings; where they are not legible, record the adjacency with
  `confidence.adjacency: "low"` or omit it. The image descriptions in
  `building-book.md` warn about exactly this ("exact door-by-door adjacency
  should be verified in the scan") — believe them.
- **Normalise the vocabulary once**, in one table, and keep the raw label too:
  `bed room` → `bedroom`, `parlor` → `parlour`, `W.C.` → `wc`,
  `wash house`/`washhouse` → `washhouse`, `chamber` → `bedroom` **only** when
  the floor is an upper one. That last one is a judgement; write it down.
- **Feet, not metres, in the file.** Convert once at load. The plates are
  imperial and rounding twice loses the 6-inch increments that carry the
  proportions.
- Areas follow from the dimensions; do not store both.

**Acceptance for Stage A:** ≥ 120 plates transcribed (matching the paper), each
with ≥ 3 dimensioned rooms, spread across cottage / villa / farm house /
mansion. A report that prints the room-frequency and co-occurrence tables from
the JSON and shows they match §2 above within a few per cent — if they do not,
the transcription is drifting from the book.

---

## 4. Stage B — the network

Two versions, in this order. **Build the second only when the first is
generating plausible houses**, because a hand-authored network is a working
generator and also the baseline the learned one has to beat.

### B0. A prior network, hand-authored

The user's call, and the right one: *"we can create some network ourselves based
on common knowledge"*. Sanctioned, and it gets a generator running in a day.

Structure, from what the corpus already shows and from how these houses work:

```
Standing ─┬─► Storeys ─► TotalArea ─► Footprint
          ├─► KitchenExists (always, in a dwelling)
          ├─► ParlourExists
          ├─► ScullleryExists      ← p high given a kitchen
          ├─► BedroomCount
          └─► ServiceRooms{larder, pantry, dairy, washhouse, cellar}

TotalArea ─► <room>Area ─► <room>AspectRatio     (the paper's post-process wiring)
KitchenExists + ScullleryExists ─► Kitchen-Scullery-Adjacency
HallExists ─► Hall-<anything>-Adjacency          (the hall is the circulation hub)
```

Two structural rules to encode directly rather than hope to learn, both from the
paper's §3:

- **The privacy gradient.** Common rooms near the entrance, private rooms deep.
  It is what makes a plan read as a house rather than as a set of boxes.
- **Circulation is a room.** Hall, passage, landing, staircase are in the
  vocabulary (hall 115, passage 45, landing 28) and the adjacency graph should
  route through them, not around them.

### B1. Learned from the corpus

Follow §4.2. The details that matter:

- **One network per storey count.** Do not try to learn one network with a
  storey variable; the paper says the structure varies too much and hidden
  variables need more data than exists.
- **Local search**: add / remove / flip one edge per step, hill-climbing on the
  Bayesian score, `p(S^h)` uniform, `p(D|S^h)` by BIC. Restart from several
  random structures; keep the best.
- **Leave aspect ratios out of the search** and wire each to its own Area node
  afterwards. Free halving of the variable count.
- **Quantise areas and aspect ratios** before fitting. Sensible bins for this
  corpus: area in 25 sq ft steps to 200, then 50 to 600, then one tail bin;
  aspect ratio in {1:1, 5:4, 3:2, 2:1, >2:1}.
- **Acyclicity** is the only hard constraint in the search; enforce it on every
  proposed edge rather than repairing afterwards.
- Expect a network in the paper's size range — 40-ish nodes for a single-storey
  cottage, 80-ish for two storeys. Our vocabulary is larger than theirs
  (scullery, larder, dairy, washhouse have no American-suburban equivalent) so
  plan for more, and prune room types occurring on fewer than ~8 plates.

**No third-party library.** The paper used PNL; we should not take a dependency
for 40 discrete nodes. Exact inference by variable elimination over a junction
tree is a few hundred lines, and our networks are small enough that it will be
instant. Put it in `src/runtime/procedural/learn/` — it is not geometry and does
not belong in `geom2`.

**Acceptance for Stage B:** reproduce the paper's Figure 5. Print bubble
diagrams sampled from the network at 10 / 100 / 1000 structure-learning
iterations next to one transcribed straight from a plate. At convergence: no
house without a kitchen, no kitchen–bathroom–bedroom path, and the
kitchen–scullery and landing–staircase edges present.

---

## 5. Stage C — sampling a programme

```cpp
// src/runtime/procedural/world/generator/ArchitecturalProgram.hpp
enum class RoomUse : uint8_t { Kitchen, Scullery, Larder, Pantry, Dairy,
  Washhouse, Cellar, Parlour, DiningRoom, DrawingRoom, BreakfastRoom, Study,
  Hall, Passage, Landing, Staircase, Lobby, Porch, Bedroom, DressingRoom,
  Nursery, Closet, WaterCloset, Bathroom, Office, Shop, Workshop, Store, Yard };

struct RoomSpec
{
  RoomUse use;
  uint32_t floor;
  double areaMetres;      // Sampled, not averaged.
  double aspect;          // Long side over short.
};

struct Adjacency { uint32_t a, b; bool door; };  // false = open wall

struct Programme
{
  std::vector<RoomSpec> rooms;
  std::vector<Adjacency> links;
  double totalAreaMetres = 0;
  double footprintX = 0, footprintZ = 0;   // The network's own Footprint node.
  uint32_t storeys = 1;
};

// Any field of the brief that is set is clamped; the rest is sampled.
struct ProgrammeBrief
{
  PlotStanding standing;
  std::optional<uint32_t> storeys, bedrooms;
  std::optional<double> totalAreaMetres;
  std::optional<double> footprintX, footprintZ;   // What the lot will allow.
};

bool sampleProgramme(const ProgrammeBrief &, uint64_t seed, Programme &out);
```

Two notes that will otherwise be got wrong:

- **Keep the distribution, not the mean.** §5.2's cost function scores a plan by
  the log-likelihood of its room areas, so a single number per room throws away
  what the optimiser needs. Either carry the bin and its weights, or expose a
  `logLikelihood(RoomUse, area)` alongside.
- **`Footprint` is a network variable.** That is the hook that makes this drive
  the *exterior* as well as the interior — the massing can take its outline from
  the sampled footprint rather than from a coverage fraction of the lot. The
  clamp goes the other way when the lot is tight: fix `footprintX/Z` from the
  plot and let the network choose a programme that fits it.

---

## 6. Stage D — the second dwelling generator

**Do not modify `ComplexDwellingPlanner`.** It is the recursive-cut generator,
it works, and having two lets a town carry both. Add:

```
src/runtime/procedural/world/generator/ProgramDwellingPlanner.{hpp,cpp}
```

with the same external shape as the existing one, so `PlotPlanner` can choose
between them:

```cpp
enum class DwellingMethod : uint8_t { RecursiveCut, Programme };
```

picked per standing and seed in `describeStanding`, the same place
`RoofStyle` is chosen. Start it on Wealthy and Grand only — the big plans are
where a programme pays and where the current cut-based interiors are weakest —
then widen.

The pipeline:

1. `sampleProgramme(brief, seed)` → rooms, adjacencies, footprint.
2. **Massing from the programme.** Group rooms by floor; the ground-floor area
   sets the footprint, the wings come from the adjacency graph's articulation
   points — a programme that splits into two weakly-joined clusters is a house
   with a wing. Feed the result to the existing `massBuilding` /
   `groupWingsIntoClusters`.
3. **Roof** unchanged: `roofFromSkeleton` with a `RoofStyle` from the standing.
   This part is done and should be touched for nothing.
4. **Interior** from the programme by §5's optimisation — that is the other
   plan file. Until it exists, place the programme with the current recursive
   cut but *seeded by the programme's room list and areas* rather than by a
   generic room-area target. That alone is most of the visible gain.

**Acceptance for Stage D:** the dwelling gallery grows a section that plans a
street with `DwellingMethod::Programme` and prints, per house, the sampled
programme beside the realised plan — room count, area per room asked for versus
achieved, and the fraction of the programme's adjacencies the plan actually
delivers. That last number is the one to watch; it is what distinguishes a plan
from a partition.

---

## 7. What this must reuse, and not re-derive

- `geom2` — `Polygon`, `Boolean`, `Arrangement`, `Partition`, `Quantise`.
  Everything is on a micrometre integer grid; keep it there.
- `roofFromSkeleton` (`geom2/Skeleton.cpp`) — the roof is settled. Lowest wall
  plane per motorcycle cell, one face per wall, gables by leaving a wall out of
  the minimum. Do not rebuild it, do not simulate a wavefront.
- `PlotPlanner` — standing, house share, garden share, storeys, jetty, arcade,
  and now `RoofStyle`. `DwellingMethod` goes here too.
- `ComplexDwellingPlanner` — `massBuilding`, `massComposedBuilding`,
  `buildStoreys`, `buildRoofs`. The massing is worth reusing whole.
- The tests are the interface: `tests/procedural/DwellingGalleryTests.cpp` and
  `RoofSkeletonTests.cpp`. Add to them.

Build with `cmake --build build --target runtime <TestName>` — **there is no
`Engine` build target**, and `--target Engine` fails with a message no
`grep error:` catches, so you will silently test a stale binary.

Run world-gen tests only:

```
ctest -R "^(Geom2.*|DwellingGalleryTests|WorldGalleryTests|GeneratorGalleryTests|IntegrationGalleryTests|RoofSkeletonTests|TownLayoutTests|WorldCity.*|WorldSettlement.*|TerrainSettlementTests|TerrainParcelTests)$"
```

Never run the sparse-voxel or rendering suites.

---

## 8. Deliberate departures from the paper

Write these down so they are decisions rather than drift.

| Paper | Here | Why |
|---|---|---|
| 2000s American suburban catalogue | 19th-century English builder's book | It is the corpus we have, and it matches the town being generated |
| Garage, patio, nook, foyer, utility | Scullery, larder, dairy, washhouse, cellar, parlour | Different building culture; the service wing is the whole point of these plans |
| Bathrooms as a clamp variable | Water closet and bathroom are rare and late | Clamp on bedrooms and storeys instead |
| Probabilistic Networks Library | Our own variable elimination | 40 discrete nodes does not justify a dependency |
| One programme per house | Programme also drives the massing via `Footprint` | We generate the exterior too; the paper took the footprint as given |

---

## 9. Traps already paid for

- **Positional struct initialisers.** `Settings{a, b, c}` has caused three bugs
  in a day here; a field inserted above the last silently re-maps every later
  value and reads as "no change". Use designated initialisers or setters.
- **When a metric is *exactly* unchanged after an edit, suspect a stale binary
  before suspecting the algorithm.**
- **Never `git stash` in this repo.** The working tree has held uncommitted work
  that HEAD lacked.
- **Do not name a new file after an existing one** without grepping first — a
  `LotPlanner` was overwritten that way; `generator/` already holds `LotPlanner`,
  `RingLotPlanner` and `PlotPlanner`.
- **Judge geometry with the skeleton renderer, never the shaded one.**
  `tools/terrain/render_roof_skeleton.py <solids.json> <out.png> [--holes]`.
  A fold and a hole shade identically.

---

## 10. Order of work

1. **Triage** the 223 candidate plates to the house plans (expect ~150).
   Cheap, and it sizes everything after it.
2. **Stage A**, 120+ plates transcribed to JSON, with the frequency report.
3. **B0**, the hand-authored network, plus `sampleProgramme`. A generator exists
   from here on.
4. **Stage D** wired to B0, on Wealthy and Grand, with the gallery section.
5. **B1**, structure learning, with the Figure 5 reproduction as the gate.
6. Widen `DwellingMethod::Programme` to more standings as it earns it.
7. Only then §5, the floor-plan optimisation — see the other plan file.

Steps 1–2 are most of the calendar time and none of the difficulty. Do not skip
to step 5; an unlearned network is a working generator and a learned network
with no data is not.

---

# 11. Two generators, and what each is

Added after the programme generator was working and the plans were drawn. The
renders made it obvious that the two generators are not two implementations of
one idea; they are three different orderings of the same three things, and
Watabou has shipped two of them.

| | orders | exterior | interior |
|---|---|---|---|
| **Watabou, Dwellings** | geometry → labels | falls out | cut first, named after |
| **Watabou, Procgen Mansion** | exterior → interior | composed of wings | byproduct of the wings |
| **ours, programme** | programme → geometry | *should* follow the rooms | asked for first |

`ComplexDwellingPlanner` is our Dwellings: it cuts a footprint and labels the
pieces. It stays. `ProgramDwellingPlanner` is the third ordering and is the new
one. Both are wanted, and a town should carry both.

## What the references actually say

**Procgen Mansion** (<https://watabou.itch.io/procgen-mansion>):

- **A wing is a rectangular block with its own roof.** Not a shape that came
  out of a subdivision - a rectangle, deliberately.
- **A cluster is a group of wings whose roofs merge organically.** That is the
  unit the roof is solved over, not the building.
- **At most one door per wing.** A hard rule, and a good one: it is what stops
  a composed building reading as a row of sheds.
- **Storeys follow the ground plan.** "One-cell-wide pieces usually end up
  being 1 storey tall" - already quoted in `ComplexDwellingSettings`.
- **"There is a one-one match between its exterior and floor plan."** The
  exterior is designed and the rooms are a byproduct.
- There is a blueprint editor: mark towers, add and remove doors, edit the
  ground plan before generating.

**Dwellings** (<https://watabou.itch.io/dwellings>):

- Two phases, in this order: **make the floor plan, then label the rooms.**
- It generates "house-like dungeons" and says so - loops and unconventional
  layouts are wanted *because* they are explorable, not in spite of being
  unrealistic. That is a different goal from ours and worth not copying.
- Multi-floor with basements and interconnected staircases; spiral and
  stairwell are distinct options.
- **Restrictive rerolls**: shift-enter rerolls the walls and keeps the layout,
  control-enter rerolls only the labels. Worth having - it is the interface a
  person needs to argue with a generator.

## What that means for our generator

The rectangular wing is the answer to the thing the renders show worst: every
wall in our plans is a Voronoi edge, so they read as diagrams rather than
buildings. The fix is not a better partition, it is a different order:

1. **Mass first.** `massComposedBuilding` already lays down rectangular wings
   and pushes further wings off the sides of those standing - that *is*
   Watabou's rule and it is already written and tested here.
2. **Assign the programme's rooms to wings**, by floor, and by cluster in the
   adjacency graph: a programme that splits into two weakly joined groups is a
   house with a wing, which §6 of this plan already says.
3. **Partition each wing rectangle** among its rooms. Inside a rectangle the
   subdivision can be rectilinear, which is what makes a plan read as a plan.
4. **Roofs come free**: `massComposedBuilding` returns roof faces, and
   `roofFromSkeleton` is settled.

The power diagram stays useful for the *placement* question - which room is
next to which - but it should decide the arrangement and not the walls.

# 12. What is left, in order

1. **`ProgrammeDwellingPlan` → `rewrite::DwellingPlan`.** Walls, doors,
   storeys, stairs, wings, roofs, so the existing solids pipeline and the
   renders work on it unchanged. This is what plugs the new generator into
   everything already built.
2. **Mass-first placement**, per §11 above: rectangular wings, rooms assigned
   to them, rectilinear subdivision inside each.
3. **One door per wing**, as Procgen Mansion has it, and the entrance chosen
   rather than emergent.
4. **`DwellingMethod` in `PlotPlanner`** - `RecursiveCut` or `Programme`,
   picked per standing and seed in `describeStanding`, so a town carries both.
   Nothing generates programme buildings in a settlement until this exists.
5. **Structure learning**, B1's second half, with the Figure 5 reproduction as
   the gate. 52 dwelling records against 357 nodes will overfit; restrict the
   search to the existence nodes, where the data is densest.
6. **Stage A to 120 designs.** 59 now. The yield has flattened - the remaining
   pages are mostly quantity tables and construction plates.
7. **§5's floor-plan optimisation**, which is the other plan file, and which is
   most of the gap between 77% of doors delivered and all of them.
