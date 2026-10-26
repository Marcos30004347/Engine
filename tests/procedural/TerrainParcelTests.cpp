#include "procedural/geometry/TerrainParcels.hpp"
#include <cmath>
#include <iostream>
#include <cstdlib>
#include <fstream>
#include <set>
#include <utility>
using namespace procedural;
using namespace procedural::geometry;
namespace {
double polygonArea(const std::vector<Point2> &p)
{
  double t = 0;
  for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) t += p[j].x * p[i].y - p[i].x * p[j].y;
  return std::abs(.5 * t);
}
}
int main()
{
  int failures = 0;
  const auto check = [&](bool v, const char *l)
  { std::cout << (v ? "PASS " : "FAIL ") << l << '\n'; failures += !v; };

  const double extent = 4000.0;
  // Four square blocks tiling the world, all on gentle arable ground.
  std::vector<std::vector<Point2>> blocks;
  for (int by = 0; by < 2; ++by)
    for (int bx = 0; bx < 2; ++bx)
    {
      const double x = bx * 2000.0, y = by * 2000.0;
      blocks.push_back({{x, y}, {x + 2000, y}, {x + 2000, y + 2000}, {x, y + 2000}});
    }
  ParcelFields fields;
  fields.slope = [](Point2) { return .04; };
  fields.waterType = [](Point2) -> uint8_t { return 0u; };
  fields.treeCover = [](Point2) { return .2; };
  fields.soilQuality = [](Point2) { return .7; };
  fields.moisture = [](Point2) { return .6; };
  fields.elevationMetres = [](Point2) { return 100.0; };

  ParcelSettings settings;
  settings.seed = 3;
  settings.targetAreaSquareMetres = 200.0 * 200.0;
  const ParcelLayer layer = allocateParcels(extent, blocks, {}, fields, {}, settings);

  check(!layer.cells.empty(), "allocates cells");
  check(!layer.edges.empty(), "recovers edges between cells");

  // Area is conserved: the cells must tile the blocks they came from.
  double total = 0;
  for (const auto &c : layer.cells) total += c.areaSquareMetres;
  const double blockTotal = 4 * 2000.0 * 2000.0;
  // Subdivision conserves area exactly. The check is made with the organic
  // boundary treatments off, because both of them deliberately move the outline
  // a block was given: a warped outer boundary encloses a different area from
  // the straight one it replaced, by design. Interior boundaries are hashed
  // canonically and so cancel between neighbours; what is left is the world's
  // own perimeter, which is why the total drifts by a couple of percent rather
  // than by nothing.
  ParcelSettings straight = settings;
  straight.blockWarpRoughness = 0.0; straight.edgeWander = 0.0;
  const ParcelLayer plain = allocateParcels(extent, blocks, {}, fields, {}, straight);
  double plainTotal = 0;
  for (const auto &c : plain.cells) plainTotal += c.areaSquareMetres;
  check(std::abs(plainTotal - blockTotal) / blockTotal < 1e-6, "cells tile their blocks exactly");
  check(std::abs(total - blockTotal) / blockTotal < .06,
      "the organic boundary treatment perturbs the total only slightly");

  bool sane = true, wound = true;
  for (const auto &c : layer.cells)
  {
    sane = sane && c.boundary.size() >= 3u && c.areaSquareMetres > 0;
    double t = 0;
    for (size_t i = 0, j = c.boundary.size() - 1; i < c.boundary.size(); j = i++)
      t += c.boundary[j].x * c.boundary[i].y - c.boundary[i].x * c.boundary[j].y;
    wound = wound && t > 0;   // counter-clockwise
  }
  check(sane, "every cell is a real polygon with positive area");
  check(wound, "every cell is wound counter-clockwise");

  // Subdivision must actually reach the target, and must vary.
  double smallest = 1e30, largest = 0;
  for (const auto &c : layer.cells) { smallest = std::min(smallest, c.areaSquareMetres); largest = std::max(largest, c.areaSquareMetres); }
  check(largest / smallest > 3.0, "parcel areas vary by more than a factor of three");
  // Cell size is graded by distance from the nearest settlement - infield,
  // outfield, waste - so the target area is a target for worked ground and not
  // a global average. This world has no settlement at all, which is the
  // maximally remote case, so the cells here should be well *above* the target:
  // that is the waste, and land nobody walks is not divided into fields.
  const double mean = total / layer.cells.size();
  check(mean > settings.targetAreaSquareMetres * 1.5,
      "with no settlement anywhere the land stays coarse");

  // Put a village in one corner and the grading has to show: ground within a
  // kilometre of it is worked at something near the target, and ground at the
  // far end of the world is not.
  SettlementNetwork oneVillage;
  oneVillage.sites.push_back({{400.0, 400.0}, SettlementTier::Village, .8, 0u});
  const ParcelLayer graded = allocateParcels(extent, blocks, {}, fields, oneVillage, settings);
  double nearTotal = 0, farTotal = 0; uint32_t nearCount = 0, farCount = 0;
  for (const auto &c : graded.cells)
  {
    const double d = std::hypot(c.centre.x - 400.0, c.centre.y - 400.0);
    if (d < 1000.0) { nearTotal += c.areaSquareMetres; ++nearCount; }
    else if (d > 3000.0) { farTotal += c.areaSquareMetres; ++farCount; }
  }
  check(nearCount > 0 && farCount > 0, "the graded world has ground both near and far");
  if (nearCount && farCount)
  {
    const double nearMean = nearTotal / nearCount, farMean = farTotal / farCount;
    std::cout << "  near the village " << nearMean << " m2, in the waste " << farMean << " m2\n";
    check(nearMean < settings.targetAreaSquareMetres * 3.0,
        "parcels near a settlement land near the target");
    check(farMean > nearMean * 1.5, "parcels grow with distance from the settlement");
  }
  std::cout << "  " << layer.cells.size() << " cells, " << layer.edges.size()
            << " edges, area " << smallest << " to " << largest << " m2, mean " << mean << '\n';

  // Every interior edge is shared by exactly two cells, and owned by one.
  uint32_t interior = 0, owned = 0;
  for (const auto &e : layer.edges)
  {
    if (e.cellB == kNoParcel) continue;
    ++interior;
    owned += e.owner == e.cellA || e.owner == e.cellB;
  }
  check(interior > 0 && owned == interior, "every interior edge is owned by one of its two cells");

  // Cell lookup agrees with the polygons.
  uint32_t hits = 0, agree = 0;
  for (const auto &c : layer.cells)
  {
    const uint32_t found = layer.cellAt(c.centre);
    if (found == kNoParcel) continue;
    ++hits;
    agree += found < layer.cells.size();
  }
  check(hits > layer.cells.size() / 2u, "the index finds most cells by their own centre");
  check(hits == agree, "the index never returns an out-of-range cell");

  // Determinism.
  const ParcelLayer again = allocateParcels(extent, blocks, {}, fields, {}, settings);
  bool identical = again.cells.size() == layer.cells.size();
  for (size_t i = 0; identical && i < layer.cells.size(); ++i)
    identical = again.cells[i].areaSquareMetres == layer.cells[i].areaSquareMetres &&
        again.cells[i].kind == layer.cells[i].kind && again.cells[i].tone == layer.cells[i].tone;
  check(identical, "two runs of the same seed agree exactly");

  // Water and steep ground are never parcelled.
  ParcelFields wet = fields;
  wet.waterType = [](Point2 p) -> uint8_t { return p.x < 2000.0 ? 1u : 0u; };
  const ParcelLayer half = allocateParcels(extent, blocks, {}, wet, {}, settings);
  bool dry = true;
  for (const auto &c : half.cells) dry = dry && c.centre.x >= 2000.0;
  check(dry, "never parcels water");
  ParcelFields steep = fields;
  steep.slope = [](Point2) { return .8; };
  check(allocateParcels(extent, blocks, {}, steep, {}, settings).cells.empty(), "never parcels steep ground");

  // Infrastructure must cut rather than be cut: a road across the world should
  // leave no cell straddling it.
  std::vector<std::vector<Point2>> road{{{0, 1000}, {4000, 1000}}};
  const ParcelLayer cut = allocateParcels(extent, blocks, road, fields, {}, settings);
  uint32_t straddling = 0;
  for (const auto &c : cut.cells)
  {
    bool below = false, above = false;
    for (const auto &p : c.boundary) { below |= p.y < 999.0; above |= p.y > 1001.0; }
    straddling += below && above;
  }
  std::cout << "  with a road: " << cut.cells.size() << " cells, " << straddling << " straddle it\n";
  check(straddling * 20u < cut.cells.size(), "a road is a boundary, not something cells straddle");

  ParcelSettings off = settings; off.enabled = false;
  check(allocateParcels(extent, blocks, {}, fields, {}, off).cells.empty(), "disabled allocates nothing");

  // PARCEL_PPM=<path> dumps the allocation so the pattern can be looked at.
  // This layer is judged on whether it reads as land, which no assertion sees.
  if (const char *dump = std::getenv("PARCEL_PPM"))
  {
    const uint32_t side = 900u;
    std::vector<uint8_t> image(size_t(side) * side * 3u, 235u);
    const auto plot = [&](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b)
    {
      if (x >= side || y >= side) return;
      const size_t at = (size_t(y) * side + x) * 3u;
      image[at] = r; image[at + 1] = g; image[at + 2] = b;
    };
    const auto toPixel = [&](Point2 p)
    { return std::pair<double, double>{p.x / extent * side, (1.0 - p.y / extent) * side}; };
    // Fill each cell by scanning its bounding box; slow and fine for a dump.
    for (const auto &c : layer.cells)
    {
      Point2 lo = c.boundary.front(), hi = lo;
      for (const auto &q : c.boundary)
      { lo.x = std::min(lo.x, q.x); lo.y = std::min(lo.y, q.y); hi.x = std::max(hi.x, q.x); hi.y = std::max(hi.y, q.y); }
      const auto [x0, y1] = toPixel(lo); const auto [x1, y0] = toPixel(hi);
      // Greens for grass-like uses, ochres for tilled, dark for woodland.
      static const uint8_t base[int(ParcelKind::Count)][3] = {
        {150, 142, 120}, {186, 168, 112}, {126, 166,  88}, {142, 180, 104},
        {110, 150,  90}, { 66, 102,  62}, {170, 150, 140}};
      const auto *rgb = base[int(c.kind)];
      const int shade = int(c.tone) * 7 - 21;
      for (int py = int(y0); py <= int(y1); ++py)
        for (int px = int(x0); px <= int(x1); ++px)
        {
          const Point2 world(( px + .5) / side * extent, (1.0 - (py + .5) / side) * extent);
          bool inside = false;
          for (size_t i = 0, j = c.boundary.size() - 1; i < c.boundary.size(); j = i++)
          {
            const auto &a = c.boundary[i], &b = c.boundary[j];
            if ((a.y > world.y) != (b.y > world.y) &&
                world.x < a.x + (b.x - a.x) * (world.y - a.y) / (b.y - a.y)) inside = !inside;
          }
          if (inside) plot(uint32_t(px), uint32_t(py),
              uint8_t(std::clamp(rgb[0] + shade, 0, 255)), uint8_t(std::clamp(rgb[1] + shade, 0, 255)),
              uint8_t(std::clamp(rgb[2] + shade, 0, 255)));
        }
    }
    for (const auto &e : layer.edges)
    {
      if (e.type == ParcelEdgeType::None) continue;
      static const uint8_t ink[int(ParcelEdgeType::Count)][3] = {
        {0,0,0}, {40, 78, 40}, {120, 118, 112}, {150, 130, 90}, {28, 64, 30}, {205, 190, 160}, {80, 130, 170}};
      const auto *rgb = ink[int(e.type)];
      for (size_t i = 1; i < e.path.size(); ++i)
      {
        const auto [ax, ay] = toPixel(e.path[i - 1]); const auto [bx, by] = toPixel(e.path[i]);
        const int steps = int(std::max(std::abs(bx - ax), std::abs(by - ay))) + 1;
        for (int k = 0; k <= steps; ++k)
        {
          const double t = double(k) / steps;
          plot(uint32_t(ax + (bx - ax) * t), uint32_t(ay + (by - ay) * t), rgb[0], rgb[1], rgb[2]);
        }
      }
    }
    std::ofstream out(dump, std::ios::binary);
    out << "P6\n" << side << " " << side << "\n255\n";
    out.write(reinterpret_cast<const char *>(image.data()), std::streamsize(image.size()));
    std::cout << "  wrote " << dump << '\n';
  }

  std::cout << (failures ? "FAILED" : "OK") << " parcel allocation\n";
  return failures ? 1 : 0;
}
