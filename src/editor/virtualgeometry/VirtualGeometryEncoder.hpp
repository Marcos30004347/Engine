#pragma once

#include <string>
#include <vector>

#include "VirtualGeometryCompressor.hpp"
#include "VirtualGeometryBuilder.hpp"

namespace virtualgeometry
{

class VirtualGeometryEncoder
{
public:
  // Raw data
  static VirtualGeometryEncodedData encode(
      const std::vector<Vertex> &vertices,
      const Shape &shape,
      const QuantizationConfig &config = QuantizationConfig(),
      const VirtualGeometryBuildSettings &buildSettings = {});

  static VirtualGeometryEncodedData encode(
      const std::vector<Vertex> &vertices,
      const std::vector<Shape> &shapes,
      const QuantizationConfig &config = QuantizationConfig(),
      const VirtualGeometryBuildSettings &buildSettings = {});

  // OBJ helpers
  static VirtualGeometryEncodedData encodeFromOBJFile(
      const std::string &path,
      const QuantizationConfig &config = QuantizationConfig(),
      const VirtualGeometryBuildSettings &buildSettings = {},
      const std::string &materialOutputDirectory = {});

  static VirtualGeometryEncodedData encodeFromOBJString(
      const std::string &obj_source,
      const QuantizationConfig &config = QuantizationConfig(),
      const VirtualGeometryBuildSettings &buildSettings = {});
  static VirtualGeometryEncodedData encodeFromGLTFFile(
      const std::string &path,
      const QuantizationConfig &config = QuantizationConfig(),
      const VirtualGeometryBuildSettings &buildSettings = {});
  static VirtualGeometryBuildData buildFromOBJFile(
      const std::string &path,
      const VirtualGeometryBuildSettings &buildSettings = {},
      const std::string &materialOutputDirectory = {});
  static void loadOBJ(const std::string &source, bool from_file, std::vector<Vertex> &out_vertices, std::vector<Shape> &out_shapes);
  static void loadOBJ(const std::string &source, bool from_file, std::vector<Vertex> &out_vertices, Shape &out_shape);

private:
  static VirtualGeometryBuildData buildVirtualMesh(
      const std::vector<Vertex> &vertices,
      const std::vector<Shape> &shapes,
      const VirtualGeometryBuildSettings &buildSettings);
};

} // namespace virtualgeometry
