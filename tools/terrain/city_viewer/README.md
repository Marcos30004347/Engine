# Native world-city explorer

The viewer consumes the **world → site selection → city territory → settlement fabric → city completion → structural city mesh** pipeline. It does not assemble a demonstration street or replace buildings with boxes. Room surfaces, doors, stairs, furniture, roof structures, foundations, town walls and generated compound wings remain in the mesh.

Build and generate a city, pack it, and serve it locally:

```sh
cmake --build build --target WorldSettlementPreview -j4
python3 tools/terrain/export_city_viewer.py --generate --seed 41 --population 1600 --relief 500 --output build/city-viewer --serve
```

Open `http://127.0.0.1:8765`. Python requires NumPy and Pillow. The default world settings are seed 41, population brief 1,600 and relief 500 m. The browser uses native WebGL2: no JavaScript packages, external services or CDN. An existing export can be packed with `--input path/to/city-model-c10.json`. Add `--port 8766` to select another port.

`--city-region 1` on the native generator still builds the world map and chooses its settlement cells, but skips unrelated kingdom road meshes. The exporter selects that city's buildings and surrounding ground. It keeps all intersecting triangles whole, recentres coordinates, partitions them into spatial batches, and deduplicates complete vertex attributes. Per-corner texture coordinates, normals and ground weights survive the conversion. It rejects demo exports without world-pipeline provenance.

## Controls

- **WASD:** fly forward/back/sideways. **Q/E** (or C/Space): descend/ascend. **Shift:** move faster.
- Drag the canvas to look. **Fly around** locks the mouse; **Escape** releases it. Dragging remains available where pointer lock is unsupported.
- **O:** city overview. **G:** a generated street near multistorey housing.
- Select a building, then **Outside**, **Enter room**, or **Next room**. Interior viewpoints are generated from actual room polygons; their initial look direction is chosen against the native room surfaces. Free flight can pass through closed doors/walls.
- Click the map to move horizontally. View options expose roofs, horizontal cutaway height, movement speed and textures.

All interiors load by default. Frustum culling only skips batches outside the camera view. Roof/cutaway controls are inspection options. There is no collision or gameplay simulation.

## Street generation review

The review used [Watabou's TownGeneratorOS Model](https://github.com/watabou/TownGeneratorOS/blob/master/Source/com/watabou/towngenerator/building/Model.hx) and [Topology](https://github.com/watabou/TownGeneratorOS/blob/master/Source/com/watabou/towngenerator/building/Topology.hx) as references for gate-to-centre paths, connected street chains and shared layout relaxation. The implementation here remains the engine's own generator.

Repeated longest-edge bisections previously inherited two axes through successive subdivisions. The engine now creates irregular, partially relaxed Voronoi routing cells inside the selected world planning cells **before** A* routing. Original cell borders and gates stay fixed. The native pipeline then reserves smoothed arterial widths, derives blocks, adds local lanes and allocates frontage lots. Final rounded alley ends are subtracted from lots, including concave lots, before buildings are requested. Residual oversized block splits vary their local angle rather than recreating parallel strips.

The castle planner can fit a precinct across the union of adjacent reserved world cells; internal planning seams no longer receive independent street margins. City-class castle districts reserve room for their inhabited ranges, towers and perimeter clearance before housing allocation. Geometrically inadequate sites remain rejected.

## Verification

```sh
build/tests/TownLayoutTests
build/tests/WorldGeneratorIntegrationTests
python3 tools/terrain/test_city_viewer.py
python3 tools/terrain/test_dwelling_materials.py
```

The exporter tests reconstruct packed triangles and compare positions, texture seams, material IDs and interior viewpoints with the native source, and check that distant world geometry and demonstration layouts are excluded. `window.cityViewer` exposes read-only readiness, generation provenance, draw/triangle counts, camera position, FPS and WebGL errors for browser smoke tests.

Native generation is still under development: some seeds reject terrain/compound fits, and the broader dwelling suite has known stair/gallery/support failures. Native rejection notes remain in `city.json`; this viewer does not repair or disguise those failures. Initial city downloads can be several hundred MB because complete interiors are included.


## Checked city

The bundled local run uses world seed 41, site cell 512, a population brief of 1,600, and relief 500 m. Its selected city region contains 2,312 generated buildings and 9,298 rooms, including a keep, two castle ranges, 62 defence towers and three gatehouses. The browser draws 8,873,928 native triangles. The indexed bundle is about 481 MB; it includes complete interiors.

Validation: 385 town-layout assertions, 93 world-generator integration checks, two viewer-export tests and six material-rendering tests passed. Chrome browser checks covered scene loading, movement, street navigation, keep entry, switching rooms, roof hiding and cutaways, with no WebGL errors. Both overview and street snapshots reported about 60 FPS on the test machine; hardware results vary.

The separate native runtime terrain build still rejects two of eight seed-41 tiles (a pinched arrangement face and a CDT constraint failure). This viewer loads the successfully generated C10 city surface/structure mesh, not those failed runtime tile jobs. Other world configurations tested during development rejected site/terrace/compound constraints; no constraints were relaxed to make them pass. The wider dwelling suite's previously recorded six stair/gallery/area/furniture/support failures remain outside this viewer validation.
