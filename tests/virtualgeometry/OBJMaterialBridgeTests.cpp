#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"

using namespace virtualgeometry;

namespace
{

namespace fs = std::filesystem;

fs::path sourceRoot()
{
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

fs::path sampleTexturePath()
{
  return sourceRoot() / "assets/meshes/gltf/CesiumMan_img0.jpg";
}

void writeTextFile(const fs::path &path, const std::string &contents)
{
  std::ofstream file(path);
  assert(file.good());
  file << contents;
  file.close();
  assert(file.good());
}

std::string buildGridOBJ(uint32_t quadsPerSide)
{
  assert(quadsPerSide > 0u);

  std::ostringstream obj;
  obj << "mtllib scene.mtl\n";
  obj << "o material_grid\n";

  const uint32_t vertsPerSide = quadsPerSide + 1u;
  const uint32_t verticesPerLayer = vertsPerSide * vertsPerSide;

  auto appendLayer = [&](float zOffset)
  {
    for (uint32_t y = 0u; y < vertsPerSide; ++y)
    {
      for (uint32_t x = 0u; x < vertsPerSide; ++x)
      {
        obj << "v " << static_cast<float>(x) << ' ' << static_cast<float>(y) << ' ' << zOffset << '\n';
      }
    }

    for (uint32_t y = 0u; y < vertsPerSide; ++y)
    {
      for (uint32_t x = 0u; x < vertsPerSide; ++x)
      {
        obj << "vt " << (static_cast<float>(x) / static_cast<float>(quadsPerSide)) << ' ' << (static_cast<float>(y) / static_cast<float>(quadsPerSide)) << '\n';
      }
    }
  };

  appendLayer(0.0f);
  appendLayer(1.0f);

  auto appendFaces = [&](const char *materialName, uint32_t layerIndex)
  {
    const uint32_t baseVertex = layerIndex * verticesPerLayer;
    const uint32_t baseTexcoord = layerIndex * verticesPerLayer;

    obj << "usemtl " << materialName << '\n';
    for (uint32_t y = 0u; y < quadsPerSide; ++y)
    {
      for (uint32_t x = 0u; x < quadsPerSide; ++x)
      {
        const uint32_t i0 = baseVertex + y * vertsPerSide + x + 1u;
        const uint32_t i1 = i0 + 1u;
        const uint32_t i2 = i0 + vertsPerSide;
        const uint32_t i3 = i2 + 1u;

        const uint32_t t0 = baseTexcoord + y * vertsPerSide + x + 1u;
        const uint32_t t1 = t0 + 1u;
        const uint32_t t2 = t0 + vertsPerSide;
        const uint32_t t3 = t2 + 1u;

        obj << "f " << i0 << '/' << t0 << ' ' << i1 << '/' << t1 << ' ' << i3 << '/' << t3 << '\n';
        obj << "f " << i0 << '/' << t0 << ' ' << i3 << '/' << t3 << ' ' << i2 << '/' << t2 << '\n';
      }
    }
  };

  appendFaces("mat_a", 0u);
  appendFaces("mat_b", 1u);

  return obj.str();
}

void testOBJMaterialBridge()
{
  const fs::path tempRoot = fs::temp_directory_path() / "engine_obj_material_bridge_tests";
  const fs::path texturesDir = tempRoot / "textures";
  const fs::path materialsDir = tempRoot / "generated_materials";
  const fs::path objPath = tempRoot / "scene.obj";
  const fs::path mtlPath = tempRoot / "scene.mtl";
  const fs::path vgPath = tempRoot / "scene.virtualgeometry";
  std::error_code error;
  fs::remove_all(tempRoot, error);
  fs::create_directories(texturesDir, error);
  assert(!error);

  const fs::path copiedTexturePath = texturesDir / "basecolor.jpg";
  fs::copy_file(sampleTexturePath(), copiedTexturePath, fs::copy_options::overwrite_existing, error);
  assert(!error);

  writeTextFile(
      mtlPath,
      "newmtl mat_a\n"
      "map_Kd textures/basecolor.jpg\n"
      "\n"
      "newmtl mat_b\n"
      "map_Kd textures/basecolor.jpg\n");

  writeTextFile(objPath, buildGridOBJ(12u));

  VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = 1u;
  buildSettings.maxRootPageGroups = 1u;

  VirtualGeometryBuildData build = VirtualGeometryEncoder::buildFromOBJFile(objPath.string(), buildSettings, materialsDir.string());
  assert(build.materialFiles.size() == 2u);
  assert(build.shapes.size() == 2u);
  assert(build.shapes[0].materialIndex == 0u);
  assert(build.shapes[1].materialIndex == 1u);
  assert(build.shapes[0].root_page_index != UINT32_MAX);
  assert(build.shapes[1].root_page_index != UINT32_MAX);
  assert(!build.materialFiles[0].empty());
  assert(!build.materialFiles[1].empty());
  assert(fs::exists(build.materialFiles[0]));
  assert(fs::exists(build.materialFiles[1]));

  const QuantizationConfig config;
  const VirtualGeometryEncodedData encoded = VirtualGeometryCompressor::encode(build, config);
  assert(encoded.materialFiles.size() == 2u);
  assert(encoded.materialFiles[0] == build.materialFiles[0]);
  assert(encoded.materialFiles[1] == build.materialFiles[1]);
  assert(encoded.shapes.size() == 2u);
  assert(encoded.shapes[0].root_page_index == build.shapes[0].root_page_index);
  assert(encoded.shapes[1].root_page_index == build.shapes[1].root_page_index);

  {
    VirtualGeometryFile writer(vgPath.string(), true);
    assert(writer.isOpen());
    assert(writer.write(encoded, build.pages, MESHLET_LZ4));
  }

  VirtualGeometryFile reader(vgPath.string(), false);
  assert(reader.isOpen());
  VirtualGeometryEncodedData roundTrip;
  assert(reader.readAll(roundTrip));
  assert(roundTrip.materialFiles.size() == 2u);
  assert(roundTrip.materialFiles[0] == build.materialFiles[0]);
  assert(roundTrip.materialFiles[1] == build.materialFiles[1]);
  assert(roundTrip.shapes.size() == 2u);
  assert(roundTrip.shapes[0].root_page_index == build.shapes[0].root_page_index);
  assert(roundTrip.shapes[1].root_page_index == build.shapes[1].root_page_index);

  fs::remove_all(tempRoot, error);
}

} // namespace

int main()
{
  testOBJMaterialBridge();
  std::cout << "OBJMaterialBridgeTests passed" << std::endl;
  return 0;
}
