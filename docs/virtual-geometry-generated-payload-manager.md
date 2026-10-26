# Virtual Geometry generated payload manager

Terrain chunks, voxel remesh pages, trace pages, and future backend-specific
builders should publish ordinary Virtual Geometry runtime pages.  The producer
may be CPU or GPU, but the scene install contract is the same:

1. request a page payload for an existing VG runtime object;
2. reserve scene ranges for encoded bytes and/or decoded geometry;
3. run producer passes that write those reserved ranges;
4. publish one small install record;
5. cancel the reservation if the producer is superseded or fails.

`VirtualGeometryGeneratedPayloadManager` owns the CPU control plane beside the
file-backed `VirtualGeometryStreamingManager`.

## Responsibilities

CPU-side:

- dedupe repeated page requests by object/local page;
- prioritize requests without full per-frame candidate sorting;
- reserve VG scene buffer ranges;
- preserve parent/fallback page protection;
- publish or cancel page reservations;
- expose counters for regression tests and future “CPU generated payloads must
  be zero” assertions.

GPU-side producers:

- generate terrain/mesh/trace payload bytes;
- write directly into the reserved VG scene ranges;
- emit completion records consumed by CPU install logic;
- never require terrain-specific draw/cull paths once the payload is published.

## Runtime contract

The generic scene API is:

- `VirtualGeometryScene::installRuntimeEncodedObject` for CPU/oracle encoded
  assets;
- `VirtualGeometryScene::reserveRuntimeGeneratedPage` for GPU build admission;
- `VirtualGeometryScene::installRuntimeGeneratedPage` for completed GPU output;
- `VirtualGeometryScene::cancelRuntimeGeneratedPageReservation` for abandoned
  work.

The manager wraps those low-level calls with request state:

- `enqueueRequest`
- `reserveReady`
- `publishCompleted`
- `cancel`

This keeps terrain/chunk code as a producer of VG payloads, not as an alternate
renderer or alternate residency system.

## Next wiring step

GPU terrain generation should feed the manager rather than installing
terrain-specific scene pages directly:

```text
GPU feedback/culling
  -> page request buffer
CPU manager
  -> dedupe + dependency checks + reserve ranges
GPU terrain payload passes
  -> write encoded VG page and/or decoded VG cluster buffers
CPU manager
  -> publish generated page install
VirtualGeometryRendererPass
  -> draw normal VG clusters
```

Trace acceleration payloads can use the same page generation/revision identity
and parent/fallback protection, even if their storage is a sidecar in the first
implementation.
