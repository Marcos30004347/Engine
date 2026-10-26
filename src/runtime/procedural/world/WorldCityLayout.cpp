#include "WorldCityLayout.hpp"

#include "generator/LotPlanner.hpp"
#include "generator/RingLotPlanner.hpp"
#include "generator/RoadGrowth.hpp"
#include "generator/WardBuildings.hpp"
#include "generator/WardPlanner.hpp"
#include "procedural/ConstrainedDelaunay.hpp"
#include "procedural/world/citylayout/HousingStrategy.hpp"
#include "procedural/world/citylayout/LayoutStage.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>

namespace procedural::world::rewrite {
namespace {
// Contour budget for one layout arrangement. Road bands split per usable
// piece, so outer wards multiply contours; the arrangement accepts 4096.
constexpr size_t kCityContourBudget = 4096;
using Poly = std::vector<Position>;

// One definition of each planar predicate, shared with the housing strategies
// rather than copied into them.
using citylayout::area;
using citylayout::clip;
using citylayout::convex;
using citylayout::coveredSegment;
using citylayout::cross;
using citylayout::dist;
using citylayout::hull;
using citylayout::inside;
using citylayout::intersection;
using citylayout::merge;
using citylayout::mix;
using citylayout::ribbon;
using citylayout::segmentDistance;
using citylayout::segmentsMeet;
using citylayout::subtract;

// Exact interval coverage over convex pieces catches exclusions narrower than
// the centerline sampling spacing.
} // namespace

bool cityPolygonsOverlap(const Poly &a, const Poly &b)
{
  if (a.empty() || b.empty())
  {
    return false;
  }
  for (auto *p : {&a, &b})
  {
    for (size_t i = 0; i < p->size(); ++i)
    {
      auto u = (*p)[i], v = (*p)[(i + 1) % p->size()];
      bool outside = true;
      for (auto q : p == &a ? b : a)
      {
        if (cross(u, v, q) >= -1e-7)
        {
          outside = false;
          break;
        }
      }
      if (outside)
      {
        return false;
      }
    }
  }
  return true;
}

bool generateCityLayout(
    const CityPolygon &inputDomain,
    const std::vector<Poly> &inputReserved,
    const std::vector<CityStreet> &arteries,
    const std::vector<CityGate> &gates,
    const CitySettings &inputSettings,
    const std::function<double(double, double)> &terrain,
    CityLayout &out,
    std::string *error)
{
  CitySettings s = inputSettings;
  CityPolygon domain = inputDomain;
  std::vector<Poly> reserved = inputReserved;
  auto fail = [&](const char *m) {
    if (error)
    {
      *error = m;
    }
    return false;
  };
  if (s.districts.size() > 32)
  {
    return fail("city district budget exceeded");
  }
  std::set<uint64_t> districtIDs;
  std::vector<Poly> districtMasks;
  for (const auto &district : s.districts)
  {
    if (!district.id || !districtIDs.insert(district.id).second || unsigned(district.use) >= unsigned(DistrictUse::Count) || district.pieces.empty() || district.pieces.size() > 128)
    {
      return fail("invalid accepted city district");
    }
    for (const auto &p : district.pieces)
    {
      if (!convex(p))
      {
        return fail("district pieces must be convex CCW");
      }
      for (const auto &other : districtMasks)
      {
        if (area(intersection(p, other)) > 1e-6)
        {
          return fail("overlapping accepted districts");
        }
      }
      districtMasks.push_back(p);
    }
    if (district.use == DistrictUse::Residential || district.use == DistrictUse::Suburb || district.use == DistrictUse::Merchant || district.use == DistrictUse::Craftsmen ||
        district.use == DistrictUse::WealthyResidential || district.use == DistrictUse::WorkerHousing)
    {
      double plotArea = district.use == DistrictUse::Suburb ? 45 : district.use == DistrictUse::Merchant ? 95 : district.use == DistrictUse::Craftsmen ? 120 : 65;
      if (district.use == DistrictUse::WealthyResidential)
      {
        plotArea = 650;
      }
      if (district.use == DistrictUse::WorkerHousing)
      {
        plotArea = 140;
      }
      // Wealth sets the density: houses per hectare of the district's use.
      if (unsigned(district.use) < s.housesPerHectare.size() && s.housesPerHectare[unsigned(district.use)] > 0)
      {
        plotArea = 7000. / s.housesPerHectare[unsigned(district.use)];
      }
      s.housingAreas.push_back({district.id, district.use, district.pieces, plotArea, district.use == DistrictUse::WealthyResidential, district.use == DistrictUse::WealthyResidential ? .55 : 0});
    }
    else
    {
      // Reserve net compound space inside the coarse allocation, leaving the
      // published bordering street hierarchy available for public access.
      double reservedArea = 0;
      const double margin = std::max(4., s.streetHalfWidth * 1.75 + s.setback + 1);
      auto compoundPieces = district.pieces;
      merge(compoundPieces); // Reclaim internal allocation seams before setting back the compound boundary.
      for (const auto &piece : compoundPieces)
      {
        Poly inner = piece;
        for (size_t k = 0; k < piece.size(); ++k)
        {
          auto a = piece[k], b = piece[(k + 1) % piece.size()];
          double nx = b.z - a.z, nz = a.x - b.x;
          inner = clip(inner, nx, nz, nx * a.x + nz * a.z - margin * dist(a, b));
        }
        if (area(inner) >= 40)
        {
          reservedArea += area(inner);
          reserved.push_back(std::move(inner));
        }
      }
      if (reservedArea < 200)
      {
        return fail("district cannot retain a useful compound after street clearance");
      }
    }
  }
  for (double v : {s.siteSpacing, s.targetBlockArea,  s.minimumBlockArea,        s.streetHalfWidth,   s.minimumWidth,    s.maximumWidth,      s.minimumDepth,    s.maximumDepth,   s.frontageGap,
                   s.setback,     s.overhang,         s.maximumFoundationRelief, s.houseWeight,       s.workshopWeight,  s.gardenWeight,      s.streetCurvature, s.junctionRadius, s.targetPlotArea,
                   s.wallSetback, s.suburbRoadLength, s.suburbGrowthRadius,      s.suburbCellSpacing, s.wallCornerInset, s.wallSectionLength, s.wallTowerRadius, s.wallTowerHeight})
  {
    if (!std::isfinite(v) || v < 0 || v > 1e5)
    {
      return fail("invalid city settings");
    }
  }
  if (!std::isfinite(s.plannedAxisRadians) || !std::isfinite(s.plannedDistrictTurnRadians) || !std::isfinite(s.plannedBlockAspect) || std::abs(s.plannedAxisRadians) > 6.3 || s.plannedDistrictTurnRadians < 0 ||
      s.plannedDistrictTurnRadians > .6 || s.plannedBlockAspect < 1 || s.plannedBlockAspect > 3)
  {
    return fail("invalid planned street axes");
  }
  if (s.houseWeight + s.workshopWeight + s.gardenWeight <= 0 || int(s.recipe) > 3 || !terrain || s.siteSpacing < 5 || s.minimumBlockArea < 1 || s.targetBlockArea < s.minimumBlockArea || s.streetHalfWidth < .5 ||
      s.targetPlotArea < 20 || s.targetPlotArea > 2000 || s.minimumWidth < 2 || s.minimumDepth < 2 || s.maximumWidth < s.minimumWidth || s.maximumDepth < s.minimumDepth || s.maximumSites < 4 || s.maximumSites > 128 ||
      s.maximumObjects > 10000 || !s.maximumObjects || s.maximumSplitDepth > 12 || domain.outer.size() < 3 || domain.outer.size() > 128 || domain.holes.size() > 16 || reserved.size() > 32 || arteries.size() > 64 ||
      gates.empty())
  {
    return fail("city input budget or dimensions invalid");
  }
  for (auto &p : reserved)
  {
    if (!convex(p))
    {
      return fail("reservation must be convex CCW");
    }
  }
  if (s.suburbLand.size() > 96 || s.suburbRoadLength < 10 || s.suburbRoadLength > 200 || s.suburbGrowthRadius < 10 || s.suburbGrowthRadius > 160 || s.suburbCellSpacing < 15 || s.suburbCellSpacing > 100 ||
      !s.maximumSuburbWards || s.maximumSuburbWards > 64 || s.wallCornerInset > 60 || s.wallSectionLength < 12 || s.wallSectionLength > 100 || s.wallSetback > 50 || s.wallTowerRadius < 1 || s.wallTowerRadius > 5 ||
      s.wallTowerHeight < 4 || s.wallTowerHeight > 20)
  {
    return fail("invalid suburb or wall setback dimensions");
  }
  if (!s.suburbLand.empty() && (!s.walls || !s.blockHousing))
  {
    return fail("gate suburbs require walls and block housing");
  }
  for (const auto &p : s.suburbLand)
  {
    if (!convex(p))
    {
      return fail("suburb land must be convex CCW");
    }
  }
  if (s.housingAreas.size() > 32)
  {
    return fail("housing area budget exceeded");
  }
  std::set<uint64_t> housingIDs;
  std::vector<Poly> housingMasks;
  for (const auto &housing : s.housingAreas)
  {
    if (!housing.id || !housingIDs.insert(housing.id).second || !std::isfinite(housing.targetPlotArea) || housing.targetPlotArea < 20 || housing.targetPlotArea > 2000 || housing.pieces.empty() ||
        housing.pieces.size() > 128 || !std::isfinite(housing.gardenFraction) || housing.gardenFraction < 0 || housing.gardenFraction > .85 || (housing.gardenFraction > 0 && !s.regularBuildingShells) ||
        (housing.use != DistrictUse::Residential && housing.use != DistrictUse::Suburb && housing.use != DistrictUse::Merchant && housing.use != DistrictUse::Manor && housing.use != DistrictUse::Craftsmen &&
         housing.use != DistrictUse::WealthyResidential && housing.use != DistrictUse::WorkerHousing))
    {
      return fail("invalid ordinary housing area");
    }
    for (const auto &p : housing.pieces)
    {
      if (!convex(p))
      {
        return fail("housing mask must be convex CCW");
      }
      for (const auto &q : housingMasks)
      {
        if (area(intersection(p, q)) > 1e-6)
        {
          return fail("overlapping housing masks");
        }
      }
      housingMasks.push_back(p);
    }
  }
  std::set<uint64_t> arterialIds;
  for (const auto &a : arteries)
  {
    if (!a.id || !arterialIds.insert(a.id).second || !std::isfinite(a.a.x) || !std::isfinite(a.a.z) || !std::isfinite(a.b.x) || !std::isfinite(a.b.z) || !std::isfinite(a.halfWidth) || a.halfWidth < .5 ||
        a.halfWidth > 20 || dist(a.a, a.b) < 1)
    {
      return fail("invalid fixed arterial identity or geometry");
    }
  }
  std::set<uint64_t> gateIds;
  for (const auto &g : gates)
  {
    if (!g.id || !gateIds.insert(g.id).second || !std::isfinite(g.position.x) || !std::isfinite(g.position.z))
    {
      return fail("invalid gate identity or position");
    }
  }
  // Two faceting passes round the wall (Watabou smooths the circumference
  // vertices); gates keep their straight stretch on both passes.
  for (int pass = 0; pass < 1 && s.walls && s.wallCornerInset > 0; ++pass)
  {
    // The first pass triples the corner count; the second doubles it and is
    // skipped when it cannot fit the 128-vertex envelope budget.
    if (domain.outer.size() * (pass ? 2 : 3) > 128)
    {
      break;
    }
    Poly faceted;
    const double inset = pass ? s.wallCornerInset * .5 : s.wallCornerInset;
    for (size_t i = 0; i < domain.outer.size(); ++i)
    {
      auto prev = domain.outer[(i + domain.outer.size() - 1) % domain.outer.size()], corner = domain.outer[i], next = domain.outer[(i + 1) % domain.outer.size()];
      double cut = std::min({inset, dist(prev, corner) * .3, dist(corner, next) * .3});
      for (const auto &gate : gates)
      {
        if (segmentDistance(gate.position, prev, corner) < 1e-5 || segmentDistance(gate.position, corner, next) < 1e-5)
        {
          cut = std::min(cut, std::max(0., dist(gate.position, corner) - 12));
        }
      }
      if (cut < .1 || cross(prev, corner, next) <= 1e-6)
      {
        faceted.push_back(corner);
        continue;
      }
      auto a = mix(corner, prev, cut / dist(prev, corner)), b = mix(corner, next, cut / dist(corner, next));
      // Two deliberately straight facets replace the formerly sampled curve;
      // the rounding pass cuts each corner once more.
      faceted.push_back(a);
      if (!pass)
      {
        faceted.push_back(mix(mix(a, corner, .5), mix(corner, b, .5), .5));
      }
      faceted.push_back(b);
    }
    if (faceted.size() > 128)
    {
      return fail("faceted wall envelope vertex budget exceeded");
    }
    domain.outer = std::move(faceted);
  }
  CityLayout r;
  r.envelope = domain;
  r.revision = s.revision;
  r.wallSetback = s.walls ? s.wallSetback : 0;
  r.districts = s.districts;
  r.reservations = reserved;
  // CDT is used only to decompose the input envelope, not to infer land labels.
  BoundaryRegistry registry;
  std::vector<BoundaryInput> edges;
  PatchDomain patch;
  patch.id = 1;
  uint64_t eid = 1, vid = 1;
  auto ring = [&](const Poly &p, std::vector<BoundaryUse> &uses) {
    uint64_t first = vid;
    for (size_t i = 0; i < p.size(); ++i)
    {
      edges.push_back({eid, {vid++, p[i]}, {i + 1 == p.size() ? first : vid, p[(i + 1) % p.size()]}, {}});
      uses.push_back({eid++, i + 1 == p.size()});
    }
  };
  ring(domain.outer, patch.outer);
  for (auto &p : domain.holes)
  {
    patch.holes.emplace_back();
    ring(p, patch.holes.back());
  }
  PatchMesh base;
  if (!registry.commit(edges, error) || !compilePatchMesh(
                                            patch,
                                            registry,
                                            {},
                                            [](double, double) {
                                              return 0.;
                                            },
                                            base,
                                            error))
  {
    return false;
  }
  std::vector<Poly> usable;
  for (auto &t : base.triangles)
  {
    Poly p;
    for (auto i : t)
    {
      p.push_back(base.vertices[i].position);
    }
    if (area(p) < 0)
    {
      std::reverse(p.begin(), p.end());
    }
    usable.push_back(p);
  }
  merge(usable);
  for (auto &q : reserved)
  {
    std::vector<Poly> next;
    for (auto &p : usable)
    {
      auto pieces = subtract(p, q);
      next.insert(next.end(), pieces.begin(), pieces.end());
    }
    usable = std::move(next);
  }
  merge(usable);
  if (usable.empty())
  {
    return fail("no usable city land");
  }
  // Wall sections are decided before the wards: the chords between the
  // towers are the wall, and nothing inside the walls is built beyond them.
  std::vector<std::pair<double, double>> wallSolid;
  std::function<Position(double)> wallAt;
  Poly wallRing;
  std::vector<Poly> wallMasks;
  if (s.walls)
  {
    // Gate openings in perimeter arc-length; every solid arc between them is
    // resampled into equal chords no longer than the section length, so the
    // wall reads as a ring of same-sized sections joined by towers.
    const size_t n = domain.outer.size();
    std::vector<double> arc(n + 1, 0);
    for (size_t i = 0; i < n; ++i)
    {
      arc[i + 1] = arc[i] + dist(domain.outer[i], domain.outer[(i + 1) % n]);
    }
    const double perimeter = arc[n];
    wallAt = [&, n, arc, perimeter](double u) {
      u = std::fmod(std::fmod(u, perimeter) + perimeter, perimeter);
      size_t i = 0;
      while (i + 1 < n && arc[i + 1] < u)
      {
        ++i;
      }
      double l = arc[i + 1] - arc[i];
      return mix(domain.outer[i], domain.outer[(i + 1) % n], l > 1e-9 ? (u - arc[i]) / l : 0);
    };
    std::vector<std::pair<double, double>> openings;
    for (size_t i = 0; i < n; ++i)
    {
      auto a = domain.outer[i], b = domain.outer[(i + 1) % n];
      double l = dist(a, b);
      for (auto &g : gates)
      {
        if (segmentDistance(g.position, a, b) < 1e-5)
        {
          double t = ((g.position.x - a.x) * (b.x - a.x) + (g.position.z - a.z) * (b.z - a.z)) / (l * l), width = 0;
          for (auto &road : arteries)
          {
            if (road.id == g.externalRoad)
            {
              const double crossing = std::abs((b.x - a.x) * (road.b.z - road.a.z) - (b.z - a.z) * (road.b.x - road.a.x)) / (l * dist(road.a, road.b));
              if (crossing < 1e-5)
              {
                return fail("wall gate route is tangent to perimeter");
              }
              width = (road.halfWidth + 1.5 + s.wallTowerRadius) / crossing;
            }
          }
          openings.push_back({arc[i] + l * t - width, arc[i] + l * t + width});
        }
      }
    }
    std::sort(openings.begin(), openings.end());
    if (openings.empty())
    {
      wallSolid.push_back({0, perimeter});
    }
    else
    {
      for (size_t k = 0; k < openings.size(); ++k)
      {
        double from = openings[k].second, to = k + 1 < openings.size() ? openings[k + 1].first : openings.front().first + perimeter;
        if (to - from > 1e-6)
        {
          wallSolid.push_back({from, to});
        }
      }
    }
    // The ring through every section end (a chord bridges each gate opening).
    for (auto [from, to] : wallSolid)
    {
      const unsigned count = std::max(1u, unsigned(std::ceil((to - from) / s.wallSectionLength)));
      for (unsigned k = 0; k <= count; ++k)
      {
        const auto p = wallAt(from + (to - from) * k / count);
        if (wallRing.empty() || dist(wallRing.back(), p) > 1e-6)
        {
          wallRing.push_back(p);
        }
      }
    }
    if (wallRing.size() > 1 && dist(wallRing.front(), wallRing.back()) < 1e-6)
    {
      wallRing.pop_back();
    }
    // The wall clearance: the land between each chord and the envelope plus
    // the setback strip inside the chord, one convex cap and strip per
    // section. Published as pavement below, so no block face forms there.
    if (!s.wardCells.empty() && wallRing.size() >= 3)
    {
      const double orientation = area(domain.outer) < 0 ? -1 : 1;
      auto along = [&](Position p) {
        double best = 0, bestDistance = 1e30;
        for (size_t k = 0; k < n; ++k)
        {
          const double d = segmentDistance(p, domain.outer[k], domain.outer[(k + 1) % n]);
          if (d < bestDistance)
          {
            bestDistance = d;
            best = arc[k] + dist(domain.outer[k], p);
          }
        }
        return best;
      };
      for (size_t i = 0; i < wallRing.size(); ++i)
      {
        const auto a = wallRing[i], b = wallRing[(i + 1) % wallRing.size()];
        const double l = dist(a, b);
        if (l < 1e-6)
        {
          continue;
        }
        double ua = along(a), ub = along(b);
        if (ub < ua)
        {
          ub += perimeter;
        }
        Poly cap{a};
        for (size_t k = 0; k < n; ++k)
        {
          for (double u : {arc[k], arc[k] + perimeter})
          {
            if (u > ua + 1e-6 && u < ub - 1e-6)
            {
              cap.push_back(domain.outer[k]);
            }
          }
        }
        std::sort(cap.begin() + 1, cap.end(), [&](Position p, Position q) {
          double up = along(p), uq = along(q);
          if (up < ua)
          {
            up += perimeter;
          }
          if (uq < ua)
          {
            uq += perimeter;
          }
          return up < uq;
        });
        cap.push_back(b);
        const Position in{-(b.z - a.z) / l * orientation, 0, (b.x - a.x) / l * orientation};
        // The strip reaches half a metre past the ward edge, so a street that
        // ends on that edge ends inside the pavement and its faces separate.
        const double reach = s.wallSetback + .5;
        Poly strip{a, b, {b.x + in.x * reach, 0, b.z + in.z * reach}, {a.x + in.x * reach, 0, a.z + in.z * reach}};
        for (auto piece : {cap, strip})
        {
          if (piece.size() < 3 || std::abs(area(piece)) < 1e-6)
          {
            continue;
          }
          if (area(piece) < 0)
          {
            std::reverse(piece.begin(), piece.end());
          }
          if (convex(piece))
          {
            wallMasks.push_back(piece);
          }
        }
      }
    }
  }
  double minX = 1e30, maxX = -1e30, minZ = 1e30, maxZ = -1e30;
  for (auto p : domain.outer)
  {
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
    minZ = std::min(minZ, p.z);
    maxZ = std::max(maxZ, p.z);
  }
  if (maxX - minX > 2000 || maxZ - minZ > 2000)
  {
    return fail("city extent budget exceeded");
  }
  auto random = [&](uint64_t id, uint64_t purpose) {
    return double(featureSeed(s.seed, 11, id, purpose) >> 11) * 0x1.0p-53;
  };
  auto valid = [&](Position p) {
    for (auto &q : usable)
    {
      if (inside(q, p))
      {
        return true;
      }
    }
    return false;
  };
  // Wards through the class-based planner (generator::WardPlanner): organic
  // relaxed Voronoi patches, gridded planned wards, or the usable pieces when
  // the roads come from the road growth generator.
  generator::WardPlanInput wardInput;
  wardInput.recipe = !s.wardCells.empty()                      ? generator::WardRecipe::Given
                     : s.recipe == CityRecipe::OrganicVoronoi  ? generator::WardRecipe::Organic
                     : s.recipe == CityRecipe::RecursiveBlocks ? generator::WardRecipe::Recursive
                     : s.recipe == CityRecipe::PlannedBlocks   ? generator::WardRecipe::Planned
                                                               : generator::WardRecipe::Roads;
  wardInput.domain = domain.outer;
  wardInput.usable = usable;
  wardInput.givenCells = s.wardCells;
  // Given wards (Watabou's patches) stop at the wall: every cell is clipped by
  // the half-planes of the wall chords moved inward by the setback, so no lot
  // reaches beyond the chords (the ring is nearly convex; its kernel is used).
  if (s.walls && wallRing.size() >= 3)
  {
    const double orientation = area(wallRing) < 0 ? -1 : 1;
    for (auto &cell : wardInput.givenCells)
    {
      for (size_t i = 0; i < wallRing.size() && !cell.empty(); ++i)
      {
        const auto a = wallRing[i], b = wallRing[(i + 1) % wallRing.size()];
        const double l = dist(a, b);
        if (l < 1e-6)
        {
          continue;
        }
        // Only the chords the cell reaches cut it (the ring is not convex, a
        // far chord's half-plane would slice across the town): a vertex within
        // the chord's span that lies outside it or within the setback inside.
        const double nx = (b.z - a.z) / l * orientation, nz = (a.x - b.x) / l * orientation;
        bool reaches = false;
        for (auto v : cell)
        {
          const double t = ((v.x - a.x) * (b.x - a.x) + (v.z - a.z) * (b.z - a.z)) / (l * l);
          const double outward = nx * (v.x - a.x) + nz * (v.z - a.z);
          reaches |= t > -.05 && t < 1.05 && outward > -(s.wallSetback + 2);
        }
        if (!reaches)
        {
          continue;
        }
        cell = clip(cell, nx, nz, nx * a.x + nz * a.z - s.wallSetback);
      }
    }
    wardInput.givenCells.erase(
        std::remove_if(
            wardInput.givenCells.begin(),
            wardInput.givenCells.end(),
            [](const Poly &c) {
              return c.size() < 3 || area(c) < 20;
            }),
        wardInput.givenCells.end());
  }
  wardInput.minX = minX;
  wardInput.maxX = maxX;
  wardInput.minZ = minZ;
  wardInput.maxZ = maxZ;
  wardInput.siteSpacing = s.siteSpacing;
  wardInput.targetBlockArea = s.targetBlockArea;
  wardInput.minimumBlockArea = s.minimumBlockArea;
  wardInput.plannedBlockAspect = s.plannedBlockAspect;
  wardInput.plannedAxisRadians = s.plannedAxisRadians;
  wardInput.plannedDistrictTurnRadians = s.plannedDistrictTurnRadians;
  wardInput.frontageInset = s.streetHalfWidth + s.setback + s.overhang + s.minimumDepth;
  wardInput.minimumWidth = s.minimumWidth;
  wardInput.maximumSites = s.maximumSites;
  wardInput.maximumSplitDepth = s.maximumSplitDepth;
  wardInput.siteRelaxations = s.siteRelaxations;
  wardInput.mixedBlockGrammar = s.mixedBlockGrammar;
  wardInput.maximumSides = s.maximumBlockSides;
  wardInput.maximumBlockDepth = s.maximumBlockDepth;
  wardInput.random = random;
  wardInput.wardSeed = [&](uint64_t i) {
    return featureSeed(s.seed, 11, i, 30);
  };
  wardInput.plannedAt = [&](const Poly &ward) {
    // Middle-class and merchant wards are laid out on a grid; the rest stay organic.
    Position c{};
    for (auto v : ward)
    {
      c.x += v.x / ward.size();
      c.z += v.z / ward.size();
    }
    for (const auto &housing : s.housingAreas)
    {
      for (const auto &piece : housing.pieces)
      {
        if (inside(piece, c))
        {
          return housing.use == DistrictUse::Residential || housing.use == DistrictUse::Merchant || housing.use == DistrictUse::WealthyResidential;
        }
      }
    }
    return false;
  };
  generator::WardPlan wardPlan;
  generator::Ledger wardLedger;
  std::string wardError;
  if (!generator::WardPlanner::plan(wardInput, wardPlan, wardLedger, &wardError))
  {
    return fail(wardError.c_str());
  }
  r.sites = wardPlan.sites;
  r.splitAttempts += wardPlan.splitAttempts;
  r.unrefinedBlocks += wardPlan.unrefinedBlocks;
  std::vector<generator::PlannedCut> plannedCuts = wardPlan.plannedCuts;
  const bool planned = s.recipe == CityRecipe::PlannedBlocks || wardPlan.anyPlanned;
  std::vector<Poly> cells = wardPlan.cells;
  if (cells.size() > 256)
  {
    return fail("district subdivision budget exceeded");
  }
  uint64_t wid = 1;
  for (auto &cell : cells)
  {
    CityWard w;
    w.id = wid++;
    for (auto &p : usable)
    {
      auto q = intersection(p, cell);
      if (!q.empty())
      {
        w.pieces.push_back(q);
      }
    }
    merge(w.pieces);
    if (!w.pieces.empty())
    {
      r.wards.push_back(w);
    }
  }
  r.subdivisionCells = r.wards.size();
  // Candidate streets are shared boundaries of different wards, never decomposition diagonals.
  std::map<uint64_t, std::pair<uint64_t, uint64_t>> streetWards;
  uint64_t sid = 1000;
  auto nextStreetId = [&]() {
    while (arterialIds.count(sid))
    {
      ++sid;
    }
    return sid++;
  };
  // Shared edges of wards from index `first` on become streets (called again
  // for the outer wards once they exist: Watabou's outskirts are patches too).
  auto wardEdgeStreets = [&](size_t first, bool withPath) {
    for (size_t i = first; s.recipe != CityRecipe::LSystemRoads && i < r.wards.size(); ++i)
    {
      for (size_t j = i + 1; j < r.wards.size(); ++j)
      {
        for (auto &p : r.wards[i].pieces)
        {
          for (auto &q : r.wards[j].pieces)
          {
            for (size_t a = 0; a < p.size(); ++a)
            {
              for (size_t b = 0; b < q.size(); ++b)
              {
                auto u = p[a], v = p[(a + 1) % p.size()], c = q[b], d = q[(b + 1) % q.size()];
                double l = dist(u, v);
                if (l < 1e-6 || std::abs(cross(u, v, c)) / l > 1e-5 || std::abs(cross(u, v, d)) / l > 1e-5)
                {
                  continue;
                }
                double t0 = ((c.x - u.x) * (v.x - u.x) + (c.z - u.z) * (v.z - u.z)) / (l * l), t1 = ((d.x - u.x) * (v.x - u.x) + (d.z - u.z) * (v.z - u.z)) / (l * l);
                double lo = std::max(0., std::min(t0, t1)), hi = std::min(1., std::max(t0, t1));
                if ((hi - lo) * l > 1)
                {
                  // A ward edge that already carries a fixed artery is that artery.
                  bool onArtery = false;
                  for (const auto &artery : arteries)
                  {
                    const double al = dist(artery.a, artery.b);
                    if (al < 1e-6)
                    {
                      continue;
                    }
                    const Position m0 = mix(u, v, lo), m1 = mix(u, v, hi);
                    const double d0 = std::abs(cross(artery.a, artery.b, m0)) / al, d1 = std::abs(cross(artery.a, artery.b, m1)) / al;
                    const double t0 = ((m0.x - artery.a.x) * (artery.b.x - artery.a.x) + (m0.z - artery.a.z) * (artery.b.z - artery.a.z)) / (al * al);
                    const double t1 = ((m1.x - artery.a.x) * (artery.b.x - artery.a.x) + (m1.z - artery.a.z) * (artery.b.z - artery.a.z)) / (al * al);
                    onArtery |= d0 < .05 && d1 < .05 && t0 > -1e-3 && t0 < 1 + 1e-3 && t1 > -1e-3 && t1 < 1 + 1e-3;
                  }
                  if (onArtery)
                  {
                    continue;
                  }
                  const auto id = nextStreetId();
                  r.streets.push_back({id, mix(u, v, lo), mix(u, v, hi), s.streetHalfWidth * (.7 + .3 * random(id, 31)), false});
                  if (planned)
                  {
                    auto &street = r.streets.back();
                    street.halfWidth = s.streetHalfWidth;
                    const auto mid = mix(street.a, street.b, .5);
                    for (const auto &cut : plannedCuts)
                    {
                      if (std::abs(cut.nx * mid.x + cut.nz * mid.z - cut.offset) < 1e-5)
                      {
                        street.hierarchy = cut.depth == 0 ? 2 : cut.depth < 3 ? 1 : 0;
                        street.halfWidth = s.streetHalfWidth * (street.hierarchy == 2 ? 1.75 : street.hierarchy == 1 ? 1.3 : cut.depth >= 4 ? .6 : 1.);
                        break;
                      }
                    }
                  }
                  streetWards[id] = {r.wards[i].id, r.wards[j].id};
                  if (withPath)
                  {
                    r.streets.back().path = {r.streets.back().a, r.streets.back().b};
                  }
                }
              }
            }
          }
        }
      }
    }
  };
  wardEdgeStreets(0, false);
  if (s.recipe == CityRecipe::LSystemRoads)
  {
    // Parish and Muller road growth (generator::RoadGrowth). Seeds: six major
    // bearings from a ring around the centre (the plaza is a reservation) and
    // minor roads leaving every fixed artery at right angles.
    generator::RoadGrowthInput growth;
    growth.pattern = generator::RoadPattern(std::min<uint8_t>(s.lsystemPattern, 3));
    growth.rasterAxisRadians = s.plannedAxisRadians;
    growth.highwayLength = s.lsystemHighwayLength;
    growth.streetLength = s.lsystemStreetLength;
    growth.intersectionSnapMetres = s.lsystemIntersectionSnapMetres;
    growth.roadSnapMetres = s.lsystemRoadSnapMetres;
    growth.highwayBranchProbability = s.lsystemHighwayBranchProbability;
    growth.streetBranchProbability = s.lsystemStreetBranchProbability;
    growth.streetDelay = s.lsystemStreetDelay;
    growth.maximumSegments = s.lsystemMaximumSegments;
    growth.onLand = valid;
    growth.covered = [&](Position a, Position b) {
      return coveredSegment(a, b, usable);
    };
    growth.terrain = terrain;
    growth.random = random;
    Position centre{};
    for (auto p : domain.outer)
    {
      centre.x += p.x / domain.outer.size();
      centre.z += p.z / domain.outer.size();
    }
    growth.centre = centre;
    double reach = 1;
    for (auto p : domain.outer)
    {
      reach = std::max(reach, dist(p, centre));
    }
    // Population density: housing districts are dense, other usable land less
    // so, both falling off toward the wall (the paper's density map).
    growth.density = [&, centre, reach](Position p) {
      double base = .45;
      for (const auto &housing : s.housingAreas)
      {
        for (const auto &piece : housing.pieces)
        {
          if (inside(piece, p))
          {
            base = 1;
          }
        }
      }
      return base * (1 - .5 * std::min(1., dist(p, centre) / reach));
    };
    // Seeds: highways from a ring around the centre (the plaza is a
    // reservation) and streets leaving every fixed artery at right angles.
    for (int k = 0; k < 6; ++k)
    {
      const double bearing = s.plannedAxisRadians + k * 1.0471975511965976;
      growth.seeds.push_back({{centre.x + std::cos(bearing) * 46, 0, centre.z + std::sin(bearing) * 46}, bearing, true, 0});
    }
    for (const auto &artery : arteries)
    {
      for (double t : {.4, .7})
      {
        const double bearing = std::atan2(artery.b.z - artery.a.z, artery.b.x - artery.a.x);
        for (int side : {-1, 1})
        {
          growth.seeds.push_back({mix(artery.a, artery.b, t), bearing + side * 1.5707963267948966, false, s.lsystemStreetDelay});
        }
      }
    }
    generator::RoadNetwork network;
    generator::Ledger growthLedger;
    std::string growthError;
    if (!generator::RoadGrowth::grow(growth, network, growthLedger, &growthError))
    {
      return fail(("road growth: " + growthError).c_str());
    }
    for (const auto &sg : network.segments)
    {
      r.streets.push_back({nextStreetId(), sg.a, sg.b, s.streetHalfWidth * (sg.highway ? 1.4 : .8), false, {}, uint8_t(sg.highway ? 1 : 0)});
    }
    r.lsystemSegments = uint32_t(network.segments.size());
  }
  // Re-cut wards at immutable arterial centerlines, so those streets also own frontage.
  for (auto &w : r.wards)
  {
    for (const auto &street : arteries)
    {
      if (!s.wardCells.empty())
      {
        break;
      }
      std::vector<Poly> pieces;
      double nx = street.b.z - street.a.z, nz = street.a.x - street.b.x, c = nx * street.a.x + nz * street.a.z;
      for (auto &p : w.pieces)
      {
        auto left = clip(p, nx, nz, c), right = clip(p, -nx, -nz, -c);
        if (!left.empty())
        {
          pieces.push_back(left);
        }
        if (!right.empty())
        {
          pieces.push_back(right);
        }
      }
      w.pieces = std::move(pieces);
    }
  }
  std::set<uint64_t> ids;
  for (auto &a : arteries)
  {
    if (!a.id || !ids.insert(a.id).second || dist(a.a, a.b) < 1 || !std::isfinite(a.halfWidth) || a.halfWidth < .5 || a.halfWidth > 20)
    {
      return fail("invalid fixed artery");
    }
    std::vector<std::pair<double, double>> intervals;
    for (const auto &p : usable)
    {
      double lo = 0, hi = 1;
      for (size_t k = 0; k < p.size(); ++k)
      {
        auto u = p[k], v = p[(k + 1) % p.size()];
        double da = cross(u, v, a.a), db = cross(u, v, a.b);
        if (da < -1e-8 && db < -1e-8)
        {
          hi = -1;
          break;
        }
        if (da < -1e-8)
        {
          lo = std::max(lo, da / (da - db));
        }
        if (db < -1e-8)
        {
          hi = std::min(hi, da / (da - db));
        }
      }
      if (hi >= lo)
      {
        intervals.push_back({lo, hi});
      }
    }
    std::sort(intervals.begin(), intervals.end());
    double covered = 0;
    for (auto [lo, hi] : intervals)
    {
      if (lo > covered + 1e-8)
      {
        break;
      }
      covered = std::max(covered, hi);
    }
    if (covered < 1 - 1e-8)
    {
      if (std::getenv("CITY_DEBUG"))
      {
        std::fprintf(stderr, "artery %llu (%.1f,%.1f)-(%.1f,%.1f) covered %.3f of its length\n", (unsigned long long)a.id, a.a.x, a.a.z, a.b.x, a.b.z, covered);
      }
      return fail("fixed artery leaves usable city land");
    }
    for (const auto &q : reserved)
    {
      if (area(intersection(ribbon(a), q)) > 1e-7)
      {
        return fail("fixed artery width crosses a reservation");
      }
    }
    r.streets.push_back({a.id, a.a, a.b, a.halfWidth, true});
  }
  if (arteries.empty())
  {
    return fail("city needs fixed external circulation");
  }
  // Keep only the circulation component attached to the first artery. Reject stranded gates.
  std::vector<bool> connected(r.streets.size());
  connected[r.streets.size() - arteries.size()] = true;
  bool changed = true;
  while (changed)
  {
    changed = false;
    for (size_t i = 0; i < r.streets.size(); ++i)
    {
      if (connected[i])
      {
        for (size_t j = 0; j < r.streets.size(); ++j)
        {
          if (!connected[j] && segmentsMeet(r.streets[i].a, r.streets[i].b, r.streets[j].a, r.streets[j].b))
          {
            connected[j] = true;
            changed = true;
          }
        }
      }
    }
  }
  std::vector<CityStreet> network;
  for (size_t i = 0; i < r.streets.size(); ++i)
  {
    if (connected[i])
    {
      network.push_back(r.streets[i]);
    }
    else if (r.streets[i].arterial)
    {
      return fail("disconnected fixed artery");
    }
  }
  r.streets = network;
  if (s.laneHierarchy && s.recipe == CityRecipe::OrganicVoronoi)
  {
    // Watabou draws only the gate-to-plaza paths as streets; the other ward
    // edges are lanes. Here ward edges become narrow lanes, and the shortest
    // ward-edge path between neighbouring gates (arteries excluded) is a
    // district street, so the fabric reads as a few streets plus lanes.
    for (auto &street : r.streets)
    {
      if (!street.arterial && street.hierarchy == 0)
      {
        street.halfWidth = s.streetHalfWidth * .55;
      }
    }
    std::vector<Position> nodes;
    auto nodeOf = [&](Position p) {
      for (size_t i = 0; i < nodes.size(); ++i)
      {
        if (dist(nodes[i], p) < 1e-5)
        {
          return i;
        }
      }
      nodes.push_back(p);
      return nodes.size() - 1;
    };

    struct Edge
    {
      size_t to, street;
      double cost;
    };

    std::vector<std::vector<Edge>> graph;
    for (size_t k = 0; k < r.streets.size(); ++k)
    {
      const auto &street = r.streets[k];
      if (street.arterial)
      {
        continue;
      }
      size_t a = nodeOf(street.a), b = nodeOf(street.b);
      graph.resize(nodes.size());
      graph[a].push_back({b, k, dist(street.a, street.b)});
      graph[b].push_back({a, k, dist(street.a, street.b)});
    }
    graph.resize(nodes.size());
    auto nearestNode = [&](Position p) {
      size_t best = nodes.size();
      double bestDistance = 1e30;
      for (size_t i = 0; i < nodes.size(); ++i)
      {
        if (dist(nodes[i], p) < bestDistance)
        {
          bestDistance = dist(nodes[i], p);
          best = i;
        }
      }
      return best;
    };
    std::vector<size_t> gateNodes;
    for (const auto &gate : gates)
    {
      if (nodes.size())
      {
        gateNodes.push_back(nearestNode(gate.position));
      }
    }
    for (size_t g = 0; g + 1 < gateNodes.size() || (gateNodes.size() > 2 && g < gateNodes.size()); ++g)
    {
      const size_t from = gateNodes[g], to = gateNodes[(g + 1) % gateNodes.size()];
      std::vector<double> cost(nodes.size(), 1e30);
      std::vector<size_t> via(nodes.size(), SIZE_MAX);
      std::set<std::pair<double, size_t>> open;
      cost[from] = 0;
      open.insert({0, from});
      while (!open.empty())
      {
        auto [c, i] = *open.begin();
        open.erase(open.begin());
        if (i == to)
        {
          break;
        }
        for (const auto &edge : graph[i])
        {
          if (c + edge.cost < cost[edge.to])
          {
            open.erase({cost[edge.to], edge.to});
            cost[edge.to] = c + edge.cost;
            via[edge.to] = edge.street;
            open.insert({cost[edge.to], edge.to});
          }
        }
      }
      for (size_t i = to; i != from && via[i] != SIZE_MAX;)
      {
        auto &street = r.streets[via[i]];
        street.hierarchy = 1;
        street.halfWidth = s.streetHalfWidth * 1.15;
        i = nodeOf(street.a) == i ? nodeOf(street.b) : nodeOf(street.a);
      }
    }
  }
  // Remove a bounded selection of redundant local links while preserving all
  // remaining streets' connection to fixed circulation. Not every ward edge is a road.
  for (size_t omitted = 0; omitted < r.streets.size();)
  {
    bool reclaim = false;
    if (!r.streets[omitted].arterial && r.streets[omitted].hierarchy == 0)
    {
      for (const auto &housing : s.housingAreas)
      {
        if (housing.reclaimLocalStreets)
        {
          auto band = ribbon(r.streets[omitted]);
          double covered = 0;
          for (const auto &piece : housing.pieces)
          {
            covered += area(intersection(band, piece));
          }
          reclaim |= std::abs(covered - area(band)) < 1e-6;
        }
      }
    }
    // Given wards (Watabou: patch = block) keep every edge as a street.
    if (r.streets[omitted].arterial || (!reclaim && (planned || !s.wardCells.empty() || random(r.streets[omitted].id, 12) > .16)))
    {
      ++omitted;
      continue;
    }
    std::vector<bool> seen(r.streets.size());
    std::vector<size_t> queue;
    for (size_t i = 0; i < r.streets.size(); ++i)
    {
      if (r.streets[i].arterial)
      {
        seen[i] = true;
        queue.push_back(i);
        break;
      }
    }
    for (size_t k = 0; k < queue.size(); ++k)
    {
      auto &a = r.streets[queue[k]];
      for (size_t j = 0; j < r.streets.size(); ++j)
      {
        if (j != omitted && !seen[j] && segmentsMeet(a.a, a.b, r.streets[j].a, r.streets[j].b))
        {
          seen[j] = true;
          queue.push_back(j);
        }
      }
    }
    if (queue.size() + 1 == r.streets.size())
    {
      if (reclaim)
      {
        r.reclaimedStreets.push_back(r.streets[omitted].id);
      }
      r.streets.erase(r.streets.begin() + omitted);
    }
    else
    {
      ++omitted;
    }
  }
  // Dissolve ownership seams where a local street was discarded. Retained
  // pavement still bounds every plot through the later clearance checks.
  std::map<uint64_t, uint64_t> parentWard;
  for (const auto &w : r.wards)
  {
    parentWard[w.id] = w.id;
  }
  auto rootWard = [&](uint64_t id) {
    while (parentWard.at(id) != id)
    {
      id = parentWard.at(id);
    }
    return id;
  };
  std::set<uint64_t> retained;
  for (const auto &street : r.streets)
  {
    retained.insert(street.id);
  }
  for (const auto &[id, pair] : streetWards)
  {
    if (!retained.count(id))
    {
      const auto a = rootWard(pair.first), b = rootWard(pair.second);
      parentWard[std::max(a, b)] = std::min(a, b);
    }
  }
  std::map<uint64_t, CityWard> mergedWards;
  for (const auto &w : r.wards)
  {
    auto &joined = mergedWards[rootWard(w.id)];
    joined.id = rootWard(w.id);
    joined.pieces.insert(joined.pieces.end(), w.pieces.begin(), w.pieces.end());
  }
  r.wards.clear();
  for (auto &[id, w] : mergedWards)
  {
    merge(w.pieces);
    r.wards.push_back(std::move(w));
  }
  // Re-cut given wards at immutable arterial centerlines after the ward merge
  // (the merge would join the halves again), so those streets own frontage.
  // A gate road is a polyline: every segment cuts only the pieces it runs
  // through, along its own line, so the pieces' edges follow the road.
  unsigned arterialCuts = 0;
  for (auto &w : r.wards)
  {
    for (const auto &street : arteries)
    {
      if (s.wardCells.empty())
      {
        break;
      }
      const std::vector<Position> path = street.path.size() >= 2 ? street.path : std::vector<Position>{street.a, street.b};
      for (size_t i = 1; i < path.size(); ++i)
      {
        const auto sa = path[i - 1], sb = path[i];
        if (dist(sa, sb) < 1e-6)
        {
          continue;
        }
        std::vector<Poly> pieces;
        const double nx = sb.z - sa.z, nz = sa.x - sb.x, c = nx * sa.x + nz * sa.z;
        for (auto &p : w.pieces)
        {
          // The segment runs through this piece when a sample of it lies well inside.
          bool through = false;
          for (int k = 1; k < 8 && !through; ++k)
          {
            const auto q = mix(sa, sb, k / 8.);
            double edge = 1e30;
            for (size_t e = 0; e < p.size(); ++e)
            {
              edge = std::min(edge, segmentDistance(q, p[e], p[(e + 1) % p.size()]));
            }
            through = inside(p, q) && edge > .3;
          }
          if (!through)
          {
            pieces.push_back(p);
            continue;
          }
          auto left = clip(p, nx, nz, c), right = clip(p, -nx, -nz, -c);
          ++arterialCuts;
          if (!left.empty())
          {
            pieces.push_back(left);
          }
          if (!right.empty())
          {
            pieces.push_back(right);
          }
        }
        w.pieces = std::move(pieces);
      }
    }
  }
  if (std::getenv("CITY_DEBUG"))
  {
    std::fprintf(stderr, "wards: %zu, arterial re-cuts %u\n", r.wards.size(), arterialCuts);
  }
  for (auto &g : gates)
  {
    bool ok = false;
    if (!g.id || !g.externalRoad)
    {
      return fail("gate has no external road");
    }
    for (auto &a : r.streets)
    {
      if (a.arterial && a.id == g.externalRoad && segmentDistance(g.position, a.a, a.b) < 1e-5)
      {
        ok = true;
      }
    }
    bool boundary = false;
    for (size_t i = 0; i < domain.outer.size(); ++i)
    {
      if (segmentDistance(g.position, domain.outer[i], domain.outer[(i + 1) % domain.outer.size()]) < 1e-5)
      {
        boundary = true;
      }
    }
    if (!ok || !boundary)
    {
      return fail("gate disconnected from named arterial or envelope boundary");
    }
  }
  // Lock every graph intersection before adding one modest bend per span.
  for (size_t i = 0; i < r.streets.size(); ++i)
  {
    auto &street = r.streets[i];
    auto a = street.a, b = street.b;
    double dx = b.x - a.x, dz = b.z - a.z, length = dist(a, b);
    std::vector<double> cuts{0, 1};
    for (size_t j = 0; j < r.streets.size(); ++j)
    {
      if (i != j)
      {
        auto c = r.streets[j].a, d = r.streets[j].b;
        double ex = d.x - c.x, ez = d.z - c.z, den = dx * ez - dz * ex;
        if (std::abs(den) < 1e-9)
        {
          continue;
        }
        double t = ((c.x - a.x) * ez - (c.z - a.z) * ex) / den, u = ((c.x - a.x) * dz - (c.z - a.z) * dx) / den;
        if (t > 1e-6 && t < 1 - 1e-6 && u >= -1e-6 && u <= 1 + 1e-6)
        {
          cuts.push_back(t);
        }
      }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(
        std::unique(
            cuts.begin(),
            cuts.end(),
            [](double a, double b) {
              return std::abs(a - b) < 1e-6;
            }),
        cuts.end());
    street.path.push_back(a);
    for (size_t k = 1; k < cuts.size(); ++k)
    {
      auto from = mix(a, b, cuts[k - 1]), to = mix(a, b, cuts[k]);
      double span = dist(from, to), amplitude = (street.arterial || planned) ? 0 : std::min(s.streetCurvature, span * .10) * (random(street.id * 17 + k, 20) < .5 ? -1 : 1);
      const double bend = .35 + .3 * random(street.id * 17 + k, 32);
      const int count = street.arterial ? 1 : 2; // One bend, without collinear sample fragments.
      std::vector<Position> curve;
      for (int retry = 0; retry < 5; ++retry)
      {
        curve.clear();
        bool fits = true;
        for (int n = 1; n <= count; ++n)
        {
          const double t = n == count ? 1. : bend;
          auto p = mix(from, to, t);
          double offset = amplitude * (t < bend ? t / bend : (1 - t) / (1 - bend));
          p.x += -dz / length * offset;
          p.z += dx / length * offset;
          if (!valid(p) || !coveredSegment(curve.empty() ? from : curve.back(), p, usable))
          {
            fits = false;
          }
          curve.push_back(p);
        }
        if (fits)
        {
          break;
        }
        if (retry == 4)
        {
          return fail("curved street crosses excluded land");
        }
        amplitude = retry == 3 ? 0 : amplitude * .5;
      }
      street.path.insert(street.path.end(), curve.begin(), curve.end());
    }
  }
  if (s.walls)
  {
    auto at = wallAt;
    const auto &solid = wallSolid;
    for (auto [from, to] : solid)
    {
      unsigned count = std::max(1u, unsigned(std::ceil((to - from) / s.wallSectionLength)));
      for (unsigned k = 0; k < count; ++k)
      {
        double u0 = from + (to - from) * k / count, u1 = from + (to - from) * (k + 1) / count;
        // Same-sized straight sections: one chord per section, whatever the
        // envelope does in between (the wall ribbon protects the land it cuts).
        std::vector<double> stops{u0, u1};
        for (size_t j = 1; j < stops.size(); ++j)
        {
          if (stops[j] - stops[j - 1] > 1e-6)
          {
            r.walls.push_back({nextStreetId(), at(stops[j - 1]), at(stops[j]), .7, false});
          }
        }
      }
    }
  }
  for (const auto &wall : r.walls)
  {
    for (auto center : {wall.a, wall.b})
    {
      if (std::any_of(r.towers.begin(), r.towers.end(), [&](const auto &tower) {
            return dist(tower.center, center) < 1e-5;
          }))
      {
        continue;
      }
      CityWallTower tower;
      tower.id = (uint64_t(1) << 63) | nextStreetId();
      tower.center = center;
      tower.height = s.wallTowerHeight;
      for (unsigned k = 0; k < 8; ++k)
      {
        double angle = k * 3.14159265358979323846 / 4;
        tower.footprint.push_back({center.x + s.wallTowerRadius * std::cos(angle), 0, center.z + s.wallTowerRadius * std::sin(angle)});
      }
      r.towers.push_back(std::move(tower));
    }
  }
  // Very short wall piers share one elongated tower instead of overlapping
  // separate structural instances. Their endpoint connections remain covered.
  for (size_t i = 0; i < r.towers.size(); ++i)
  {
    for (size_t j = i + 1; j < r.towers.size();)
    {
      if (cityPolygonsOverlap(r.towers[i].footprint, r.towers[j].footprint))
      {
        auto points = r.towers[i].footprint;
        points.insert(points.end(), r.towers[j].footprint.begin(), r.towers[j].footprint.end());
        r.towers[i].footprint = hull(points);
        r.towers[i].center = {};
        for (auto p : r.towers[i].footprint)
        {
          r.towers[i].center.x += p.x / r.towers[i].footprint.size();
          r.towers[i].center.z += p.z / r.towers[i].footprint.size();
        }
        r.towers.erase(r.towers.begin() + j);
        j = i + 1;
      }
      else
      {
        ++j;
      }
    }
  }
  if (r.towers.size() > 512)
  {
    return fail("wall tower budget exceeded");
  }
  const size_t interiorPieces = usable.size();
  uint64_t firstOuterWard = std::numeric_limits<uint64_t>::max(); // Ward ids from here on are outer wards.
  // Outer wards are their own stage: conditional, self-contained, and the only
  // place that appends to the ward, cell and street lists after the interior
  // is final.
  if (!s.suburbLand.empty())
  {
    const citylayout::OuterWardInput input{s, r, patch, domain, wallRing, reserved, arteries, gates, registry, nextStreetId, random, terrain};
    const citylayout::OuterWardState state{usable, cells, firstOuterWard, minX, minZ, maxX, maxZ};
    std::string outerError;

    if (!citylayout::planOuterWards(input, state, &outerError))
    {
      return fail(outerError.c_str());
    }
  }
  // Publishing the ground is its own stage: it is the only part of this
  // function that writes contours, and the rule it enforces - a road band is
  // published only when it lies entirely within the usable land, proven by an
  // exact area comparison - belongs with it rather than in the middle of the
  // street planning above.
  citylayout::GroundPublishResult ground;

  {
    const citylayout::GroundPublishInput input{s, r, usable, wallMasks, terrain, interiorPieces};
    std::string groundError;

    if (!citylayout::publishLayoutGround(input, ground, &groundError))
    {
      return fail(groundError.c_str());
    }
  }

  const std::vector<Poly> &roads = ground.roads;
  if (r.contours.size() > kCityContourBudget)
  {
    return fail("city contour budget exceeded");
  }
  if (!s.emitBuildings)
  {
    out = std::move(r);
    return true;
  }
  // Which housing strategy fills the blocks. This was a two-thousand-line
  // `if (s.blockHousing)` branch against a three-hundred-line `else`: two
  // complete, independent ways of turning a street network into buildings,
  // chosen by a boolean. They are peers now, and a third is a new class rather
  // than a third branch.
  citylayout::LayoutBuild build{s, r, cells, roads, wallMasks, usable, firstOuterWard, random, terrain, reserved, wallRing, gates, minX, minZ, maxX, maxZ};

  const citylayout::StrategyPtr strategy =
      s.blockHousing ? citylayout::StrategyPtr(std::make_shared<citylayout::BlockHousingStrategy>()) : citylayout::StrategyPtr(std::make_shared<citylayout::FrontageHousingStrategy>());

  {
    std::string strategyError;

    if (!strategy->allocate(build, &strategyError))
    {
      return fail(strategyError.c_str());
    }
  }

  if (r.contours.size() > kCityContourBudget)
  {
    return fail("city contour budget exceeded");
  }
  out = std::move(r);
  return true;
}

bool meshCityInstances(const CityLayout &city, const std::function<double(double, double)> &terrain, std::vector<CityInstanceTriangle> &out, std::string *error)
{
  std::vector<CityInstanceTriangle> result;
  auto add = [&](Position a, Position b, Position c, uint64_t id, uint32_t mat) {
    result.push_back({{a, b, c}, id, mat});
  };
  auto block = [&](Poly p, double foundation, double height, uint64_t id, bool roof, bool hip = false) {
    if (roof && p.size() == 4 && dist(p[0], p[1]) < dist(p[0], p[3]))
    {
      std::rotate(p.begin(), p.begin() + 1, p.end()); // Ridge follows the wing's long axis.
    }
    Poly bottom = p, top = p;
    for (size_t i = 0; i < p.size(); ++i)
    {
      bottom[i].y = terrain(p[i].x, p[i].z);
      top[i].y = foundation + height;
      if (!std::isfinite(bottom[i].y) || bottom[i].y > foundation + 1e-6)
      {
        return false;
      }
    }
    for (size_t i = 0; i < p.size(); ++i)
    {
      size_t j = (i + 1) % p.size();
      add(bottom[i], top[i], top[j], id, 12);
      add(bottom[i], top[j], bottom[j], id, 12);
    }
    if (roof && p.size() != 4)
    {
      Position peak{};
      double perimeter = 0;
      for (size_t i = 0; i < p.size(); ++i)
      {
        peak.x += p[i].x / p.size();
        peak.z += p[i].z / p.size();
        perimeter += dist(p[i], p[(i + 1) % p.size()]);
      }
      peak.y = foundation + height + std::min(3., area(p) / perimeter * .5);
      for (size_t i = 0; i < p.size(); ++i)
      {
        add(top[i], peak, top[(i + 1) % p.size()], id, 13);
      }
    }
    else if (roof)
    {
      Position a = mix(top[0], top[3], .5), b = mix(top[1], top[2], .5);
      a.y = b.y = foundation + height + dist(p[0], p[3]) * .28;
      if (hip)
      {
        const auto start = a, end = b;
        a = mix(start, end, .18);
        b = mix(start, end, .82);
        a.y = b.y = foundation + height + (dist(p[0], p[3]) + dist(p[1], p[2])) * .14;
      }
      add(top[0], a, b, id, 13);
      add(top[0], b, top[1], id, 13);
      add(top[3], top[2], b, id, 13);
      add(top[3], b, a, id, 13);
      add(top[0], top[3], a, id, hip ? 13 : 12);
      add(top[1], b, top[2], id, hip ? 13 : 12);
    }
    else
    {
      for (size_t i = 1; i + 1 < top.size(); ++i)
      {
        add(top[0], top[i + 1], top[i], id, 12);
      }
    }
    return true;
  };
  if (!terrain)
  {
    if (error)
    {
      *error = "missing instance terrain";
    }
    return false;
  }
  for (auto &o : city.objects)
  {
    if (o.kind == CityObjectKind::House || o.kind == CityObjectKind::Workshop)
    {
      if (o.footprint.size() < 3 || o.footprint.size() > 128 || !convex(o.footprint) || !std::isfinite(o.foundation) || !std::isfinite(o.height) || o.height <= 0 || o.parts.empty())
      {
        if (error)
        {
          *error = "invalid building or unsupported foundation";
        }
        return false;
      }
      if (!o.courtyard.empty() && (!convex(o.courtyard) || area(intersection(o.courtyard, o.footprint)) < area(o.courtyard) - 1e-6))
      {
        if (error)
        {
          *error = "invalid courtyard";
        }
        return false;
      }
      for (size_t i = 0; i < o.parts.size(); ++i)
      {
        const auto &part = o.parts[i];
        for (size_t j = 0; j < i; ++j)
        {
          if (area(intersection(part.footprint, o.parts[j].footprint)) > 1e-6)
          {
            if (error)
            {
              *error = "overlapping building wings";
            }
            return false;
          }
        }
        if (!o.courtyard.empty() && area(intersection(part.footprint, o.courtyard)) > 1e-6)
        {
          if (error)
          {
            *error = "building wing covers courtyard";
          }
          return false;
        }
        if (part.footprint.size() < 3 || part.footprint.size() > 128 || (!o.block && part.footprint.size() != 4) || !convex(part.footprint) || !std::isfinite(part.height) || part.height <= 0 ||
            area(intersection(part.footprint, o.footprint)) < area(part.footprint) - 1e-6 || !block(part.footprint, o.foundation, part.height, o.id, true, o.block != 0))
        {
          if (error)
          {
            *error = "invalid building wing";
          }
          return false;
        }
      }
    }
  }
  for (auto &w : city.walls)
  {
    auto p = ribbon(w);
    double high = -1e30;
    for (auto v : p)
    {
      high = std::max(high, terrain(v.x, v.z));
    }
    if (!block(p, high, 4, w.id, false))
    {
      if (error)
      {
        *error = "invalid wall foundation";
      }
      return false;
    }
  }
  for (const auto &tower : city.towers)
  {
    if (!convex(tower.footprint) || !std::isfinite(tower.height) || tower.height <= 0)
    {
      if (error)
      {
        *error = "invalid wall tower";
      }
      return false;
    }
    double high = -1e30;
    for (auto v : tower.footprint)
    {
      high = std::max(high, terrain(v.x, v.z));
    }
    if (!block(tower.footprint, high, tower.height, tower.id, false))
    {
      if (error)
      {
        *error = "invalid tower foundation";
      }
      return false;
    }
  }
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
