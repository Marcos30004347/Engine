#pragma once

#include "procedural/PolygonTerrain.hpp"
#include "procedural/geometry/TerrainMeshBuild.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace procedural::geometry
{

// A raster heightfield as the source for the low-poly mesh.
//
// The analytic map graph places ranges from a formula and routes rivers over
// them afterwards, and it reads as exactly that: smooth swells with a drainage
// net laid on top. The polygon terrain generator - the planet generator's
// original planar pipeline - instead runs tectonics, coast-distance elevation,
// downhill rivers and a climate model over a real raster, so its valleys were
// cut by its own drainage and its ranges are where its tectonics put them.
// Sampling points onto *that* and tessellating where it has detail is what
// makes the mesh look like terrain rather than like a formula.
struct RasterTerrain
{
  const PolygonTerrainMaps *maps = nullptr;
  // World extent of the raster, in metres, and the elevation the normalized
  // heightmap spans.
  double extentMetres = 0.0;
  double reliefMetres = 1200.0;
  double seaLevelMetres = 0.0;

  bool valid() const;
  uint32_t side() const;
  // Metres per raster cell.
  double cellMetres() const;

  // Bilinear, in metres. Water reads as sea level rather than as a hole.
  double height(Point2 position) const;
  // Nearest-neighbour lookups; these are classifications, not fields, and
  // interpolating a class produces a class that is not in the set.
  PolygonTerrainMaterial material(Point2 position) const;
  uint8_t terrainForm(Point2 position) const;
  bool water(Point2 position) const;
  uint8_t riverFlow(Point2 position) const;
};

// How densely to sample, and where to spend the extra.
struct RasterSamplingSettings
{
  // Vertical error budget for the interior, as before.
  double toleranceMetres = 5.0;
  // Multipliers on the tolerance by terrain form. Below one means "refine here
  // harder than the error alone would ask": a valley floor and a ridge crest
  // carry the shapes an eye reads a landscape by, and letting a pure error
  // criterion decide leaves both under-sampled next to a big smooth flank.
  double plainsTolerance = 2.2;
  double valleyTolerance = 0.55;
  double hillTolerance = 1.0;
  double mountainTolerance = 0.6;
  double peakTolerance = 0.35;

  // Contour ladder, and the coastline.
  double contourIntervalMetres = 0.0;
  uint32_t contourBandTarget = 24u;
  double chainSpacingMetres = 0.0;
  // Semantic lines are never dropped based on length. Only exactly collinear
  // vertices can be removed while preserving the represented constraints.

  // A channel appears where accumulated flow exceeds this many cells.
  double channelThresholdCells = 260.0;
  // Ridges are traced along cells the generator classified as peaks and
  // mountains that also separate two basins.
  bool extractRidges = true;
  // Boundaries between the generator's own cover classes.
  //
  // These are what a texture pass will later lay its seams along, and what
  // stops a biome transition cutting diagonally across a triangle: with the
  // boundary as a constraint every face belongs to exactly one class, so a
  // per-face material byte is a fact rather than a majority vote.
  bool extractBiomeBorders = true;
  bool extractRegionBorders = true;

  // Detail near channels. Rivers are what an eye follows through a landscape,
  // and an error criterion measuring height alone spends nothing on them -
  // a valley floor is flat, so it converges immediately.
  double riverToleranceScale = 0.3;
  double riverInfluenceMetres = 0.0;
  double riverInfluenceCells = 6.0;
  uint64_t seed = 0u;
};

struct RasterChains
{
  std::vector<Chain> chains;
  uint32_t coastRuns = 0u;
  uint32_t contourLines = 0u;
  uint32_t ridgeRuns = 0u;
  uint32_t biomeRuns = 0u;
  uint32_t regionEdges = 0u;
  double contourIntervalMetres = 0.0;
};

// Coastline, contours and ridges, traced from the raster itself rather than
// from a graph laid over it.
RasterChains extractRasterChains(
    const RasterTerrain &terrain, const RasterSamplingSettings &settings);

// Resolve retained source-graph edges into reaches and junctions. Rasters
// without graph metadata fall back to D8 receivers and flow accumulation.
//
// Same structure the map path produces, so the stroke expansion that carves
// banks and merges confluences is shared rather than duplicated.
ChannelNetwork resolveRasterChannels(
    const RasterTerrain &terrain,
    const RasterSamplingSettings &settings,
    const StrokeSettings &strokes);

// Height and material closures over the raster, ready for the mesh builder.
HeightFunction rasterHeightFunction(const RasterTerrain &terrain);
// Ocean is a level water surface, independent of coastal ground/river beds.
void flattenRasterOcean(const RasterTerrain &terrain, TerrainMesh &mesh);
// Expanded source river corridors carve the sampled field too, so interior
// refinement cannot put the original heightmap back inside the river bed.
HeightFunction rasterRiverHeightFunction(
    const RasterTerrain &terrain, const ChannelNetwork &channels);
MaterialFunction rasterMaterialFunction(
    const RasterTerrain &terrain, MaterialPalette &palette);

// Per-point tolerance scale, so the tessellation spends its budget where the
// generator says there is something to resolve.
std::function<double(Point2)> rasterToleranceScale(
    const RasterTerrain &terrain, const RasterSamplingSettings &settings);

} // namespace procedural::geometry
