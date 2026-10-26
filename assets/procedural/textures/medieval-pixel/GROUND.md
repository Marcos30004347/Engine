# Ground material allocation and blending

The source of geometry is `buildCitySurfaces`, not a shader mask:

1. Join widened road polygons into one arrangement; assign one owner per face.
2. Allocate block ground outside the road reservation, in stable ID order.
3. Clip lots to their parent blocks and roads; resolve overlapping lots in stable ID order.
4. Subtract allocated lots from block ground. Emit all ground at the same offset.
5. Fit each building to the largest connected component of its allocated lot.

The last rule preserves holes; remaining disconnected parts stay ground cover.
Suburban lots with no block parent are supported. Logical region IDs remain
unchanged; the resulting surface polygons are the legal buildable allocation.

## Material contract

`CityModelPlan.groundMaterialIds = [11,12,13]` names grass, earth and cobble.
`groundWeights[triangle][corner][channel]` has three nonnegative weights that
sum to one. Zero triples designate ordinary building surfaces. These corner
attributes preserve seams while mesh positions are welded. Tile clipping
interpolates both UVs and weights barycentrically. Do not interpolate IDs.

Weights come from signed distance to the **union** of each road material's
polygons and to allocated lot polygons. Segment ownership cuts do not create
material seams. Road influences take precedence over lot influences; dirt and
stone roads mix at junctions. Road feathering spans 0.9 m on each side; lot
feathering spans 1.2 m. The geometry boundary itself does not move.

`CitySmoothedRoute.groundCover` and `CityLotDevelopment.groundCover` allow
Automatic, Grass, Dirt or Stone. Automatic broad streets use cobble; narrow
lanes/field tracks use earth. Developed ordinary lots use earth, affluent or
commercial lots use stone, and undeveloped ground stays grassy. These are
visual allocation defaults, not a physical road-engineering simulation.

Ground triangles refine to `CityModelSettings.groundVertexSpacing` (default
1.5 m, demonstration 0.75 m) so long polygons have interior blend samples.
World-XZ UVs stay continuous across ownership boundaries. Building platforms
remain their own raised stone structures, not blended into grass.

## Shader and validation paths

- `tools/terrain/ground_material_preview.html`: executable WebGL2 shader
  validation client. Uses the native JSON, real texture sampling, normalized
  interpolated weights and linear-light color blending. Includes weight debug.
- `assets/shaders/material/wgsl/city-ground-blend.wgsl`: compiled WGSL fragment
  module. Bind an sRGB texture array in palette order, repeat/nearest sampler,
  and repeat sizes of (3,3), (3,3), (2,2) metres. Output is linear albedo.
- `tools/terrain/render_city_model.py`: CPU rendering of the same contract.

The production virtual-geometry renderer still needs an attribute upload and
material-binding adapter. The standalone shader is not automatically connected
to that renderer. JSON carries the blend weights; conventional OBJ/MTL does
not represent this layered material and is not a lossless terrain export.
Source images are stored at their generated sizes. Texture-array upload must
use consistent dimensions (these three assets have matching dimensions).

## Reproduce

```sh
cmake --build build --target WorldSettlementPreview WorldGeneratorIntegrationTests -j4
build/tests/WorldSettlementPreview --ground-demo build/renders/ground-materials/neighbourhood.json
build/tests/WorldGeneratorIntegrationTests
python3 -m unittest discover -s tools/terrain -p test_dwelling_materials.py
python3 scripts/wgsl2spirv.py assets/shaders/material/wgsl/city-ground-blend.wgsl -o build/renders/ground-materials/city-ground-blend.spirv
python3 -m http.server 8768 --bind 127.0.0.1
```

Open http://127.0.0.1:8768/tools/terrain/ground_material_preview.html.
The four-lot scene is a deterministic planner fixture: wooden worker dwelling,
craftsman's house, stone house and inn, with a dirt lane crossing a paved street.
It is not a separately painted illustration or a hand-positioned mesh collage.

## Generated assets and exact prompts

Generated with the imagegen skill's built-in tool; saved in this directory as
`grass-ground.png`, `packed-earth.png`, and `cobblestone-ground.png`.
No supplied Desktop files were modified. Exact seamlessness of generated
images is not guaranteed.

### grass-ground.png

Use case: stylized-concept. Asset type: seamless square repeating terrain albedo for a medieval pixel-art game. Full-bleed orthographic top-down material: muted olive meadow grass, short irregular blades and tiny mossy tufts, restrained earthy green palette, no flowers, no soil patches. Crisp deliberately chunky pixel clusters, limited palette, moderate contrast, even diffuse illumination. No perspective, no directional shadows, no objects, no borders, no text, no watermark. Pattern and color must repeat seamlessly on all four edges. Texture fills every pixel, a material swatch rather than a scene.

### packed-earth.png

Use case: stylized-concept. Asset type: seamless square repeating terrain albedo for a medieval pixel-art game. Full-bleed orthographic top-down material: warm desaturated brown packed dirt, fine granular earth with sparse tiny pebbles and faint irregular wear, no distinct road shape, no grass. Crisp deliberately chunky pixel clusters, limited palette, moderate contrast, even diffuse illumination. No perspective, no directional shadows, no objects, no borders, no text, no watermark. Pattern and color must repeat seamlessly on all four edges. Texture fills every pixel, a material swatch rather than a scene.

### cobblestone-ground.png

Use case: stylized-concept. Asset type: seamless square repeating terrain albedo for a medieval pixel-art game. Full-bleed orthographic top-down material: worn gray medieval cobblestone paving, small irregular rounded rectangular stones in loose staggered rows, narrow earthy mortar gaps, restrained warm gray palette, no grass. Crisp deliberately chunky pixel clusters, limited palette, moderate contrast, even diffuse illumination. No perspective, no directional shadows, no objects, no borders, no text, no watermark. Pattern and color must repeat seamlessly on all four edges. Texture fills every pixel, a material swatch rather than a scene.

