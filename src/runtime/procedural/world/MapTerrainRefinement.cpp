#include "procedural/world/MapTerrainRefinement.hpp"

#include "procedural/world/MapClimate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>

namespace procedural::world {
namespace {

struct FloodEntry
{
  double elevation = 0.0;
  uint32_t cell = 0u;

  bool operator<(const FloodEntry &other) const
  {
    if (elevation != other.elevation)
    {
      return elevation > other.elevation;
    }
    return cell > other.cell;
  }
};

constexpr std::array<int32_t, 8> kDx{-1, 0, 1, -1, 1, -1, 0, 1};
constexpr std::array<int32_t, 8> kDy{-1, -1, -1, 0, 0, 1, 1, 1};

double biomeErodibility(const uint8_t encoded)
{
  const Biome biome = encoded < static_cast<uint8_t>(Biome::Count) ? static_cast<Biome>(encoded) : Biome::Grassland;
  switch (biome)
  {
  case Biome::BareRock:
  case Biome::Glacier:
    return 0.35;
  case Biome::Desert:
  case Biome::SandDesert:
  case Biome::ColdDesert:
    return 0.70;
  case Biome::Wetland:
  case Biome::Swamp:
  case Biome::Mangrove:
    return 0.82;
  case Biome::SaltFlat:
    return 0.22;
  default:
    return 0.55;
  }
}

uint8_t normalizedByte(const double value)
{
  return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

} // namespace

bool refineTerrainRaster(const TerrainRefinementInput &input, const TerrainRefinementSettings &settings, TerrainRefinementMaps &maps, TerrainRefinementReport *report, std::string *error)
{
  maps = TerrainRefinementMaps{};
  if (report != nullptr)
  {
    *report = TerrainRefinementReport{};
  }
  const auto fail = [&](const std::string &message) {
    if (error != nullptr)
    {
      *error = message;
    }
    return false;
  };
  const size_t count = static_cast<size_t>(input.side) * input.side;
  if (input.side < 2u || !(input.extentMetres > 0.0) || !std::isfinite(input.extentMetres) || input.elevationMetres.size() != count || input.moisture.size() != count || input.biome.size() != count ||
      input.waterType.size() != count)
  {
    return fail("terrain refinement input fields are inconsistent");
  }
  if (!(settings.maximumIncisionMetres >= 0.0) || !(settings.maximumDepositionMetres >= 0.0) || !(settings.drainageExponent > 0.0) || !(settings.slopeExponent > 0.0) || !std::isfinite(settings.maximumIncisionMetres) ||
      !std::isfinite(settings.maximumDepositionMetres))
  {
    return fail("terrain refinement settings are invalid");
  }

  const uint32_t side = input.side;
  const double texelMetres = input.extentMetres / side;
  const double pixelArea = texelMetres * texelMetres;
  const double drainageEpsilon = std::max(1.0e-5, texelMetres * 1.0e-7);
  std::vector<double> filled = input.elevationMetres;
  std::vector<uint8_t> visited(count, 0u);
  std::priority_queue<FloodEntry> flood;
  const auto seed = [&](const uint32_t cell) {
    if (visited[cell] != 0u)
    {
      return;
    }
    visited[cell] = 1u;
    flood.push({filled[cell], cell});
  };
  for (uint32_t y = 0u; y < side; ++y)
  {
    for (uint32_t x = 0u; x < side; ++x)
    {
      const uint32_t cell = y * side + x;
      if (x == 0u || y == 0u || x + 1u == side || y + 1u == side || input.waterType[cell] != 0u)
      {
        seed(cell);
      }
    }
  }
  while (!flood.empty())
  {
    const FloodEntry current = flood.top();
    flood.pop();
    const int32_t x = static_cast<int32_t>(current.cell % side);
    const int32_t y = static_cast<int32_t>(current.cell / side);
    for (size_t direction = 0u; direction < kDx.size(); ++direction)
    {
      const int32_t nx = x + kDx[direction];
      const int32_t ny = y + kDy[direction];
      if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(side) || ny >= static_cast<int32_t>(side))
      {
        continue;
      }
      const uint32_t neighbour = static_cast<uint32_t>(ny) * side + static_cast<uint32_t>(nx);
      if (visited[neighbour] != 0u)
      {
        continue;
      }
      visited[neighbour] = 1u;
      filled[neighbour] = std::max(filled[neighbour], current.elevation + drainageEpsilon);
      flood.push({filled[neighbour], neighbour});
    }
  }

  std::vector<uint32_t> receiver(count, std::numeric_limits<uint32_t>::max());
  std::vector<double> receiverDistance(count, texelMetres);
  std::vector<double> slope(count, 0.0);
  for (uint32_t y = 0u; y < side; ++y)
  {
    for (uint32_t x = 0u; x < side; ++x)
    {
      const uint32_t cell = y * side + x;
      if (input.waterType[cell] != 0u)
      {
        continue;
      }
      double best = filled[cell];
      for (size_t direction = 0u; direction < kDx.size(); ++direction)
      {
        const int32_t nx = static_cast<int32_t>(x) + kDx[direction];
        const int32_t ny = static_cast<int32_t>(y) + kDy[direction];
        if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(side) || ny >= static_cast<int32_t>(side))
        {
          continue;
        }
        const uint32_t neighbour = static_cast<uint32_t>(ny) * side + static_cast<uint32_t>(nx);
        if (filled[neighbour] < best || (filled[neighbour] == best && neighbour < receiver[cell]))
        {
          best = filled[neighbour];
          receiver[cell] = neighbour;
          receiverDistance[cell] = direction == 0u || direction == 2u || direction == 5u || direction == 7u ? texelMetres * std::sqrt(2.0) : texelMetres;
        }
      }
      if (receiver[cell] != std::numeric_limits<uint32_t>::max())
      {
        slope[cell] = std::max(0.0, (filled[cell] - filled[receiver[cell]]) / receiverDistance[cell]);
      }
    }
  }

  std::vector<uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(), [&](const uint32_t left, const uint32_t right) {
    if (filled[left] != filled[right])
    {
      return filled[left] > filled[right];
    }
    return left > right;
  });
  std::vector<double> accumulation(count, 0.0);
  for (uint32_t cell = 0u; cell < count; ++cell)
  {
    if (input.waterType[cell] == 0u)
    {
      accumulation[cell] = 0.20 + std::clamp(static_cast<double>(input.moisture[cell]), 0.0, 1.0);
    }
  }
  for (const uint32_t cell : order)
  {
    if (receiver[cell] != std::numeric_limits<uint32_t>::max())
    {
      accumulation[receiver[cell]] += accumulation[cell];
    }
  }
  const double maximumFlow = *std::max_element(accumulation.begin(), accumulation.end());
  const double flowDenominator = std::log1p(std::max(1.0, maximumFlow));

  maps.side = side;
  maps.elevationMetres = input.elevationMetres;
  maps.wear.assign(count, 0.0f);
  maps.flow.assign(count, 0.0f);
  maps.deposition.assign(count, 0.0f);
  maps.soilDepthMetres.assign(count, 0.0f);
  maps.substrate.assign(count, static_cast<uint8_t>(GroundSubstrate::None));
  maps.soilLayers.assign(count, {0u, 0u, 0u, 0u});
  std::vector<double> sediment(count, 0.0);
  double erodedDepthSum = 0.0;
  double depositedDepthSum = 0.0;
  double exportedDepthSum = 0.0;
  for (const uint32_t cell : order)
  {
    if (input.waterType[cell] != 0u)
    {
      exportedDepthSum += sediment[cell];
      continue;
    }
    const double flow = std::log1p(accumulation[cell]) / flowDenominator;
    const double slopeResponse = std::clamp(slope[cell] / 0.025, 0.0, 1.0);
    const double streamPower = std::pow(flow, settings.drainageExponent) * std::pow(slopeResponse, settings.slopeExponent);
    const double wear = std::clamp(streamPower * biomeErodibility(input.biome[cell]), 0.0, 1.0);
    const double eroded = settings.maximumIncisionMetres * wear;
    const double available = sediment[cell] + eroded;
    const double capacity = settings.maximumIncisionMetres * (0.08 + 1.8 * streamPower) * (0.35 + flow);
    const double deposited = std::min(settings.maximumDepositionMetres, std::max(0.0, available - capacity) * std::clamp(1.0 - slopeResponse, 0.15, 0.85));
    maps.wear[cell] = static_cast<float>(wear);
    maps.flow[cell] = static_cast<float>(flow);
    maps.deposition[cell] = static_cast<float>(deposited / std::max(1.0e-9, settings.maximumDepositionMetres));
    maps.elevationMetres[cell] = std::max(input.seaLevelMetres + 0.01, input.elevationMetres[cell] - eroded + deposited);
    erodedDepthSum += eroded;
    depositedDepthSum += deposited;
    const double outgoing = available - deposited;
    if (receiver[cell] != std::numeric_limits<uint32_t>::max())
    {
      sediment[receiver[cell]] += outgoing;
    }
    else
    {
      exportedDepthSum += outgoing;
    }
  }

  for (uint32_t cell = 0u; cell < count; ++cell)
  {
    if (input.waterType[cell] != 0u)
    {
      continue;
    }
    const Biome biome = input.biome[cell] < static_cast<uint8_t>(Biome::Count) ? static_cast<Biome>(input.biome[cell]) : Biome::Grassland;
    const double moisture = std::clamp(static_cast<double>(input.moisture[cell]), 0.0, 1.0);
    const double steep = std::clamp(slope[cell] / 0.08, 0.0, 1.0);
    const double alluvium = std::clamp(static_cast<double>(maps.deposition[cell]) * 1.4 + static_cast<double>(maps.flow[cell]) * (1.0 - steep) * 0.32, 0.0, 1.0);
    double organic = moisture * (1.0 - steep) * 0.45;
    if (biome == Biome::Wetland || biome == Biome::Swamp || biome == Biome::Mangrove)
    {
      organic = std::max(organic, 0.82);
    }
    double bedrock = std::clamp(steep * 0.9 + maps.wear[cell] * 0.55, 0.0, 1.0);
    double regolith = std::max(0.0, 1.0 - bedrock - alluvium - organic);
    const double total = std::max(1.0e-9, bedrock + regolith + alluvium + organic);
    bedrock /= total;
    regolith /= total;
    const double normalizedAlluvium = alluvium / total;
    organic /= total;
    std::array<uint8_t, 4> layers{normalizedByte(bedrock), normalizedByte(regolith), normalizedByte(normalizedAlluvium), normalizedByte(organic)};
    int32_t remainder = 255 - static_cast<int32_t>(layers[0]) - layers[1] - layers[2] - layers[3];
    const size_t dominant = static_cast<size_t>(std::distance(layers.begin(), std::max_element(layers.begin(), layers.end())));
    layers[dominant] = static_cast<uint8_t>(static_cast<int32_t>(layers[dominant]) + remainder);
    maps.soilLayers[cell] = layers;
    maps.soilDepthMetres[cell] = static_cast<float>(std::clamp((0.15 + regolith * 2.8 + normalizedAlluvium * 7.0 + organic * 3.5) * (1.0 - steep * 0.85), 0.02, 12.0));

    GroundSubstrate substrate = GroundSubstrate::Regolith;
    if (biome == Biome::SaltFlat)
    {
      substrate = GroundSubstrate::SaltCrust;
    }
    else if (biome == Biome::Glacier || biome == Biome::Tundra)
    {
      substrate = GroundSubstrate::GlacialTill;
    }
    else if (biome == Biome::Shore || biome == Biome::Desert || biome == Biome::SandDesert)
    {
      substrate = GroundSubstrate::Sand;
    }
    else if (organic > 0.48)
    {
      substrate = GroundSubstrate::Peat;
    }
    else if (normalizedAlluvium > 0.38)
    {
      substrate = GroundSubstrate::Alluvium;
    }
    else if (bedrock > 0.62)
    {
      substrate = steep > 0.72 ? GroundSubstrate::Scree : GroundSubstrate::Bedrock;
    }
    else if (moisture > 0.58 && steep < 0.30)
    {
      substrate = GroundSubstrate::ClayLoam;
    }
    maps.substrate[cell] = static_cast<uint8_t>(substrate);
  }

  if (report != nullptr)
  {
    report->erodedVolumeCubicMetres = erodedDepthSum * pixelArea;
    report->depositedVolumeCubicMetres = depositedDepthSum * pixelArea;
    report->sedimentExportedVolumeCubicMetres = exportedDepthSum * pixelArea;
    report->maximumFlowCells = maximumFlow;
    report->massBalanceRelativeError = erodedDepthSum <= 1.0e-12 ? 0.0 : std::abs(erodedDepthSum - depositedDepthSum - exportedDepthSum) / erodedDepthSum;
  }
  return true;
}

} // namespace procedural::world
