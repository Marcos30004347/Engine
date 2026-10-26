# Terrain VG asynchronous streaming and atomic64 rasterization

The terrain correctness fixes remain in `terrain-vg-refinement-stall-fix-plan.md`.
This change addresses publication and rendering performance on top of that work.

## Findings

- The sample defaulted to hardware-only VG rasterization. The opt-in native
  encoder waited for the whole Vulkan frame, then ran after its G-buffer consumer.
- Live page and hierarchy updates used host writes into shared buffers while
  another frame could still be reading them.
- The source installer read/decompressed scratch pages on the render thread.
- Feedback queries repeatedly copied and sorted the same priority data. LRU
  tracking separately read the live GPU priority buffer, racing its reset.
- Raising meshing throughput exposed a scheduler bug: apply-budget deferrals did
  not schedule another apply after the workers became idle. Finished meshes could
  stay cached indefinitely while coarse fallbacks remained visible.
- Frame-slot reuse waits are necessary bounded back-pressure, not a full-device
  drain. `SVR_WAIT_IDLE_EACH_FRAME` and `SVR_TERRAIN_VG_SERIALIZE` are diagnostics.
  Normal world terrain streaming does not call those waits.

## Implementation

Both VG visibility phases use a native Metal atomic64 rasterizer on supported
Apple hardware. A triangle thread writes one packed reverse-depth/payload key
with `atomic_max_explicit`. An indirect dispatch consumes the culler's software
cluster list. Depth/payload outputs feed the existing VG visibility merge and
G-buffer resolve. The WGSL two-pass implementation remains the portable fallback;
`--disable-vg-software-raster` selects hardware-only rasterization for comparison.
`--vg-metal-raster` now selects the in-graph path.

The RHI splits a command buffer at a native compute boundary. Vulkan signals an
exported timeline event, a separate Metal queue waits for it and signals completion,
and the following Vulkan consumer waits on that completion value. The ordinary
frame fence owns the lifetime of the prefixes, event and continuation. There is
no CPU fence wait on the successful native submission path.

Page bytes, hierarchy edits, page-table entries and priority resets are queued
on the CPU. Overlapping edits preserve last-write-wins order and are combined
into disjoint ranges. A reusable staging allocation per frame slot is populated
only after the slot is safe to reuse. GPU copies precede page decoding and culling;
live GPU page pools are no longer host-mapped for updates. Oversized staging
allocations shrink after eight small/idle uploads for their frame slot.

A single background reader handles scratch-file page reads and decompression.
At most 64 requests/results are retained. Pending I/O is retried without counting
as an install failure, stale locations are rejected, and shutdown joins the worker
before closing the file. The first bootstrap/root publication retains its existing
synchronous setup; demand-driven page reads use the background reader.

One completed-frame priority snapshot serves page requests, geometric demand,
LOD diagnostics and residency touches. Whole visibility/queue diagnostic copies
are only allocated with `SVR_TRACE_TERRAIN`. The unsafe live hierarchy/table dumps
were removed. Mesh admission rises from 4 to 8 jobs/frame, page installation from
8 to 32 pages/frame, and the source upload packet from 32 to 64 MiB at the requested
resolution (16 to 32 MiB at 1440p and above). Chunk publication retains its separate
4-chunk CPU budget and retries all budget-deferred work.

## Validation

- Portable and native VG visibility suites pass: exact hardware/software coverage
  in the fixture, depth and triangle parity, mixed-route stability, replacement,
  and installation/retirement with two frames in flight.
- Background source tests pass: byte-identical reads and rejection of a chunk
  retired while a read is pending or completed.
- The final 5 km capture reaches 510 source chunks, 616
  installed pages and visible 10 cm detail. The final capture has zero holes,
  enclosed gaps, oversized clusters, install failures, missing slots and repairs.
  Exactly one capacity-deferred empty source job occurs; the former retry loop
  does not recur. Settled footprint is approximately 5.5 GiB.
  Final image: `/tmp/terrain-vg-fix/performance-final-capture.png`.

- The final build passes all ten focused regressions: portable/native visibility,
  runtime object lifecycle, scratch reads, first-missing-node culling, rendered
  cut error, 300-frame capacity refinement, 600-frame movement and settling,
  and the 1,200-frame close-up LOD profile. The moving camera sees 10 cm voxels
  throughout its 0–8 m band, with no install failures, missing slots or coverage
  gaps. The formerly failing stress runs finish at about 6 GiB under a 7 GiB guard.
- Completed-frame requests are filtered against current chunk ownership before
  reinstalling. Final LOD diagnostics sample the final completed frame, rather
  than reusing the previous periodic sample; chunk publications also count as
  streaming activity.

## Frame-time comparison

Two 600-frame runs use the requested 1220x720 output (610x360 internal raster),
5 km radius, static camera, 1.5-pixel threshold, Lambert shading, and
`--skip-output-signature`. These are CPU frame wall times including GPU
back-pressure, over the final 176 settled frames, not isolated kernel timings.

| Raster route | Mean | Median | p95 | Drawn cut clusters |
| --- | ---: | ---: | ---: | ---: |
| Hardware only | 25.900 ms | 25.898 ms | 26.765 ms | 106,754 |
| Native atomic64 | 12.497 ms | 13.946 ms | 17.012 ms | 106,836 |

Both runs settle at 510 chunks with zero oversized clusters and failed installs.
Asynchronous source admission means their final cuts are not bit-identical, but
rendered cluster counts differ by less than 0.1%. The result demonstrates roughly
twice the throughput for this view. Bounded runs with frame captures enabled are
not comparable: signature/capture work adds GPU copies and expensive CPU image
analysis which normal interactive rendering does not perform.
