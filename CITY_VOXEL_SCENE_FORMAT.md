# City voxel scene format, phases 1 and 9

This file specifies the **index ABI** in `CityVoxelSceneFormat.hpp/.cpp` and the seekable `.cvscene` container in `CityVoxelSceneFile.cpp`. An index-only file from phase 1 has empty page records and is not renderable. A complete scene file stores payloads after the same versioned catalogue.

## Identity and coordinates

- `GeometryAsset.id`, `AppearanceAsset.id`, `Instance.stableId` and `SpatialCell.id` are stable within a build. IDs are never transient GPU array positions. An appearance names exactly one geometry asset and repeats that geometry's `voxelAddressHash`; this prevents a material stream from being read with the wrong occupied-cell order.
- `frameId` is reserved as zero in index version 1.0; the building-frame table and nonzero frame IDs arrive with the assembly/file payload phases.
- World positions use a city origin in signed **millimetres**. Each spatial cell has an origin in the same coordinates. A static part uses integer local voxel coordinates, a quarter turn `0..3`, and a model pitch in **micrometres**. The nominal scene pitch is a default, not a reason to rescale a voxelized asset.
- Voxel bounds are signed integer cells with inclusive minimum and exclusive maximum. A face on a shared boundary has one owner. The eventual material lookup uses model-local hit position and a documented half-open cell rule, then `(brick ID, rank of set occupancy bit)`; `voxelAddressHash` versions that order.
- Material profile IDs select shared shading behavior. Each occupied voxel's exact color and material value will be stored in appearance pages, independent of geometry pages. Codec IDs are `Raw`, `LZ4`, `Palette8`, `Palette16`, `SparseOverride`; no codec implies lossy color quantization.
- Source and settings hashes identify the offline inputs and compiler configuration. Record hashes are content identity, not security signatures. The file uses FNV-1a 64 checksums to detect accidental corruption; a trusted build process is assumed.

## Disk layout, version 1.0

All fields are explicitly encoded little endian. No C++ `sizeof(struct)` or host pointer is serialized. The 272-byte header has an 8-byte `CVSCENE\0` magic; major/minor `u16`; endian marker `0x01020304`; header size and section count `u32`; total index file bytes, source hash and settings hash `u64`; three signed `i64` city-origin millimetres; nominal pitch and required feature bits `u32`; one `u64` overall checksum; and seven 24-byte section descriptors starting at byte 96. Reserved bytes are zero.

Each section descriptor is `(offset u64, count u32, stride u32, checksum u64)`. Sections are contiguous in this order, with no gaps: geometry (80 bytes/record), appearance (56), instances (56), spatial cells (48), page directory (56), page dependencies (`u32`), material profile references (16). Each section and the overall section region has a checksum. `requiredFeatures` can declare shared geometry and trace pages; readers reject other bits before constructing any scene.

| Record | Required fields |
| --- | --- |
| Geometry | ID, content/address/connector hashes, root page and page range, pitch, integer cell bounds, flags. |
| Appearance | ID, content/address hash, geometry ID, optional base appearance ID, root/page range, codec and flags. |
| Instance | stable ID, geometry/appearance/owner IDs, spatial cell ID, integer XYZ, quarter turn, flags and building frame ID. |
| Spatial cell | ID, signed millimetre origin, contiguous instance span, optional fallback page, flags. |
| Page | sequential ID, geometry/appearance/trace/fallback kind, codec, owning asset, reserved file offset and byte lengths, checksum, dependency span, flags. |
| Material reference | shared profile ID, flags and asset hash. |

Dependencies refer to page IDs and form a directed acyclic graph. Page and appearance base cycles are invalid. A geometry/appearance root must belong to its own page range and kind. Every instance belongs to exactly one spatial cell, and an appearance must match its instance's geometry. Index-only page records have zero offsets, stored lengths and checksums. A complete `.cvscene` has nonempty page payloads immediately after the index, ordered by page ID. The existing 1.0 catalogue ABI accommodates this without changing record strides or minor version.

The reader checks file size and the fixed header/section directory before allocating the index buffer. It then checks section and overall hashes, record limits, all references, page ownership, dependency cycles and appearance cycles. The page reader also rejects missing, overlapping and out-of-file payload ranges. A direct page read allocates at most one configured stored-page limit, checks the file size has not changed since open, verifies the page checksum and returns stored bytes. Codec decoding belongs to the geometry/appearance/trace payload consumer. Writer output goes through a unique temporary file, streams one page at a time, rewrites the final catalogue, reopens and verifies every page, then atomically renames. A failed read leaves previous caller data intact; a failed write leaves the published file intact.

## Initial limits and memory baseline

`CityVoxelSceneLimits` provides caller-configurable **hard caps**: 256 MiB total index bytes, 1 million geometry assets, 1 million appearances, 4 million instances, 1 million spatial cells, 4 million pages, 16 million dependencies, 65,536 material profiles, 64 MiB stored page bytes, 256 MiB decoded page bytes and 64 GiB total file bytes. These are corruption/overrun guards, not performance targets. The reader never allocates according to unvalidated file counts.

The selected existing full-city export is `build/renders/city-explorer-41-final/city-model-c10.json`. The reproducible extractor `tools/terrain/city_voxel_phase1_baseline.py` produced `tools/terrain/data/city-voxel-phase1-baseline.json`: **2,312 buildings, 9,195,644 triangles, 24 geometry material IDs and 13 surface material IDs**. It includes 2,009 houses, 125 workshops, 86 pubs, 62 defence towers and smaller civic/castle/religious groups. The flattened JSON is 2.116 GB; it is a baseline for coverage, not a proposed runtime representation. The cpoextractor scans the large arrays in bounded chunks.

The local development machine reports an Apple M4 with **16 GiB unified memory** and Metal support. It does not provide an independent VRAM budget. For the first end-to-end city prototype, start with configurable ceilings of 256 MiB index, 512 MiB transient CPU compiler scratch, 512 MiB encoded/decoded VG working set, 256 MiB appearance pages, 256 MiB trace data, 128 MiB instance/spatial tables and 128 MiB upload staging. These are provisional caps only; phase 10 must measure peak and steady CPU/GPU use, page misses and visual fallbacks on the full city before accepting or changing them. Shared-memory pools must be accounted together on this device.

Phase 2 must replace the mesh-only baseline with counts of occupied voxels, distinct geometry keys, distinct appearance keys, placements, connector failures and compressed bytes per occupied voxel. That is the first point where a meaningful per-city voxel memory estimate can be made.
