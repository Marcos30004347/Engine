# Living city and kingdom history

## Objective and boundaries

Generate a reproducible local history in which named people form families, migrate,
work, inherit offices and property, and collectively change a settlement from farms
and a manor into a town or city. The resulting buildings, walls, roads and terraces
must explain that history and remain physically accessible at every saved date.

This is a small settlement simulation, not a Dwarf Fortress world simulation.
Start with one focal city, its agricultural hinterland and a few abstract neighboring
settlements. Neighbors supply trade and migration without requiring full meshes.
Growth is conditional: an impoverished or isolated settlement can stagnate or shrink.
An elapsed year alone must never unlock a castle, wall or stone street.

## Existing systems and integration points

- `assets/graphrewrite/story.graphwrite` and `StoryRunner` already supply weekly
  graph matching, births, marriage, ancestry, death, dynasties, succession and conflict.
  Reuse these rules rather than introducing a second demographic random simulator.
- `WorldSettlementDirector` has households, tenancy, employment, building programmes
  and capacity-aware allocation. It currently generates aggregate population; the
  history adapter will supply real named households instead.
- `WorldCityDemand` derives programme/land demand and agricultural support.
- `WorldCityGrowth` currently describes standing, absorbed and planned wall envelopes;
  it is not an execution history. Future wall envelopes must not appear as built walls.
- `WorldCityBase`, relief planning and completion reserve terraces, stairs, circulation
  and lots before geometry. Historical projects must go through the same clearance checks.
- Castle compound planning and dwelling blueprints supply the actual structures.

## State ownership and simulation contract

The story graph owns people, ancestry, relationships, dynasty membership and offices.
The economic ledger owns jobs, money, inventories, household budgets and projects.
The spatial ledger owns parcels, buildings, roads, walls and their version lifetimes.
Adapters expose facts to graph rules, but only one system writes each fact domain.
A rule proposes a city action; it cannot directly insert arbitrary building geometry.

Use graph node IDs for social identities, tagged stable IDs for other entities, and
an explicit mapping across domains. Zero is a valid graph ID. Missing references are
optional values. A dynasty is an extended lineage, a household is a domestic/economic
unit, and a tenancy is occupancy of a specific building. Do not conflate them.

Use the existing absolute weekly clock (52 weeks/year). Observe social changes every
week; budgets and project scheduling can run monthly, strategic review yearly. Process
in a fixed order: graph step -> validate social facts -> household changes -> production
and consumption -> labor/tenancy allocation -> project proposals -> reservation and
commit -> publish next week's graph facts. Newly completed capacity cannot pay for its
own construction in the same tick. All externally visible changes commit atomically.

Every event ultimately needs stable sequence ID, time, actors, location, cause/rule,
before/after references and schema version. Preserve dead and emigrated people, old
buildings and retired roads. Save initial conditions, rule/config hashes, seed and
checkpoints; replay must never depend on unordered iteration or wall-clock time.

## Phase 1 — Social history bridge and demographic demand

**Implement first.** Read the existing graph without modifying its behavior. Track
people, birth dates, parents, spouses, dynasty, residence, life stage and political
role. Observe exact weekly deltas for birth, death, migration, relationship changes
and succession. Include vacancies between rulers. Produce annual inspection snapshots
and provisional demand using living residents only.

Initial life-stage policy: under 12 dependent, 12–17 apprentice-age, 18–64 working-age,
65+ elder. These are configurable game policies in the later labor phase, not historical
claims. Working-age capacity is not proof of a job; apprentice-age is not an assignment.
The first bridge groups co-resident dynasty members as **provisional demand cohorts**.
These are explicitly not final households, tenancies or employment records.

**Gate:** graph node zero works; backward/forward indexes give the same projection;
required schema omissions and contradictory facts fail transactionally; ancestors
survive death and migration; invalid chronology/cycles fail; no false founding births;
weekly gaps fail; identical runs produce identical JSON excluding timing. Run actual
story assets, not only synthetic fixtures. Missing matcher rows are a fatal diagnostic.

**First implementation scope:** `WorldCityHistory`, `CityHistoryTests` and
`CityHistoryPreview`. Physical city mutation is deliberately gated on subsequent phases.
The initial event stream is social observation, not yet a replayable economic event store.

## Phase 2 — Explicit households, family trees and migration

Add household IDs independent of dynasty. Seed domestic units from authored founders;
marriage may merge, split or leave households unchanged. Children join a guardian's
household; adult children can remain, apprentice elsewhere or establish a new home.
Guardianship handles orphaned children and dependent elders. Preserve both biological
ancestry and domestic membership; adoption must not rewrite the parent tree.

Generate incoming households at named origin settlements with persistent identities
and known or explicitly unknown ancestry. Moves transfer residence and employment,
not destroy/recreate characters. Represent homelessness and waiting lists. Jobs, food,
safety, kinship and available housing influence migration; emigration is possible.
A population cap is a performance guard, not a hidden death multiplier in city mode.

**Gate:** each living local person belongs to exactly one domestic unit; no person
occupies two settlements; death releases occupancy; births/deaths/arrivals/departures
reconcile population exactly. Multi-generation scenarios cover marriage, widowhood,
guardianship, split households, return migrants and extinction. Test no forced growth.

## Phase 3 — Livelihoods, status and local economy

Provide a small occupation catalogue: farmer, laborer, carpenter, mason, smith,
merchant, innkeeper, guard, cleric, administrator and domestic/service worker.
Every adult has an explicit livelihood state: employed, self-employed, caregiver,
unemployed, unable to work or retired. Every child has a dependency/education state;
apprentices require a mentor slot and never count as a full worker. Model occupation,
social class, political office and household wealth separately.

Create workplaces with finite slots, skill requirements, input needs and production.
Use simple authored units for food, timber, stone, tools and coin. Farming and food
storage support population; construction consumes labor and stock. Assign work using
skills, travel access and wages; self-employed farmers still need accessible land.
Introduce taxes and household upkeep, with a subsistence fallback and explicit deficits.

**Gate:** no negative inventory or double-spent work; employment never exceeds slots;
workers are alive and eligible; apprentices have mentors; commuting uses reachable
paths; food accounting balances. Test unemployment, harvest failure, trade isolation,
no labor, no stone and large immigration. Economic coefficients are game tuning data.

## Phase 4 — Ruling family, ownership and decisions

Read realm offices and succession from graph rules. Define a configurable succession
policy (initially adult heir, then regency/election fallback); retain regents and claimants
as distinct offices. Do not infer civic rights from a settlement's visual size class.
Assign property, rents and public treasury; inheritance transfers claims separately
from residence. A ruler may commission a keep, walls, market or road, while households
commission homes and merchants commission shops/inns.

Expose need, affordability and safety facts to Graph Pattern Matching. Rules emit
project intents with reasons, sponsors and priorities. Resolve competing claims and
budget reservations deterministically. Conflict changes priorities and resources;
avoid making violence necessary for growth.

**Gate:** one active holder per exclusive office; no dead claimant owns a live office;
succession/inheritance conserve property and money; minority/regency and no-heir cases
terminate; unaffordable intentions remain unbuilt. Rule firing order cannot double-book
a parcel or spend a treasury twice. Existing story regression tests remain green.

## Phase 5 — Persistent parcels and construction lifecycle

Plan terrain and a circulation backbone first. Reserve future growth envelopes, water,
farmland, access and stair courts; build only the initial occupied subset. Stable parcel
and building IDs survive changes to style, floors and neighboring development.

Projects have proposed, reserved, funded, building, completed, suspended, cancelled and
retired states. A completed project publishes capacity only after spatial validation.
Dwellings evolve through repair, extension, added floor, rebuild and replacement.
A rebuild relocates residents and workplaces before demolition. Rejected plans release
reservations without destroying the old structure. Keep ruins or archaeological traces
as optional render states, not overlapping active meshes.

**Gate:** untouched buildings keep IDs, footprints and deterministic geometry hashes;
no occupied building is demolished without relocation; projects consume resources over
time; cancelled work releases reservations; lot overlap, entrance and stair tests pass.
Snapshot before/after a rebuild must show exactly one active version at each date.

## Phase 6 — Historical roads, alleys and vertical expansion

Give routes stable IDs and lifetimes. Footpaths can widen into lanes, then paved streets
when traffic and budget justify it. Surface changes preserve route topology unless a
separate rerouting project succeeds. Introduce stairs inside blocks, Z flights in
retaining walls, bridges and raised districts as independent funded structures.

Select connected buildable terraces from terrain and existing access rather than raising
random occupied lots. Reserve full flight/landing/headroom envelopes before subdivision.
Higher lots cannot be occupied until their access is complete. Roads may close only
after required access is preserved or replacements open; allow abandoned lanes and
courtyards to explain irregular blocks. Do not regenerate the entire city each year.

**Gate:** graph reachability from every occupied home to work, food/water and an exit;
no stair-solid intersection; slope/riser/landing limits; bridge headroom; roads and lot
entrances at matching elevations. Test hillside/ridge/valley and tapered/staggered/broad
layouts across fixed seeds. Reject an isolated district rather than silently teleporting.

## Phase 7 — Manor, castle and defensive expansion

Use demand, prestige, threat and available labor to evolve a manor into fortified manor,
keep, curtain enclosure and larger castle. Preserve an old core where possible, then
attach halls, service wings, yards, gardens and new towers. Use compound blueprint
connectivity, real stairs and tower-to-wall passages; decorations never substitute for
access. Gardens/fountains compete for land, water and upkeep.

Construct a palisade or initial wall in funded segments, with gates aligned to roads.
Plan an outer circuit when occupied suburbs and threat justify it. Complete gates and
access before retiring inner defenses. Retained inner walls become district boundaries,
walkways or ruins; demolished sections become recorded projects. Incomplete walls are
not reported as closed defensive circuits.

**Gate:** wall closure and gate connectivity; no route severed by wall construction;
tower floors/doors align; castle rooms connect; all entrances reach public circulation;
foundation and terrain clearances pass. Test partial construction, no funds, abandoned
outer walls and wall-attached keeps. Render orthographic and perspective comparisons.

## Phase 8 — Replay, inspection and player-facing history

Add versioned checkpoints plus event replay, timeline scrubbing and incremental mesh
updates. Inspect a person for family tree, home, job, employer and rank; inspect a building
for occupants, sponsor, builder and previous versions. Explain why a project was approved,
blocked or abandoned. Offer population/jobs/food/treasury charts and spatial overlays.

**Gate:** direct simulation to year N equals checkpoint+replay; save/load resumes with
identical future hashes; scrubbing does not mutate history; lineage links remain valid;
no future buildings leak into older renders. Compare top/front/back/side/perspective
at founding, growth, wall expansion and late-city dates, plus interior access checks.

## Phase 9 — Long-run balancing and performance

Run a seed matrix over 25, 100 and 200 years, population targets from dozens to several
thousand, benign and constrained resources, different terrain and political policies.
Batch aggregate economics but preserve named social identities. Keep full meshes out of
weekly simulation; rebuild only changed structures at requested dates. Measure matcher
rows, event memory, snapshot size, project solve time and geometry cost before setting
budgets. Use the CPU matcher as reference; GPU acceleration is a separate equivalence gate.

**Gate:** no invariant failures or unnoticed matcher overflow; bounded queue growth;
repeatable hashes; population/resource balances; incremental vs full rebuild equivalence.
Publish timings and supported limits rather than assuming city-scale performance from
small story fixtures. Include stable villages, declining towns and growing cities.

## Delivery order and acceptance policy

Advance only after each phase's automated gates pass; retain fixtures as regressions.
Phases 2–4 establish the reason and resources to build. Phases 5–7 make those decisions
physical. Phase 8 makes the result inspectable. Phase 9 determines supported scale.
Geometry phases also require rendered review; a pretty image alone does not prove
reachability, and a graph path alone does not prove a stair is free of geometry.

All nine phases are implemented on the terraced city base. What the preview shows is
built from projects, crews and stock over weeks; nothing appears because a year passed.
The perspective renders come from the existing city model builder, whose house fitter
still rejects some terraced lots.

## Running the first slice

```sh
cmake -S . -B build
cmake --build build --target CityHistoryTests CityHouseholdTests CityEconomyTests CityRealmTests CityFabricTests CityDefenceTests CityChronicleTests CityHistoryPreview -j6
ctest --test-dir build -R '^City' -E Integration --output-on-failure
./build/tests/CityHistoryPreview assets/graphrewrite build/city-history.json 10 7 --renders build/renders/city-history
python3 tools/terrain/render_city_model.py build/renders/city-history/Wealdholt-year10.json build/renders/city-history/Wealdholt-year10
ctest --test-dir build -R CityHistoryIntegration --output-on-failure   # ten minutes, replay-verified
```

`--renders DIR` writes, for every site at years 0, 3, 6 and the last, a plan view
(`<name>-year<N>.svg`: terraces, parcels, ways by surface, stairs, what stands, walls,
gates and towers) and the mesh the existing render script reads (`<name>-year<N>.json`),
built from the completion plan of that date.

The preview runs every week, records social changes, and writes annual named snapshots
with provisional population/land demand. Its seed controls the existing story rules;
it is not yet a city-growth balancing preset. The output's schema and scope fields
identify these limitations for downstream tools.

The integration gate compiles the real story once and runs two independent ten-year
simulations with `--verify-replay`, comparing their complete serialized outputs. The
rule compiler is expensive in this development build, so that test has a ten-minute
process budget. It does not turn off matcher overflow checks or skip quiet weeks.
Large dynasty cohorts are split into estimates of at most eight residents for the
existing demand API; these estimates must not be used as persistent households.

### Measured integration risk

A development-build process sample placed the preview's startup time in
`PatternMerger::searchOverlap`, during compilation of the existing story rules,
not in the history adapter. Before growing the rule catalogue, measure compilation
separately from weekly simulation and add a versioned compiled-program cache keyed
by rule content and compiler version. Cache equivalence and invalidation need their
own tests; increasing a timeout is not a city-scale performance solution.

## First implementation delivery

The social bridge, weekly ledger, capacity-sized provisional housing estimates and
JSON preview are implemented. The focused suite passes 20 checks. The integration
suite passes two independent ten-year simulations of the real story rules with
identical serialized output. Seed 7 produces 11 annual snapshots, 36 known people,
11 births, 24 deaths and 114 social changes. It is a declining narrative fixture,
not a growth-balanced city preset.

The existing `GraphRewriteStoryTests` regression suite also passes with this bridge added.

## Second implementation delivery (phase 2)

`WorldCityHouseholds` owns domestic units: `CityHouseholdLedger` seeds one household
per co-resident authored house (a priest of no house lives alone), then applies a fixed
weekly order over each validated snapshot: deaths and departures, moves, arrivals and
births, marriages, optional adult independence, guardianship repair, extinction, then
housing. Households have tagged stable ids independent of dynasty, a head (the oldest
working-age member, not the story's `is head of`), an origin kind (founding, local,
migration, unknown) and a housing state. A settlement with a `housingCapacity` policy
keeps a waiting list in service order; death and relocation release homes. Marriage
merges two heads, moves a non-head (with their dependent children) in with a head, or
founds a new household when neither is a head. Orphans go to a parent, grandparent,
adult sibling, then a household headed by their own house; otherwise the household is
reported `unguarded` and retried weekly. Membership never touches the parent tree.

`WorldCityMigration` is the single writer of residence. `CityMigrationAdapter` names
origin settlements as graph nodes, authors incoming households there (a couple with
children, a childless couple or a single adult, with the same edges a founder carries),
then moves them by rewriting `lives in` the following week, so the social ledger
records an arrival at the origin and a move into the city. Work, food and safety are
authored prospects until phases 3 and 4 compute them; arrivals need a free home or a
short waiting list, waiting past `patienceWeeks` emigrates a household to a named
origin, a bleak destination pushes its newest household out, and a household abroad
with kin returns before strangers are invited. `maxLivingPeople` suspends arrivals;
nothing in the city kills anyone. `StoryRunner::mutableGraph()` is the only new
runner surface; the story rules are unchanged.

`cityPopulationReconciles` closes the gate every week: residents equal the previous
week plus births, arrivals and moves in, minus deaths, departures and moves out.

`CityHouseholdTests` passes 44 checks across marriage, widowhood, guardianship, split
and whole-household moves, return migrants, extinction, gap rejection, independence
with and without a free home, migration in and out of a synthetic graph, the
population guard, a bleak destination, missing schema, and identical event logs on
repeated runs. The integration run (`CityHistoryPreview ... 10 7 --verify-replay`,
schema version 2) is byte-for-byte deterministic and records 88 people, 264 social
events, 228 household events and 27 migration events; `--no-migration` restores the
phase 1 behaviour. Both preview policies are printed into the output.

Not done here, by design: apprenticeship elsewhere (phase 3), housing as real
dwellings (phase 5), and graph rules that emit migration intents.

## Third implementation delivery (phase 3)

`WorldCityEconomy` owns jobs, workplaces, stocks, coin, household budgets and deficits.
The catalogue is the eleven occupations of the plan, each an authored profile (skill
required, monthly wage, inputs and outputs per worker, public service, needs land, needs
trade, clergy only). Every living adult holds one livelihood state (employed,
self-employed, caregiver, unemployed, unable to work, retired) and every child a
dependency state (dependent, apprentice); occupation, social class, political office and
household wealth are separate fields. Weekly, in a fixed order: sync people with the
snapshot (release the dead, the absent, the retired, the unable and anyone whose
workplace is no longer reachable), name one caregiver per home with an infant and two
adults, trim overfilled workplaces, fill an owner's workplace from the owner's own
household, hire the idle by skill, wage and workplace age (a priest serves before he
farms), bind apprentice-age children to a master with the skill and a free mentor slot
(a parent first), recompute class. Monthly: production from the owner's purse or the
settlement store, inputs bounding the run so nothing goes negative; wages from the owner
or the treasury with unpaid wages recorded; household consumption from the purse, then
purchases, then the store's subsistence ration, then explicit hunger; sales of surplus
above a reserve; tax by an administrator's presence; upkeep; and trade where open, which
is the only way coin enters or leaves. Yearly: skills grow. Commuting is an authored
route set until roads exist. `facts()` gives the work and food prospects migration reads.

`CityEconomyTests` passes 31 checks: slots never exceeded, no double work, workers alive
and eligible, apprentices with masters and re-bound when one dies, no commuting without
a route and none while local slots are free, harvest failure to rations to hunger,
closed trade, no labour, no tools so no stone, retirement at sixty-five, an unable
adult, a thirty-person immigration, and food and coin balances checked after every
month: food in the ledger equals the founding stock plus produced minus consumed and
exported plus imported; coin equals grants plus trade in minus trade out minus escrow.

## Fourth implementation delivery (phase 4)

`WorldCityRealm` reads the crown, consort, heir, pretenders and `wants a king` from the
graph (the phase 1 snapshot now carries these and the feuds, all optional so the
minimal-schema tests stand) and adds what the story does not say. A configurable
succession policy names a regent when the ruler is under the majority age or the throne
is bare: the adult consort, then the eldest adult of the ruling house, then the head of
the largest household at the seat; failing all three the realm is recorded as having no
authority, once, and stays so until something changes. Claimants and regents are
distinct offices; a dead person holds nothing. Property is the workplaces with a person
as owner: on death the eldest living child (adults first), then the spouse, eldest
sibling or a parent inherits, else the workplace escheats to the treasury; a household
dying out passes its purse the same way. Every transfer moves a whole stock between two
named holders, so property and coin are conserved.

Decisions are deterministic rules over exposed facts, reviewed monthly: a household
waiting for a home asks for one; a merchant, innkeeper or craftsman head asks for a
shop, inn or workshop; the crown, when someone can act for it, asks for a market, a
keep (threat lowers the bar and raises the priority), a wall behind a funded keep, a
road for its merchants and a church for a priest without one. Feuds move priorities.
Proposals resolve by priority, then treasury before households: a named site is booked
once and a second claim is cancelled as "site taken"; funding is one atomic
check-and-debit of the sponsor's stock into the project's escrow, so a treasury is
never spent twice and an unaffordable intention stays proposed with a reason a player
can read. A housed household's home project, or a dead household's project, is cancelled
and its escrow refunded or escheated. `propose()` is the entry a future graph rule can
use; emitting intents from the story rules themselves is deferred because the runner
executes one compiled programme, and the story assets are unchanged.

`CityRealmTests` passes 23 checks: offices from the graph, a vacancy with a consort
regent, an adult heir ending the regency, a child king with the consort back, election
of the largest household's head, claims raised and dropped, no authority terminating,
inheritance of a farm, escheat of an heirless purse, a home blocked while unaffordable
and funded once it is not, cancellation with refund on housing, one site and one
treasury under any proposal order, and identical event logs on repeated runs.

The ten-year integration run (seed 7, `--verify-replay`) is byte-for-byte deterministic
on the real story with all four ledgers: 93 people, 292 social, 248 household, 27
migration, 3314 economy and 445 realm events. It also caught one defect the synthetic
suites missed: a master trimmed from an overfilled workplace, made a caregiver or
retired kept his apprentice bound; releasing an adult now releases their apprentices.

Measured, not tuned: with the preview's founding grants (80 coin per treasury, ten
public slots at 3 to 6 coin a month, tax on incomes too small to round to a coin) both
treasuries are empty by the third year, unpaid public wages and hunger accumulate,
and every proposal stays blocked as unaffordable. The mechanisms are exercised and the
balances hold; the coefficients are phase 9's work, as the plan says, and a preview
that looks prosperous would have been a lie.

The preview (schema version 3) founds a farm per authored house and one public
workplace per trade in each settlement, prints its economy and project-cost policies,
and now feeds migration the computed work, food and safety instead of authored numbers.
The existing story regression suite stays green: the only runner change is the mutable
graph accessor.

## Fifth and sixth implementation delivery (phases 5 and 6)

`WorldCityFabric` is the spatial ledger. A settlement is founded on a terraced city
base (`planCityBase` then `populateCityBase`): terrain, the circulation backbone, stair
courts, reserved envelopes and their lots come first, and only the founding homes on
the first terrace are occupied. Every lot is a parcel with a stable id, a level and the
way it fronts; every way is a route with a surface (footpath, lane, street), an opening
week and a possible closing week; every stair, deck and bridge is an access structure
whose state gates occupancy: the approach and the first terrace are complete at
founding, the stairs above are planned. Buildings carry versions with non-overlapping
week intervals and a deterministic geometry hash over footprint, base, floors and use.

Construction consumes a funded project: materials leave the escrow when the works
start, coin follows the work month by month into the crew's households (labourers,
carpenters, masons employed at the site), works with no crew are suspended and resume
when one returns, and a project never finishes the month it starts. Completion runs the
spatial checks against the world as it stands, the old version excluded: the footprint
stays in its parcel and overlaps nothing standing, the entrance is within the doorstep
tolerance of its way, and the terrace's stair is finished. Only then is capacity
published: a home raises the household ledger's stock, a shop, inn, workshop, farm,
market, church, keep or manor opens a workplace. A rejected completion cancels the
project with the reason, refunds the escrow and releases the parcel; a project
cancelled from outside is swept the same way. Tenancy maps housed households to
specific homes and never exceeds them. A rebuild relocates its tenant to a vacant home
first, blocks with a readable reason when there is none, replaces the version in one
step and brings the tenant back; a repair or added floor is a new version on the same
footprint. Homes ask for their own repair, extension and rebuild by age and crowding.

Phase 6 lives in the same ledger. The walkable network is the open ways split at every
junction and crossing, the finished stairs, and the parcels and court hung off them.
Every week each commuter's shortest path from home to workplace leaves traffic on the
ways it uses; a footpath busy enough for long enough wears into a lane; a road project
paves the busiest way into a street. A home cannot be started on a terrace whose stair
is planned: the fabric asks the crown for that stair and the home waits with the
reason. A way may close only if every occupied home still reaches the court, the exit
and its tenants' work, and a refusal is recorded. The gate checks run every week: no
overlap, one standing version, entrances at way level, stair risers, goings and
flights within the relief planner's limits, bridge headroom, street grade, and
reachability of every occupied home; an isolated home fails the week rather than being
teleported.

`CityFabricTests` passes 36 checks across founding, hashes stable under other changes,
a household home from purse to move-in, crown homes filling the first terrace, the
stair asked for and built, a rebuild with relocation and exactly one version at every
date, suspension without a crew and resumption with coin drawn month by month,
cancellation releasing the parcel, a paved street, one way closed and a severing
closure refused, traffic, identical event logs on repeated runs, and all nine
layout-terrain pairings founding and validating.

## Seventh implementation delivery (phase 7)

`WorldCityDefence` grows the seat's defences in stages the realm can justify: the crown
asks for a hall, then, once it stands and the seat is grown or unsafe, a palisade, then
a keep, then a wall behind the keep; feuds raise the priorities. The manor and the keep
are fabric buildings on reserved parcels (the keep on the highest reachable terrace).
A wall or palisade project plans a circuit around the occupied terraces with a margin,
cut into funded segments, a tower at every corner and at the spacing, and a gate
wherever an open way, the approach or a stair crosses the line, so nothing is severed.
The crew builds segments in order, then the gates and towers on finished segments,
paid from the escrow as the work is done; no crew suspends the works. The circuit is
reported closed only when every segment and gate stands; an abandoned circuit keeps
its finished segments as a trace and is never closed. A stone wall may replace a
palisade around the same terraces, a second circuit around already-enclosed ground is
rejected, and an outer circuit around more terraces retires the inner one only once it
is closed and gated, as a district boundary whose record and project are kept. The
stage follows what stands: manor, fortified manor, keep, curtain, castle (a keep inside
a towered stone wall). Validation checks closure, chaining, gates on their segments at
every current crossing, towers on segment ends, no segment through a standing building
or across an ungated stair, foundation drop, and the keep inside its curtain.

`applyTo` publishes standing segments (as prisms), gatehouses and towers at a date into
a completion plan for meshing. Castle interiors are not solved compounds here: the keep
is a massing with an entrance on circulation, and that limit is stated rather than
hidden. `CityDefenceTests` passes 23 checks: the feud makes the seat unsafe, hall then
palisade then keep in order, segments paid and closed with a gate at the approach and
corner towers, works suspended without builders and the wall not closed, the stone
wall replacing the palisade and the castle stage, the second terrace settled through
the wall's gate, an outer circuit planned then abandoned with the inner one standing,
a second outer attempt closing and retiring the inner wall only then, reachability
after it all, publication for meshing with nothing before the first segment stood, an
unaffordable palisade never started, and identical event logs on repeated runs.

## Eighth implementation delivery (phase 8)

`WorldCityChronicle` makes the whole city a function of three things it can save: the
policies, the founding script (sites, workplaces, grants, skills, hand-made proposals
and a founding purse, as data with the week each applies) and the recorded weekly
social snapshot stream. `observe` records the week and runs every ledger in the fixed
order; `replay` rebuilds fresh ledgers from the stream; `save` and `load` round-trip
all three through JSON; `hashOf` digests every ledger's event log, the coin in the
ledger, the households and every building version. `at(week)` scrubs the timeline
from the version intervals, route lifetimes and wall completion weeks the ledgers keep
and mutates nothing. `inspectPerson` gives parents, children, spouse, house, household,
home, livelihood, employer, mentor, class and offices; `inspectBuilding` gives versions,
tenant, occupants, sponsor, builders and workers; `explainProject` gives the intent,
the state, why it is blocked and its dated timeline across the realm and the fabric;
`series` keeps a yearly row per settlement for charts.

The story runner has no save. A resumed run re-simulates the story from its seed,
which is deterministic, and feeds the chronicle from there; the chronicle itself
resumes from its stream. `CityChronicleTests` passes 16 checks: sixty weeks direct,
replay equal hash for hash, partial replay stopping early, save and load to the same
hash, a checkpoint at week thirty resumed alongside the uninterrupted run to the same
future hash, scrubbing without mutation, no future building at an older date and
nothing before the founding, the people alive at a date, every lineage link resolving,
and inspection of a king, a married son, a home and a crown project.

## Measured on the real story after phase 8

Seed 7, ten years, both settlements founded on three-terrace bases (Wealdholt a tapered
hillside, Stanmere a staggered ridge): 79 people, 244 social, 244 household, 23
migration, 2950 economy, 591 realm, 33 fabric and 2 defence events; the checkpoint
replays and reloads to the live hash (7258116317085626212 over 521 weeks). Each seat
raised its hall from the crown's founding purse and reached the manor stage; after that
the treasuries were empty, every later intention stayed blocked as unaffordable, and no
palisade, keep or wall was started. That is the phase 3 balance, not a spatial fault:
the synthetic suites drive every stage to a castle and an outer circuit. The mesh
renders at years 0 and 10 show the terraces, the founding homes and the hall; the
house fitter rejects some terraced lots, as it did before this work.

Not done, by design or by limit: phase 9 (balancing and scale), castle interiors as
solved compounds, and graph rules that emit intents. A 2.9-minute ten-year run is
almost entirely the story matcher; every city ledger together adds under two seconds.

## Ninth implementation delivery (phase 9)

**Scale without the matcher.** The story matcher is the reference social history and
the cost that grows with the cast. To measure and balance the city ledgers at hundreds
and thousands of people over centuries, `WorldCitySociety` produces the same weekly
snapshot the story does from a small authored demographic model: named people with
parents, houses, spouses and settlements; births, marriages that forbid siblings and
parents, age-graded deaths, a crown passing to the eldest son or a sworn house, feuds
that heal, whole households leaving, and newcomer households drawn by the pull the
ledgers report (work, food, safety, a free home or a short waiting list). It is a
scaling and balancing fixture and says so in its header; it is not the game's social
simulation. `WorldCityScale` is the matrix: a village (six houses, newcomers only into
an empty home), a declining town (twelve houses, high mortality, emigration, nobody
coming) and a growing city (two sites on eight-terrace bases, strong immigration).
`CityScaleBenchmark MODE YEARS SEED [--repeat] [--no-stream] [--json OUT]` runs a cell
and reports population, homes, walls, stage, events and their bytes, the stream's
bytes, seconds per ledger, the slowest week and the hash; `--repeat` runs it twice and
the hashes must agree; `--no-stream` keeps memory bounded by not recording the stream.

**What the first measurements found, and what changed.** Every ledger copied its whole
event log each week for the transactional update (quadratic in run length) and the
fabric copied its base plans: logs are now shared between a ledger and its weekly copy
and cut back to a mark on failure, and the ground is a shared immutable plan. The
realm scanned every project ever proposed each review and every ledger copied that
map several times a week: closed projects now move to a shared archive consulted by
id. The fabric searched a path per commuter per week: it now searches once per
destination, monthly, and validates reachability with one search from the court, one
from the exit and one per workplace. Two accounting leaks broke the balances: an inn's
food input and coin output were untracked, and are now consumption and trade income.
Then the balance itself. Farmers hoarded thousands of food beside starving neighbours
because the store had no coin to buy and the hungry none to pay: households now buy
food from a neighbour's surplus directly, and a neighbour's surplus beyond twice his
reserve feeds a household under the subsistence line as charity. Nobody could build a
home because nobody sold timber: a household funding a project now buys the materials
it lacks from the store. Coin pooled in savings and never came back to whoever paid
wages: upkeep is per person, a tenth of savings above a reserve is spent at the market
monthly, a wealth tax joins the income tax, the store pays dues to the treasury above a
float, and one realm has one treasury at its seat. Emigrants kept their farms at home
and the hands they left worked for a purse abroad: a workplace whose owner is not
resident runs for the market. Hands worked unpaid for insolvent owners: they quit and
do not return for a season. The crown drained its purse on a keep before a stair and
built one home at a time beside a hoard: it commissions homes in proportion to those
waiting and a farm or a workshop when adults stand idle, and merchants import timber,
stone and tools below a floor. Villages carried thirteen crown-paid posts for twenty
people: two houses in three farm their own land, the third keeps a workshop, and only
the yard, the watch and the clerk are public.

**Gates.** `CityScaleTests` runs all three modes for twenty-five years: every weekly
gate of every ledger holds, the hash repeats, coin is conserved and food balances over
the whole run, every resident is in exactly one household, waiting lists and open
proposals stay bounded by the households, the village stays within its band with no
hunger, the town declines, the city grows in people, homes and newcomers, and
rebuilding only what changed between two dates equals a full rebuild at the later one.
The long matrix and its timings are recorded below.

## Working lives

A working life now has a shape. An apprentice-age child binds to a master with the
skill and a free mentor slot, a parent first, and the ledger remembers whether the
trade came from a parent or a stranger; the apprentice's free work counts as one
worker in two at the master's bench and is never paid. At eighteen, after enough years
under a master, the youth graduates a journeyman with the trade's skill and goes back
to his master's shop for wages before any other; cut short, he leaves as he came. A
journeyman with the years and the skill is recognised a master. A journeyman or master
who keeps no shop of his own resolves to (an ambition the inspection shows), and once
his household can pay the better part of it and the trade has no idle slot in the
settlement, his household asks for the shop, inn, workshop or farm in his name; when
the fabric raises it he leaves wage work to run it, and it is his. A head who already
keeps a shop of his trade asks for no second. `CityCareerTests` follows a smith who
teaches his son and a neighbour's boy for free work, both graduating, the son marrying,
mastering, saving and opening a forge of his own from his own household's project.

### Measured: the scale matrix

Debug build, one core, `--repeat --no-stream`; every cell's hash repeated and every
weekly gate held; coin conserved and food balanced over the whole run in every cell.

| Cell | People | Homes / buildings | Walls, stage | Hungry months | Wall time | ms/week | Slowest week | Events (MB) |
|---|---|---|---|---|---|---|---|---|
| village, 100 y | 18 to 29 | 8 / 13 | 1, keep | 0 | 3.9 s | 0.76 | 1.8 ms | 39 702 (2.5) |
| town, 100 y | 43 to 1 | 12 / 16 | 1, keep | 0 | 5.3 s | 1.02 | 16 ms | 40 109 (2.4) |
| city, 100 y | 79 to 1 998 (cap) | 377 / 588 | 2, keep and fortified manor | 0 | 515 s | 99 | 601 ms | 2 607 964 (150) |
| village, 200 y | 18 to 53 | 17 / 35 | 1, keep | 0 | 22 s | 2.1 | 12 ms | 170 985 (11) |
| city, 200 y | 77 to 854 | 250 / 453 | 1, manor and fortified manor | 0 | 750 s | 72 | 298 ms | 3 332 974 (200) |

Where the week goes at two thousand people: the fabric takes 327 of 515 seconds (its
weekly validation is quadratic in standing buildings and its reachability search grows
with the network), the realm 55, the economy 30, the defences 30, the households 25,
the social ledger 8. Two thousand people on two eight-terrace sites is the published
limit of this build; beyond it the fabric's per-week checks need spatial indexing and
an incremental validation, which is the next performance work. The declining town runs
to one resident in a century: high mortality and a household leaving every year with
nothing coming in is extinction, and the mechanisms let it happen rather than propping
it up. The growing city leaves a third of the second settlement's adults idle because
its crown works arrive after the newcomers; nobody starves, and the hoard at the seat
(three hundred thousand coin) says the fiscal loop now over-collects at scale, which is
tuning left for the game. The two-century city (seed 7) settles at eight hundred and
fifty people with twenty-four households waiting at the second settlement and six
hundred thousand coin at the seat: the same over-collection, and the same next tuning.
The real-story preview passes its replay-verified integration run after the rebalance.
