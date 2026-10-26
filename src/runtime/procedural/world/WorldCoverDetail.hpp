#pragma once
#include "WorldFinalPatches.hpp"

namespace procedural::world::rewrite {
struct CoverDefinition
{
  uint32_t id = 0;
  std::vector<uint32_t> materials;
  bool hard = false, crop = false;
  double rowSpacingMetres = 1, rowAngleRadians = 0;
};

struct CoverDetailSettings
{
  uint64_t seed = 1;
  uint32_t layer = 0;
  double transitionMetres = 3, warpMetres = 1, noiseWavelengthMetres = 12;
};

struct CoverSample
{
  std::vector<double> weights; // Definition order, normalized; at most 16 covers.
  uint32_t owner = 0;
  bool hard = false;
  double variation = 0, cropRows = 0;
};

struct CoverPage
{
  int64_t originX = 0, originZ = 0; // World lattice indices, independent of patch/tile IDs.
  uint32_t width = 0, height = 0, gutter = 0;
  double spacingMetres = 0, footprintMetres = 0;
  std::vector<CoverSample> samples; // Row-major, including gutters.
};

class WorldCoverDetail
{
public:
  bool build(const WorldFinalPatches &, std::vector<CoverDefinition>, CoverDetailSettings, std::string *error = nullptr);
  bool sample(double x, double z, double footprintMetres, CoverSample &, std::string *error = nullptr) const;
  bool page(int64_t originX, int64_t originZ, uint32_t width, uint32_t height, uint32_t gutter, double spacingMetres, double footprintMetres, CoverPage &, std::string *error = nullptr) const;
  // Map-space candidates (y=0); consumers lift them onto the authoritative surface.
  // A full disc must fit the requested cover's domain.
  bool scatter(uint32_t cover, double spacingMetres, double radiusMetres, std::vector<Position> &, std::string *error = nullptr) const;

private:
  SurfaceArrangement map_;
  std::vector<CoverDefinition> definitions_;
  CoverDetailSettings settings_;
  std::vector<uint32_t> faceCover_;
  std::vector<std::vector<std::pair<Position, Position>>> boundaries_;
  // Conservative bins retain face order, including both sides of boundaries.
  // Sampling cost depends on local coverage, not every face in the city tile.
  uint32_t ownerGridSize_ = 1;
  size_t maximumOwnerCandidates_ = 0;
  std::vector<std::vector<uint32_t>> ownerGrid_;
  uint32_t ownerBin(double coordinate, double minimum, double maximum) const;
  uint32_t owner(double x, double z) const;
  double distance(uint32_t cover, double x, double z) const;
};
} // namespace procedural::world::rewrite
