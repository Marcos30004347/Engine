#pragma once
#include "WorldGenerationContracts.hpp"
#include <array>
#include <functional>

namespace procedural::geometry {
struct RockFormation;
}

namespace procedural::world::rewrite {
enum class SplineKind
{
  Plateau,
  OpenCliff
};
enum class SplineRegion
{
  Ground,
  Cap,
  Brink,
  FreeFace,
  WeatheredFace,
  Apron
};

struct SplineFeature
{
  uint64_t id = 1, parentId = 0, seed = 0;
  uint32_t version = 1;
  SplineKind kind = SplineKind::Plateau;
  std::vector<Position> controls;
  // Roots use an absolute cap plane; children add this plane to their parent.
  double heightMetres = 100, slopeX = 0, slopeZ = 0;
  double faceWidthMetres = 15, weatheredWidthMetres = 25, apronWidthMetres = 40;
  double freeFaceFraction = .65, weatheredFraction = .25;
  double highSideWidthMetres = 100, endTaperMetres = 40;
  int highSide = 1; // +1 is left of the directed open stroke in x,z.
  double roughnessMetres = 0, noiseWavelengthMetres = 30;
};

struct SplineSampling
{
  double toleranceMetres = .05, maxSegmentMetres = 10;
  size_t maximumSegments = 4096;
};

struct SplineStation
{
  Position position, tangent;
  double distanceMetres = 0, signedDistanceMetres = 0, alongMetres = 0;
};

struct SplineSurfaceSample
{
  double height = 0, influence = 0;
  uint64_t featureId = 0;
  SplineRegion region = SplineRegion::Ground;
};

struct SplineFaceMesh
{
  std::vector<Position> vertices;
  std::vector<std::array<uint32_t, 3>> triangles;
  std::vector<Position> expectedNormals;
};

class WorldSplineLandforms
{
public:
  using BaseSurface = std::function<double(double, double)>;
  bool build(std::vector<SplineFeature>, const SplineSampling & = {}, std::string *error = nullptr);
  bool sample(double x, double z, const BaseSurface &, SplineSurfaceSample &, bool indexed = true) const;
  bool station(uint64_t id, double x, double z, SplineStation &, bool indexed = true) const;
  bool verticalFaces(const BaseSurface &, SplineFaceMesh &, std::string *error = nullptr) const;

  const std::vector<SplineFeature> &features() const
  {
    return definitions_;
  }

  const std::vector<Position> &polyline(size_t index) const
  {
    return forms_.at(index).points;
  }

private:
  struct Form
  {
    struct Node
    {
      double minX = 0, minZ = 0, maxX = 0, maxZ = 0;
      size_t begin = 0, end = 0, left = SIZE_MAX, right = SIZE_MAX;
    };

    std::vector<Node> tree;
    std::vector<size_t> segments;
    std::vector<Position> points;
    std::vector<double> along;
    double minX = 0, minZ = 0, maxX = 0, maxZ = 0;
    size_t parent = SIZE_MAX;
  };

  std::vector<SplineFeature> definitions_;
  std::vector<Form> forms_;
  SplineStation closest(size_t, double, double, bool) const;
  double apply(size_t, double, double, double, SplineSurfaceSample &, bool) const;
};

// Migration deliberately rejects access ramps: those are Step 06 records.
bool migrateClosedLandforms(const std::vector<procedural::geometry::RockFormation> &, std::vector<SplineFeature> &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
