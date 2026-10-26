# Terrain / Virtual Geometry Phase 6 GPU Traversal Cut

Phase 6 moves terrain cut selection into the shared traversal model.

The CPU can still install pages and provide producer metadata, but it no longer
needs to publish a separate terrain visible-page set. Traversal starts from
streamed instances and reads the shared page table:

1. choose the desired page for the view/error target;
2. if the desired page is resident, emit it;
3. if the desired page is missing, emit the resident root/fallback page;
4. write a page request for the missing desired page with a view-derived
   priority.

`traverseSparseVoxelStreamedInstancesLOD` is the CPU oracle for the future GPU
pass. It records emitted pages, missing-page requests, and diagnostics counters
from the same page table state that render traversal will consume.

## Camera-driven feedback

Page priority is calculated with `scoreSparseVoxelPagePriority`, so camera
movement changes streaming feedback directly. The CPU does not need to author a
new terrain cut for a camera move; it installs completed pages, then traversal
selects the best resident representation for the current frame.

## Hole prevention

A streamed terrain instance is invalid for traversal unless its root fallback is
resident. Missing fine pages produce parent fallback emissions plus page
requests. Missing roots fail traversal rather than emitting "selected neither
parent nor child".

## Test coverage

`SparseVoxelTerrainConvergencePhase6` verifies that:

- traversal emits resident fallback pages and requests missing fine terrain
  pages;
- camera movement changes request priority/order through traversal feedback;
- installing a requested fine page makes traversal emit that page and removes
  its request;
- one chunk can refine while another remains on fallback;
- an unloaded fallback root is rejected before it can create a hole.
