# Procedural façade atlas

The canonical atlas is `FacadeAtlas.cpp`. Runtime generation and OBJ export use
the same cached triangle arrays; exported files are inspection/interchange
artifacts, not a second hand-authored source of geometry.

```sh
cmake --build build --target FacadeAtlasTests
build/tests/FacadeAtlasTests build/renders/facade-atlas
```

`asset-A-S.obj` uses these stable keys:

| A | Component |
|---|---|
| 0 | Window frame |
| 1 | Mullion |
| 2 | Glazing skin |
| 3 | Single shaped door leaf |
| 4 | Column |
| 5 | Left half-column |
| 6 | Right half-column |
| 7 | Portal frame, without a sill across the threshold |
| 8 | Left shaped door leaf |
| 9 | Right shaped door leaf |

S: 0 rectangular, 1 round arch, 2 pointed arch, 3 circular, 4 shallow segmental
passage arch (high spring line to preserve pedestrian headroom). Shape-independent
components have identical variants. Circular portal/door assets are available
for inspection but not automatically selected as pedestrian entrances.

Local X is width, Y is height, Z is depth, normalized to the unit box. Structural
instances supply the world frame, dimensions, support-face index and owning
segment. Frames and glazing have two skin instances; door leaves have one owner
(two complementary assets for double doors). Glass is deliberately a planar
skin, not a watertight solid. Other atlas entries are volumetric triangle meshes.

The first columns are square masonry piers with bases and capitals. Cylindrical
classical columns, ornament, tracery and projecting oriels remain later atlas
families. The structure must reserve clearance/support before selecting them.
