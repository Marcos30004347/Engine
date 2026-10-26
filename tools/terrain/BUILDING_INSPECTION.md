# Native building inspection

Generate fixtures with `build/tests/FacadeAtlasTests build/renders/tower-repair`, then run:

```sh
python3 tools/terrain/render_building_views.py build/renders/tower-repair/generated-church.json build/renders/tower-repair/views
```

The output includes top, front, back, left, right, oblique and true perspective images plus a contact sheet. Orthographic cameras fit actual mesh bounds, including tower peaks. The loader preserves inline per-vertex UVs. Images render exported native triangles without geometry repair.

Fixture exports use polygon triangulation for outlines larger than quads, matching the city exporter. A triangle fan over a concave foundation incorrectly filled the empty corner between church wings; the corrected sample occupies 324 square metres, including when rotated.

Bell towers author the deck, stair opening, chamber and roof in the shaft frame. The chamber is a registered room so wall thickness goes inward. The deck replaces the original shaft ceiling, preserves its stair aperture and has a separate underside. The chamber ceiling closes the cap; deck and cap normals face outward.
