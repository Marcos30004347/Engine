# Building layouts after Merrell, Schkufza & Koltun

*Computer-Generated Residential Building Layouts*, SIGGRAPH Asia 2010.
<https://vladlen.info/papers/architecture.pdf>

The paper is a three-stage pipeline, and the three stages map onto layers we
already have. This is what each stage says, what we have, and what to build.

```
  architectural program   →   floor plans        →   3D model
  (Bayesian network)          (optimisation)         (style template)
  §4                          §5                     §6
```

Our equivalents today: `PlotPlanner` picks what a plot is for, the
`ComplexDwellingPlanner` cuts rooms, and `massBuilding` / `Skeleton` produce the
solid. The paper's middle stage is the one we do not have at all: we *cut* a
plan and it *optimises* one.

---

## 1. Roofs — done

> "Roofs are constructed using the straight skeleton algorithm [Aichholzer et
> al. 1995] (Figure 10(a)). This yields a hipped roof with no gables (Figure
> 10(b)). A gable is a triangular shape below the roof that becomes part of the
> façade. Gables are added if their existence is specified in the style
> template. For a given face of a hipped roof, its vertices are simply projected
> onto the façade plane to create a gable. If a roof face contains horizontal
> top edges (parallel to the ground plane), it is not converted into a gable,
> since this would distort other roof faces (Figure 10(c))."

Implemented in `geom2/Skeleton.{hpp,cpp}`:

- **(a)** uniform-weight straight skeleton, swept as bands between events;
- **(b)** that *is* the hipped roof, no extra step;
- **(c)** `RoofOptions::gables` marks edges; each marked face has its raised
  vertices projected onto the vertical plane of its own eaves edge, and the
  guard is the paper's second sentence — a face topping out along a ridge
  rather than at a point is left hipped.

The projection is applied to **positions**, not to one face's triangles, because
the apex is shared with the faces either side and they must stretch to follow.

Two things the paper gives us that the weighted-skeleton approach did not:
the simulation always runs on uniform weights (a weight-zero edge never
collapses, which is what stalled it), and hip/gable becomes a *style* property
rather than a second code path.

### What replaced the event simulation

Felkel's queue of edge and split events computes the skeleton exactly, and
exactly is where it kept failing: a wavefront is degenerate at every turn —
parallel bisectors, simultaneous events, corners that ridge — and each fix
uncovered another. Two thirds of a town's masses ended up on a fallback.

The wavefront is now **sampled** instead. A mitred inward offset *is* the
wavefront at that distance, and the offset is resolved by a winding-number
union, which takes an arm pinching off or a part vanishing as ordinary
outcomes. There are no events to order and nothing to stall, so there is no
fallback.

Two constructions inside it, both arrived at by measuring:

- **Strips, not annulus triangulation.** Triangulating the region between two
  contours as one polygon produces *ears* — triangles with all three corners
  on the same contour, flat where the roof should slope. Twenty on one small
  house, interleaved with the sloping triangles, and that is the comb the
  renders showed. A strip cannot make one: every quad runs from an edge of the
  lower contour to the matching stretch of the upper.
- **Correspondence by bisector, not by nearest point.** Matching a corner to
  the nearest point on the contour above sends several corners of a reflex
  notch to the same place and their strips overlap — an L sweeping 199.63 m²
  over 175 of plan. The wavefront's own bisector is the correct
  correspondence.

Cost: ~100 triangles per roof instead of ~10, and ridges are polylines at the
sampling step rather than exact segments. Slopes are unaffected — between two
contours each face is planar however coarse the step.

### Still open

**Gables.** Merrell's projection is exact on a simulated skeleton, where a hip
face rises to one shared apex and its neighbours follow it out. On a sampled
wavefront a face is dozens of independent vertices at a dozen levels, and
projecting them tears the surface. The construction that works on a sampled
surface is to gable *before* the roof: push the outline out at the gabled edge
by about half the width of the mass so the apex lands on the original edge,
hip that, and clip the result back to the true outline — the cut profile is
the gable. Everything is hipped until that is built.

**The outline the roof is built on.** Merging coplanar faces back together is
the right thing for a *sampled* wavefront — one plane arrives as dozens of
strip triangles and a ridge as a row of points, and merging undoes that
tessellation exactly, because every one of those points lies in both planes
and so on their true intersection. That much is intrinsic and would be needed
however clean the input was.

But a good part of what it is cleaning up is not the sampling. It is the
outline. The eaves ring reaches the roof having been through a wing
decomposition, an outward offset, a difference against the neighbouring
masses and two unions, and it arrives carrying near-collinear runs, jogs a
few decimetres across, and vertices that are artefacts of how it was
computed rather than corners of a building. Those are what force the
conditioning, what make some merges fail their own area check, and what leave
the last slivers on a terrace.

The cure is upstream: the roof should be handed the *walls* — maximal
collinear runs of a rectilinear plan, which is exactly what §5 calls a wall —
rather than a boolean residue. Analysing the floor and its edges before
roofing is the same work as doing §5 properly, and it is the reason to do
that before any further roof work.

**Floors planned for roofing.** The storey plates are derived from the wings,
so their outlines carry every notch the wing decomposition and the overhang
trim leave behind, and the roof then has to cope with staircases a few
decimetres across. Planning the floors as clean rectilinear plates in the
first place — which is what §5 of this paper does, with rooms as rectilinear
polygons and walls as maximal collinear runs — would hand the roof an outline
it can skeletonise without any conditioning at all. That is the strongest
argument for doing §5 next: it is not only a better interior, it is the input
the exterior wants.

**Skeleton exactness.** The sampled wavefront overlaps slightly at reflex
corners: an asymmetric L sweeps 176.10 m² over 172.91 of plan, 1.8% over.

**Was:** the skeleton's own robustness. It is exact on square, rect, L,
T, U, trapezoid and pentagon, and on 469 of ~800 real masses it stalls or
overshoots its own coverage check and the mass falls back to a plain centre-fan
hip. Two reproducible cases are in `/tmp/skel3.cpp` (`L-6`, 4.6% short;
`notch8`, 42% short) — both asymmetric rings with a split event. That is the
next roof work, and it is a bug hunt, not a design question.

---

## 2. The architectural program — a Bayesian network (§4)

> **Superseded.** This section is kept for the corpus measurements it records.
> The plan of record is `.plan/bayesian-architectural-program.md`, which has
> §4 read off the paper rather than remembered (the feature table, the three
> per-storey networks, the 120 encoded programmes, BIC and the local edge
> search), a measured survey of the plate corpus, and the staging for a second
> dwelling generator. Two things below are now known to be wrong: the corpus
> has **223** plates whose drawing labels name two or more rooms, not 59; and
> the dimensions **are** on the plates, in small italic under each label, so
> they are read as images rather than recovered from the OCR.


> "The architect toggles... a set of rooms and their adjacencies. An adjacency
> indicates direct access, such as a door or an open wall."

Attributes per room: **type**, **area**, **aspect ratio**, **floor**, and for
each pair whether they are adjacent and "whether the adjacency is open-wall or
mediated by a door". Continuous attributes are **quantised**.

The network is trained on a corpus and sampled to produce a program; any subset
of variables can be **clamped** as the brief ("a three-bedroom house with a
garage") and the rest sampled conditionally.

### There is a corpus after all

`~/Desktop/building-book-markdown-and-images (1)/` — *The Builder's Practical
Director*, 784 page scans at 150 DPI with OCR text and a description per page.
Measured, not guessed:

| what | count |
|---|---|
| page scans with a description | 784 |
| pages described as a floor plan | 59 |
| pages with "orthogonal, rectilinear planning" | 80 |
| dimensioned room entries the OCR yields cleanly | 8 |

The **room vocabulary** is the usable part, and it is exactly the network's
node set — staircase 228 pages, kitchen 212, closet 207, bedroom 193, passage
192, hall 182, cellar 164, scullery 135, office 108, parlour 90, landing 90,
basement 82, larder 66, water closet 66, dining room 65, drawing room 59,
living room 51, dressing room 49, lobby 48, stable 42, privy 41, breakfast
room 36, entrance hall 31.

That gives co-occurrence per building directly: which rooms appear together, at
what frequency, and which are circulation. It is enough to fit the *structure*
of §4's network — the room set and the adjacency priors — without any image
work.

What it does **not** give is dimensions. The schedules are in the prose
("One best Parlour. 15' 0" by 12' 0" 9 ft. high") but only eight survive OCR
cleanly enough to parse, because the feet-and-inches marks are mangled. Two
ways to get the rest, in order of cost:

1. Re-parse the prose with a tolerant reader — the schedules sit in tables
   with a consistent shape, and most failures are the `'` / `"` marks rather
   than the digits.
2. Read the 59 floor-plan scans directly. They carry dimensions *on the
   drawing*, which is the authority the book itself points to.

So the plan for §4 changes: fit the room set and adjacencies from the label
co-occurrence now, and keep the area and aspect distributions hand-authored
per standing until the schedules are extracted. The interface is unchanged.

**What to build.** The interface the next stage wants:

```
struct RoomSpec { RoomUse use; uint32_t floor; Distribution area, aspect; };
struct Program  { std::vector<RoomSpec> rooms; std::vector<Adjacency> links; };
Program sampleProgram(PlotStanding, uint64_t seed, const Brief &clamped);
```

Keeping a *distribution* per room rather than a single value matters — §5.2
uses the log-likelihood, not the mean.

The **privacy gradient** ("placing common areas, such as the [entrance]... and
private areas deeper in") is the one structural rule worth encoding directly
rather than learning: it is what makes a plan read as a house.

---

## 3. Floor plans — optimisation, not subdivision (§5)

This is the stage we lack. A floor plan `x` is a set of `n` rooms, each a
**rectilinear polygon**; a maximal run of collinear wall segments is a **wall**.

### Proposal moves (§5.1)

- **Sliding a wall.** Pick a wall and a split point; the wall splits into two
  collinear walls, one stays, the other moves by `d ~ N(0, σ²)`. The split point
  may be an endpoint (whole wall moves) and prioritises existing vertices, but
  splitting a segment has strictly positive probability — *"such splits are
  essential since they introduce concave corners."*
- Plus global moves (swapping rooms, moving a room between floors) so that any
  layout is reachable from any other with the same room count.

### Cost function (§5.2)

```
C(x) = ka·Ca(x) + kd·Cd(x) + kf·Cf(x) + ks·Cs(x)
```

| term | meaning |
|---|---|
| `Ca` **accessibility** | number of missing connections, plus enclosed patios, entrances and garages. Two rooms can connect only if they share a wall long enough for a door. |
| `Cd` **dimensions** | `−Σᵢ (ℓⁱₐ(x) + ℓⁱₐₛ(x))` — log-likelihood of each room's observed area and aspect ratio under its distribution. |
| `Cf` **floors** | `Σᵢ A(Fˣᵢ − Fˣⁱ⁻¹) / Σᵢ A(Fˣᵢ)` — each floor inside the envelope of the one below; `Fˣ⁰` is the building lot, so **the lot constrains the ground floor for free**. |
| `Cs` **shapes** | `Σᵢ (1−hᵢ)·Mc(Rˣᵢ)` with `Mc(S) = (A(H(S)) − A(S))/A(S) + e(S)`; `H` is the convex hull, `e(S)` the edge count as a complexity regulariser, `hᵢ` a hallway/stair indicator so circulation is **not** penalised for concavity. |

`Cf` is the piece that makes the exterior and the interior one problem rather
than two: the storey plates we currently derive *from* the wings would instead
fall out of the optimisation.

### Weights per building type

`ka, kd, kf, ks` are exactly the knob the request asked for. A first table,
to be tuned against the gallery:

| standing | `ka` | `kd` | `kf` | `ks` | reading |
|---|---|---|---|---|---|
| Slum | 1.0 | 0.4 | 3.0 | 0.3 | anything goes but it must stack and connect |
| Modest | 1.0 | 1.0 | 2.0 | 0.8 | ordinary house |
| Comfortable | 1.0 | 1.5 | 2.0 | 1.2 | rooms are the right size |
| Wealthy | 1.2 | 1.5 | 1.5 | 2.0 | shape matters; grand rooms are convex |
| Grand | 1.5 | 1.2 | 1.2 | 3.0 | formal, near-convex, symmetric |
| Rural | 0.8 | 0.6 | 2.5 | 0.4 | a working building |

---

## 4. The 3D model — a style template (§6)

> "Each available style is specified in a style template, listing the geometric
> and material properties of every building element: windows, doors, wall
> segments, gables, stairs, roofs, and patio poles and banisters. The style
> template also records the desired spacings of windows and banister poles, the
> roof angle, the presence of gables, and the length of roof overhangs."

We have the overhang, the roof pitch and the gable flag as loose settings; they
should become one `StyleTemplate` bound per standing, which is also where jetty
and arcade depth belong.

Three pieces worth taking directly:

- **Passageways.** Door type follows room privacy — ordinary doors for private
  rooms, French doors for semi-private, wide openings between public rooms, a
  banister wherever adjacent rooms change level. And placement: *"We place the
  door in a location that minimizes the sum of the distances from the door to
  the nearest corner in each of the adjacent rooms"* — quoting Alexander, a door
  mid-wall *"breaks the room in two, destroys the center."* Our doors are
  centred on the shared run today, which is the thing Alexander warns against.
- **Windows.** Laid along exterior wall segments at regular intervals per room,
  with a smaller type substituted where a lower roof obstructs, and none if even
  that will not fit. In the entrance room the central window is replaced by the
  front door — which is the entrance geometry we still do not have.
- **Stairs.** Shape follows the space: narrow rectangle → straight flight, wide
  → U-shaped with two flights, L-shaped → a 90° turn. Tread 9–11 in, riser
  ≤ 8¼ in, width ≥ 36 in.

---

## Order of work

1. **Roof robustness** — fix the two reproducible skeleton failures; the
   fallback rate is the metric. *(No new concepts, pure debugging.)*
2. **Style templates** — collapse the loose exterior settings into one
   per-standing template. Cheap, and everything below reads from it.
3. **Entrances and windows** — front door from the plot plan's entrance point,
   windows per §6. Visible immediately in the 3D view.
4. **Programme sampling** — the hand-authored stand-in for §4, with
   distributions rather than values.
5. **Floor-plan optimisation** — §5 proper, replacing the recursive cut. The
   big one; `Cf` subsumes our storey-plate derivation.
6. **Doors by Alexander's rule** — falls out of 5 and is a one-line objective.
