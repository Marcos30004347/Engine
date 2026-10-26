# Moving terrain refinement

The compressed geometry path remains the terrain default, with native atomic64
compute rasterization and G-buffer Lambert lighting.

## Reproduction

The old movement contract travelled 24 m and returned to its initial position.
It did not prove that a filled source cut could refine new ground. The new
forward fixture settles for 400 frames, travels 256 m over 600 frames, and
settles at the destination for 400 frames. It checks visible nearby pages as
well as the page directly under the camera.

Before the fix, this fixture ended with three visible coarse pages within 8 m
(10–20 cm voxel sizes), despite the camera's own page passing the 10 cm ground
contract. Source streaming had stopped at 506 resident chunks with pending
requests. See `/tmp/terrain-vg-fix/movement-settle-before.log`.

Full internal-resolution travelling runs reached the 7 GiB memory guard before
completion. Disabling TAA in the diagnostic command also disabled scaled
rasterization, making those runs 1220x720 internally rather than the normal
610x360. They are not successful refinement or performance results; the normal
scaled configuration completed the trip below the guard.

## Changes

Capacity admission now follows ownership-group error and camera priority
sorting. The capacity publication closure also restores that demand order
before bounded generation. Previously admission consumed slots in the quadtree traversal order,
then sorted only the surviving requests, so it could exclude a nearby page
before considering its priority.

During CPU target convergence, staged pages that no longer contribute to the
requested cut are retired before capacity admission. Their published owners
continue to cover the ground. Previously an unfinished refinement remained
staged whenever its parent was retained, including when movement had made that
refinement obsolete. These hidden pages could hold the transition slots needed
at the new camera location.

The deterministic camera harness accepts `--test-camera-travel-start-frame`
and `--test-camera-travel-frames` to separate initial settling, movement, and
settling at the destination. The movement contract retains the original return
trip and adds both a forward trip captured while still moving and the
forward trip with destination settling. A CPU test also moves a constrained source cut
away from its initial settled near field and requires a finest-level owner at
the destination.

## Validation

- CPU page determinism (including the constrained moving cut) and cut admission
  tests pass. The priority-order regression reverses demand relative to the old
  published cut and checks which new child is generated first.
- The final 5 km run used 1220x720 output, 610x360 internal rendering, 1.5 px VG
  error, and the 7 GiB guard. It warmed up for 400 frames, travelled 256 m over
  600 frames, and settled until frame 1400.
- Final visible profile: four touched pages within 8 m, all 10 cm; zero coarse
  near pages, LOD violations, install failures, exhausted slices, or uncovered
  retired owners. In the ground-facing test view, all 219,600 internal pixels were covered by LOD-0 terrain,
  with zero holes or enclosed gaps. Ground clearance was 1.7 m.
- Last terrain activity was frame 1078, 78 frames after travel ended. Final cut:
  509 source chunks, including 192 at LOD 0; 774 installed VG pages. The encoded
  geometry route retained only the one-element compatibility vertex/index
  buffers. Native atomic64 compute rasterization was active.
- Final logged footprint: 5.42 GiB. This diagnostic run includes readbacks and
  tracing, so it is not a frame-time benchmark.
- Log: `/tmp/terrain-vg-fix/movement-default-scaled-5km-final.log`.
- All five GPU terrain contracts pass: cut error, capacity refinement,
  movement (return trip, active forward motion, and destination settling),
  settle, and LOD profile. Total suite time: 217.39 seconds. The active-motion
  checkpoint at frame 999 also reported four nearby visible pages at 10 cm,
  zero LOD violations, and zero install failures.
- Logs: `/tmp/terrain-vg-fix/movement-gpu-final-tests.log` and
  `/tmp/terrain-vg-fix/movement-cpu-final.log`.
