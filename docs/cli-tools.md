# CLI Tools

The CLI layer now contains three standalone executables, each built from its own CMake project under `src/cli`.

## Build Targets

Configure the project as usual:

```bash
cmake -S . -B build
```

Build the CLI tools:

```bash
cmake --build build --target vgimport_obj vgimport_gltf vtmaterial_create --parallel 4
```

Every executable also supports `-h` and `--help`.

## `vgimport_obj`

Creates a `.virtualgeometry` file from an OBJ mesh.

Example:

```bash
./build/src/cli/virtualgeometry/importer/obj/vgimport_obj \
  --input assets/mesh.obj \
  --output build/mesh.virtualgeometry \
  --materials-dir build/materials \
  --compression lz4
```

Notes:

- `--materials-dir` is optional. When supplied, OBJ materials with supported textures are converted into `.vmat` files.
- Compression values: `raw`, `miniz`, `lz4`.
- Quantization and page-packing knobs are exposed through the command flags shown in `-h`.

## `vgimport_gltf`

Creates a `.virtualgeometry` file from a glTF or GLB scene.

Example:

```bash
./build/src/cli/virtualgeometry/importer/gltf/vgimport_gltf \
  --input assets/scene.glb \
  --output build/scene.virtualgeometry \
  --compression lz4
```

Notes:

- The same quantization, compression, and page-packing flags used by the OBJ importer are available here too.
- glTF import currently writes the virtual geometry asset only; there is no separate `--materials-dir` step on this tool.

## `vtmaterial_create`

Creates a `.vmat` file from one or more source images such as PNG or JPEG.

Example:

```bash
./build/src/cli/virtualtexture/importer/vtmaterial_create \
  --output build/materials/stone.vmat \
  --texture textures/stone_albedo.png \
  --texture textures/stone_normal.png \
  --filter linear \
  --address-mode-u repeat \
  --address-mode-v repeat
```

Notes:

- Each `--texture` fills the next texture slot in the material file.
- Up to `5` texture slots are currently supported.
- Sampling options apply to every texture passed on the command line.
