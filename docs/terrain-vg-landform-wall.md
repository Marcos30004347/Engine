# Continuous world-map terrain heights

The straight wall visible near the default world-map camera came from height
synthesis. `landform_id` is a nearest-filtered categorical map. Runtime terrain
selected one procedural formula from that ID, so crossing a label boundary
switched the entire detail contribution instantaneously. Near source coordinates
(79375, 67333.33), the old Cliff/Hill detail formulas differed by approximately
3.3 m before erosion/coast scaling. The refined elevation field is linearly
filtered; a continuous base height did not prevent this added discontinuity.

Cliff and Plateau detail also rounded broad noise directly to integer shelves.
Those hard quantizers created additional vertical steps inside one landform,
even with the optional terrace layer disabled.

The terrain builder now samples a sparse mixture of up to four neighboring
category IDs and blends their height contributions with Hermite weights. IDs are
never interpolated numerically, and ordinary semantic ID sampling remains
nearest-filtered. Duplicate IDs are merged so uniform regions evaluate a single
formula. Sampling uses the resolved map page and its gutter, including ancestor
fallback, independently of terrain chunk or LOD boundaries.

Procedural shelves use continuous Hermite transitions in place of rounding.
Generator revisions are advanced to invalidate old cached payloads. Compressed
VG storage, rasterization, page installation, and lighting settings are unchanged.

Validation: CPU regressions pass for a Cliff/Hill transition across a map-page
seam, cursor reuse, normalized category weights, and continuity within
Cliff/Plateau terrain. All five renderer contracts pass: cut error, capacity
refinement, movement, settling, and the 1,200-frame LOD profile.

A 480-frame capture using the 5 km launch shows the diagonal wall removed. The
final cut has 510 chunks, 632 installed pages, visible 10 cm detail, no oversized
clusters, failed installs, missing slots, or root repairs. Its final ten frames
have identical output signatures; the coverage diagnostic reports one vertically
bracketed uncovered pixel and zero enclosed uncovered regions. Settled footprint
is about 4.7 GiB. Expanded vertex/index usage remains 0/1, confirming compressed
geometry is active.

Capture: `/tmp/terrain-vg-fix/height-continuity.png`.
Logs: `/tmp/terrain-vg-fix/height-continuity.log`,
`/tmp/terrain-vg-fix/height-cpu-tests.log`, and
`/tmp/terrain-vg-fix/height-terrain-tests.log`.
