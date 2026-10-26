#pragma once
#include "procedural/Geometry.hpp"
#include <cstdint>
#include <vector>

namespace procedural::geometry
{
class TerrainMapSource;

// A high-resolution map of one chunk, at a metre per pixel.
//
// The world map is a single 125 m field, and the erosion solver a 250 m one.
// Everything this generator has been asked for that it could not deliver - a
// visible cliff, a carriageway that differs by class, a river channel, a sand
// beach with a shape - failed for the same measured reason: the feature is
// smaller than one cell of the field it has to live in. A 7 m road cannot exist
// in a 125 m raster, and no amount of work on the road will change that.
//
// A chunk map is the answer the scale of the problem asks for: the coarse map
// decides where the mountains, rivers, lakes and deserts are, and a chunk map
// decides what the ground actually does over the next four kilometres. It is
// built on demand, used, and thrown away - a 4 km chunk at a metre is 4096^2,
// which is 67 MB for one float layer and perfectly affordable one chunk at a
// time, and utterly unaffordable for the 65,536 km^2 world at once.
enum class ChunkFeature : uint8_t
{
  None = 0u,
  River,      // inside the wet channel
  Bank,       // the carved bank either side of it
  Road,       // carriageway
  Shore,      // the sand or shingle band
  Cliff,      // a scarp face
  Count,
};

struct ChunkMapSettings
{
  double metresPerPixel = 1.0;
  // Fractal detail added on top of the upsampled coarse field. The coarse map
  // has nothing below 125 m, so without this a chunk map is a smooth
  // interpolation - more pixels carrying no more information.
  double detailAmplitudeMetres = 9.0;
  double detailScaleMetres = 220.0;
  uint32_t detailOctaves = 6u;
  // Detail is scaled by the ground: a mountainside is rough, a floodplain is
  // not, and a road surface is not rough at all.
  double detailSlopeGain = 2.6;
  // Thermal erosion at chunk resolution, which is the one erosion that acts at
  // the scale of a cliff - see the scarp findings. Rounds trade sharpness for
  // talus.
  uint32_t thermalRounds = 18u;
  double thermalRate = .45;
  double reposeDegrees = 36.0;
  // Hydraulic smoothing along the carved channels, so a river bed reads as a
  // bed rather than a trench stamped into noise.
  uint32_t channelSmoothing = 2u;
  bool carveFeatures = true;
};

struct ChunkMap
{
  Point2 origin{};            // world metres, lower corner
  double sizeMetres = 0.0;
  double metresPerPixel = 1.0;
  uint32_t side = 0u;
  std::vector<float> elevation;   // metres
  std::vector<float> flow;        // relative, 0..1, for wetness and material
  std::vector<uint8_t> water;     // 0 land, 1 ocean, 2 lake
  std::vector<uint8_t> biome;     // world::Biome
  std::vector<uint8_t> feature;   // ChunkFeature

  Point2 positionOf(uint32_t x, uint32_t y) const
  { return {origin.x + (x + .5) * metresPerPixel, origin.y + (y + .5) * metresPerPixel}; }
  float elevationAt(uint32_t x, uint32_t y) const { return elevation[size_t(y) * side + x]; }
};

// Builds the chunk map for the square of `sizeMetres` with its lower corner at
// `origin`. Deterministic for a given source and origin, so neighbouring chunks
// agree wherever they sample the same ground.
ChunkMap buildChunkMap(const TerrainMapSource &source, Point2 origin, double sizeMetres,
    const ChunkMapSettings &settings = {});
}
