# Direct encoded VG geometry

Baseline commit: `cadef4e9` (terrain refinement, asynchronous uploads, atomic64 raster).

The generated-world terrain scene now defaults to `Settings::compressedGeometry`.
`SVR_VG_DECODED_GEOMETRY=1` selects the previous expanded storage for an A/B run.
Other scene producers can opt in; GPU-generated pages that directly emit decoded
geometry retain their existing storage mode.

Page installation still publishes culling bounds, LOD errors, and cluster lookup
metadata. It does not expand positions, normals, UVs, indices, or skin weights.
Encoded scenes reserve only a one-element compatibility buffer for each expanded
stream, keeping the binding layouts compatible with direct-generated geometry.

For encoded clusters, the existing 32-byte geometry record stores the descriptor
word address in vertexOffset, the page word address in indexOffset, and unit-scale
bits in its formerly unused padding0 field. A zero scale denotes expanded data.
The owning instance and cluster counts are unchanged. Page retirement still uses
the existing frame-delayed lifetime rules; raster and resolve passes explicitly
register reads of the encoded page pool.

Hardware drawing, portable compute rasterization, visibility resolve and virtual
shadow-map drawing share WGSL fetch functions. The native atomic64 rasterizer
uses matching position/index extraction. Positions retain the existing integer
grid and dequantization arithmetic, normals retain octahedral encoding, and
indices stay byte-packed. No additional lossy quantization is introduced.

This removes **install-time geometry expansion**, not the small on-demand unpack
operations in shaders or scratch-file decompression on the background I/O worker.

## Validation

The compressed visibility tests compare against an expanded hardware reference,
including coverage, depth, triangle identity, replacement, and two frames in
flight. They also require zero expanded vertex/index allocations and one-element
compatibility buffers. Both portable and atomic64 routes are exercised.

The four visibility suites pass (expanded/encoded × portable/native), including
comparison of encoded output against the expanded hardware reference.

Initial A/B: 600 frames, 5 km world, 1220×720 output, 610×360 internal raster,
1.5 px threshold, native atomic64, static camera, output captures disabled.
The final 176 frames averaged 12.497 ms encoded versus 12.498 ms expanded.
Settled footprint was 4.47 GiB encoded versus 5.21 GiB expanded, a reduction of
about 0.74 GiB. Both reached 510 chunks without failed installs or giant clusters.
These are end-to-end CPU frame wall times including GPU back-pressure, not kernel
timings; this run demonstrates a memory saving without a measurable speed change.

All five terrain contracts pass: cut error, capacity refinement, movement,
settling, and the 1,200-frame LOD profile. The moving camera sees 10 cm voxels in
its 0–8 m band. Final signatures report zero holes and enclosed gaps; installation
failures, missing slots and root repairs are zero. Encoded scenes no longer
allocate/copy obsolete expanded-stream debug snapshots.

## Full-resolution comparison

With `--render-scale 1.0 --disable-frame-generation`, both first runs drew exactly
112,573 clusters from 723 source chunks. The capacity-limited fallback error was
3.85 px in both modes; compression did not change that cut.

| Storage | Internal raster | Mean frame time | Settled footprint |
| --- | --- | ---: | ---: |
| Expanded | 610×360 | 12.498 ms | 5.21 GiB |
| Encoded | 610×360 | 12.497 ms | 4.47 GiB |
| Expanded | 1220×720 | 21.893 ms | 6.89 GiB |
| Encoded | 1220×720 | 22.533 ms | 6.09 GiB |

The matched full-resolution pair shows roughly 3% additional frame time for
on-demand unpacking. A reverse-order repeat measured 22.212 ms expanded and
22.588 ms encoded, but the asynchronous cuts differed (122,956 versus 113,438
clusters), so that repeat is only a variability check, not an exact speed ratio.
The benefit demonstrated here is memory rather than an FPS increase.

All runs kept the 7 GiB memory guard enabled. Shader builds include the shadow
fetch path; runtime parity tests cover the active hardware/compute/resolve paths,
not skinned or shadow-map rendering. No lighting traces were enabled.
