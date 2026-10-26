#pragma once
#include "WorldRoadNetwork.hpp"
#include "WorldSurfaceArrangement.hpp"
namespace procedural::world::rewrite
{
struct RoadWaterSample
{
  bool protectedChannel = false;
  double waterHeight = 0;
};
using RoadWaterField = std::function<RoadWaterSample(double, double)>;
struct BridgeSpan
{
  uint32_t road = 0, firstStation = 0, lastStation = 0;
  double deckThicknessMetres = .6, minimumWaterClearanceMetres = 1, maximumSpanMetres = 40;
};
struct RoadEarthworkSettings
{
  SurfaceMapSettings map;
  double maximumReachMetres = 20, rootSpacingMetres = .5, cutSlope = 1, fillSlope = .67;
  double maximumAbutmentHeightMetres = 5;
  double ditchDepthMetres = 0, ditchWidthMetres = .6; // Optional cut-side drainage trough.
};
struct StructuralTriangle
{
  std::array<Position, 3> points;
};
struct RoadEarthworkProduct
{
  SurfaceArrangement map;
  std::vector<SurfaceContour> contours;
  std::vector<StructuralTriangle> structures; // Deck sides/underside and dry abutment faces.
  uint32_t bridges = 0;
  double maximumReachMetres = 0;
};
// Generates labeled map contours only. Final CDT is a separate consumer.
// Seed contours supply land/material patches, river banks/beds and separate water.
// Cuts/fills must meet terrain within budget; unsupported walls/culverts reject.
bool generateRoadEarthworks(
    const RoadNetworkProduct &,
    const RoadField &terrain,
    const RoadWaterField &,
    const std::vector<SurfaceContour> &seedContours,
    const std::vector<BridgeSpan> &,
    const RoadEarthworkSettings &,
    RoadEarthworkProduct &,
    std::string *error = nullptr);
} // namespace procedural::world::rewrite
