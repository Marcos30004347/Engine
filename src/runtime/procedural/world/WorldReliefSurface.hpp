#pragma once
#include "WorldWatersheds.hpp"

namespace procedural::world::rewrite {
struct TerrainPrimitive
{
  uint64_t id = 0, noiseSeed = 0;
  Position centre;
  double radiusMetres = 1, noiseAmplitudeMetres = 0, noiseWavelengthMetres = 100;
};

class CompactTerrainField
{
public:
  bool build(std::vector<TerrainPrimitive>, std::string *error = nullptr);
  bool sample(double x, double z, double &height, bool accelerated = true) const;
  static double weight(double distance, double radius);

  const std::vector<TerrainPrimitive> &primitives() const
  {
    return primitives_;
  }

private:
  struct Node
  {
    size_t begin = 0, end = 0, left = 0, right = 0;
    double minX = 0, minZ = 0, maxX = 0, maxZ = 0;
  };

  std::vector<TerrainPrimitive> primitives_;
  std::vector<Node> tree_;
  std::pair<double, double> evaluate(size_t, double, double, bool) const;
};

struct ReliefSurfaceSettings
{
  double sampleSpacingMetres = 150;
  uint32_t poissonSamplesPerCell = 0; // Optional discrete dart sampling; zero preserves legacy supports.
  double supportScale = 1.25;         // >1 proves positive support on every source triangle.
  double roughnessFraction = 0.08;
  double shoreTransitionMetres = 30;
  double maximumCoastalSlope = 0; // Optional coast-envelope compatibility before blending.
};

class WorldReliefSurface
{
public:
  bool build(const LandProduct &, const RiverGrowthProduct &, const WatershedProduct &, const ReliefSurfaceSettings &, std::string *error = nullptr);
  // Ocean/outside-domain and missing support are failures, never height zero.
  bool sample(double x, double z, double &height, bool accelerated = true) const;

  const CompactTerrainField &field() const
  {
    return field_;
  }

private:
  CompactTerrainField field_;
  std::vector<std::vector<Position>> domain_;
  std::vector<std::pair<Position, Position>> coast_;
  double shoreWidth_ = 30, seaLevel_ = 0;
};
} // namespace procedural::world::rewrite
