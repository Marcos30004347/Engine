#include "WorldCoverDetail.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace procedural::world::rewrite
{
namespace
{
bool fail(std::string *e, const char *s)
{
  if (e)
    *e = s;
  return false;
}
double smooth(double x)
{
  x = std::clamp(x, 0., 1.);
  return x * x * (3 - 2 * x);
}
double noise(uint64_t seed, double x, double z)
{
  auto ix = int64_t(std::floor(x)), iz = int64_t(std::floor(z));
  double u = smooth(x - ix), v = smooth(z - iz);
  auto value = [&](int64_t a, int64_t b)
  {
    return double(featureSeed(seed, 16, uint64_t(a), uint64_t(b)) >> 11) * 0x1.0p-53 * 2 - 1;
  };
  return (value(ix, iz) * (1 - u) + value(ix + 1, iz) * u) * (1 - v) + (value(ix, iz + 1) * (1 - u) + value(ix + 1, iz + 1) * u) * v;
}
} // namespace
uint32_t WorldCoverDetail::owner(double x, double z) const
{
  uint32_t candidate = UINT32_MAX;
  for (const auto &f : map_.faces)
  {
    if (f.layer != settings_.layer)
      continue;
    bool inside = false, boundary = false;
    for (size_t i = 0, j = f.vertices.size() - 1; i < f.vertices.size(); j = i++)
    {
      auto a = map_.vertices[f.vertices[j]], b = map_.vertices[f.vertices[i]];
      double dx = b.x - a.x, dz = b.z - a.z, len = std::hypot(dx, dz), dot = (x - a.x) * dx + (z - a.z) * dz;
      if (std::abs(dx * (z - a.z) - dz * (x - a.x)) < 1e-8 * len && dot >= 0 && dot <= len * len)
        boundary = true;
      if ((a.z > z) != (b.z > z) && x < (b.x - a.x) * (z - a.z) / (b.z - a.z) + a.x)
        inside = !inside;
    }
    if (boundary || inside)
    {
      auto cover = faceCover_[f.id - 1];
      if (definitions_[cover].hard)
        return cover;
      if (candidate == UINT32_MAX)
        candidate = cover;
    }
  }
  return candidate;
}
double WorldCoverDetail::distance(uint32_t cover, double x, double z) const
{
  double best = INFINITY;
  for (auto [a, b] : boundaries_[cover])
  {
    double dx = b.x - a.x, dz = b.z - a.z, t = std::clamp(((x - a.x) * dx + (z - a.z) * dz) / (dx * dx + dz * dz), 0., 1.);
    best = std::min(best, std::hypot(x - a.x - t * dx, z - a.z - t * dz));
  }
  return best;
}
bool WorldCoverDetail::build(const WorldFinalPatches &world, std::vector<CoverDefinition> defs, CoverDetailSettings settings, std::string *error)
{
  if (defs.empty() || defs.size() > 16 || settings.layer > 2 || !std::isfinite(settings.transitionMetres) || settings.transitionMetres <= 0 || settings.transitionMetres > 1000 || !std::isfinite(settings.warpMetres) ||
      settings.warpMetres < 0 || settings.warpMetres > settings.transitionMetres || !std::isfinite(settings.noiseWavelengthMetres) || settings.noiseWavelengthMetres < .1 || settings.noiseWavelengthMetres > 10000)
    return fail(error, "invalid cover settings");
  if (!auditWorldFinalPatches(world, error))
    return false;
  std::map<uint32_t, uint32_t> materials;
  std::set<uint32_t> ids;
  for (uint32_t i = 0; i < defs.size(); ++i)
  {
    const auto &d = defs[i];
    if (!ids.insert(d.id).second || d.materials.empty() || !std::isfinite(d.rowSpacingMetres) || d.rowSpacingMetres < .1 || !std::isfinite(d.rowAngleRadians) || (d.crop && !d.hard))
      return fail(error, "invalid cover definition");
    for (auto m : d.materials)
      if (!materials.emplace(m, i).second)
        return fail(error, "duplicate cover material mapping");
  }
  WorldCoverDetail result;
  result.map_ = world.map;
  result.settings_ = settings;
  result.definitions_ = std::move(defs);
  result.boundaries_.resize(result.definitions_.size());
  result.faceCover_.resize(world.map.faces.size(), UINT32_MAX);
  bool found = false;
  for (const auto &f : world.map.faces)
    if (f.layer == settings.layer)
    {
      auto it = materials.find(f.material);
      if (it == materials.end())
        return fail(error, "unmapped cover material");
      if (f.role != SurfaceRole::Ground && f.role != SurfaceRole::RiverBank && !result.definitions_[it->second].hard)
        return fail(error, "engineered and water surfaces require hard cover masks");
      result.faceCover_[f.id - 1] = it->second;
      found = true;
    }
  if (!found)
    return fail(error, "empty cover layer");
  for (auto e : world.map.edges)
    if (e.layer == settings.layer)
    {
      uint32_t a = e.leftFace ? result.faceCover_[e.leftFace - 1] : UINT32_MAX, b = e.rightFace ? result.faceCover_[e.rightFace - 1] : UINT32_MAX;
      if (a == b)
        continue; // Internal planning/tile/CDT edges never appear in the distance field.
      if (a != UINT32_MAX)
        result.boundaries_[a].emplace_back(world.map.vertices[e.a], world.map.vertices[e.b]);
      if (b != UINT32_MAX)
        result.boundaries_[b].emplace_back(world.map.vertices[e.a], world.map.vertices[e.b]);
    }
  *this = std::move(result);
  return true;
}
bool WorldCoverDetail::sample(double x, double z, double footprint, CoverSample &out, std::string *error) const
{
  if (definitions_.empty() || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(footprint) || footprint < 0 || footprint > 10000 || x < map_.settings.minX || x > map_.settings.maxX || z < map_.settings.minZ ||
      z > map_.settings.maxZ)
    return fail(error, "invalid cover sample");
  auto center = owner(x, z);
  if (center == UINT32_MAX)
    return fail(error, "cover sample outside layer");
  CoverSample result;
  result.owner = definitions_[center].id;
  result.hard = definitions_[center].hard;
  result.weights.resize(definitions_.size(), 0);
  auto filteredNoise = [&](uint64_t purpose)
  {
    double value = 0;
    for (int octave = 0; octave < 3; ++octave)
    {
      double wavelength = settings_.noiseWavelengthMetres / std::pow(2, octave), gain = std::pow(.5, octave) / 1.75;
      double filter = 1 - smooth(footprint * 2 / wavelength);
      value += gain * filter * noise(featureSeed(settings_.seed, 16, purpose, octave), x / wavelength, z / wavelength);
    }
    return value;
  };
  result.variation = filteredNoise(1);
  if (result.hard)
    result.weights[center] = 1;
  else
  {
    double dx = filteredNoise(2), dz = filteredNoise(3), length = std::max(1., std::hypot(dx, dz));
    double wx = x + settings_.warpMetres * dx / length, wz = z + settings_.warpMetres * dz / length;
    auto warped = owner(wx, wz);
    double total = 0, width = settings_.transitionMetres + footprint * .5;
    for (uint32_t i = 0; i < definitions_.size(); ++i)
      if (!definitions_[i].hard && !boundaries_[i].empty())
      {
        double d = distance(i, wx, wz) * (warped == i ? 1 : -1);
        result.weights[i] = smooth(.5 + d / (2 * width));
        total += result.weights[i];
      }
    if (total <= 1e-15)
      result.weights[center] = 1;
    else
      for (auto &w : result.weights)
        w /= total;
  }
  if (definitions_[center].crop)
  {
    auto d = definitions_[center];
    double phase = (x * std::cos(d.rowAngleRadians) + z * std::sin(d.rowAngleRadians)) / d.rowSpacingMetres;
    // Smooth periodic rows converge to their area mean instead of flickering at distance.
    double filter = 1 - smooth(footprint * 2 / d.rowSpacingMetres);
    result.cropRows = .5 + .5 * filter * std::cos(phase * 6.283185307179586);
  }
  out = std::move(result);
  return true;
}
bool WorldCoverDetail::page(int64_t ox, int64_t oz, uint32_t width, uint32_t height, uint32_t gutter, double spacing, double footprint, CoverPage &out, std::string *error) const
{
  if (!width || !height || width > 2048 || height > 2048 || gutter > 16 || !std::isfinite(spacing) || spacing <= 0 || spacing > 1000 || std::abs(double(ox)) > 1e9 || std::abs(double(oz)) > 1e9 ||
      uint64_t(width + 2 * gutter) * (height + 2 * gutter) > 1048576 || uint64_t(width + 2 * gutter) * (height + 2 * gutter) * map_.faces.size() > 50000000)
    return fail(error, "invalid cover page budget");
  CoverPage result{ox, oz, width, height, gutter, spacing, footprint, {}};
  for (int64_t z = -int64_t(gutter); z < int64_t(height) + gutter; ++z)
    for (int64_t x = -int64_t(gutter); x < int64_t(width) + gutter; ++x)
    {
      double px = std::clamp((ox + x) * spacing, map_.settings.minX, map_.settings.maxX), pz = std::clamp((oz + z) * spacing, map_.settings.minZ, map_.settings.maxZ);
      CoverSample sampleValue;
      if (!sample(px, pz, footprint, sampleValue, error))
        return false;
      result.samples.push_back(std::move(sampleValue));
    }
  out = std::move(result);
  return true;
}
bool WorldCoverDetail::scatter(uint32_t cover, double spacing, double radius, std::vector<Position> &out, std::string *error) const
{
  auto it = std::find_if(
      definitions_.begin(),
      definitions_.end(),
      [&](auto d)
      {
        return d.id == cover;
      });
  if (it == definitions_.end() || !std::isfinite(spacing) || spacing < .1 || !std::isfinite(radius) || radius < 0 || radius > spacing * .5)
    return fail(error, "invalid scatter settings");
  const auto &s = map_.settings;
  double candidates = (std::ceil(s.maxX / spacing) - std::floor(s.minX / spacing)) * (std::ceil(s.maxZ / spacing) - std::floor(s.minZ / spacing));
  if (candidates > 100000 || candidates * map_.faces.size() > 50000000)
    return fail(error, "scatter candidate budget exceeded");
  uint32_t index = it - definitions_.begin();
  std::vector<Position> result;
  for (int64_t z = int64_t(std::floor(s.minZ / spacing)); z < int64_t(std::ceil(s.maxZ / spacing)); ++z)
    for (int64_t x = int64_t(std::floor(s.minX / spacing)); x < int64_t(std::ceil(s.maxX / spacing)); ++x)
    {
      auto jitter = [&](uint64_t purpose)
      {
        return double(featureSeed(settings_.seed, 16, uint64_t(x), uint64_t(z), purpose) >> 11) * 0x1.0p-53;
      };
      Position p{(x + .2 + .6 * jitter(4)) * spacing, 0, (z + .2 + .6 * jitter(5)) * spacing};
      if (p.x < s.minX || p.x > s.maxX || p.z < s.minZ || p.z > s.maxZ)
        continue;
      if (owner(p.x, p.z) == index && distance(index, p.x, p.z) >= radius)
        result.push_back(p);
    }
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
