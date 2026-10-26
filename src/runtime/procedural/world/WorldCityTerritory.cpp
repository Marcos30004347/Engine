#include "WorldCityTerritory.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace procedural::world::rewrite {
namespace {
using Poly = std::vector<Position>;

double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

double area(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  }
  return a * .5;
}

Poly intersection(Poly p, const Poly &mask)
{
  for (size_t i = 0; i < mask.size(); ++i)
  {
    Poly out;
    auto a = mask[i], b = mask[(i + 1) % mask.size()];
    for (size_t k = 0; k < p.size(); ++k)
    {
      auto u = p[k], v = p[(k + 1) % p.size()];
      double du = cross(a, b, u), dv = cross(a, b, v);
      if (du >= 0)
      {
        out.push_back(u);
      }
      if ((du < 0) != (dv < 0))
      {
        double t = du / (du - dv);
        out.push_back({u.x + (v.x - u.x) * t, 0, u.z + (v.z - u.z) * t});
      }
    }
    p = std::move(out);
  }
  return p;
}

// Dissolve triangulation seams only when their union is exactly convex. These
// masks retain the same land; the original patch/cell identities remain upstream.
void mergeGroundPieces(std::vector<Poly> &pieces)
{
  for (size_t i = 0; i < pieces.size(); ++i)
  {
    for (size_t j = i + 1; j < pieces.size();)
    {
      Poly points = pieces[i];
      points.insert(points.end(), pieces[j].begin(), pieces[j].end());
      std::sort(points.begin(), points.end(), [](Position a, Position b) {
        return a.x == b.x ? a.z < b.z : a.x < b.x;
      });
      points.erase(
          std::unique(
              points.begin(),
              points.end(),
              [](Position a, Position b) {
                return a.x == b.x && a.z == b.z;
              }),
          points.end());
      Poly hull;
      for (auto q : points)
      {
        while (hull.size() > 1 && cross(hull[hull.size() - 2], hull.back(), q) <= 1e-8)
        {
          hull.pop_back();
        }
        hull.push_back(q);
      }
      const size_t lower = hull.size();
      for (size_t k = points.size() - 1; k > 0; --k)
      {
        auto q = points[k - 1];
        while (hull.size() > lower && cross(hull[hull.size() - 2], hull.back(), q) <= 1e-8)
        {
          hull.pop_back();
        }
        hull.push_back(q);
      }
      hull.pop_back();
      if (std::abs(area(hull) - area(pieces[i]) - area(pieces[j])) < 1e-6)
      {
        pieces[i] = std::move(hull);
        pieces.erase(pieces.begin() + j);
        j = i + 1;
      }
      else
      {
        ++j;
      }
    }
  }
}
} // namespace

bool generateCityTerritory(const CityTerritoryInput &in, const WorldSuitability &world, const CityTerritorySettings &s, const std::function<double(double, double)> &terrain, CityTerritory &out, std::string *error)
{
  auto fail = [&](const std::string &m) {
    if (error)
    {
      *error = m;
    }
    return false;
  };
  if (!in.producer || !in.revision || !terrain || !world.fingerprint() || !std::isfinite(s.maximumFoundationRelief) || s.maximumFoundationRelief < 0 || in.occupied.size() > 512)
  {
    return fail("invalid city territory identity, terrain or bounds");
  }
  for (const auto &mask : in.occupied)
  {
    if (mask.loop.size() < 3 || mask.loop.size() > 256 || area(mask.loop) <= 0)
    {
      return fail("invalid occupied territory mask");
    }
    for (size_t k = 0; k < mask.loop.size(); ++k)
    {
      auto p = mask.loop[k];
      if (!std::isfinite(p.x) || !std::isfinite(p.z) || std::abs(p.x) > 1e6 || std::abs(p.z) > 1e6)
      {
        return fail("invalid occupied territory coordinate");
      }
      for (auto q : mask.loop)
      {
        if (cross(p, mask.loop[(k + 1) % mask.loop.size()], q) < -1e-7)
        {
          return fail("occupied territory masks must be convex CCW");
        }
      }
    }
  }
  CityTerritory r;
  r.revision = in.revision;
  r.suitabilityFingerprint = world.fingerprint();
  r.surfaces.producer = in.producer;
  r.surfaces.priority = 10;
  auto reservations = in.occupied;
  if (!in.districts.empty())
  {
    auto cells = in.cells;
    for (auto &cell : cells)
    {
      for (const auto &mask : in.occupied)
      {
        if (area(intersection(cell.boundary, mask.loop)) > 1e-6)
        {
          cell.buildable = false;
        }
      }
    }
    for (auto &cell : cells)
    {
      for (const auto &region : s.rural.regions)
      {
        if (region.cover != RuralLandCover::Open && area(intersection(cell.boundary, region.boundary)) > 1e-6)
        {
          cell.buildable = false;
        }
      }
    }
    // Keep source land available for both urban circulation and rural travel:
    // a cell containing an exit, or within a rural cell of it (the exit may
    // lie on a cell edge), is never allocated.
    const double exitClearance = std::max(s.rural.trackWidth, world.settings().spacingMetres) * .5 + 1e-6;
    for (auto &cell : cells)
    {
      for (const auto &source : in.ruralSources)
      {
        bool contains = !cell.boundary.empty();
        double nearest = std::numeric_limits<double>::infinity();
        for (size_t k = 0; k < cell.boundary.size(); ++k)
        {
          const Position a = cell.boundary[k], b = cell.boundary[(k + 1) % cell.boundary.size()];
          contains &= cross(a, b, source.exit) >= -1e-7;
          const double l2 = std::max(1e-12, (b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
          const double t = std::clamp(((source.exit.x - a.x) * (b.x - a.x) + (source.exit.z - a.z) * (b.z - a.z)) / l2, 0., 1.);
          nearest = std::min(nearest, std::hypot(source.exit.x - (a.x + (b.x - a.x) * t), source.exit.z - (a.z + (b.z - a.z) * t)));
        }
        if (contains || nearest <= exitClearance)
        {
          cell.allocatable = false;
        }
      }
    }
    auto planning = s.planning;
    planning.revision = in.revision;
    if (!generateCityMasterPlan(cells, in.districts, in.gateCells, planning, r.regions, error))
    {
      // A district the brief demanded that the ground cannot carry is a
      // recorded omission, not a reason to have no settlement: the plan is
      // tried again with every demand optional, and the shortfall is
      // written down against the districts that went unplaced.
      std::vector<DistrictRequest> relaxed = in.districts;
      bool anyRequired = false;

      for (auto &district : relaxed)
      {
        anyRequired = anyRequired || district.required;
        district.required = false;
      }

      std::string firstError = error ? *error : std::string{};

      if (!anyRequired || !generateCityMasterPlan(cells, relaxed, in.gateCells, planning, r.regions, error))
      {
        if (error && !firstError.empty())
        {
          *error = firstError + (anyRequired ? " (and again with every district optional: " + *error + ")" : "");
        }

        return false;
      }

      r.rejections.push_back("master plan: " + firstError + "; replanned with every district optional");

      for (const auto &district : relaxed)
      {
        const bool placed = std::any_of(r.regions.districts.begin(), r.regions.districts.end(), [&](const auto &d) { return d.id == district.id && !d.pieces.empty(); });

        if (!placed)
        {
          r.rejections.push_back("district " + std::to_string(district.id) + " (" + std::string(districtUseName(district.use)) + ") demanded by the brief was not placed");
        }
      }
    }
    for (const auto &district : r.regions.districts)
    {
      auto regionPieces = district.pieces;
      mergeGroundPieces(regionPieces);
      for (const auto &piece : regionPieces)
      {
        SurfaceContour c;
        c.id = reservations.size() + 1;
        c.material = 100 + unsigned(district.use);
        c.loop = piece;
        reservations.push_back(c);
        c.id = r.surfaces.contours.size() + 1;
        r.surfaces.contours.push_back(c);
      }
    }
  }
  auto ruralSettings = s.rural;
  // Match the field road width so farm plots and regional access share one clearance.
  ruralSettings.trackWidth = std::max(ruralSettings.trackWidth, s.fields.trackWidth);
  if (!growRuralPatches(world, reservations, in.ruralSources, ruralSettings, r.rural, error))
  {
    return false;
  }
  std::vector<Poly> tracks;
  for (const auto &c : r.rural.contours)
  {
    if (c.role == SurfaceRole::Road)
    {
      bool duplicate = std::any_of(tracks.begin(), tracks.end(), [&](const Poly &p) {
        return p.size() == c.loop.size() && std::all_of(c.loop.begin(), c.loop.end(), [&](Position a) {
                 return std::any_of(p.begin(), p.end(), [&](Position b) {
                   return a.x == b.x && a.z == b.z;
                 });
               });
      });
      if (!duplicate)
      {
        tracks.push_back(c.loop);
      }
    }
  }
  uint32_t farmsteadsMade = 0;
  for (const auto &patch : r.rural.patches)
  {
    const bool farmstead = s.farmsteads && (s.farmsteadsWanted ? farmsteadsMade < s.farmsteadsWanted : (patch.id - 1) % std::max(1u, s.farmsteadInterval) == 0);
    FarmHolding holding;
    holding.patch = patch.id;
    holding.source = patch.source;
    std::vector<Poly> support;
    for (const auto &c : r.rural.contours)
    {
      if (c.material == 20 + patch.id && c.role != SurfaceRole::Road)
      {
        support.push_back(c.loop);
      }
    }
    mergeGroundPieces(support);
    // Fit a whole farm court beside a real track, protecting neighbouring routes.
    for (size_t k = patch.access.size(); farmstead && holding.yard.empty() && k > 1; --k)
    {
      auto a = patch.access[k - 2], b = patch.access[k - 1];
      double len = std::hypot(b.x - a.x, b.z - a.z);
      if (len < 26)
      {
        continue;
      }
      double tx = (b.x - a.x) / len, tz = (b.z - a.z) / len;
      for (double t = 13; t <= len - 13 + 1e-6 && holding.yard.empty(); t += 8)
      {
        for (int side : {1, -1})
        {
          if (!holding.yard.empty())
          {
            break;
          }
          Position origin{a.x + tx * t, 0, a.z + tz * t};
          auto at = [&](double u, double v) {
            return Position{origin.x + tx * u - tz * v * side, 0, origin.z + tz * u + tx * v * side};
          };
          auto box = [&](double x0, double z0, double x1, double z1) {
            Poly p{at(x0, z0), at(x1, z0), at(x1, z1), at(x0, z1)};
            if (side < 0)
            {
              std::reverse(p.begin(), p.end());
            }
            return p;
          };
          double edge = ruralSettings.trackWidth * .5 + 2;
          auto yard = box(-13, edge, 13, edge + 22);
          const auto yardClearance = box(-14, edge - 1, 14, edge + 23);
          double covered = 0;
          for (const auto &piece : support)
          {
            covered += area(intersection(yardClearance, piece));
          }
          if (std::abs(covered - 28 * 24) > 1e-5)
          {
            continue;
          }
          bool clear = true;
          for (const auto &track : tracks)
          {
            clear &= !cityPolygonsOverlap(yard, track);
          }
          if (!clear)
          {
            continue;
          }
          double low = INFINITY, high = -INFINITY;
          for (double x = -13; x <= 13; x += 6.5)
          {
            for (double z = edge; z <= edge + 22; z += 5.5)
            {
              auto p = at(x, z);
              double y = terrain(p.x, p.z);
              if (!std::isfinite(y))
              {
                return fail("nonfinite farm foundation terrain");
              }
              low = std::min(low, y);
              high = std::max(high, y);
            }
          }
          if (high - low > s.maximumFoundationRelief)
          {
            continue;
          }
          holding.yard = yard;
          holding.yardArea = 572;
          holding.access = {origin, at(0, edge), at(0, edge + 11)};
          holding.buildings = {
            {1, FarmBuildingUse::Farmhouse, box(-11, edge + 2, -1, edge + 9), at(-1, edge + 6), high, 7},
            {2, FarmBuildingUse::Barn, box(-11, edge + 14, 1, edge + 20), at(0, edge + 14), high, 6},
            {3, FarmBuildingUse::WorkerCottage, box(4, edge + 3, 11, edge + 8), at(4, edge + 6), high, 4}};
        }
      }
    }
    auto fieldInput = farmFieldInput(patch, in.revision);
    for (const auto &track : tracks)
    {
      if (std::any_of(support.begin(), support.end(), [&](const auto &piece) {
            return area(intersection(track, piece)) > 1e-6;
          }))
      {
        fieldInput.sharedTracks.push_back(track);
      }
    }
    if (!holding.yard.empty())
    {
      fieldInput.holes.push_back(holding.yard);
    }
    FarmFieldProduct fields;
    if (!generateFarmFields(fieldInput, s.fields, fields, error))
    {
      return fail("patch " + std::to_string(patch.id) + " fields: " + (error ? *error : std::string("generation failed")));
    }
    r.cropArea += fields.cropArea;
    // Publish the original holding ground once. The field compiler's base and
    // clipped track pieces are accounting geometry, already covered by this
    // ground, explicit yards and the shared upstream tracks below.
    for (const auto &piece : support)
    {
      // Track subtraction can leave a collinear sliver; the world builder
      // rejects any loop below three vertices, so record it instead.
      if (piece.size() < 3 || area(piece) < 1e-6)
      {
        r.rejections.push_back("patch " + std::to_string(patch.id) + ": degenerate ground sliver omitted from publication");
        continue;
      }
      SurfaceContour c;
      c.id = r.surfaces.contours.size() + 1;
      c.material = 16;
      c.loop = piece;
      r.surfaces.contours.push_back(std::move(c));
    }
    for (auto c : fields.contours)
    {
      if (c.role != SurfaceRole::Road && c.material != 11)
      {
        c.id = r.surfaces.contours.size() + 1;
        r.surfaces.contours.push_back(std::move(c));
      }
    }
    r.fields.push_back(std::move(fields));
    if (!holding.yard.empty())
    {
      SurfaceContour c;
      c.id = r.surfaces.contours.size() + 1;
      c.material = 17;
      c.loop = holding.yard;
      r.surfaces.contours.push_back(c);
      r.farmyardArea += holding.yardArea;
      r.holdings.push_back(std::move(holding));
      ++farmsteadsMade;
    }
    else if (farmstead)
    {
      r.rejections.push_back("patch " + std::to_string(patch.id) + ": no broad, level farmyard beside available track");
    }
  }
  // Upstream roads retain precedence across every field producer.
  for (const auto &track : tracks)
  {
    SurfaceContour c;
    c.id = r.surfaces.contours.size() + 1;
    c.material = 1;
    c.role = SurfaceRole::Road;
    c.loop = track;
    r.surfaces.contours.push_back(std::move(c));
  }
  if (r.surfaces.contours.size() > 8192)
  {
    return fail("city territory contour budget exceeded; split the territory job");
  }
  if (error)
  {
    error->clear();
  }
  out = std::move(r);
  return true;
}
} // namespace procedural::world::rewrite
