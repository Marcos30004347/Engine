# Terrain GPU V2 Phase 10: qualified rollout and soak boundary

**Status:** GPU-v2 is the default generated-world terrain path; `auto` remains
an explicit compatibility fallback  
**Date:** 2026-08-27

## Outcome

Phase 10 adds the shipping mode selector and a session owner around the V2
runtime. The new public modes are exactly `auto`, `cpu`, and `gpu-v2`, with
`gpu-v2` as the default. `gpu-v2` is intentionally strict: if compute support,
pipeline creation, visual qualification, or runtime limits are missing, startup
fails closed instead of silently generating terrain payloads on the CPU.

`auto` remains available as an explicit compatibility mode. It selects V2 only
when all of these are true:

- the device compute features and limits passed qualification;
- all V2 shader pipelines were created; and
- the Phase 9 ground-level visual report is present and qualified.

Missing or failed gates in `auto` select compact CPU rebuild requests. They
never select the old `gpu-full` builder. The sample retains `--terrain-builder`
only as a deprecated, explicit soak/debug override; the normal sample path is
GPU-owned and fails closed when the requested GPU path is unavailable. This
prevents either the CPU oracle or the broken monolithic chain from being
mistaken for the V2 default.

The rollout owner opens a sticky, one-session circuit breaker for device loss,
pipeline or qualification failure, repeated build/allocation/validation
failure, and sustained hard-budget violations. Stopping the feature destroys
its runtime state but does not clear that reason. A fresh application session is
required to retry GPU ownership.

Generated payload bytes on the CPU remain exactly zero in the GPU-owned path.
Runtime fallback records contain only chunk keys, shape, generation, and
failure metadata. The frozen V2 payload, owner, group, and publication
structures are unchanged. CPU-authored install patches are still allowed, but
they are bounded metadata records rather than terrain payloads.

## Controls

The shared rollout parser implements:

```text
--terrain-generation=cpu|gpu-v2|auto
--terrain-gpu-budget-ms=<value>
--terrain-gpu-chunk-budget-ms=<value>
--terrain-gpu-max-chunks-per-frame=<N>
--terrain-gpu-max-new-chunks-per-frame=<N>
--terrain-gpu-max-workgroups-per-frame=<N>
--terrain-gpu-chunk-bricks=<x,y,z>
--terrain-gpu-raster-hierarchy=off|on|auto
```

Chunk shape parsing always fixes the brick side to 8. The three supplied
dimensions count independent 8-cubed bricks and allow vertical stacks. Budget
overrides are validated together: target cannot exceed the hard limit, the
single-dispatch limit cannot exceed the hard limit, and new chunks cannot
exceed the total chunk cap.

## Stress and soak gates

`SparseVoxelTerrainV2Phase10Tests` covers:

- qualified default GPU-v2 selection, fail-closed unqualified GPU-v2, explicit
  auto compatibility fallback, explicit CPU, missing pipelines, and strict CLI
  parsing;
- cold start and 128 independently scheduled chunks spread across 16 km;
- teleport reprioritization by projected screen error;
- 31 rapid generation/LOD replacements and atlas upload churn;
- repeated allocation failures falling back before publication;
- persistence of the circuit breaker across disable/enable;
- 128 lifecycle cycles with periodic device loss and no live runtime-owner
  leak; and
- frozen publication ABI sizes and zero CPU generated-payload bytes.

The Phase 4 occupancy differential, Phase 5 Tree64 differential, Phase 7
publication/residency, Phase 8 budget runtime, and Phase 9 ground-stability
tests remain mandatory qualification gates.

## Legacy deletion rule

The old monolithic builder source is quarantined behind its deprecated explicit
flag for one release/soak interval. It is not an automatic fallback. Removing
the source and its generated-payload readbacks is the post-soak cleanup, after a
shipping renderer has recorded qualified V2 pipelines and consumed the V2
seqlock owner table directly. Deleting the CPU semantic oracle is not part of
that cleanup.
