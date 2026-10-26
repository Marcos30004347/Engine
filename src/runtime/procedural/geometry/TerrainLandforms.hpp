#pragma once
#include "procedural/geometry/TerrainMesh.hpp"
#include <functional>

namespace procedural::geometry
{
// Closed Catmull-Rom footprints in world metres. Long narrow footprints form
// escarpments; broad footprints form plateaus; small ones form isolated buttes.
struct RockFormation
{
  std::vector<Point2> controls;
  double topMetres = 500;
  double faceWidthMetres = 180;
  // A weathered escarpment is not one straight ramp. It is a resistant free
  // face at the brink over a debris slope standing at the angle of repose - the
  // Wood/Savigear model - and the debris slope carries most of the height. The
  // face was a single linear ramp at 58 to 64 degrees, which no slope mantled
  // in its own scree can hold; that is what made the cliffs read as extruded
  // rather than eroded.
  //
  // The landslide pass in the stream-power solver would impose this by itself,
  // but it runs on a 250 m grid where a 40 m cliff is a 0.12 gradient - far
  // under tan(37 deg) - so it never sees one. The repose angle has to be
  // enforced on the analytic profile, at the profile's own resolution.
  double reposeAngleDegrees = 35.0;
  // Share of the total rise taken by the free face. The rest is debris.
  double freeFaceFraction = .32;
  // Depth of the gullies cut across the face, as a fraction of its width, and
  // their spacing in metres. A face with no along-strike variation is a
  // curtain; a real one is a row of spurs and re-entrants.
  double gullyDepth = .22;
  double gullySpacingMetres = 190.0;
  double talusWidthMetres = 450;
  double stratumMetres = 60;
  double rampWidthMetres = 0;
  double rampAzimuth = 0;
  PlanetLithology lithology = PlanetLithology::Sandstone;
  uint64_t seed = 0;
  uint32_t parent = UINT32_MAX; // earlier formation index, or an independent root
  double capReliefMetres = 0.5;
  double capRoundness = 0.0;
  double capResistance = 40.0;
  double upliftMetresPerYear = 0;
  bool cutCap = false; // permit trimming higher bedrock to the cap
};
struct LandformSettings
{
  bool enabled = false;
  uint32_t maximumCount = 32; // root formations, excluding nested tiers
  uint32_t cliffLevels = 3;
  // Authored formations replace automatic placement when nonempty.
  std::vector<RockFormation> authored;
};
struct LandformSample
{
  double height = 0;
  double influence = 0;
  double hardness = 1;
  double upliftMetresPerYear = 0;
  SurfaceRole role = SurfaceRole::Ground;
  PlanetLithology lithology = PlanetLithology::Sedimentary;
};
std::vector<RockFormation> nestRockFormations(std::vector<RockFormation> roots, uint32_t levels, double spacing);

class SplineLandforms
{
public:
  void build(std::vector<RockFormation> forms, double extent, double spacing);
  LandformSample sample(Point2 point, double ground) const;
  const std::vector<Chain> &chains() const { return chains_; }
  std::vector<RockFormation> definitions() const
  { std::vector<RockFormation> result; for(const auto &f:forms_)result.push_back(f.spec); return result; }
  size_t size() const { return forms_.size(); }
private:
  // `reach` bounds how far this formation can influence a sample: the
  // farthest rim vertex from `center`, plus the widest apron the face,
  // ramp and talus can ever reach. Outside it the rim walk cannot change
  // the answer, so it is skipped rather than performed and discarded.
  // A bounding-volume hierarchy over the rim's segments. The rim is a closed
  // ring of up to a few hundred Catmull-Rom samples and every surface query
  // walked all of them, twice over: once for the nearest boundary distance and
  // once for the ray-crossing parity that gives it a sign. Both prune exactly
  // - the nearest query on box distance, the parity query on the fact that a
  // segment can only cross the ray if the node spans the sample's y and
  // reaches past its x - so the answer is the one the linear scan gave.
  struct RimTree
  {
    struct Node
    {
      double minX = 0, minY = 0, maxX = 0, maxY = 0;
      uint32_t start = 0, count = 0, right = 0; // count == 0 marks an interior node
    };
    std::vector<Node> nodes;
    std::vector<uint32_t> segments;
    void build(const std::vector<Point2> &rim);
    // Signed distance to the ring: negative inside, matching a linear scan.
    double signedDistance(Point2 p, const std::vector<Point2> &rim) const;
  };
  struct Form { Point2 center; RockFormation spec; std::vector<Point2> rim; double reach = 0; RimTree tree; };
  size_t bucket(Point2 p) const;
  double extent_ = 1;
  static constexpr uint32_t side_ = 128;
  std::vector<Form> forms_;
  std::vector<std::vector<uint32_t>> buckets_;
  std::vector<Chain> chains_;
};
} // namespace procedural::geometry
