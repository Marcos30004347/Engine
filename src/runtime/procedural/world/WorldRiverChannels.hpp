#pragma once
#include "WorldRiverPath.hpp"
#include "WorldWatersheds.hpp"
#include <functional>

namespace procedural::world::rewrite {
class WorldPhysicalTerrain;

struct ChannelNode
{
  uint64_t id = 0;
  Position water;
  double lossCumecs = 0;
};

struct CatchmentRunoff
{
  uint64_t id = 0, nodeId = 0;
  double areaMetres2 = 0, rainfallMmPerHour = 0, runoffCoefficient = 1;
};

struct ChannelSettings
{
  double minimumBottomWidthMetres = 2, maximumBottomWidthMetres = 20, widthPerSqrtDischarge = 1;
  double bankSideSlope = 1.5, manningN = .035, minimumEnergySlope = .001;
  double confluenceBlendMetres = 20, bankTransitionMetres = 8, maximumDepthMetres = 100;
};

struct ChannelSection
{
  double discharge = 0, bottomWidth = 0, depth = 0, waterWidth = 0, area = 0, velocity = 0, capacity = 0;
};

struct ChannelLedger
{
  uint64_t nodeId = 0;
  double incrementalArea = 0, localRunoff = 0, incoming = 0, loss = 0, outgoing = 0;
};

struct ChannelReach
{
  uint64_t from = 0, to = 0;
  Position a, b;
  double length = 0, discharge = 0, energySlope = 0;
  ChannelSection normal, fromSection, toSection;
  RiverPath path;
};

struct ChannelSample
{
  double terrainHeight = 0, waterHeight = 0, signedWaterDepth = -1e30;
  bool wet = false;
};

bool solveChannelSection(double discharge, double energySlope, const ChannelSettings &, ChannelSection &, std::string *error = nullptr);

class WorldRiverChannels
{
public:
  using BaseSurface = std::function<double(double, double)>;
  // Optional paths use sorted node indices, one outgoing path per source node.
  // Empty path collection retains straight-link/split compatibility.
  bool build(std::vector<ChannelNode>, std::vector<FlowLink>, std::vector<CatchmentRunoff>, const ChannelSettings &, std::string *error = nullptr, const std::vector<RiverPath> &paths = {});
  bool buildFromWatersheds(const RiverGrowthProduct &, const WatershedProduct &, double rainfallMmPerHour, double runoffCoefficient, const ChannelSettings &, bool allowTruncated = false, std::string *error = nullptr);
  // Reject profile growth/topology edits outside the previously reserved canyon.
  bool validateReservations(const WorldPhysicalTerrain &, std::string *error = nullptr) const;
  bool sample(double x, double z, const BaseSurface &, ChannelSample &) const;
  bool section(size_t reach, double distance, ChannelSection &, double &waterHeight) const;

  const std::vector<ChannelLedger> &ledger() const
  {
    return ledger_;
  }

  const std::vector<ChannelReach> &reaches() const
  {
    return reaches_;
  }

  const ChannelSettings &settings() const
  {
    return settings_;
  }

private:
  ChannelSettings settings_;
  std::vector<ChannelNode> nodes_;
  std::vector<ChannelLedger> ledger_;
  std::vector<ChannelReach> reaches_;
  std::vector<ChannelSection> junctions_;
  std::vector<Position> gradients_;
  std::vector<size_t> degree_;
  double resolvedWater(const ChannelReach &, double, double, double) const;
};
} // namespace procedural::world::rewrite
