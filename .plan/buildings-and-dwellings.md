# Buildings: dwellings, manors and mansions

Written 2026-09-14. Sequenced **after**
[`city-blocks-and-neighbourhoods.md`](city-blocks-and-neighbourhoods.md): a
building needs a lot, and the lot algorithm is what that plan fixes.

Sources: Watabou's [Dwellings](https://watabou.itch.io/dwellings) and
[ProcGen Mansion](https://watabou.itch.io/procgen-mansion). Neither is open
source, unlike TownGeneratorOS, so this plan is built from the authors' own
statements, the published output, and the structure our existing planner
already derived from a recorded trace — and it says so wherever it is
inferring rather than quoting.

---

## 1. What Watabou says, and what the output shows

### 1.1 Dwellings

The author's own framing, quoted:

> "I'm not making an app for planning real houses… this generator is designed
> to create maps of **house-like dungeons**."

That single sentence sets the objective function, and it is not ours by
default. A house-like dungeon wants rooms you can walk between, a reason to
go upstairs, and a plan that reads at a glance. It does *not* want a plausible
building code. When our planner refuses a layout for being architecturally
unsound, it is optimising for something Watabou deliberately does not.

Stated structure:

- **Two stages, in this order**: first cut the floor plan into rooms of varying
  size, *then* try to assign purposes to the spaces that resulted. The author
  notes this second stage **sometimes fails**, because a purpose has
  requirements the room does not meet.
- **Multiple floors**, including basements and upper storeys.
- **Corridors and staircases** connect areas; stairs come in `spiral` and
  `stairwell` forms.
- **Entrances** are marked on the plan.
- **Furniture** as abstract shapes inside rooms.
- Layout tags: `slab`, `tower`, `compact`; size tags `small`, `medium`,
  `large`.

The load-bearing observation is the ordering. **Shape first, purpose second.**
Our planner reserves the hall and the stair core *first* and then cuts the
remaining plate — purpose before shape. That is a defensible choice and it is
the opposite one; it is why our plans come out regular and why they decline
more often.

### 1.2 ProcGen Mansion

A larger building on the same idea: wings around courts, a clear principal
axis, service rooms behind and reception rooms in front. The relevant
difference from Dwellings is **composition** — a mansion is several
rectangular wings joined at a court, each of which is a Dwellings-shaped
problem, rather than one plate subdivided.

Our `generator::CompoundPlanner` already works this way: castle, manor, church
and hall as precinct + walls + towers + wings, where each inhabited wing is a
`DwellingBrief` planned by the dwelling planner. **That composition is already
right.** What is missing is the quality of the plan inside each wing.

---

## 2. What we have

`generator::DwellingPlanner` — "reserve the entrance hall and a compact dog-leg
stair core, cut the remaining plate into rooms with bounded recursive
guillotine cuts, connect rooms through doors on shared walls from the hall
outward, then assign purposes by programme. Straight walls in the wing's local
frame; up to three floors. **Fails explicitly instead of forcing rooms.**"

Model: `DwellingUse {Cottage, ShopHouse, Lodging, Hall, Warehouse}`,
`RoomUse {Living, Sleeping, Shop, Store, Common, Service, Hall}`, with rooms,
walls, doors, stairs, floors, and a `fallback` flag recording when the legacy
two-band recipe was used because the planner declined.

| | Watabou Dwellings | Ours |
|---|---|---|
| Order | shape, then purpose | purpose, then shape |
| On failure | assigns what it can, leaves the rest generic | declines the whole plan, falls back to two bands |
| Circulation | corridors as first-class spaces | doors on shared walls, no corridor spaces |
| Stairs | spiral or stairwell, a chosen type | one dog-leg core |
| Floors | basements and upper storeys | up to three, no basement |
| Furniture | abstract shapes per room | none |
| Massing tags | slab / tower / compact | none |

---

## 3. Plan

### D1 — Invert the order: shape, then purpose

The single change with the most effect. Cut the plate into rooms of varied size
*first*, then fit the programme to the rooms that exist.

- Guillotine cuts as now, but sized from a log-uniform distribution the way
  Watabou sizes lots (`target * 2^(k(r-0.5))`), so a floor has a mix.
- Then match: for each programme entry, pick the best unassigned room that
  meets its requirements; leave the rest `Common`.
- **Stop declining.** A room that meets nothing becomes generic, which is what
  "sometimes fails" means in the author's description. `fallback` should
  become rare enough to be worth investigating when it fires.

*Gate*: the decline rate on a corpus of real lots from the settlement gallery
drops to near zero, and plans still satisfy the existing door-reachability and
stair checks.

### D2 — Corridors as spaces

A corridor is a room with a purpose, not the absence of one. Reserve a
circulation spine where the plate is deep enough to need one, and hang rooms
off it; keep the current door-graph repair as the check that everything is
reachable from the entrance.

*Gate*: every room reachable from the entrance; the median door-to-entrance
path shortens on deep plates.

### D3 — Stair types and basements

`spiral` (a square core, one room's footprint) and `stairwell` (a straight or
dog-leg run against a wall) as two bindings rather than one hard-coded core.
Add a basement storey where the programme wants storage and the ground is
suitable.

*Gate*: a section render showing headroom and landings for both types.

### D4 — Massing tags

`slab`, `tower`, `compact` as a property of the brief that picks the plate
proportions and floor count before any cutting happens — the thing that makes a
warehouse look like a warehouse and a tower like a tower.

*Gate*: the same lot with three tags gives three visibly different buildings.

### D5 — Furniture

Abstract shapes per room, by purpose. Cheap, and it is most of what makes a
Dwellings plan readable at a glance.

*Gate*: a floor-plan render legible without a legend.

### M1 — Mansions and manors on the compound planner

`CompoundPlanner` already composes wings around a court. What it needs from
this plan is D1–D4 applied per wing, plus:

- a **principal axis** for the whole compound, so reception wings face the
  approach and service wings sit behind;
- **wing roles** (`keep`, `hall`, `service`, `chapel`, `store`) driving each
  wing's programme, which the struct already carries as a string;
- courts as real regions — they are ground something can stand on, exactly as
  a road ribbon is.

*Gate*: a manor render where the approach, the court, the hall and the service
range are each identifiable without labels.

---

## 4. Order and dependencies

```
  city-blocks-and-neighbourhoods  B1..B6      (lots exist, and are good lots)
        |
        v
  D1 shape-then-purpose  ->  D2 corridors  ->  D3 stairs/basements
        |                                            |
        v                                            v
  D4 massing tags  ------------------------->  D5 furniture
        |
        v
  M1 mansions and manors on the compound planner
```

D1 is the one worth doing first and alone: it is a reordering of two stages we
already have, and it should visibly reduce the decline rate before anything
else is attempted.

## 5. A caution about the objective

Watabou is explicit that these are "house-like dungeons", not buildings. We
want both: a plan legible as a map *and* a building a settlement can house
people in, because our director measures bedroom capacity off it and the
runtime meshes it.

So port the **method** — shape before purpose, varied room sizes, corridors,
generic rooms instead of refusals — and keep our own **constraints** —
measured capacity, reachability, headroom, walls that meet. Where the two
conflict, the constraint wins and the ledger records what it cost. That is the
difference between porting an idea and copying an output.
