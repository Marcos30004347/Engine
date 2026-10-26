# Architectural programmes, transcribed from the plates

Training data for the Bayesian network in
`src/runtime/procedural/world/generator/ArchitecturalProgram.cpp`. See
`.plan/bayesian-architectural-program.md` for why, and Merrell, Schkufza and
Koltun, *Computer-Generated Residential Building Layouts* (SIGGRAPH Asia 2010)
§4 for what a programme is.

## `candidates.json` — the work queue

Produced by `tools/procedural/plate_triage.py`, which reads
*The Builder's Practical Director* and keeps every page whose drawing-label
block names two or more rooms. Regenerate rather than hand-edit:

```
python3 tools/procedural/plate_triage.py --json assets/procedural/programmes/candidates.json
```

As of the triage: **182 candidates** of 784 pages, of which **78** are drawings
naming three or more rooms of a dwelling. The remainder are specifications,
details and engineering plates, and the ones that are wanted have to be picked
out by eye — the label filter passes `Sustaining & Wing Walls` and it will pass
anything else that happens to say "area" twice.

## `plate-NNN.json` — one transcribed programme each

**The plates have to be read as images.** The OCR yields fourteen usable
`N ft. by N ft.` dimensions in the whole book, because the feet-and-inches
marks are mangled; the dimensions are printed in small italic under each room
label on the plate itself, with a `SCALE OF FEET` bar. There is no shortcut.

Schema, and the rules that keep the data honest, are in the plan file's §3. The
short version:

- **Only what is printed.** A room with no dimension on the plate gets none.
  The network handles a missing value; a guess corrupts it.
- **Adjacency is direct access** — a door or an open wall, not a shared wall.
- **Feet, not metres**, converted once at load.
- Areas follow from the dimensions; do not store both.

## What a record holds

Beyond the room list, each record carries a `massing` block: how the building
stands (detached, semi-detached, terrace, row, flats), the shape of its plan,
the form of its roof, storeys inside the roof, the stated frontage and depth
where the source gives them, the allotment frontage it is sold for, and the
features on the outside - porches, bay windows, conservatories, areas, vaults,
gables, a granary loft, a clock pediment.

This is here because the programme drives the *exterior* as well as the
interior. The paper took the footprint as given; here `Footprint` is a variable
of the network, so the corpus has to carry what the elevation shows and not
only what the plan does. A pair of gabled villas with a conservatory on the
flank is a different building from a hipped labourer's cottage of the same
floor area, and nothing in a room list says so.

Two conventions, both deliberate:

- **A stair reaches the landing above it.** The specifications say so - "the
  stairs lead to the bedroom floor, which contains" - so where a record has
  both and the link was not written down, it is written down.
- **The yard, the area and the court are outside.** They are rooms of the
  record because the plates label them and the back door opens onto them, but
  they are not floor area, and a room reached only from them is still reachable
  because you can walk round the outside.

## What the loader refuses

`loadProgrammeCorpus` is strict, because a typo in hand-written training data
is a silent corruption rather than a crash. It rejects an adjacency naming a
room that is not there, an adjacency from a room to itself, a room with no use,
a massing feature with no kind, and - the one that has actually caught
something - two rooms sharing a key, which makes every door meant for either
land on the first.

## Rules the book states about plans

Worth having in front of you when the network's numbers are chosen, because
these are the corpus arguing about its own structure rather than us inferring
it from counts.

**The service side is shut off by a door.** PDF page 277: "The living
apartments for the family should be kept as distinct as possible from the
kitchen offices, and in most of the designs we have given, they are separated
by a door. The advantage of this arrangement must be very obvious; all noises
and disagreeable odour are cut off, and the comfort of both the family and the
servants is increased; draughts are lessened and warmth is more readily
retained. Where a door cannot, from peculiarities of plan, be conveniently
used, it will be as well to establish a current of air ventilating the kitchen
from the main portion of the house."

**Servants should not have to cross the polite side.** The Italian villa of
page 474 puts a lobby under the stairs because it is "convenient to the
servants going from the Kitchen to the Dining Room"; the suburban villa of
page 505 places the coal cellar and water closet "so as to be accessible
without entering the Kitchen department"; the eight-roomed house of page 526
brings the tradesmen's stair down into the area and on to the garden.

**A private suite is one door deep.** Page 474 again: the bed and dressing
rooms, boudoir and water closet of the proprietor and his lady "are separated
by a door, leading into their passage from the other portion of the house. A
desirable privacy is thus obtained."

**A simple outline is a cheaper and drier building.** Page 502: "The simply
oblong form of the house, without breaks and recesses has many advantages. It
is always the most economical, and allows of the simplest possible kind of
roof. Where also there are many angles to a building, damp is more likely to
be retained, as the sun cannot shine so equally on the walls, and the angles
are apt to retain moisture."

**A tower should do something.** Twice, and both times as a criticism of
towers that do not - page 474: "the tower is a prominent feature, but, as will
be perceived, it is not a mere adjunct, but forms an intimate portion of the
mansion itself"; page 505: "not, as is too often the case, a comparatively
useless adjunct, but serves to contain an open and well lighted staircase,
which, in general, should be its use in houses of this description."

**The minimum for a labouring family is five rooms.** PDF page 214: "the
minimum of accomodation as a living-room, scullery and three bed-rooms; one
for the labourer and his wife, another for the boys, and a third for the
girls."

## Rules the book states about siting and plots

These are not about a house's rooms but about where it stands, which is the
plot planner's problem rather than the programme's - recorded here because
they come from the same source and there is nowhere better.

**How big an allotment is.** PDF page 121: "An allotment with a frontage of
twenty-five feet, and a depth of one hundred and nine feet, contains ten poles
of land, being one-sixteenth part of an acre."

**What may be built on one.** Same page: "It is advisable to provide a
sufficient guarantee, by covenant in the deed of conveyance, that no
continuous rows of houses be erected; none but detached or semi-detached
erections should be permitted. By this precaution a pretty and pleasing little
village may be formed, no matter what the shape or position of the ground."

**How they face each other.** Same page: "It should also be a condition to be
insisted upon, that no building be erected immediately opposite to another,
but that it face the opening between the houses" - by alternating the
divisions of the allotments on each side of a road, so that the left-hand
party fence faces the centre of the opposite allotment. And "at the corners of
roads the allotments should be larger, giving opportunities for the erection of
buildings for the purpose of trade."

**Which way a house should face.** PDF page 113, quoting Repton: "a southern
aspect is most desirable; but few are aware of the total difference in the
effect of turning the front of the house a few points to the east or to the
west of the south; because, although the south-east is the best, yet the
south-west is the worst of all possible aspects, for this reason, that all
blustering winds and driving rains come from the south-west." And the rule
behind it, from Mr. Carr of York: "to build a house we had only to provide all
that was wanted and no more; then to place the best rooms to the best aspects
and the best views."
