# Medieval pixel materials

Imported unchanged from the user's Desktop/textures folder. Original files
remain untouched. Source names:

- terracotta-roof-tiles.png: ChatGPT Image Sep 20, 2026, 12_51_39 PM (1).png
- gray-stone-masonry.png: ChatGPT Image Sep 20, 2026, 12_51_40 PM (4).png

Rights/provenance beyond user supply have not been independently verified.
The images are used as supplied; seamless boundary matching is not guaranteed.

The generated mesh has per-corner metre-space UV0 coordinates, material IDs
(Solid::SurfaceMaterial), and independent surface island IDs. These are
repeat-texture UVs, not a non-overlapping lightmap bake atlas. Divide UV0 by
repeatMetres, repeat-wrap both axes, and use nearest-neighbour filtering for
pixel styling. UV V points up; image row zero is at the top.

Walls retain vertical courses, roofs orient rows across the slope, and flat
floors use XZ mapping. Coplanar panels share a phase even around openings.
Plaster currently uses a solid color. Wood uses the generated
dark-oak-timber.png for doors, wooden frames, beams and furniture.
Paving/foundations reuse stone with a tint.

## Door and architectural trim update

The Desktop/doorAndWindow folder contained one image when inspected:
`ChatGPT Image Sep 20, 2026, 02_10_36 PM (1).png`, an iron-bound oak door.
It was inspected as a design reference and left unchanged. Its surrounding
border is not mapped onto the opening: actual frames remain geometry.

New project assets generated with the imagegen skill, built-in tool mode:

- `iron-bound-door.png`: aspect-preserving crop of the 2:3 image on each front/back door leaf, mirrored
  for the right leaf of double doors. Leaf edges retain ordinary oak.
- `carved-limestone.png`: repeating lighter stone for opening surrounds,
  columns, mullions and structural arch ribs. Open windows remain open.
- `leaded-glass.png`: diamond leading on existing glazed panes only. This
  preview material is opaque albedo, not a transmission shader.

Material 8 uses normalized, clamped UVs; other materials retain metre-space
UVs. Authored door UVs survive the generic projection pass. OBJ exports
carry `-clamp on` for the door texture. Boarding has a separate material ID
so wall grain stays vertical while beam grain follows member length.
Timber facade infill and gables use warm plaster. Poor dwelling fixtures use
boarded walls; stone site platforms remain separate materials.

Final prompts (new images; no input image editing):

### iron-bound-door.png

> Use case: stylized-concept. Asset type: medieval game door-leaf albedo texture. Generate a single rectangular wooden door surface filling the entire image edge to edge, orthographic front view, portrait 2:3 aspect. Warm medium oak vertical boards, two slender dark forged iron horizontal hinge straps at 22% and 75% height, small iron ring handle below centre toward right, tiny rivets. Stylized crisp chunky pixel art with limited warm brown and charcoal palette. Flat neutral illumination, no perspective, no cast shadow. IMPORTANT no surrounding wall, no door frame, no border, no background or margin: wood reaches all four image edges. This will be mapped once to an actual geometric door, not tiled. Keep upper 20% plain wood so curved door geometry can crop it. No text or watermark.

### carved-limestone.png

> Use case: stylized-concept. Asset type: tileable medieval carved limestone trim albedo for real 3D arches and window frames. Single full-bleed square texture of pale warm gray limestone, subtle hand-chisel marks and sparse tiny chips, stylized crisp chunky pixel art, restrained contrast, small palette. Flat surface orthographic, perfectly even diffuse illumination. No masonry grid, no mortar, no objects, no arch silhouette, no perspective, no shadows, no borders, no text. Stone fills every pixel; seamless repeating all edges. This is a continuous stone material for narrow arch ribs and carved window mullions, visually lighter and less busy than a brick wall.

As with other generated textures, exact seamlessness is not guaranteed.

### Rebuild the material gallery

From the repository root:

```sh
cmake --build build --target FacadeAtlasTests -j4
build/tests/FacadeAtlasTests build/renders/facade-materials-v2
python3 tools/terrain/render_dwelling_interiors.py build/renders/facade-materials-v2/wood-hut.json build/renders/facade-materials-v2/wood-hut --section-height 1.6
python3 tools/terrain/render_dwelling_interiors.py build/renders/facade-materials-v2/inn.json build/renders/facade-materials-v2/inn --section-height 1.6
python3 tools/terrain/render_dwelling_interiors.py build/renders/facade-materials-v2/raised-platform.json build/renders/facade-materials-v2/raised-platform --section-height 2.4
python3 tools/terrain/render_dwelling_interiors.py build/renders/facade-materials-v2/generated-church.json build/renders/facade-materials-v2/church --section-height 2
python3 tools/terrain/render_dwelling_eye_level.py build/renders/facade-materials-v2/stone-vault.json build/renders/facade-materials-v2/vault-eye.png --eye 1.2 1.6 3.5 --target 11 2.8 3.5
```

These are the native CPU preview/export materials. They do not bind textures
to the live GPU viewport. The eye-level vault is a structural hall fixture;
the hut, inn, church and raised house use cell-planner output.

### leaded-glass.png

> Use case: stylized-concept. Asset type: seamless repeating albedo texture for small medieval glazed window panes on actual 3D windows. Full bleed square flat orthographic texture, crisp chunky pixel art with limited palette. A regular lattice of thin charcoal lead came dividing small diamond panes, approximately 6 diamonds across. Muted pale blue-gray and desaturated sea-green glass with subtle mottling, gentle variation between panes, no opaque black gaps. Flat even illumination, no shine, no reflection of scenery, no perspective, no frame, no stone, no window silhouette, no border, no background, no text or watermark. Repeating pattern continues across every edge. The texture will only be used on existing glass surfaces, not open windows.

## Generated timber texture

Created with the imagegen skill using the built-in image generation tool.
Saved project asset: assets/procedural/textures/medieval-pixel/dark-oak-timber.png.

Final prompt:

> Use case: stylized-concept. Asset type: seamless square game material albedo texture for medieval wooden doors, door frames, window frames, beams and furniture. Generate one full-bleed tileable dark warm oak wood texture, flat orthographic surface, no object or door silhouette. Stylized low-resolution pixel art with crisp deliberately chunky pixel clusters, limited warm brown palette, subtle vertical grain and sparse small knots. About six broad vertical timbers across the square, extremely subtle seams, grain running straight top to bottom. Uniform illumination, no vignette, no perspective, no directional cast shadows, no painted hardware, no metal, no borders, no text, no watermark. Left/right and top/bottom edges should repeat seamlessly. Keep contrast moderate, dark brown mids and warm muted highlights, consistent with hand-painted pixel-art gray masonry and terracotta roof tiles. This is a reusable UV texture, not a picture of a building.

## Tower masonry

`tower-masonry.png` was already imported when this implementation continued.
The handoff identifies it as the supplied tower texture; the original filename,
creator and license are not recorded, so no further provenance is asserted.
SHA-256: `3f14ea5138c4961a80cb1634d9437db4e16903c9f55627c7ed02ce4a5741821c`.
Material 15 uses cylindrical metre UVs on vertical tower walls and planar XZ
metre UVs on horizontal masonry surfaces, including parapet tops.
