# Landform-first fantasy city generation

Reference priority: the user's first image supplies the irregular rocky shelves, lower town, intermediate civic court and summit sanctuary. The second supplies dense housing, defensive depth, gate towers and a dominant uphill keep. The objective is connected, usable architecture, not noise with buildings scattered over it.

## Generation contract

World cells → landform graph → access and water reservations → defensive precincts → streets and blocks → modular lots → dwellings → shared render/collision geometry.

The city base owns slopes, exposed rock and drainage. A dwelling owns its horizontal stone foundation. A retained district owns its deck, retaining wall, stairs and access openings as one transaction. No later stage may overwrite a reserved access volume. Keep the established 1.2 m house cell and 2.4 m structural bay; simplify a small building rather than scale its doors or rooms.

## Phase 1 — coherent cell landforms

Select a population-scaled urban catchment from the existing refined world Voronoi polygons, including their source identity. Do not allow distant agricultural cells to attract the summit. Exclude water and bank reservations. Conform T junctions; build adjacency from actual shared edges rather than nearby centroids. Select a connected settlement component, a lower approach and a defensible summit inside the inner 45% of the urban catchment, with a multi-cell crown sized for the castle precinct and its shoulders. Geodesic distance along the cell graph produces nested, irregular shelf elevations. Several adjacent cells can share an elevation; they are not Cartesian bands. Preserve the underlying world's broad elevation and join the structural uplift to zero at the external boundary. One height on each shared vertex and one shared-edge interpolation must serve all cells.

**Implemented in this change (opt-in):** `WorldCityLandforms.hpp`; site integration before suitability, measured slopes, defense scoring and district allocation; stable cell IDs, shelf and uphill hierarchy exported with the native city. `--landforms 12 --landform-shelves 4` enables the initial integration mode. Larger relief values remain experimental: cell allocation can reject disconnected traversable components until engineered transitions are reserved upstream. The legacy `--hills` modifier is bypassed for this surface. Topological gate roads penalize measured grades along their entire edges, and the summit is a preferred castle anchor. Prepared required fortresses receive their complete minimum precinct before housing; their target allows one additional whole planning cell instead of shrinking cells to hit an arbitrary area cap. Runtime boundary sampling is tightened to 1 m for this mode. The default remains unchanged until the following gates pass.

**Tests:** deterministic output under reordered cells; finite/valid input; transactional rejection; shared edges and T junctions; unchanged exterior and zero-relief mode; monotonic, acyclic summit hierarchy; actual-world preview. The uphill hierarchy is a planning dependency, not a claim that a walkable stair or road exists.

## Phase 2 — landform-aware circulation and drainage

For each required district connection, reserve a full width corridor and choose a contour road, switchback road, stair court, tunnel or bridge. Search candidate routes using distance, earthwork volume and curvature costs, with a hard road-grade bound. Stair routes reserve flights, intermediate landings, headroom and both exits before lots exist. Bridges connect separate high shelves across a lower occupied quarter or water; never over the stairs they serve. A graph spanning tree guarantees access; optional loops improve routes without excessive empty space.

Preserve actual rivers and coastline. Establish downstream water levels before moving land. Reserve channels and spillways, masonry banks, quays, culverts and bridge abutments. Existing automatic flat street canals remain a separate feature until connected to the world drainage graph.

**Gate:** every inhabited shelf has an entrance path; measured grade/tread/landing/headroom limits; no intersection with reserved solids; downstream water monotonicity; water cannot be raised by the landform plan. Test ridge, basin, coast, disconnected islands and narrow saddle cases over at least 20 seeds.

## Phase 3 — explicit cliffs, retaining structures and defenses

Merge equal-level adjacent cells into regions before meshing. Preserve genuine cliff faces as independent closed meshes; do not encode vertical faces in a single-height terrain function. Classify region boundaries as natural rock, gentle shoulder, retained edge, gate, quay or bridge abutment. Reserve each stair wall opening and building setback in the same boundary graph.

Choose nested defensive circuits around the lower city, civic shelf and summit. Gates must coincide with approved access corridors. Wall walks end at tower openings and floors rather than clipping into tower solids. The keep precinct must have enough contiguous area for the full Gothic blueprint (roughly 140 × 120 m for the current showcase); otherwise deliberately choose a smaller keep, not a distorted version. Allocate the summit castle before housing.

**Gate:** watertight support meshes; paired shared boundaries; no walls across stairs; tower floors and door connections align; buttresses stay below supported storeys; no bridge over a stair volume. Reject an entire invalid structure without deleting its lots.

## Phase 4 — compact urban fabric and architecture

Partition the reserved buildable regions into small street-front blocks, courts and alleys. Fit a local rectangular modular grid within each irregular region. Dedicate selected lots to stairs or passages. Build density is controlled by usable frontage and occupancy, not by enlarging every block's setback. Merchants and inns face main routes; houses fill secondary alleys; civic landmarks face courts. Add dormers, steep roofs, arcades and Gothic openings according to building use and available bay dimensions.

**Gate:** compare occupied footprint / usable land, serviced lots and building count against a same-seed baseline. Require at least 90% of the baseline occupied footprint unless a measured infrastructure reservation explains the reduction. All entrance paths and interior minimum dimensions must pass. Existing window, roof, wall, compound and stair regression suites remain mandatory.

## Phase 5 — integration, screenshots and default rollout

Run terrain-only, infrastructure-only and populated native exports. Capture top, front, back, side and perspective views for several site shapes and seeds; include a summit close-up and ground-level stair exit. Audit finite vertices, triangle indices, duplicate faces, openings, terrain tile failures, route reachability and density. Inspect seams at cell borders, tower joins, retaining walls, waterways and building foundations.

**Gate:** no newly failing runtime tiles, no missing occupied district access, no density regression beyond the above budget. Only then make the new mode the full-city default. Do not silently count a partial mesh as a successful full-world build.

## Phase 6 — history on the same spatial model

Keep persistent cell, region, lot, road and building IDs. Record foundations, road paving, wall extensions, demolitions and rebuilt dwellings as dated events. Population, households, work and ruling-family simulation request spatial changes through capacity and access checks. Replay a year without resampling terrain or regenerating unrelated houses. Coordinate with the existing economy/history implementation rather than replacing its state.

**Gate:** deterministic year replay; household/workplace references resolve; migration respects capacity; demolished buildings vacate their lots; surviving routes remain connected after wall and road edits.

## Current limits

Phase 1 and the first route-cost integration establish the earlier planning stage and continuous structural relief. It does not yet implement a cliff boundary mesh, certified graph-edge stair routing, nested defensive circuits, or a guaranteed fit for the largest Gothic keep. Those require phases 2–3; raising amplitude alone is not a substitute. Existing post-layout hill smoothing, local retained blocks and street grading are still downstream, so the opt-in preview is an integration test rather than the final reference-quality city.
