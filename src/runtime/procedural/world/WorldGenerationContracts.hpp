#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world::rewrite
{
// All generation positions are world metres; GPU conversion belongs to consumers.
struct Position
{
  double x = 0, y = 0, z = 0;
};
struct WorldSpec
{
  uint64_t seed = 7;
  uint32_t generatorVersion = 1;
  double extentMetres = 4000;
  double seaLevelMetres = 0;
  bool validate(std::string *error = nullptr) const;
};
// Purpose-addressed randomness, independent of traversal and worker order.
uint64_t featureSeed(uint64_t worldSeed, uint64_t stageVersion, uint64_t featureId, uint64_t purpose, uint64_t sampleIndex = 0);

struct BoundaryEndpoint
{
  uint64_t id = 0;
  Position position;
};
struct BoundaryInput
{
  uint64_t id = 0;
  BoundaryEndpoint from, to;
  // Parameters measured from from toward to. Callers collect these before commit.
  std::vector<double> splits;
};
struct BoundaryVertex
{
  // Endpoints use owner=0, index=endpoint ID; interiors use owner=edge ID.
  // This tuple avoids a lossy hash as geometric identity.
  uint64_t owner = 0, index = 0;
  Position position;
};
struct BoundaryRecord
{
  uint64_t id = 0;
  std::vector<BoundaryVertex> vertices; // Canonical smaller endpoint ID first.
};
class BoundaryRegistry
{
public:
  // Atomic replacement on success; failures leave the last snapshot unchanged.
  // Inputs are copied and canonicalized, so publication order is irrelevant.
  bool commit(const std::vector<BoundaryInput> &inputs, std::string *error = nullptr);
  const std::vector<BoundaryRecord> &records() const
  {
    return records_;
  }
  const BoundaryRecord *find(uint64_t edgeId) const;
  static constexpr uint64_t parameterTicks = 1000000000000ULL;

private:
  std::vector<BoundaryRecord> records_;
};

struct FlowNode
{
  uint64_t id = 0;
  double localRunoffCumecs = 0, lossCumecs = 0;
};
struct FlowLink
{
  uint64_t from = 0, to = 0;
  double fraction = 1;
};
struct FlowResult
{
  uint64_t id = 0;
  double incomingCumecs = 0, outgoingCumecs = 0;
};
// A steady-flow DAG; runoff is incremental and all outlet fractions must sum to 1.
bool accumulateDischarge(const std::vector<FlowNode> &nodes, const std::vector<FlowLink> &links, std::vector<FlowResult> &result, std::string *error = nullptr);

struct RoadStation
{
  double distanceMetres = 0, heightMetres = 0, halfWidthMetres = 3;
  // Signed dz/dl across a section; no implicit inheritance from hillside slope.
  double crossSlope = 0;
};
class RoadDesignProfile
{
public:
  bool build(const std::vector<RoadStation> &stations, double maximumGrade, double maximumCrossSlope, std::string *error = nullptr);
  // Reject outside-domain samples rather than extending an infinite road plane.
  bool height(double distanceMetres, double lateralMetres, double &heightMetres) const;
  const std::vector<RoadStation> &stations() const
  {
    return stations_;
  }

private:
  std::vector<RoadStation> stations_;
};
} // namespace procedural::world::rewrite
