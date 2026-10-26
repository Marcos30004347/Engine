#include "WorldRuntimeCover.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace procedural::world::rewrite {
namespace {
bool fail(std::string *e, const char *s)
{
  if (e)
  {
    *e = s;
  }
  return false;
}
} // namespace

bool validateRuntimeCoverMips(const RuntimeWorldAssets &a, std::string *error)
{
  if (a.coverDefinitions.empty())
  {
    return a.coverMips.empty() || fail(error, "cover mip definitions missing");
  }
  if (a.levels.empty() || a.coverPages.empty())
  {
    return fail(error, "cover mip world or base pages missing");
  }
  if (a.coverDefinitions.size() > 16 || a.coverMips.size() > 3)
  {
    return fail(error, "cover mip budget exceeded");
  }
  std::set<uint32_t> ids, materials;
  for (const auto &d : a.coverDefinitions)
  {
    if (!ids.insert(d.id).second || d.materials.empty() || (d.crop && !d.hard) || !std::isfinite(d.rowSpacingMetres) || d.rowSpacingMetres <= 0 || !std::isfinite(d.rowAngleRadians))
    {
      return fail(error, "invalid runtime cover definitions");
    }
    for (auto m : d.materials)
    {
      if (!materials.insert(m).second)
      {
        return fail(error, "ambiguous runtime cover material");
      }
    }
  }
  for (const auto &face : a.levels[0].world.map.faces)
  {
    if (face.layer == 0)
    {
      auto d = std::find_if(a.coverDefinitions.begin(), a.coverDefinitions.end(), [&](const auto &v) {
        return std::find(v.materials.begin(), v.materials.end(), face.material) != v.materials.end();
      });
      if (d == a.coverDefinitions.end() || (face.role != SurfaceRole::Ground && !d->hard))
      {
        return fail(error, "runtime cover definitions do not preserve ground ownership");
      }
    }
  }
  uint64_t samples = 0;
  auto inspect = [&](const std::vector<RuntimeCoverPage> &pages) {
    for (const auto &p : pages)
    {
      for (const auto &s : p.page.samples)
      {
        if (++samples > 2000000 || s.weights.size() != a.coverDefinitions.size())
        {
          return false;
        }
        auto d = std::find_if(a.coverDefinitions.begin(), a.coverDefinitions.end(), [&](const auto &v) {
          return v.id == s.owner;
        });
        if (d == a.coverDefinitions.end() || d->hard != s.hard)
        {
          return false;
        }
        if (s.hard)
        {
          for (size_t i = 0; i < s.weights.size(); ++i)
          {
            if (s.weights[i] != (a.coverDefinitions[i].id == s.owner ? 1. : 0.))
            {
              return false;
            }
          }
        }
      }
    }
    return true;
  };
  if (!inspect(a.coverPages))
  {
    return fail(error, "invalid cover vocabulary or sample budget");
  }
  for (size_t i = 0; i < a.coverMips.size(); ++i)
  {
    const auto &pages = a.coverMips[i];
    if (pages.size() != a.coverPages.size() || !inspect(pages))
    {
      return fail(error, "incomplete runtime cover mip");
    }
    const double scale = std::ldexp(1., int(i) + 1);
    for (const auto &p : pages)
    {
      if (p.page.spacingMetres != a.coverPages[0].page.spacingMetres * scale || p.page.footprintMetres != a.coverPages[0].page.footprintMetres * scale)
      {
        return fail(error, "invalid cover mip lattice");
      }
    }
    // Reuse the complete page/gutter/version validator, with no recursive mips.
    auto level = a;
    level.coverMips.clear();
    level.coverDefinitions.clear();
    level.coverPages = pages;
    if (!validateRuntimeAssets(level, error))
    {
      return false;
    }
  }
  return true;
}

bool buildRuntimeCoverMips(RuntimeWorldAssets &assets, const WorldCoverDetail &detail, std::vector<CoverDefinition> definitions, uint32_t levels, std::string *error)
{
  if (levels < 1 || levels > 4 || !validateRuntimeAssets(assets, error))
  {
    return fail(error, "invalid cover mip build input");
  }
  auto result = assets;
  result.coverDefinitions = std::move(definitions);
  result.coverMips.clear();
  const auto &map = assets.levels[0].world.map;
  std::vector<double> xs{map.settings.minX}, zs{map.settings.minZ};
  xs.insert(xs.end(), map.settings.cutsX.begin(), map.settings.cutsX.end());
  xs.push_back(map.settings.maxX);
  zs.insert(zs.end(), map.settings.cutsZ.begin(), map.settings.cutsZ.end());
  zs.push_back(map.settings.maxZ);
  for (uint32_t level = 1; level < levels; ++level)
  {
    std::vector<RuntimeCoverPage> pages;
    double scale = std::ldexp(1., level), spacing = assets.coverPages[0].page.spacingMetres * scale, footprint = assets.coverPages[0].page.footprintMetres * scale;
    for (const auto &tile : assets.levels[0].world.tiles)
    {
      double x0 = std::floor(xs[tile.x] / spacing), z0 = std::floor(zs[tile.z] / spacing), x1 = std::ceil(xs[tile.x + 1] / spacing), z1 = std::ceil(zs[tile.z + 1] / spacing);
      if (std::max(std::abs(x0), std::abs(z0)) > 1e9 || x1 - x0 > 2047 || z1 - z0 > 2047)
      {
        return fail(error, "cover mip page exceeds budget");
      }
      CoverPage page;
      if (!detail.page(int64_t(x0), int64_t(z0), uint32_t(x1 - x0 + 1), uint32_t(z1 - z0 + 1), 1, spacing, footprint, page, error))
      {
        return false;
      }
      pages.push_back({map.revision, assets.dependencies, std::move(page)});
    }
    result.coverMips.push_back(std::move(pages));
  }
  if (!validateRuntimeAssets(result, error))
  {
    return false;
  }
  assets = std::move(result);
  return true;
}

bool sampleRuntimeCover(const RuntimeWorldAssets &a, double x, double z, uint32_t material, double footprint, CoverSample &out, std::string *error)
{
  if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(footprint) || footprint < 0 || a.coverDefinitions.empty() || a.coverPages.empty())
  {
    return fail(error, "invalid runtime cover query");
  }
  if (a.levels.empty())
  {
    return fail(error, "cover world geometry missing");
  }
  const auto &bounds = a.levels[0].world.map.settings;
  if (x < bounds.minX || x > bounds.maxX || z < bounds.minZ || z > bounds.maxZ)
  {
    return fail(error, "cover query outside world");
  }
  auto definition = std::find_if(a.coverDefinitions.begin(), a.coverDefinitions.end(), [&](const auto &d) {
    return std::find(d.materials.begin(), d.materials.end(), material) != d.materials.end();
  });
  if (definition == a.coverDefinitions.end())
  {
    return fail(error, "material has no cover definition");
  }
  const std::vector<RuntimeCoverPage> *pages = &a.coverPages;
  for (const auto &mip : a.coverMips)
  {
    if (mip.empty())
    {
      return fail(error, "cover mip missing pages");
    }
    if (pages->front().page.footprintMetres >= footprint)
    {
      break;
    }
    pages = &mip;
  }
  for (const auto &versioned : *pages)
  {
    const auto &p = versioned.page;
    if (!p.width || !p.height || p.width > 2048 || p.height > 2048 || p.gutter > 16 || !std::isfinite(p.spacingMetres) || p.spacingMetres <= 0 ||
        p.samples.size() != uint64_t(p.width + 2 * p.gutter) * (p.height + 2 * p.gutter))
    {
      return fail(error, "invalid cover page reader input");
    }
    double px = x / p.spacingMetres - p.originX, pz = z / p.spacingMetres - p.originZ;
    if (px < 0 || pz < 0 || px > p.width - 1 || pz > p.height - 1)
    {
      continue;
    }
    const auto ix = uint32_t(std::floor(px + .5)) + p.gutter, iz = uint32_t(std::floor(pz + .5)) + p.gutter;
    CoverSample sample = p.samples.at(size_t(iz) * (p.width + 2 * p.gutter) + ix);
    // Preserve the triangle's hard owner, even when a narrow road is smaller
    // than a mip texel. Soft triangles cannot borrow a hard owner's texel.
    if (definition->hard || sample.hard)
    {
      sample.owner = definition->id;
      sample.hard = definition->hard;
      sample.weights.assign(a.coverDefinitions.size(), 0);
      sample.weights[size_t(definition - a.coverDefinitions.begin())] = 1;
      if (p.samples[size_t(iz) * (p.width + 2 * p.gutter) + ix].owner != definition->id)
      {
        sample.variation = .5;
        sample.cropRows = .5;
      }
    }
    out = std::move(sample);
    return true;
  }
  return fail(error, "cover query outside page coverage");
}
} // namespace procedural::world::rewrite
