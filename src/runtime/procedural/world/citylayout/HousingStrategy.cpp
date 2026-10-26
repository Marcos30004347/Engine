#include "procedural/world/citylayout/HousingStrategy.hpp"

#include "procedural/world/generator/LotPlanner.hpp"
#include "procedural/world/generator/PatchTiling.hpp"
#include "procedural/world/generator/RingLotPlanner.hpp"
#include "procedural/world/generator/WardBuildings.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace procedural::world::citylayout {

using namespace ::procedural::world::rewrite;
using namespace ::procedural::world::generator;

bool BlockHousingStrategy::allocate(LayoutBuild &build, std::string *error)
{
  // As with the frontage strategy: the body was written against these names on
  // the shared stack, so they are bound rather than rewritten.
  const CitySettings &s = build.settings;
  CityLayout &r = build.layout;
  const auto &cells = build.cells;
  const auto &roads = build.roads;
  const auto &wallMasks = build.wallMasks;
  const auto &usable = build.usable;
  const uint64_t firstOuterWard = build.firstOuterWard;
  const auto &random = build.random;
  const auto &terrain = build.terrain;
  const auto &reserved = build.reserved;
  const auto &wallRing = build.wallRing;
  const auto &gates = build.gates;
  const double minX = build.minX;
  const double minZ = build.minZ;
  const double maxX = build.maxX;
  const double maxZ = build.maxZ;

  const auto towerClearance = [&](const CityWallTower &tower) {
    return citylayout::towerClearance(tower, s.wallSetback);
  };

  const auto fail = [&](const char *m) {
    if (error != nullptr)
    {
      *error = m;
    }

    return false;
  };

  (void)cells;
  (void)roads;
  (void)wallMasks;
  (void)usable;
  (void)firstOuterWard;
  (void)random;
  (void)terrain;
  (void)reserved;
  (void)wallRing;
  (void)gates;
  (void)minX;
  (void)minZ;
  (void)maxX;
  (void)maxZ;
  (void)towerClearance;

  // Reuse the final-map block extractor. Voronoi cells and CDT seams are not
  // house frontages; only land edges adjoining a published road qualify.
  SurfaceMapSettings bounds;
  bounds.minX = minX;
  bounds.minZ = minZ;
  bounds.maxX = maxX;
  bounds.maxZ = maxZ;
  bounds.maximumFaces = 40000;
  SurfaceArrangement arranged;
  if (!arrangeSurfaceContours(r.contours, bounds, terrain, s.revision, arranged, error) || !extractCityStreetBlocks(arranged, 11, r.blocks, error))
  {
    return false;
  }
  // Blocks are faces of the road arrangement, so a block next to the wall
  // reaches the envelope: inside the walls every block stops at the chords,
  // less the setback (the same kernel the wards were clipped by).
  if (s.walls && wallRing.size() >= 3 && !s.wardCells.empty())
  {
    const double orientation = area(wallRing) < 0 ? -1 : 1;
    std::vector<CityStreetBlock> kept;
    for (auto &block : r.blocks)
    {
      Poly outline = block.boundary.outer;
      bool nearWall = false;
      for (auto p : outline)
      {
        for (size_t i = 0; i < wallRing.size(); ++i)
        {
          nearWall = nearWall || segmentDistance(p, wallRing[i], wallRing[(i + 1) % wallRing.size()]) < s.wallSetback + 30;
        }
      }
      if (!nearWall || !block.boundary.holes.empty())
      {
        kept.push_back(std::move(block));
        continue;
      }
      for (size_t i = 0; i < wallRing.size() && !outline.empty(); ++i)
      {
        const auto a = wallRing[i], b = wallRing[(i + 1) % wallRing.size()];
        const double l = dist(a, b);
        if (l < 1e-6)
        {
          continue;
        }
        const double nx = (b.z - a.z) / l * orientation, nz = (a.x - b.x) / l * orientation;
        bool reaches = false;
        for (auto v : outline)
        {
          const double t = ((v.x - a.x) * (b.x - a.x) + (v.z - a.z) * (b.z - a.z)) / (l * l);
          reaches |= t > -.05 && t < 1.05 && nx * (v.x - a.x) + nz * (v.z - a.z) > -(s.wallSetback + 2);
        }
        if (!reaches)
        {
          continue;
        }
        outline = clip(outline, nx, nz, nx * a.x + nz * a.z - s.wallSetback);
      }
      // Drop repeated and collinear vertices left by the clips.
      Poly tidy;
      for (size_t i = 0; i < outline.size(); ++i)
      {
        const auto a = outline[(i + outline.size() - 1) % outline.size()], b = outline[i], c = outline[(i + 1) % outline.size()];
        if (dist(a, b) > 1e-3 && std::abs(cross(a, b, c)) > 1e-3 * std::max(1., dist(a, c)))
        {
          tidy.push_back(b);
        }
      }
      // A concave outline cut by a half-plane can fold onto itself; such a
      // block keeps its outline (its lots are still clipped by the wall band
      // through the ring outline below).
      auto simple = [](const Poly &p) {
        for (size_t i = 0; i < p.size(); ++i)
        {
          for (size_t j = i + 2; j < p.size(); ++j)
          {
            if (i == 0 && j + 1 == p.size())
            {
              continue;
            }
            const auto a = p[i], b = p[(i + 1) % p.size()], c = p[j], d = p[(j + 1) % p.size()];
            const double d1 = cross(a, b, c), d2 = cross(a, b, d), d3 = cross(c, d, a), d4 = cross(c, d, b);
            if (((d1 > 1e-9 && d2 < -1e-9) || (d1 < -1e-9 && d2 > 1e-9)) && ((d3 > 1e-9 && d4 < -1e-9) || (d3 < -1e-9 && d4 > 1e-9)))
            {
              return false;
            }
            if (segmentDistance(c, a, b) < 1e-6 || segmentDistance(d, a, b) < 1e-6 || segmentDistance(a, c, d) < 1e-6 || segmentDistance(b, c, d) < 1e-6)
            {
              return false;
            }
          }
        }
        return true;
      };
      if (tidy.size() >= 3 && area(tidy) > 20 && simple(tidy))
      {
        block.boundary.outer = std::move(tidy);
        kept.push_back(std::move(block));
      }
      else if (tidy.size() >= 3 && area(tidy) > 20)
      {
        if (std::getenv("CITY_DEBUG"))
        {
          std::fprintf(stderr, "wall clip: block %llu folds (%zu vertices), outline kept\n", (unsigned long long)block.id, tidy.size());
        }
        kept.push_back(std::move(block));
      }
    }
    r.blocks = std::move(kept);
  }
  std::set<uint64_t> exteriorBlocks;
  if (!s.wardCells.empty())
  {
    // Watabou: the patch is the block. Every ward piece inset by the width
    // of the street on each of its edges (getCityBlock) is a block, so no
    // lane end or leftover land can merge two wards into one face; the
    // arrangement faces serve the outskirts only.
    std::vector<CityStreetBlock> wardBlocks;
    uint64_t nextBlock = 1;
    for (const auto &w : r.wards)
    {
      const bool outerWard = w.id >= firstOuterWard;
      if (outerWard && s.outerWardCells.empty())
      {
        continue; // Lattice outskirts keep their arrangement faces.
      }
      // An outer ward is crossed by the gate road and its side roads: every
      // street segment running through a piece cuts it along its line.
      std::vector<Poly> wardPieces = w.pieces;
      for (const auto &street : r.streets)
      {
        if (!outerWard)
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
          std::vector<Poly> next;
          const double nx = sb.z - sa.z, nz = sa.x - sb.x, c = nx * sa.x + nz * sa.z;
          for (auto &p : wardPieces)
          {
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
              next.push_back(p);
              continue;
            }
            auto left = clip(p, nx, nz, c), right = clip(p, -nx, -nz, -c);
            if (left.size() >= 3 && area(left) > 1)
            {
              next.push_back(left);
            }
            if (right.size() >= 3 && area(right) > 1)
            {
              next.push_back(right);
            }
          }
          wardPieces = std::move(next);
        }
      }
      for (const auto &piece : wardPieces)
      {
        // The remainder of a compound's own cell (the temple's or the civic
        // hall's yard) wraps the compound on several sides; it stays open.
        bool yard = false;
        for (const auto &compound : reserved)
        {
          unsigned shared = 0;
          for (auto q : compound)
          {
            for (size_t e = 0; e < piece.size(); ++e)
            {
              if (segmentDistance(q, piece[e], piece[(e + 1) % piece.size()]) < 1e-3)
              {
                ++shared;
                break;
              }
            }
          }
          yard |= shared >= 3;
        }
        if (yard)
        {
          continue;
        }
        Poly outline = piece;
        if (area(outline) < 0)
        {
          std::reverse(outline.begin(), outline.end());
        }
        const Poly ring = outline;
        for (size_t k = 0; k < ring.size() && !outline.empty(); ++k)
        {
          const auto a = ring[k], b = ring[(k + 1) % ring.size()];
          const double l = dist(a, b);
          if (l < 1e-6)
          {
            continue;
          }
          // The inset of an edge is the far side of every street ribbon that
          // runs along it: a gate road is a polyline that can sit a metre
          // off the edge, so the edge is sampled against every segment.
          double width = 0;
          for (const auto &street : r.streets)
          {
            std::vector<Position> path = street.path.size() >= 2 ? street.path : std::vector<Position>{street.a, street.b};
            for (size_t i = 1; i < path.size(); ++i)
            {
              const auto sa = path[i - 1], sb = path[i];
              const double sl = dist(sa, sb);
              if (sl < 1e-6)
              {
                continue;
              }
              // Parallel within about ten degrees.
              if (std::abs((b.x - a.x) * (sb.z - sa.z) - (b.z - a.z) * (sb.x - sa.x)) / (l * sl) > .17)
              {
                continue;
              }
              double far = -1;
              for (int k = 0; k <= 8; ++k)
              {
                const auto q = mix(a, b, k / 8.);
                const double t = ((q.x - sa.x) * (sb.x - sa.x) + (q.z - sa.z) * (sb.z - sa.z)) / (sl * sl);
                const double d = std::abs(cross(sa, sb, q)) / sl;
                if (t > -1e-3 && t < 1 + 1e-3 && d < street.halfWidth + 1.5)
                {
                  far = std::max(far, d);
                }
              }
              if (far >= 0)
              {
                width = std::max(width, far + street.halfWidth);
              }
            }
          }
          // getCityBlock: an edge without a street is inset by half a regular
          // street inside the walls and half an alley outside; an edge on the
          // wall pavement by half a main street (2.5 m unit scale).
          if (width == 0)
          {
            width = outerWard ? .75 : 1.25;
            const auto m = mix(a, b, .5);
            for (size_t i = 0; i < wallRing.size(); ++i)
            {
              if (segmentDistance(m, wallRing[i], wallRing[(i + 1) % wallRing.size()]) < s.wallSetback + 1)
              {
                width = 2.5;
              }
            }
          }
          if (width > 0)
          {
            outline = clip(outline, (b.z - a.z) / l, (a.x - b.x) / l, ((b.z - a.z) * a.x + (a.x - b.x) * a.z) / l - width);
          }
        }
        if (outline.size() >= 3 && area(outline) > 20)
        {
          CityStreetBlock block;
          block.id = nextBlock++;
          block.boundary.outer = std::move(outline);
          if (outerWard)
          {
            block.use = DistrictUse::Suburb;
            exteriorBlocks.insert(block.id);
          }
          wardBlocks.push_back(std::move(block));
        }
      }
    }
    r.blocks = std::move(wardBlocks);
    if (std::getenv("CITY_DEBUG"))
    {
      std::fprintf(stderr, "ward blocks: %zu from %zu wards\n", r.blocks.size(), r.wards.size());
    }
  }
  if (!r.suburbs.empty() && (s.wardCells.empty() || s.outerWardCells.empty()))
  {
    std::vector<CityStreetBlock> suburbs;
    if (!extractCityStreetBlocks(arranged, 16, suburbs, error))
    {
      return false;
    }
    uint64_t blockID = 0;
    for (const auto &block : r.blocks)
    {
      blockID = std::max(blockID, block.id);
    }
    for (auto &block : suburbs)
    {
      block.id = ++blockID;
      block.use = DistrictUse::Suburb;
      exteriorBlocks.insert(block.id);
      r.blocks.push_back(std::move(block));
    }
  }
  if (r.blocks.size() > 256)
  {
    return fail("physical city block budget exceeded");
  }

  struct BlockFront
  {
    Position a, b;
    uint64_t street = 0;
  };

  uint64_t objectID = 1, frontageID = 1;
  const double inset = s.setback + s.overhang;
  auto closest = [](Position p, Position a, Position b) {
    double length2 = (b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z);
    return mix(a, b, std::clamp(((p.x - a.x) * (b.x - a.x) + (p.z - a.z) * (b.z - a.z)) / length2, 0., 1.));
  };
  for (auto &block : r.blocks)
  {
    // Decompose for clipping only; all pieces participate in the same parcel
    // cut, and artificial decomposition edges never acquire street access.
    BoundaryRegistry blockRegistry;
    std::vector<BoundaryInput> blockEdges;
    PatchDomain blockDomain;
    blockDomain.id = block.id;
    uint64_t edgeID = 1, vertexID = 1;
    auto addRing = [&](const Poly &p, std::vector<BoundaryUse> &uses) {
      const auto first = vertexID;
      for (size_t k = 0; k < p.size(); ++k)
      {
        blockEdges.push_back({edgeID, {vertexID++, p[k]}, {k + 1 == p.size() ? first : vertexID, p[(k + 1) % p.size()]}, {}});
        uses.push_back({edgeID++, k + 1 == p.size()});
      }
    };
    addRing(block.boundary.outer, blockDomain.outer);
    for (const auto &hole : block.boundary.holes)
    {
      blockDomain.holes.emplace_back();
      addRing(hole, blockDomain.holes.back());
    }
    PatchMesh blockMesh;
    if (!blockRegistry.commit(blockEdges, error) || !compilePatchMesh(
                                                        blockDomain,
                                                        blockRegistry,
                                                        {},
                                                        [](double, double) {
                                                          return 0.;
                                                        },
                                                        blockMesh,
                                                        error))
    {
      return false;
    }
    std::vector<Poly> pieces;
    for (auto t : blockMesh.triangles)
    {
      Poly p;
      for (auto i : t)
      {
        p.push_back(blockMesh.vertices[i].position);
      }
      if (area(p) < 0)
      {
        std::reverse(p.begin(), p.end());
      }
      pieces.push_back(std::move(p));
    }
    merge(pieces);
    const bool exterior = exteriorBlocks.count(block.id) != 0;
    // Outer neighbourhoods reserve some larger work/home courts before
    // parcel cuts, instead of trying to relabel tiny cottage plots afterward.
    const double outerProgramme = random(block.id, 95);
    double gardenFraction = 0;
    // Outskirts on planning cells are village rows: big plots, small houses far apart.
    // Outskirts thin out with the walk from the nearest gate (Watabou's outer
    // patches, a town that grew along its roads): rings like the core within
    // 70 m of a gate, rows of larger plots along the roads out to 160 m, and
    // land beyond that.
    double gateWalk = 0;
    if (exterior && !s.outerWardCells.empty())
    {
      Position blockCentre{0, 0, 0};
      for (auto p : block.boundary.outer)
      {
        blockCentre.x += p.x / block.boundary.outer.size();
        blockCentre.z += p.z / block.boundary.outer.size();
      }
      gateWalk = 1e30;
      for (const auto &gate : gates)
      {
        gateWalk = std::min(gateWalk, dist(gate.position, blockCentre));
      }
    }
    const bool outerRow = false;
    (void)gateWalk;
    double plotArea = exterior ? (!s.outerWardCells.empty() ? 90 + 60 * outerProgramme : outerProgramme < .25 ? 240 : outerProgramme < .55 ? 110 : 70) : s.targetPlotArea, bestOverlap = 0;
    for (const auto &housing : s.housingAreas)
    {
      double overlap = 0;
      for (const auto &mask : housing.pieces)
      {
        for (const auto &p : pieces)
        {
          overlap += area(intersection(p, mask));
        }
      }
      if (overlap > bestOverlap + 1e-6 || (overlap > 0 && std::abs(overlap - bestOverlap) < 1e-6 && housing.id < block.district))
      {
        bestOverlap = overlap;
        block.district = housing.id;
        block.use = housing.use;
        plotArea = housing.targetPlotArea;
        gardenFraction = housing.gardenFraction;
      }
    }
    std::vector<BlockFront> fronts;
    auto rings = block.boundary.holes;
    rings.insert(rings.begin(), block.boundary.outer);
    for (const auto &ring : rings)
    {
      for (size_t k = 0; k < ring.size(); ++k)
      {
        const auto a = ring[k], b = ring[(k + 1) % ring.size()], mid = mix(a, b, .5);
        uint64_t streetID = 0;
        double best = 1e30;
        for (const auto &street : r.streets)
        {
          for (size_t j = 1; j < street.path.size(); ++j)
          {
            auto q = closest(mid, street.path[j - 1], street.path[j]);
            double d = dist(q, mid);
            // A road edge must be parallel to, and at the width of, its
            // supporting centerline. Envelope edges near roads do not count.
            auto u = street.path[j - 1], v = street.path[j];
            double parallel = std::abs((b.x - a.x) * (v.z - u.z) - (b.z - a.z) * (v.x - u.x)) / (dist(a, b) * dist(u, v));
            if (parallel < 1e-5 && std::abs(d - street.halfWidth) < .005 && d < best)
            {
              best = d;
              streetID = street.id;
            }
          }
        }
        if (streetID)
        {
          fronts.push_back({a, b, streetID});
        }
        if (inset > 0)
        {
          auto band = ribbon({0, a, b, inset, false});
          std::vector<Poly> next;
          for (const auto &p : pieces)
          {
            auto clipped = intersection(p, band).empty() ? std::vector<Poly>{p} : subtract(p, band);
            next.insert(next.end(), clipped.begin(), clipped.end());
          }
          pieces = std::move(next);
        }
      }
    }
    // Existing wall massing is protected even on blocks crossed by its ribbon.
    for (const auto &wall : r.walls)
    {
      auto expanded = wall;
      expanded.halfWidth += std::max(inset, s.wallSetback);
      // Extend end caps too: gate corners need the same clearance as wall faces.
      const double extension = std::max(inset, s.wallSetback);
      const double length = dist(wall.a, wall.b);
      expanded.a = mix(wall.a, wall.b, -extension / length);
      expanded.b = mix(wall.a, wall.b, 1 + extension / length);
      std::vector<Poly> next;
      for (const auto &p : pieces)
      {
        auto mask = ribbon(expanded);
        auto clipped = intersection(p, mask).empty() ? std::vector<Poly>{p} : subtract(p, mask);
        next.insert(next.end(), clipped.begin(), clipped.end());
      }
      pieces = std::move(next);
    }
    for (const auto &tower : r.towers)
    {
      std::vector<Poly> next;
      for (const auto &p : pieces)
      {
        auto clipped = subtract(p, towerClearance(tower));
        next.insert(next.end(), clipped.begin(), clipped.end());
      }
      pieces = std::move(next);
    }
    // Parcel-local merging avoids a cubic all-pairs merge of an entire city block.
    if (pieces.empty() || fronts.empty())
    {
      block.rejection = "no buildable road frontage";
      continue;
    }
    std::vector<std::vector<Poly>> leaves;
    uint32_t parcelCuts = 0;
    std::function<void(std::vector<Poly>, unsigned, uint64_t)> partition;
    partition = [&](std::vector<Poly> land, unsigned depth, uint64_t key) {
      double total = 0;
      for (const auto &p : land)
      {
        total += area(p);
      }
      if (total <= plotArea * (.8 + .6 * random(key, 81)) || depth == 12 || parcelCuts >= 4096)
      {
        merge(land);
        leaves.push_back(std::move(land));
        return;
      }
      // Choose a local constructed axis from the convex hull's longest edge.
      Poly vertices;
      for (const auto &p : land)
      {
        vertices.insert(vertices.end(), p.begin(), p.end());
      }
      auto outline = hull(vertices);
      Position a{}, b{};
      double longest = 0;
      for (size_t k = 0; k < outline.size(); ++k)
      {
        if (dist(outline[k], outline[(k + 1) % outline.size()]) > longest)
        {
          a = outline[k];
          b = outline[(k + 1) % outline.size()];
          longest = dist(a, b);
        }
      }
      double nx = (b.x - a.x) / longest, nz = (b.z - a.z) / longest, lo = 1e30, hi = -1e30;
      for (auto p : outline)
      {
        lo = std::min(lo, nx * p.x + nz * p.z);
        hi = std::max(hi, nx * p.x + nz * p.z);
      }
      // Watabou's createAlleys: a small angle jitter only on large parcels,
      // so buildings stay rectangular even in chaotic wards.
      if (total >= plotArea * 4 && s.gridChaos > 0)
      {
        const double tilt = (random(key, 83) - .5) * (3.14159265358979 / 6) * s.gridChaos;
        const double rx = nx * std::cos(tilt) - nz * std::sin(tilt), rz = nx * std::sin(tilt) + nz * std::cos(tilt);
        nx = rx;
        nz = rz;
        lo = 1e30;
        hi = -1e30;
        for (auto p : outline)
        {
          lo = std::min(lo, nx * p.x + nz * p.z);
          hi = std::max(hi, nx * p.x + nz * p.z);
        }
      }
      double cut = lo + (hi - lo) * (.42 + .16 * random(key, 82));
      std::vector<Poly> left, right;
      double leftArea = 0, rightArea = 0;
      for (const auto &p : land)
      {
        auto l = clip(p, nx, nz, cut), q = clip(p, -nx, -nz, -cut);
        if (!l.empty())
        {
          leftArea += area(l);
          left.push_back(std::move(l));
        }
        if (!q.empty())
        {
          rightArea += area(q);
          right.push_back(std::move(q));
        }
      }
      if (std::min(leftArea, rightArea) < plotArea * .22)
      {
        merge(land);
        leaves.push_back(std::move(land));
        return;
      }
      ++parcelCuts;
      partition(std::move(left), depth + 1, key * 2 + 1);
      partition(std::move(right), depth + 1, key * 2 + 2);
    };
    bool stripped = false, stripsWanted = false;
    if (s.watabouLots)
    {
      // Three neighbourhood allocations: packed wards cut by createAlleys
      // (slums, workers), perimeter wards with lots on the street edges and
      // the middle free (common and merchant blocks, Watabou's ring), and
      // estates, one lot per block for the wealthy.
      // Some wards keep the middle free: merchants and a share of the common
      // wards are rung around a court (generator::RingLotPlanner) instead of
      // being bisected whole.
      const bool courtWard = !exterior && (block.use == DistrictUse::Merchant || ((block.use == DistrictUse::Residential || block.use == DistrictUse::Craftsmen) && random(block.id, 105) < .3));
      bool rung = false;
      if (s.wardBuildings && courtWard && convex(block.boundary.outer))
      {
        // A court ward: lots around the middle (generator::RingLotPlanner);
        // when the block is too narrow for a ring it is bisected like the rest.
        const double plot = block.use == DistrictUse::Merchant ? 7000. / 65 : 7000. / 80;
        generator::RingLotInput ringInput;
        ringInput.block = block.boundary.outer;
        ringInput.depth = std::max(5., std::min({block.use == DistrictUse::Merchant ? 16. : 13., plot / 4.5, std::sqrt(plot * 1.6)}));
        ringInput.targetArea = plot;
        ringInput.divideCourtAbove = block.use == DistrictUse::Merchant ? 2500 : 700;
        ringInput.fillCourtBelow = block.use == DistrictUse::Merchant ? 0 : plot * 2.5;
        ringInput.alleyWidth = std::max(2., s.frontageGap * 2);
        ringInput.key = block.id;
        ringInput.random = random;
        double longest = -1;
        for (size_t k = 0; k < block.boundary.outer.size(); ++k)
        {
          const auto a = block.boundary.outer[k], b = block.boundary.outer[(k + 1) % block.boundary.outer.size()];
          if (dist(a, b) > longest)
          {
            longest = dist(a, b);
            ringInput.axisRadians = std::atan2(b.z - a.z, b.x - a.x);
          }
        }
        generator::RingLotPlan ringPlan;
        generator::Ledger ringLedger;
        std::string ringError;
        if (generator::RingLotPlanner::plan(ringInput, ringPlan, ringLedger, &ringError) && !ringPlan.lots.empty())
        {
          for (auto &lot : ringPlan.lots)
          {
            block.lots.push_back(lot);
            leaves.push_back({lot});
          }
          for (auto &lot : ringPlan.open)
          {
            block.openLots.push_back(lot);
          }
          for (auto &court : ringPlan.courts)
          {
            block.courtyards.push_back({court, {}});
          }
          rung = true;
          stripped = true;
          if (s.lotsOnly)
          {
            continue;
          }
        }
      }
      if (s.wardBuildings && !rung && (!exterior || !s.outerWardCells.empty()))
      {
        // Watabou's wards (generator::WardBuildings): the block, already
        // inset by its streets, is bisected into buildings with alleys;
        // the ward class sets minSq, grid and size chaos and the empty
        // probability (Slum, Craftsmen, Merchant, Patriciate,
        // Administration, Gate wards), scaled from 2.5 m units.
        const double unit = 6.25;
        const double r1 = random(block.id, 101), r2 = random(block.id, 102), r3 = random(block.id, 103);
        generator::WardBuildingsInput wardInput;
        wardInput.patch = block.boundary.outer;
        wardInput.alley = 1.5;
        wardInput.key = block.id;
        wardInput.random = random;
        if (exterior)
        {
          wardInput.minSq = (10 + 50 * r1 * r2) * unit; // GateWard
          wardInput.gridChaos = .5 + .3 * r3;
          wardInput.sizeChaos = .7;
          wardInput.emptyProb = .04;
        }
        else if (block.use == DistrictUse::Suburb || block.use == DistrictUse::WorkerHousing)
        {
          wardInput.minSq = (10 + 30 * r1 * r2) * unit; // Slum
          wardInput.gridChaos = .6 + .4 * r3;
          wardInput.sizeChaos = .8;
          wardInput.emptyProb = .03;
        }
        else if (block.use == DistrictUse::Merchant || block.use == DistrictUse::Guild)
        {
          wardInput.minSq = (50 + 60 * r1 * r2) * unit; // Merchant
          wardInput.gridChaos = .5 + .3 * r3;
          wardInput.sizeChaos = .7;
          wardInput.emptyProb = .15;
        }
        else if (block.use == DistrictUse::WealthyResidential || block.use == DistrictUse::Manor)
        {
          wardInput.minSq = (80 + 30 * r1 * r2) * unit; // Patriciate
          wardInput.gridChaos = .5 + .3 * r3;
          wardInput.sizeChaos = .8;
          wardInput.emptyProb = .2;
        }
        else if (block.use == DistrictUse::Civic)
        {
          wardInput.minSq = (80 + 30 * r1 * r2) * unit; // Administration
          wardInput.gridChaos = .1 + .3 * r3;
          wardInput.sizeChaos = .3;
          wardInput.emptyProb = .04;
        }
        else
        {
          wardInput.minSq = (10 + 80 * r1 * r2) * unit; // Craftsmen (and common housing)
          wardInput.gridChaos = .5 + .2 * r3;
          wardInput.sizeChaos = .6;
          wardInput.emptyProb = .04;
        }
        if (exterior)
        {
          // filterOutskirts: edges on a road are populated (1), edges shared
          // with another outer ward at .4; a gate vertex has density 1, a
          // vertex inside the built tiling (three wards meet) 2 * random,
          // one on the open edge 0.
          wardInput.outskirts = true;
          const auto &outline = block.boundary.outer;
          for (size_t k = 0; k < outline.size(); ++k)
          {
            const auto a = outline[k], b = outline[(k + 1) % outline.size()];
            const auto m = mix(a, b, .5);
            bool onRoad = false;
            for (const auto &f : fronts)
            {
              onRoad |= segmentDistance(m, f.a, f.b) < inset + 1.5;
            }
            if (onRoad)
            {
              wardInput.populated.push_back({a, b, 1});
              continue;
            }
            double factor = 0;
            for (const auto &other : r.blocks)
            {
              if (other.id == block.id)
              {
                continue;
              }
              for (size_t j = 0; j < other.boundary.outer.size(); ++j)
              {
                const auto c = other.boundary.outer[j], d = other.boundary.outer[(j + 1) % other.boundary.outer.size()];
                if (segmentDistance(m, c, d) < 2.6 && std::abs(cross(c, d, a)) / std::max(1e-9, dist(c, d)) < 2.6 && std::abs(cross(c, d, b)) / std::max(1e-9, dist(c, d)) < 2.6)
                {
                  factor = std::max(factor, exteriorBlocks.count(other.id) ? .4 : 1.);
                }
              }
            }
            if (factor > 0)
            {
              wardInput.populated.push_back({a, b, factor});
            }
          }
          for (size_t k = 0; k < outline.size(); ++k)
          {
            const auto v = outline[k];
            double density = 0;
            bool gateVertex = false;
            for (const auto &gate : gates)
            {
              gateVertex |= dist(gate.position, v) < 12;
            }
            // A vertex on the wall pavement touches only the city.
            bool onWall = false;
            for (size_t i = 0; i < wallRing.size(); ++i)
            {
              onWall |= segmentDistance(v, wallRing[i], wallRing[(i + 1) % wallRing.size()]) < s.wallSetback + 4;
            }
            unsigned sharing = 1;
            for (const auto &other : r.blocks)
            {
              if (other.id == block.id)
              {
                continue;
              }
              bool shares = false;
              for (auto q : other.boundary.outer)
              {
                shares |= dist(q, v) < 2.6;
              }
              sharing += shares;
            }
            density = gateVertex ? 1 : (sharing >= 3 || onWall) ? 2 * random(block.id * 131 + k, 104) : 0;
            wardInput.vertexDensity.push_back(density);
          }
        }
        generator::WardBuildingsPlan wardPlan;
        generator::Ledger wardLedger;
        std::string wardError;
        if (generator::WardBuildings::plan(wardInput, wardPlan, wardLedger, &wardError))
        {
          if (exterior && std::getenv("CITY_DEBUG"))
          {
            double densest = 0;
            for (double d : wardInput.vertexDensity)
            {
              densest = std::max(densest, d);
            }
            std::fprintf(
                stderr,
                "outskirts block %llu: %zu populated edges, densest vertex %.2f, %zu kept of %zu (%u thinned)\n",
                (unsigned long long)block.id,
                wardInput.populated.size(),
                densest,
                wardPlan.buildings.size(),
                wardPlan.buildings.size() + wardPlan.filtered,
                wardPlan.filtered);
          }
          for (auto &building : wardPlan.buildings)
          {
            block.lots.push_back(building);
            leaves.push_back({building});
          }
          for (auto &yard : wardPlan.open)
          {
            block.openLots.push_back(yard);
          }
        }
        else if (std::getenv("CITY_DEBUG"))
        {
          std::fprintf(stderr, "ward buildings: block %llu use %d: %s\n", (unsigned long long)block.id, int(block.use), wardError.c_str());
        }
        stripped = true;
        if (s.lotsOnly)
        {
          continue;
        }
      }
      const bool estate = block.use == DistrictUse::WealthyResidential || block.use == DistrictUse::Manor;
      const bool perimeter = !estate && (block.use == DistrictUse::Residential || block.use == DistrictUse::Merchant || block.use == DistrictUse::Craftsmen) && random(block.id, 85) < .7;
      if (estate)
      {
        Poly lot = block.boundary.outer;
        const Poly ring = lot;
        for (size_t k = 0; k < ring.size() && !lot.empty(); ++k)
        {
          const auto a = ring[k], b = ring[(k + 1) % ring.size()];
          const double nx = b.z - a.z, nz = a.x - b.x;
          lot = clip(lot, nx, nz, nx * a.x + nz * a.z - dist(a, b) * (inset + s.frontageGap));
        }
        if (lot.size() >= 3 && area(lot) > plotArea * .5)
        {
          block.lots.push_back(lot);
          leaves.push_back({lot});
          stripped = true;
        }
      }
      // Ring lots (generator::RingLotPlanner): a convex block is copied
      // inward by the lot depth, the band is cut into equal-area lots per
      // street edge, and the copy is the common court. Poor blocks keep no
      // court: a large one splits the block with an alley, a small one is
      // cut into interior lots. Villages keep their road rows.
      // Villages ring their blocks too: only the road edges are built (the
      // Strassendorf row), the rest of the band stays open, and no court is kept.
      // Outskirts on planning cells are rung like the core (Watabou's outer
      // patches); only villages and lattice outskirts are road rows.
      const bool rowLots = s.roadRowLots || (exterior && (s.outerWardCells.empty() || outerRow));
      const bool ringWanted = !stripped && (!exterior || !s.outerWardCells.empty()) && block.boundary.holes.empty();
      if (ringWanted)
      {
        // A nearly convex outline (hair-thin notches from the arrangement)
        // is its hull; a concave one is cut into convex parts at its reflex
        // vertices, each part rung on its own (lots on the cuts stay open).
        std::vector<Poly> ringParts;
        const Poly outlineHull = hull(block.boundary.outer);
        const bool nearly = block.boundary.outer.size() >= 3 && area(block.boundary.outer) >= area(outlineHull) * .97;
        std::function<void(const Poly &, int)> convexParts = [&](const Poly &p, int depth) {
          size_t reflex = p.size();
          for (size_t k = 0; k < p.size() && reflex == p.size(); ++k)
          {
            if (cross(p[(k + p.size() - 1) % p.size()], p[k], p[(k + 1) % p.size()]) < -1.5)
            {
              reflex = k;
            }
          }
          const Poly h = hull(p);
          if (reflex == p.size() || depth > 10)
          {
            if (h.size() >= 3 && area(h) > 1 && area(h) <= area(p) * 1.15 + 1)
            {
              ringParts.push_back(h);
            }
            return;
          }
          const auto u = p[(reflex + p.size() - 1) % p.size()], v = p[reflex];
          const double nx = v.z - u.z, nz = u.x - v.x, c = nx * v.x + nz * v.z;
          const Poly left = clip(p, nx, nz, c), right = clip(p, -nx, -nz, -c);
          if (left.size() < 3 || right.size() < 3 || area(left) < 1 || area(right) < 1)
          {
            if (h.size() >= 3 && area(h) <= area(p) * 1.15 + 1)
            {
              ringParts.push_back(h);
            }
            return;
          }
          convexParts(left, depth + 1);
          convexParts(right, depth + 1);
        };
        if (nearly)
        {
          ringParts.push_back(outlineHull);
        }
        else
        {
          convexParts(block.boundary.outer, 0);
        }
        size_t partIndex = 0;
        for (const Poly &hullRing : ringParts)
        {
          Poly ringOutline = hullRing;
          for (size_t k = 0; k < hullRing.size() && !ringOutline.empty(); ++k)
          {
            const auto a = hullRing[k], b = hullRing[(k + 1) % hullRing.size()];
            const double l = dist(a, b);
            if (l < 1e-6)
            {
              continue;
            }
            ringOutline = clip(ringOutline, (b.z - a.z) / l, (a.x - b.x) / l, ((b.z - a.z) * a.x + (a.x - b.x) * a.z) / l - inset);
          }
          if (ringOutline.size() < 3 || area(ringOutline) <= plotArea)
          {
            continue;
          }
          const bool poor = (block.use == DistrictUse::Suburb && !exterior) || block.use == DistrictUse::WorkerHousing;
          generator::RingLotInput ringInput;
          ringInput.block = ringOutline;
          ringInput.depth = block.use == DistrictUse::Suburb ? 9 : block.use == DistrictUse::WorkerHousing ? 10 : block.use == DistrictUse::Craftsmen ? 15 : block.use == DistrictUse::Merchant ? 16 : 13;
          // Lots stay about 1.6 times deeper than wide, whatever the district.
          ringInput.depth = std::max(5., std::min({ringInput.depth, plotArea / 4.5, std::sqrt(plotArea * 1.6)}));
          ringInput.targetArea = plotArea;
          ringInput.fillInterior = poor && !rowLots; // A village row is a row whatever its wealth.
          // Courts: the poor keep almost none, common blocks a modest one,
          // craftsmen and merchants the yards their trades need.
          // Interiors are used: common blocks split with an alley above 700 m2
          // of court and fill a court under two and a half plots with lots;
          // craftsmen and merchants keep their yards.
          const bool yards = block.use == DistrictUse::Craftsmen || block.use == DistrictUse::Merchant;
          ringInput.divideCourtAbove = rowLots ? 0 : poor ? plotArea * 3 : yards ? 2500 : 700;
          ringInput.fillCourtBelow = rowLots || yards ? 0 : plotArea * 2.5;
          ringInput.solidFallback = !rowLots; // A row that cannot be rung leaves its land; a town sliver is cut whole.
          ringInput.depth = rowLots ? std::max(ringInput.depth, 16.) : ringInput.depth;
          if (rowLots)
          {
            // An edge has a road when its midpoint lies within the clearance of a front.
            ringInput.fronted.assign(ringOutline.size(), false);
            for (size_t k = 0; k < ringOutline.size(); ++k)
            {
              const auto m = mix(ringOutline[k], ringOutline[(k + 1) % ringOutline.size()], .5);
              for (const auto &f : fronts)
              {
                ringInput.fronted[k] = ringInput.fronted[k] || segmentDistance(m, f.a, f.b) < inset + 1.5;
              }
            }
            if (std::none_of(ringInput.fronted.begin(), ringInput.fronted.end(), [](bool f) {
                  return f;
                }))
            {
              continue;
            }
          }
          ringInput.alleyWidth = std::max(2., s.frontageGap * 2);
          ringInput.key = block.id * 8 + partIndex++;
          ringInput.random = random;
          double longest = -1;
          for (size_t k = 0; k < ringOutline.size(); ++k)
          {
            const auto a = ringOutline[k], b = ringOutline[(k + 1) % ringOutline.size()];
            if (dist(a, b) > longest)
            {
              longest = dist(a, b);
              ringInput.axisRadians = std::atan2(b.z - a.z, b.x - a.x);
            }
          }
          // Inside the walls every edge is built: lots on an edge without a
          // street (the wall, a compound) open onto the court like the rest.
          generator::RingLotPlan ringPlan;
          generator::Ledger ringLedger;
          std::string ringError;
          if (generator::RingLotPlanner::plan(ringInput, ringPlan, ringLedger, &ringError))
          {
            for (auto &lot : ringPlan.lots)
            {
              block.lots.push_back(lot);
              leaves.push_back({lot});
            }
            // A village keeps neither garden strips on its field edges nor a court: the land stays land.
            for (auto &lot : ringPlan.open)
            {
              if (!rowLots)
              {
                block.openLots.push_back(lot);
              }
            }
            for (auto &court : ringPlan.courts)
            {
              if (!rowLots)
              {
                block.courtyards.push_back({court, {}});
              }
            }
            stripped = stripped || !ringPlan.lots.empty();
          }
          else if (std::getenv("CITY_DEBUG"))
          {
            std::fprintf(stderr, "ring lots: block %llu use %d part %zu area %.0f: %s\n", (unsigned long long)block.id, int(block.use), partIndex, area(ringOutline), ringError.c_str());
          }
        }
        if (std::getenv("CITY_DEBUG") && !stripped)
        {
          std::fprintf(
              stderr, "ring lots: block %llu use %d not rung: nearly %d, outline %zu vertices, %zu parts\n", (unsigned long long)block.id, int(block.use), int(nearly), block.boundary.outer.size(), ringParts.size());
        }
      }
      stripsWanted = perimeter && !stripped;
      double gridChaos = .5 + .2 * random(block.id, 84), sizeChaos = .6, emptyProbability = .04;
      if (block.use == DistrictUse::Suburb)
      {
        gridChaos = .6 + .4 * random(block.id, 84);
        sizeChaos = .8;
        emptyProbability = .03;
      }
      else if (block.use == DistrictUse::Merchant)
      {
        sizeChaos = .7;
        emptyProbability = .15;
      }
      else if (block.use == DistrictUse::WealthyResidential || block.use == DistrictUse::Manor)
      {
        gridChaos = .5 + .3 * random(block.id, 84);
        sizeChaos = .8;
        emptyProbability = .2;
      }
      // The orthogonal slicer takes the whole outline, concave or not, so
      // one axis pair serves the block; only a block with holes is sliced
      // piece by piece.
      // The orthogonal slicer takes the whole outline, concave or not (its
      // half-plane cuts split a concave piece into parts); only a block with
      // holes is sliced piece by piece, on the block's own axis pair.
      std::vector<Poly> sources;
      if (block.boundary.holes.empty())
      {
        sources.push_back(block.boundary.outer);
      }
      else
      {
        sources = pieces;
      }
      double blockAxis = 0, longestEdge = -1;
      for (size_t k = 0; k < block.boundary.outer.size(); ++k)
      {
        const auto a = block.boundary.outer[k], b = block.boundary.outer[(k + 1) % block.boundary.outer.size()];
        if (dist(a, b) > longestEdge)
        {
          longestEdge = dist(a, b);
          blockAxis = std::atan2(b.z - a.z, b.x - a.x);
        }
      }
      // A block outline from the arrangement carries hair-thin concavities;
      // the perimeter allocation accepts a nearly convex outline.
      auto nearlyConvex = [](const Poly &p) {
        for (size_t k = 0; k < p.size(); ++k)
        {
          if (cross(p[k], p[(k + 1) % p.size()], p[(k + 2) % p.size()]) < -.5)
          {
            return false;
          }
        }
        return p.size() >= 3;
      };
      // Perimeter lots need streets on most of the block edge; a block with
      // one short frontage would become one strip.
      double fronted = 0, ringLength = 0;
      for (const auto &f : fronts)
      {
        fronted += dist(f.a, f.b);
      }
      for (size_t k = 0; k < block.boundary.outer.size(); ++k)
      {
        ringLength += dist(block.boundary.outer[k], block.boundary.outer[(k + 1) % block.boundary.outer.size()]);
      }
      // Villages and the outskirts are rows of houses along the road (the
      // Strassendorf of the typology): strips whatever the frontage share.
      const bool roadRow = s.roadRowLots || exterior;
      stripsWanted = stripsWanted || roadRow;
      const bool canStrip = stripsWanted && block.boundary.holes.empty() && (roadRow || nearlyConvex(block.boundary.outer)) && !fronts.empty() && (roadRow || fronted >= ringLength * .5);
      stripsWanted = canStrip;
      const bool packedNow = !stripped && !canStrip;
      for (size_t k = 0; packedNow && k < sources.size(); ++k)
      {
        generator::LotPlanInput lotInput;
        lotInput.block = sources[k];
        // A convex outline is inset edge by edge; a concave one cannot be (a
        // half-plane per edge would cut far parts away), so it keeps the
        // street edge, and pieces are already inset by the street ribbons.
        lotInput.inset = sources.size() == 1 && convex(sources[k]) ? inset : 0;
        lotInput.axisGiven = true;
        lotInput.axisRadians = blockAxis;
        lotInput.minimumArea = plotArea;
        lotInput.gridChaos = gridChaos;
        lotInput.sizeChaos = sizeChaos;
        lotInput.emptyProbability = emptyProbability;
        lotInput.alleyWidth = std::max(.6, s.frontageGap);
        lotInput.key = block.id * 64 + k;
        lotInput.random = random;
        generator::LotPlan lotPlan;
        generator::Ledger lotLedger;
        std::string lotError;
        if (!generator::LotPlanner::plan(lotInput, lotPlan, lotLedger, &lotError))
        {
          continue;
        }
        if (lotPlan.lots.size() <= 1 && sources.size() == 1 && area(sources[k]) > plotArea * 3 && pieces.size() > 1)
        {
          // A concave outline the slicer could not cut cleanly: slice its
          // convex pieces on the block's axis pair instead.
          lotPlan = generator::LotPlan{};
          for (const auto &piece : pieces)
          {
            generator::LotPlanInput pieceInput = lotInput;
            pieceInput.block = piece;
            pieceInput.inset = 0;
            generator::LotPlan piecePlan;
            generator::Ledger pieceLedger;
            if (generator::LotPlanner::plan(pieceInput, piecePlan, pieceLedger, &lotError))
            {
              lotPlan.lots.insert(lotPlan.lots.end(), piecePlan.lots.begin(), piecePlan.lots.end());
              lotPlan.open.insert(lotPlan.open.end(), piecePlan.open.begin(), piecePlan.open.end());
            }
          }
        }
        if (std::getenv("CITY_DEBUG") && lotPlan.lots.size() <= 1 && area(sources[k]) > plotArea * 3)
        {
          std::fprintf(
              stderr,
              "lots: block %llu use %d source %zu/%zu verts %zu area %.0f pieces %zu convex %d -> lots %zu open %zu cuts %u\n",
              (unsigned long long)block.id,
              int(block.use),
              k,
              sources.size(),
              sources[k].size(),
              area(sources[k]),
              pieces.size(),
              int(convex(sources[k])),
              lotPlan.lots.size(),
              lotPlan.open.size(),
              lotPlan.cuts);
          for (const auto &entry : lotLedger.entries())
          {
            std::fprintf(stderr, "lots:   %s\n", entry.message.c_str());
          }
        }
        for (auto &lot : lotPlan.lots)
        {
          block.lots.push_back(lot);
          leaves.push_back({lot});
        }
        for (auto &lot : lotPlan.open)
        {
          block.openLots.push_back(lot);
        }
      }
      stripped = !leaves.empty();
      if (s.lotsOnly && !stripsWanted)
      {
        continue;
      }
    }
    if (!stripped && ((s.frontageStrips && convex(block.boundary.outer)) || stripsWanted) && block.boundary.holes.empty() && !fronts.empty())
    {
      // Frontage strips: every street edge is extruded into the block to a
      // lot depth set by the block's plot area, the strip is divided into
      // lots of a matching width, and neighbouring strips meet on the
      // corner bisectors (the first step of the straight skeleton). What no
      // strip reaches is the block's court. Deep, wide lots for the wealthy,
      // shallow narrow ones for the poor, so the block type sets the lots.
      const auto &ring = block.boundary.outer;
      double narrowest = 1e30;
      for (size_t k = 0; k < ring.size(); ++k)
      {
        const auto u = ring[k], v = ring[(k + 1) % ring.size()];
        const double l = dist(u, v);
        if (l < 1e-6)
        {
          continue;
        }
        const double ex = (v.z - u.z) / l, ez = (u.x - v.x) / l;
        double lo = 1e30, hi = -1e30;
        for (auto q : ring)
        {
          lo = std::min(lo, ex * q.x + ez * q.z);
          hi = std::max(hi, ex * q.x + ez * q.z);
        }
        narrowest = std::min(narrowest, hi - lo);
      }
      double lotDepth = std::clamp(std::sqrt(plotArea * 1.5), 7., 26.);
      lotDepth = std::min(lotDepth, std::max(6., narrowest * .5 - .5));
      const double lotWidth = std::clamp(plotArea / lotDepth, 5., 30.);
      std::vector<std::vector<Poly>> strips;
      // Band lots (the user's method): the ring is extruded inward as one
      // polygon, and the band between the ring and its inset is cut along
      // the street normal at lot intervals. A corner is one lot spanning
      // the last cut of one street and the first of the next, never a fan.
      // Band segments along edges without a street stay open.
      bool banded = false;
      auto ringNearlyConvex = [](const Poly &p) {
        for (size_t k = 0; k < p.size(); ++k)
        {
          if (cross(p[k], p[(k + 1) % p.size()], p[(k + 2) % p.size()]) < -.5)
          {
            return false;
          }
        }
        return p.size() >= 3;
      };
      if (ringNearlyConvex(ring))
      {
        Poly innerRing = ring;
        for (size_t k = 0; k < ring.size() && !innerRing.empty(); ++k)
        {
          const auto a = ring[k], b = ring[(k + 1) % ring.size()];
          const double nx = b.z - a.z, nz = a.x - b.x;
          innerRing = clip(innerRing, nx, nz, nx * a.x + nz * a.z - dist(a, b) * lotDepth);
        }
        if (innerRing.size() >= 3 && area(innerRing) > 1)
        {
          // Arc-length parametrisation of both rings.
          const size_t n = ring.size(), m = innerRing.size();
          std::vector<double> arc(n + 1, 0), innerArc(m + 1, 0);
          for (size_t k = 0; k < n; ++k)
          {
            arc[k + 1] = arc[k] + dist(ring[k], ring[(k + 1) % n]);
          }
          for (size_t k = 0; k < m; ++k)
          {
            innerArc[k + 1] = innerArc[k] + dist(innerRing[k], innerRing[(k + 1) % m]);
          }
          auto outerAt = [&](double u) {
            u = std::fmod(std::fmod(u, arc[n]) + arc[n], arc[n]);
            size_t k = 0;
            while (k + 1 < n && arc[k + 1] < u)
            {
              ++k;
            }
            const double l = arc[k + 1] - arc[k];
            return mix(ring[k], ring[(k + 1) % n], l > 1e-9 ? (u - arc[k]) / l : 0);
          };
          auto innerParam = [&](Position q) {
            double best = 1e30, param = 0;
            for (size_t k = 0; k < m; ++k)
            {
              const auto a = innerRing[k], b = innerRing[(k + 1) % m];
              const double l = dist(a, b);
              if (l < 1e-9)
              {
                continue;
              }
              const double t = std::clamp(((q.x - a.x) * (b.x - a.x) + (q.z - a.z) * (b.z - a.z)) / (l * l), 0., 1.);
              const double d = dist(q, mix(a, b, t));
              if (d < best)
              {
                best = d;
                param = innerArc[k] + l * t;
              }
            }
            return param;
          };
          auto innerAt = [&](double v) {
            v = std::fmod(std::fmod(v, innerArc[m]) + innerArc[m], innerArc[m]);
            size_t k = 0;
            while (k + 1 < m && innerArc[k + 1] < v)
            {
              ++k;
            }
            const double l = innerArc[k + 1] - innerArc[k];
            return mix(innerRing[k], innerRing[(k + 1) % m], l > 1e-9 ? (v - innerArc[k]) / l : 0);
          };
          // Foot of the street normal on the inner ring: the ray from the
          // outer point inward, first hit on an inner edge.
          auto footOf = [&](Position p, size_t edge) {
            const auto a = ring[edge], b = ring[(edge + 1) % n];
            const double l = dist(a, b), nx = -(b.z - a.z) / l, nz = (b.x - a.x) / l;
            double bestT = 1e30;
            Position hit = innerAt(innerParam(p));
            for (size_t k = 0; k < m; ++k)
            {
              const auto c = innerRing[k], d = innerRing[(k + 1) % m];
              const double ex = d.x - c.x, ez = d.z - c.z, den = nx * ez - nz * ex;
              if (std::abs(den) < 1e-9)
              {
                continue;
              }
              const double t = ((c.x - p.x) * ez - (c.z - p.z) * ex) / den, u = ((c.x - p.x) * nz - (c.z - p.z) * nx) / den;
              if (t > 1e-6 && u >= -1e-6 && u <= 1 + 1e-6 && t < bestT)
              {
                bestT = t;
                hit = {p.x + nx * t, 0, p.z + nz * t};
              }
            }
            return hit;
          };

          struct Cut
          {
            double u;
            Position outer, inner;
            bool opensAfter; // The band from this cut to the next lies on an edge without a street.
          };

          std::vector<Cut> cuts;
          for (size_t k = 0; k < n; ++k)
          {
            const auto a = ring[k], b = ring[(k + 1) % n];
            const double l = dist(a, b);
            if (l < 1e-6)
            {
              continue;
            }
            bool isFront = false;
            for (const auto &f : fronts)
            {
              const double fl = dist(f.a, f.b);
              if (fl < 1e-6)
              {
                continue;
              }
              const double parallel = std::abs((b.x - a.x) * (f.b.z - f.a.z) - (b.z - a.z) * (f.b.x - f.a.x)) / (l * fl);
              const double offset = std::abs(cross(f.a, f.b, a)) / fl;
              const double t0 = ((a.x - f.a.x) * (f.b.x - f.a.x) + (a.z - f.a.z) * (f.b.z - f.a.z)) / (fl * fl), t1 = ((b.x - f.a.x) * (f.b.x - f.a.x) + (b.z - f.a.z) * (f.b.z - f.a.z)) / (fl * fl);
              isFront |= parallel < 1e-3 && offset < .05 && std::max(t0, t1) > 1e-3 && std::min(t0, t1) < 1 - 1e-3;
            }
            if (!isFront)
            {
              // The whole edge is open band: cuts at both ends.
              cuts.push_back({arc[k], a, footOf(a, k), true});
              cuts.push_back({arc[k + 1] - 1e-6, b, footOf(b, k), false});
              continue;
            }
            const unsigned count = std::max(1u, unsigned(std::round(l / lotWidth)));
            for (unsigned j = 1; j < count; ++j)
            {
              const Position p = mix(a, b, double(j) / count);
              cuts.push_back({arc[k] + l * double(j) / count, p, footOf(p, k), false});
            }
            // A corner between two streets is absorbed by its neighbours,
            // unless the corner lot would be huge: then a cut at the corner.
            if (l > lotWidth * 1.5)
            {
              cuts.push_back({arc[k + 1] - 1e-6, b, footOf(b, k), false});
            }
          }
          std::sort(cuts.begin(), cuts.end(), [](const Cut &x, const Cut &y) {
            return x.u < y.u;
          });
          if (cuts.size() >= 2)
          {
            for (size_t c = 0; c < cuts.size(); ++c)
            {
              const Cut &c1 = cuts[c], &c2 = cuts[(c + 1) % cuts.size()];
              if (c1.opensAfter)
              {
                continue;
              }
              // Outer path c1 -> c2 (through ring corners), inner path back.
              Poly lot{c1.outer};
              const double u2 = c + 1 < cuts.size() ? c2.u : c2.u + arc[n];
              for (size_t k = 0; k < n; ++k)
              {
                for (double offset : {0., arc[n]})
                {
                  if (arc[k] + offset > c1.u + 1e-6 && arc[k] + offset < u2 - 1e-6)
                  {
                    lot.push_back(ring[k]);
                  }
                }
              }
              lot.push_back(c2.outer);
              lot.push_back(c2.inner);
              const double v1 = innerParam(c1.inner), v2raw = innerParam(c2.inner);
              double v2 = v2raw;
              if (v2 < v1)
              {
                v2 += innerArc[m];
              }
              // Inner corners strictly between v1 and v2, walked backwards.
              std::vector<double> innerStops;
              for (size_t k = 0; k < m; ++k)
              {
                for (double offset : {0., innerArc[m]})
                {
                  if (innerArc[k] + offset > v1 + 1e-6 && innerArc[k] + offset < v2 - 1e-6)
                  {
                    innerStops.push_back(innerArc[k] + offset);
                  }
                }
              }
              std::sort(innerStops.rbegin(), innerStops.rend());
              if (v2 - v1 < innerArc[m] * .5)
              {
                for (double v : innerStops)
                {
                  lot.push_back(innerAt(v));
                }
              }
              lot.push_back(c1.inner);
              // Collapse repeated points.
              Poly cleaned;
              for (auto p : lot)
              {
                if (cleaned.empty() || dist(cleaned.back(), p) > 1e-6)
                {
                  cleaned.push_back(p);
                }
              }
              while (cleaned.size() > 1 && dist(cleaned.front(), cleaned.back()) < 1e-6)
              {
                cleaned.pop_back();
              }
              if (cleaned.size() < 3 || area(cleaned) < plotArea * .15)
              {
                continue;
              }
              if (area(cleaned) < 0)
              {
                std::reverse(cleaned.begin(), cleaned.end());
              }
              std::vector<Poly> leaf;
              for (const auto &piece : pieces)
              {
                auto q = intersection(piece, cleaned);
                if (area(q) > 1e-6)
                {
                  leaf.push_back(q);
                }
              }
              if (!leaf.empty())
              {
                merge(leaf);
                strips.push_back(std::move(leaf));
              }
            }
            banded = !strips.empty();
          }
        }
      }
      for (const auto &f : fronts)
      {
        if (banded)
        {
          break;
        }
        // The ring corners of this front and their interior bisectors.
        size_t ia = ring.size(), ib = ring.size();
        for (size_t k = 0; k < ring.size(); ++k)
        {
          if (dist(ring[k], f.a) < 1e-6)
          {
            ia = k;
          }
          if (dist(ring[k], f.b) < 1e-6)
          {
            ib = k;
          }
        }
        if (ia == ring.size() || ib == ring.size())
        {
          continue;
        }
        const double length = dist(f.a, f.b);
        const double tx = (f.b.x - f.a.x) / length, tz = (f.b.z - f.a.z) / length;
        const double nx = -tz, nz = tx; // Inward for a CCW ring.
        auto bisectorClip = [&](Poly q, size_t corner, Position other) {
          const auto p = ring[corner], prev = ring[(corner + ring.size() - 1) % ring.size()], next = ring[(corner + 1) % ring.size()];
          const double l0 = dist(prev, p), l1 = dist(next, p);
          if (l0 < 1e-6 || l1 < 1e-6)
          {
            return q;
          }
          double dx = (prev.x - p.x) / l0 + (next.x - p.x) / l1, dz = (prev.z - p.z) / l0 + (next.z - p.z) / l1;
          const double dl = std::hypot(dx, dz);
          if (dl < 1e-6)
          {
            return q;
          }
          dx /= dl;
          dz /= dl;
          // Keep the side of the bisector line that holds the front's other end.
          double bx = -dz, bz = dx, c = bx * p.x + bz * p.z;
          const Position mid = mix(p, other, .5);
          if (bx * mid.x + bz * mid.z > c)
          {
            bx = -bx;
            bz = -bz;
            c = -c;
          }
          return clip(q, bx, bz, c);
        };
        const unsigned count = std::max(1u, unsigned(std::round(length / lotWidth)));
        for (unsigned k = 0; k < count; ++k)
        {
          const double u0 = length * k / count, u1 = length * (k + 1) / count;
          Poly lot = ring;
          lot = clip(lot, nx, nz, nx * f.a.x + nz * f.a.z + lotDepth); // depth
          lot = clip(lot, -tx, -tz, -(tx * f.a.x + tz * f.a.z + u0));  // after u0
          lot = clip(lot, tx, tz, tx * f.a.x + tz * f.a.z + u1);       // before u1
          lot = bisectorClip(lot, ia, f.b);
          lot = bisectorClip(lot, ib, f.a);
          // Separate this strip from every other front: the bisector of the
          // two support lines (their mid-line when parallel), keeping the
          // side of this front. Strips then tile the ring without overlap.
          const Position fm = mix(f.a, f.b, .5);
          for (const auto &g : fronts)
          {
            if (&g == &f || lot.size() < 3)
            {
              continue;
            }
            const double gl = dist(g.a, g.b);
            if (gl < 1e-6)
            {
              continue;
            }
            const double gx = (g.b.x - g.a.x) / gl, gz = (g.b.z - g.a.z) / gl;
            const Position gm = mix(g.a, g.b, .5);
            const double turn = tx * gz - tz * gx;
            double bx, bz, c;
            if (std::abs(turn) < 1e-3)
            {
              // Parallel: the mid-line between the two support lines.
              bx = -tz;
              bz = tx;
              c = bx * (fm.x + gm.x) * .5 + bz * (fm.z + gm.z) * .5;
            }
            else
            {
              // Intersection of the support lines and their bisector.
              const double det = tx * (-gz) - tz * (-gx);
              const double t = ((g.a.x - f.a.x) * (-gz) - (g.a.z - f.a.z) * (-gx)) / det;
              const Position P{f.a.x + tx * t, 0, f.a.z + tz * t};
              double dx = tx + gx, dz = tz + gz;
              if (std::hypot(dx, dz) < 1e-6)
              {
                dx = tx - gx;
                dz = tz - gz;
              }
              // Of the two bisectors, the one that puts the fronts on opposite sides.
              double d1x = dx, d1z = dz, d2x = -dz, d2z = dx;
              auto side = [&](double ux, double uz, Position q) {
                return -uz * (q.x - P.x) + ux * (q.z - P.z);
              };
              const bool firstSeparates = side(d1x, d1z, fm) * side(d1x, d1z, gm) < 0;
              const double ux = firstSeparates ? d1x : d2x, uz = firstSeparates ? d1z : d2z;
              bx = -uz;
              bz = ux;
              c = bx * P.x + bz * P.z;
            }
            if (bx * fm.x + bz * fm.z > c)
            {
              bx = -bx;
              bz = -bz;
              c = -c;
            }
            lot = clip(lot, bx, bz, c);
          }
          // A concave ring clipped by half-planes carries zero-width bridges
          // (repeated vertices): keep the part that holds the front.
          for (unsigned guard = 0; guard < 8; ++guard)
          {
            bool cut = false;
            for (size_t i = 0; i < lot.size() && !cut; ++i)
            {
              for (size_t j = i + 2; j < lot.size() && !cut; ++j)
              {
                if (dist(lot[i], lot[j]) < 1e-6 && !(i == 0 && j + 1 == lot.size()))
                {
                  Poly inner(lot.begin() + i, lot.begin() + j), outer(lot.begin(), lot.begin() + i);
                  outer.insert(outer.end(), lot.begin() + j, lot.end());
                  const Position probe{fm.x + nx * 1.5, 0, fm.z + nz * 1.5};
                  const bool innerHolds = inner.size() >= 3 && inside(inner, probe);
                  lot = innerHolds ? inner : outer;
                  cut = true;
                }
              }
            }
            if (!cut)
            {
              break;
            }
          }
          if (lot.size() < 3 || area(lot) < plotArea * .2)
          {
            continue;
          }
          std::vector<Poly> leaf;
          for (const auto &piece : pieces)
          {
            auto q = intersection(piece, lot);
            if (area(q) > 1e-6)
            {
              leaf.push_back(q);
            }
          }
          if (!leaf.empty())
          {
            merge(leaf);
            strips.push_back(std::move(leaf));
          }
        }
      }
      if (!strips.empty())
      {
        for (const auto &leaf : strips)
        {
          for (const auto &lot : leaf)
          {
            block.lots.push_back(lot);
          }
        }
        leaves = std::move(strips);
        stripped = true;
        // The court: the block beyond every front's lot depth.
        Poly court = ring;
        for (const auto &f : fronts)
        {
          const double length = dist(f.a, f.b), tx = (f.b.x - f.a.x) / length, tz = (f.b.z - f.a.z) / length;
          court = clip(court, tz, -tx, tz * f.a.x - tx * f.a.z - lotDepth);
        }
        if (court.size() >= 3 && area(court) > 4)
        {
          block.courtyards.push_back({court, {}});
        }
      }
    }
    if (!stripped)
    {
      partition(std::move(pieces), 0, block.id * 8192);
    }
    if (s.lotsOnly)
    {
      continue;
    }
    for (auto &leaf : leaves)
    {
      // A mostly convex parcel may use its dominant component. Keep the
      // exact remainder separately; a convex hull would cover excluded land.
      if (leaf.size() != 1)
      {
        auto dominant = std::max_element(leaf.begin(), leaf.end(), [](const Poly &a, const Poly &b) {
          return area(a) < area(b);
        });
        double total = 0;
        for (const auto &p : leaf)
        {
          total += area(p);
        }
        if (dominant == leaf.end() || area(*dominant) < total * .75)
        {
          for (auto &p : leaf)
          {
            block.unallocated.push_back({std::move(p), {}});
          }
          continue;
        }
        std::iter_swap(leaf.begin(), dominant);
        for (size_t i = 1; i < leaf.size(); ++i)
        {
          block.unallocated.push_back({std::move(leaf[i]), {}});
        }
        leaf.resize(1);
      }
      const auto &plot = leaf.front();
      const BlockFront *chosen = nullptr;
      Position entrance{}, access{};
      double frontageLength = 0;
      bool boundaryLot = false, alleyLot = false;
      for (const auto &f : fronts)
      {
        double len = dist(f.a, f.b), tx = (f.b.x - f.a.x) / len, tz = (f.b.z - f.a.z) / len, nx = -tz, nz = tx;
        double lo = 1e30, hi = -1e30;
        for (auto p : plot)
        {
          if (std::abs((p.x - f.a.x) * nx + (p.z - f.a.z) * nz - inset) < .005)
          {
            double t = (p.x - f.a.x) * tx + (p.z - f.a.z) * tz;
            lo = std::min(lo, t);
            hi = std::max(hi, t);
          }
        }
        lo = std::max(0., lo);
        hi = std::min(len, hi);
        boundaryLot |= hi - lo > 1e-5;
        if (hi - lo < 2 || hi - lo <= frontageLength)
        {
          continue;
        }
        auto origin = mix(f.a, f.b, (lo + hi) * .5 / len);
        auto e = Position{origin.x + nx * (inset + s.frontageGap * .5), 0, origin.z + nz * (inset + s.frontageGap * .5)};
        if (!inside(plot, e))
        {
          continue;
        }
        Position roadPoint{};
        double best = 1e30;
        for (const auto &street : r.streets)
        {
          if (street.id == f.street)
          {
            for (size_t j = 1; j < street.path.size(); ++j)
            {
              auto q = closest(origin, street.path[j - 1], street.path[j]);
              if (dist(q, origin) < best)
              {
                best = dist(q, origin);
                roadPoint = q;
              }
            }
          }
        }
        if (!coveredSegment(e, roadPoint, usable))
        {
          continue;
        }
        bool blocked = false;
        auto passage = ribbon({0, e, roadPoint, .25, false});
        for (const auto &wall : r.walls)
        {
          blocked |= cityPolygonsOverlap(passage, ribbon(wall));
        }
        for (const auto &tower : r.towers)
        {
          blocked |= cityPolygonsOverlap(passage, tower.footprint);
        }
        for (const auto &o : r.objects)
        {
          blocked |= cityPolygonsOverlap(passage, o.clearance);
        }
        if (blocked)
        {
          continue;
        }
        chosen = &f;
        entrance = e;
        access = roadPoint;
        frontageLength = hi - lo;
      }
      if (!chosen && s.alleyAccess && area(plot) >= plotArea * .45 && !fronts.empty())
      {
        // Watabou's wards are built through: an interior parcel is served by
        // the alley gaps between roofs, its access the nearest point of the
        // block's nearest street. No passage clearance is asserted for it.
        Position centroid{};
        for (auto p : plot)
        {
          centroid.x += p.x / plot.size();
          centroid.z += p.z / plot.size();
        }
        double best = 1e30;
        for (const auto &f : fronts)
        {
          auto q = closest(centroid, f.a, f.b);
          if (dist(q, centroid) < best)
          {
            best = dist(q, centroid);
            chosen = &f;
          }
        }
        const double len = dist(chosen->a, chosen->b), tx = (chosen->b.x - chosen->a.x) / len, tz = (chosen->b.z - chosen->a.z) / len;
        double lo = 1e30, hi = -1e30;
        for (auto p : plot)
        {
          double t = (p.x - chosen->a.x) * tx + (p.z - chosen->a.z) * tz;
          lo = std::min(lo, t);
          hi = std::max(hi, t);
        }
        frontageLength = std::max(2.5, hi - lo);
        entrance = centroid;
        double nearest = 1e30;
        for (auto p : plot)
        {
          auto q = closest(p, chosen->a, chosen->b);
          if (dist(q, p) < nearest)
          {
            nearest = dist(q, p);
            entrance = mix(p, centroid, .15);
          }
        }
        // The attachment must lie on the accepted street's centreline.
        bool attached = false;
        for (const auto &street : r.streets)
        {
          if (street.id == chosen->street)
          {
            for (size_t j = 1; j < street.path.size(); ++j)
            {
              auto q = closest(entrance, street.path[j - 1], street.path[j]);
              if (!attached || dist(q, entrance) < dist(access, entrance))
              {
                access = q;
              }
              attached = true;
            }
          }
        }
        if (!attached || dist(entrance, access) > s.alleyReachMetres)
        {
          chosen = nullptr;
        }
        else
        {
          alleyLot = chosen != nullptr;
        }
      }
      if (!chosen)
      {
        (boundaryLot ? block.unallocated : block.courtyards).push_back({plot, {}});
        continue;
      }
      // Shrink the allocated parcel rather than guessing an independent box.
      // This preserves trapezoidal corners and coherent rows around the block.
      Poly roof = plot;
      for (size_t k = 0; k < plot.size(); ++k)
      {
        auto a = plot[k], b = plot[(k + 1) % plot.size()];
        double nx = b.z - a.z, nz = a.x - b.x;
        roof = clip(roof, nx, nz, nx * a.x + nz * a.z - dist(a, b) * s.frontageGap * .5);
      }
      if (s.regularBuildingShells && alleyLot)
      {
        // An alley-served parcel gets the largest rectangle aligned with one
        // of its edges (Watabou keeps the lot polygon; a rectangle reads as a
        // house). Too small a rectangle leaves the parcel as a court.
        Poly bestRoof;
        double bestArea = 0;
        Position centre{};
        for (auto p : roof)
        {
          centre.x += p.x / roof.size();
          centre.z += p.z / roof.size();
        }
        for (size_t k = 0; k < roof.size(); ++k)
        {
          const auto ea = roof[k], eb = roof[(k + 1) % roof.size()];
          const double el = dist(ea, eb);
          if (el < 1e-6)
          {
            continue;
          }
          const double tx = (eb.x - ea.x) / el, tz = (eb.z - ea.z) / el;
          double u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;
          for (auto p : roof)
          {
            const double u = (p.x - centre.x) * tx + (p.z - centre.z) * tz, v = -(p.x - centre.x) * tz + (p.z - centre.z) * tx;
            u0 = std::min(u0, u);
            u1 = std::max(u1, u);
            v0 = std::min(v0, v);
            v1 = std::max(v1, v);
          }
          auto at = [&](double u, double v) {
            return Position{centre.x + tx * u - tz * v, 0, centre.z + tz * u + tx * v};
          };
          // Shrink the bounding box about the centre until its corners fit.
          double lo = 0, hi = 1;
          for (unsigned step = 0; step < 24; ++step)
          {
            const double f = (lo + hi) * .5;
            const bool fits = inside(roof, at(u0 * f, v0 * f)) && inside(roof, at(u1 * f, v0 * f)) && inside(roof, at(u1 * f, v1 * f)) && inside(roof, at(u0 * f, v1 * f));
            (fits ? lo : hi) = f;
          }
          const double width = (u1 - u0) * lo, depth = (v1 - v0) * lo;
          if (width >= 3.5 && depth >= 3.5 && width * depth > bestArea)
          {
            bestArea = width * depth;
            bestRoof = {at(u0 * lo, v0 * lo), at(u1 * lo, v0 * lo), at(u1 * lo, v1 * lo), at(u0 * lo, v1 * lo)};
          }
        }
        if (bestRoof.empty() || bestArea < 20)
        {
          block.courtyards.push_back({plot, {}});
          continue;
        }
        roof = std::move(bestRoof);
      }
      if (s.regularBuildingShells && !alleyLot)
      {
        // Fit a usable orthogonal shell to the actual street frontage. The
        // irregular plot survives as ownership/yard; never roof its acute tips.
        // An alley-served parcel keeps its inset polygon (Watabou's lots).
        const double length = dist(chosen->a, chosen->b);
        const double tx = (chosen->b.x - chosen->a.x) / length, tz = (chosen->b.z - chosen->a.z) / length;
        auto at = [&](double x, double z) {
          return Position{entrance.x + tx * x - tz * z, 0, entrance.z + tz * x + tx * z};
        };
        Poly bestRoof;
        double bestArea = 0;
        const double widthLimit = std::min(s.maximumWidth, frontageLength - s.frontageGap);
        for (unsigned attempt = 0; attempt < 10; ++attempt)
        {
          const double width = widthLimit * (1 - attempt * .07);
          if (width < 3.5)
          {
            continue;
          }
          if (!inside(roof, at(-width * .5, 0)) || !inside(roof, at(width * .5, 0)))
          {
            continue;
          }
          double lo = 0, hi = std::min({s.maximumDepth, width * 3., area(plot) * (1 - gardenFraction) / width});
          for (unsigned k = 0; k < 28; ++k)
          {
            double depth = (lo + hi) * .5;
            if (inside(roof, at(-width * .5, depth)) && inside(roof, at(width * .5, depth)))
            {
              lo = depth;
            }
            else
            {
              hi = depth;
            }
          }
          if (lo < 3.5 || width > lo * 3 || width * lo <= bestArea)
          {
            continue;
          }
          bestArea = width * lo;
          bestRoof = {at(-width * .5, 0), at(width * .5, 0), at(width * .5, lo), at(-width * .5, lo)};
        }
        if (bestRoof.empty())
        {
          block.unallocated.push_back({plot, {}});
          continue;
        }
        roof = std::move(bestRoof);
      }
      // Clipping can leave sub-centimetre corner edges. Collapse those
      // conservatively inside the convex plot before float mesh packing;
      // otherwise their vertical wall normals depend on rounding noise.
      bool simplified = true;
      while (simplified && roof.size() > 3)
      {
        simplified = false;
        for (size_t k = 0; k < roof.size(); ++k)
        {
          if (dist(roof[k], roof[(k + 1) % roof.size()]) < .01)
          {
            roof.erase(roof.begin() + k);
            simplified = true;
            break;
          }
        }
      }
      double perimeter = 0;
      for (size_t k = 0; k < roof.size(); ++k)
      {
        perimeter += dist(roof[k], roof[(k + 1) % roof.size()]);
      }
      Poly roomCore = roof;
      for (size_t k = 0; k < roof.size(); ++k)
      {
        auto a = roof[k], b = roof[(k + 1) % roof.size()];
        double nx = b.z - a.z, nz = a.x - b.x;
        roomCore = clip(roomCore, nx, nz, nx * a.x + nz * a.z - dist(a, b) * 1.5);
      }
      if (roof.size() < 3 || roomCore.empty() || area(roof) < s.minimumWidth * s.minimumDepth * .6 || 2 * area(roof) < perimeter * 1.5)
      {
        block.unallocated.push_back({plot, {}});
        continue;
      }
      bool blocksEntrance = false;
      for (const auto &placed : r.objects)
      {
        blocksEntrance |= cityPolygonsOverlap(plot, ribbon({0, placed.entrance, placed.access, .25, false}));
      }
      if (blocksEntrance)
      {
        block.unallocated.push_back({plot, {}});
        continue;
      }
      double low = 1e30, high = -1e30;
      for (auto p : roof)
      {
        double h = terrain(p.x, p.z);
        if (!std::isfinite(h))
        {
          return fail("nonfinite parcel terrain");
        }
        low = std::min(low, h);
        high = std::max(high, h);
      }
      if (high - low > s.maximumFoundationRelief)
      {
        block.unallocated.push_back({plot, {}});
        continue;
      }
      if (r.objects.size() >= s.maximumObjects)
      {
        return fail("city object budget exceeded");
      }
      CityObject object;
      object.id = objectID++;
      object.block = block.id;
      object.outsideWalls = exterior;
      // Allocation ownership is metadata only; physical blocks control fits.
      Position center{};
      for (size_t k = 0; k < roof.size(); ++k)
      {
        auto a = roof[k], b = roof[(k + 1) % roof.size()];
        double weight = a.x * b.z - a.z * b.x;
        center.x += (a.x + b.x) * weight / (6 * area(roof));
        center.z += (a.z + b.z) * weight / (6 * area(roof));
      }
      for (const auto &ward : r.wards)
      {
        for (const auto &p : ward.pieces)
        {
          if (inside(p, center))
          {
            object.ward = ward.id;
          }
        }
      }
      object.street = chosen->street;
      object.frontage = frontageID++;
      object.kind = CityObjectKind::House;
      object.buildingType = object.requestedBuildingType = block.use == DistrictUse::Craftsmen                                                 ? CityBuildingType::WorkshopYard
                                                           : (block.use == DistrictUse::Manor || block.use == DistrictUse::WealthyResidential) ? CityBuildingType::MerchantHouse
                                                           : block.use == DistrictUse::WorkerHousing                                           ? CityBuildingType::WorkerLodging
                                                           : block.use == DistrictUse::Suburb                                                  ? CityBuildingType::Cottage
                                                           : area(plot) > plotArea * 1.25                                                      ? CityBuildingType::MerchantHouse
                                                                                                                                               : CityBuildingType::ShopHouse;
      if (block.use == DistrictUse::Craftsmen)
      {
        object.kind = CityObjectKind::Workshop;
      }
      object.footprint = plot;
      object.clearance = plot;
      double height = exterior ? 4 + 2 * std::floor(random(object.id, 83) * 2) : 5 + 2 * std::floor(random(object.id, 83) * 3);
      object.parts.push_back({roof, height});
      if (s.regularBuildingShells && roof.size() == 4)
      {
        const double width = dist(roof[0], roof[1]), depth = dist(roof[1], roof[2]);
        double variety = random(object.id, 94);
        if (exterior && block.use == DistrictUse::Suburb)
        {
          object.buildingType = object.requestedBuildingType = variety < .55 ? CityBuildingType::Cottage : variety < .8 ? CityBuildingType::ShopHouse : CityBuildingType::WorkshopYard;
          object.kind = object.buildingType == CityBuildingType::WorkshopYard ? CityObjectKind::Workshop : CityObjectKind::House;
        }
        // Larger programmes use a front range plus one or two rear wings.
        // Each wing is rectangular and the remaining court stays unroofed.
        if (width >= 10 && depth >= 10 && (variety > .65 || (exterior && plotArea >= 200) || block.use == DistrictUse::Manor || block.use == DistrictUse::WealthyResidential || block.use == DistrictUse::Craftsmen))
        {
          auto at = [&](double x, double z) {
            return Position{roof[0].x + (roof[1].x - roof[0].x) * x / width + (roof[3].x - roof[0].x) * z / depth, 0, roof[0].z + (roof[1].z - roof[0].z) * x / width + (roof[3].z - roof[0].z) * z / depth};
          };
          auto rectangle = [&](double x0, double z0, double x1, double z1) {
            return Poly{at(x0, z0), at(x1, z0), at(x1, z1), at(x0, z1)};
          };
          const double front = std::min(5., depth * .4), wing = std::min(4., width * .28);
          bool courtyard = width >= 13 && variety > .86;
          object.parts = {{rectangle(0, 0, width, front), height}, {rectangle(0, front, wing, depth), std::max(4., height - 2)}};
          if (courtyard)
          {
            object.parts.push_back({rectangle(width - wing, front, width, depth), std::max(4., height - 2)});
          }
          object.courtyard = rectangle(wing, front, courtyard ? width - wing : width, depth);
          object.buildingType = object.requestedBuildingType = block.use == DistrictUse::WealthyResidential || block.use == DistrictUse::Manor ? CityBuildingType::MerchantHouse
                                                               : block.use == DistrictUse::WorkerHousing                                       ? CityBuildingType::WorkerLodging
                                                               : block.use == DistrictUse::Craftsmen || exterior                               ? CityBuildingType::WorkshopYard
                                                               : courtyard                                                                     ? CityBuildingType::Inn
                                                                                                                                               : CityBuildingType::MerchantHouse;
          if (object.buildingType == CityBuildingType::WorkshopYard)
          {
            object.kind = CityObjectKind::Workshop;
          }
        }
      }
      center = {};
      double shellArea = 0;
      for (const auto &part : object.parts)
      {
        shellArea += area(part.footprint);
        for (size_t k = 0; k < part.footprint.size(); ++k)
        {
          auto a = part.footprint[k], b = part.footprint[(k + 1) % part.footprint.size()];
          double weight = a.x * b.z - a.z * b.x;
          center.x += (a.x + b.x) * weight / 6;
          center.z += (a.z + b.z) * weight / 6;
        }
      }
      center.x /= shellArea;
      center.z /= shellArea;
      for (const auto &ward : r.wards)
      {
        for (const auto &piece : ward.pieces)
        {
          if (inside(piece, center))
          {
            object.ward = ward.id;
          }
        }
      }
      object.height = object.parts.front().height;
      object.foundation = high;
      if (alleyLot)
      {
        // The planner needs the entrance on the shell: the roof edge point
        // nearest the alley access.
        double nearest = 1e30;
        for (size_t k = 0; k < roof.size(); ++k)
        {
          auto a = roof[k], b = roof[(k + 1) % roof.size()];
          double l = dist(a, b), t = l > 1e-9 ? std::clamp(((access.x - a.x) * (b.x - a.x) + (access.z - a.z) * (b.z - a.z)) / (l * l), 0., 1.) : 0;
          if (l > 3)
          {
            t = std::clamp(t, 1.2 / l, 1 - 1.2 / l); // The door needs wall on both sides.
          }
          auto q = mix(a, b, t);
          if (dist(q, access) < nearest)
          {
            nearest = dist(q, access);
            entrance = q;
          }
        }
        ++r.alleyServed;
      }
      object.entrance = entrance;
      object.access = access;
      for (const auto &street : r.streets)
      {
        if (street.id == object.street)
        {
          int side = 1;
          double nearest = 1e30;
          for (size_t k = 1; k < street.path.size(); ++k)
          {
            double distance = segmentDistance(access, street.path[k - 1], street.path[k]);
            if (distance < nearest)
            {
              nearest = distance;
              side = cross(street.path[k - 1], street.path[k], entrance) >= 0 ? 1 : -1;
            }
          }
          r.frontages.push_back({object.frontage, object.ward, street.a, street.b, object.street, side, street.path});
        }
      }
      r.objects.push_back(std::move(object));
    }
  }
  return true;
}

bool FrontageHousingStrategy::allocate(LayoutBuild &build, std::string *error)
{
  // The body below was written against these names on the shared stack.
  // Binding them here rather than rewriting three hundred lines of references
  // keeps this a move and not a rewrite.
  const CitySettings &s = build.settings;
  CityLayout &r = build.layout;
  const auto &cells = build.cells;
  const auto &roads = build.roads;
  const auto &wallMasks = build.wallMasks;
  const uint64_t firstOuterWard = build.firstOuterWard;
  const auto &random = build.random;
  const auto &usable = build.usable;
  const auto &terrain = build.terrain;
  const auto &reserved = build.reserved;
  const auto &wallRing = build.wallRing;
  const auto &gates = build.gates;
  const double minX = build.minX;
  const double minZ = build.minZ;
  const double maxX = build.maxX;
  const double maxZ = build.maxZ;

  const auto towerClearance = [&](const CityWallTower &tower) {
    return citylayout::towerClearance(tower, s.wallSetback);
  };

  const auto fail = [&](const char *m) {
    if (error != nullptr)
    {
      *error = m;
    }

    return false;
  };

  (void)cells;
  (void)roads;
  (void)wallMasks;
  (void)firstOuterWard;
  (void)random;
  (void)usable;
  (void)towerClearance;
  (void)terrain;
  (void)reserved;
  (void)wallRing;
  (void)gates;
  (void)minX;
  (void)minZ;
  (void)maxX;
  (void)maxZ;

  // Arc-length frontage walks, with bounded smaller variants and explicit failed gaps.
  const double binSize = s.maximumWidth + s.maximumDepth + 2 * s.overhang + 1;
  std::map<std::pair<int, int>, std::vector<size_t>> occupiedBins;
  auto bins = [&](const Poly &p) {
    double x0 = 1e30, x1 = -1e30, z0 = 1e30, z1 = -1e30;
    for (auto v : p)
    {
      x0 = std::min(x0, v.x);
      x1 = std::max(x1, v.x);
      z0 = std::min(z0, v.z);
      z1 = std::max(z1, v.z);
    }
    std::vector<std::pair<int, int>> keys;
    for (int x = int(std::floor(x0 / binSize)); x <= int(std::floor(x1 / binSize)); ++x)
    {
      for (int z = int(std::floor(z0 / binSize)); z <= int(std::floor(z1 / binSize)); ++z)
      {
        keys.push_back({x, z});
      }
    }
    return keys;
  };
  uint64_t oid = 1, fid = 1;
  std::map<std::tuple<uint64_t, uint64_t, int>, uint64_t> frontageIds;
  // Walk retained street paths, not the stale straight Voronoi chords or the
  // convex decomposition seams. Sampling is by arc length across path vertices.
  for (const auto &street : r.streets)
  {
    std::vector<double> lengths{0};
    for (size_t i = 1; i < street.path.size(); ++i)
    {
      lengths.push_back(lengths.back() + dist(street.path[i - 1], street.path[i]));
    }
    const double length = lengths.back();
    auto sample = [&](double along) {
      along = std::clamp(along, 0., length);
      size_t i = std::min(size_t(std::upper_bound(lengths.begin(), lengths.end(), along) - lengths.begin()), lengths.size() - 1);
      i = std::max(size_t(1), i);
      return mix(street.path[i - 1], street.path[i], (along - lengths[i - 1]) / (lengths[i] - lengths[i - 1]));
    };
    for (int side : {1, -1})
    {
      for (double cursor = 3; cursor + s.minimumWidth < length - 3;)
      {
        if (r.objects.size() >= s.maximumObjects)
        {
          return fail("city object budget exceeded");
        }
        const uint64_t id = oid++;
        const double choice = random(id, 7) * (s.houseWeight + s.workshopWeight + s.gardenWeight);
        const auto kind = choice < s.houseWeight ? CityObjectKind::House : choice < s.houseWeight + s.workshopWeight ? CityObjectKind::Workshop : CityObjectKind::Garden;
        const double variant = random(id, 40);
        CityBuildingType type = CityBuildingType::Garden;
        if (kind == CityObjectKind::Workshop)
        {
          type = variant < .7 ? CityBuildingType::WorkshopYard : CityBuildingType::Warehouse;
        }
        else if (kind == CityObjectKind::House)
        {
          type = variant < (street.arterial ? .18 : .55) ? CityBuildingType::Cottage : variant < .72 ? CityBuildingType::ShopHouse : variant < .91 ? CityBuildingType::MerchantHouse : CityBuildingType::Inn;
        }
        auto dimensions = [&](CityBuildingType program) {
          double w0 = 0, w1 = .22, d0 = 0, d1 = .25;
          if (program == CityBuildingType::ShopHouse)
          {
            w1 = .25;
            d0 = .15;
            d1 = .6;
          }
          if (program == CityBuildingType::MerchantHouse)
          {
            w0 = .4;
            w1 = .8;
            d0 = .4;
            d1 = .8;
          }
          if (program == CityBuildingType::Inn)
          {
            w0 = .6;
            w1 = 1;
            d0 = .5;
            d1 = 1;
          }
          if (program == CityBuildingType::WorkshopYard)
          {
            w0 = .2;
            w1 = .65;
            d0 = .35;
            d1 = .85;
          }
          if (program == CityBuildingType::Warehouse)
          {
            w0 = .35;
            w1 = .8;
            d0 = .55;
            d1 = 1;
          }
          return std::make_pair(s.minimumWidth + (w0 + random(id, 5) * (w1 - w0)) * (s.maximumWidth - s.minimumWidth), s.minimumDepth + (d0 + random(id, 6) * (d1 - d0)) * (s.maximumDepth - s.minimumDepth));
        };
        double advance = s.minimumWidth;
        CityObject obj;
        obj.id = id;
        obj.street = street.id;
        obj.requestedBuildingType = type;
        obj.kind = CityObjectKind::RejectedGap;
        obj.entrance = sample(cursor);
        for (int attempt = 0; attempt < 6; ++attempt)
        {
          // Large compounds yield to a compact frontage building if the plot
          // cannot fit them. Publish that substitution instead of a tiny "inn".
          const auto program = attempt < 3 || kind == CityObjectKind::Garden ? type : kind == CityObjectKind::House ? CityBuildingType::Cottage : CityBuildingType::WorkshopYard;
          auto [width, depth] = dimensions(attempt < 3 ? program : CityBuildingType::Cottage);
          const double ww = std::max(s.minimumWidth, width * (1 - .08 * (attempt % 3))), dd = std::max(s.minimumDepth, depth * (1 - .08 * (attempt % 3)));
          if (cursor + ww > length - 3)
          {
            continue;
          }
          const auto access = sample(cursor + ww * .5);
          const auto a = sample(cursor), b = sample(cursor + ww);
          const double span = dist(a, b);
          if (span < 1e-6)
          {
            continue;
          }
          const double tx = (b.x - a.x) / span * side, tz = (b.z - a.z) / span * side;
          const double nx = -tz, nz = tx;
          auto rectangle = [&](double extra) {
            auto at = [&](double t, double n) {
              return Position{access.x + tx * t + nx * n, 0, access.z + tz * t + nz * n};
            };
            const double offset = street.halfWidth + s.setback;
            return Poly{at(-ww * .5 - extra, offset - extra), at(ww * .5 + extra, offset - extra), at(ww * .5 + extra, offset + dd + extra), at(-ww * .5 - extra, offset + dd + extra)};
          };
          auto clearance = rectangle(s.overhang);
          // Sum the clipped areas: a building may cross an artificial convex
          // decomposition seam, but must fit completely inside one actual ward.
          uint64_t ward = 0;
          for (const auto &w : r.wards)
          {
            double covered = 0;
            for (const auto &piece : w.pieces)
            {
              covered += area(intersection(clearance, piece));
            }
            if (std::abs(covered - area(clearance)) < 1e-6)
            {
              ward = w.id;
              break;
            }
          }
          bool ok = ward != 0;
          for (const auto &q : roads)
          {
            if (cityPolygonsOverlap(clearance, q))
            {
              ok = false;
            }
          }
          for (const auto &wall : r.walls)
          {
            auto expanded = wall;
            expanded.halfWidth += s.wallSetback;
            expanded.a = mix(wall.a, wall.b, -s.wallSetback / dist(wall.a, wall.b));
            expanded.b = mix(wall.a, wall.b, 1 + s.wallSetback / dist(wall.a, wall.b));
            if (cityPolygonsOverlap(clearance, ribbon(expanded)))
            {
              ok = false;
            }
          }
          for (const auto &tower : r.towers)
          {
            if (cityPolygonsOverlap(clearance, towerClearance(tower)))
            {
              ok = false;
            }
          }
          std::set<size_t> candidates;
          for (auto key : bins(clearance))
          {
            auto it = occupiedBins.find(key);
            if (it != occupiedBins.end())
            {
              candidates.insert(it->second.begin(), it->second.end());
            }
          }
          for (auto index : candidates)
          {
            if (cityPolygonsOverlap(clearance, r.objects[index].clearance))
            {
              ok = false;
            }
          }
          const auto footprint = rectangle(0);
          const auto entrance = mix(footprint[0], footprint[1], .5);
          if (!coveredSegment(entrance, access, usable))
          {
            ok = false;
          }
          // Preserve both new and already published entrance paths.
          const auto passage = ribbon({0, entrance, access, .25, false});
          double passageArea = 0;
          for (const auto &piece : usable)
          {
            passageArea += area(intersection(passage, piece));
          }
          if (std::abs(passageArea - area(passage)) > 1e-6)
          {
            ok = false;
          }
          for (const auto &wall : r.walls)
          {
            if (cityPolygonsOverlap(passage, ribbon(wall)))
            {
              ok = false;
            }
          }
          for (const auto &tower : r.towers)
          {
            if (cityPolygonsOverlap(passage, tower.footprint))
            {
              ok = false;
            }
          }
          for (const auto &placed : r.objects)
          {
            if (placed.kind != CityObjectKind::RejectedGap && (cityPolygonsOverlap(passage, placed.clearance) || cityPolygonsOverlap(clearance, ribbon({0, placed.entrance, placed.access, .25, false}))))
            {
              ok = false;
            }
          }
          double low = 1e30, high = -1e30;
          for (auto q : clearance)
          {
            const double y = terrain(q.x, q.z);
            if (!std::isfinite(y))
            {
              return fail("nonfinite city terrain");
            }
            low = std::min(low, y);
            high = std::max(high, y);
          }
          if (high - low > s.maximumFoundationRelief)
          {
            ok = false;
          }
          if (!ok)
          {
            continue;
          }
          auto [it, inserted] = frontageIds.emplace(std::make_tuple(ward, street.id, side), fid);
          if (inserted)
          {
            r.frontages.push_back({fid++, ward, street.a, street.b});
            r.frontages.back().street = street.id;
            r.frontages.back().side = side;
            r.frontages.back().path = street.path;
          }
          obj.frontage = it->second;
          obj.ward = ward;
          obj.footprint = footprint;
          obj.clearance = clearance;
          obj.foundation = high;
          obj.entrance = entrance;
          obj.kind = kind;
          obj.buildingType = program;
          obj.compactFallback = attempt >= 3;
          obj.height = kind == CityObjectKind::Garden           ? 0
                       : program == CityBuildingType::Cottage   ? 3 + 2 * std::floor(random(id, 8) * 2)
                       : program == CityBuildingType::Warehouse ? 5
                                                                : 5 + 2 * std::floor(random(id, 8) * 2);
          // Non-overlapping wings share constructed axes. Clear plot setbacks
          // and roof overhangs were checked before choosing the massing.
          auto at = [&](double u, double v) {
            return Position{footprint[0].x + tx * u + nx * v, 0, footprint[0].z + tz * u + nz * v};
          };
          auto rect = [&](double u, double v, double w, double d) {
            return Poly{at(u, v), at(u + w, v), at(u + w, v + d), at(u, v + d)};
          };
          auto part = [&](double u, double v, double w, double d, double height) {
            obj.parts.push_back({rect(u, v, w, d), height});
          };
          if (kind != CityObjectKind::Garden)
          {
            if ((program == CityBuildingType::Inn || (program == CityBuildingType::MerchantHouse && variant > .82)) && ww >= 12 && dd >= 13)
            {
              const double wing = std::min(ww * .28, 5.), rear = std::min(dd * .32, 6.);
              part(0, dd - rear, ww, rear, obj.height);
              part(0, 0, wing, dd - rear, obj.height - 1);
              part(ww - wing, 0, wing, dd - rear, obj.height - 1);
              obj.courtyard = rect(wing, 0, ww - 2 * wing, dd - rear);
              // The open front gives this court a direct path to the street.
              obj.entrance = at(ww * .5, dd - rear);
            }
            else if (program == CityBuildingType::WorkshopYard && dd >= 12 && ww >= 8)
            {
              const double front = dd * .35, rear = dd * .22;
              part(0, 0, ww, front, obj.height);
              part(0, dd - rear, ww * .55, rear, 3);
              obj.courtyard = rect(0, front, ww, dd - front - rear);
            }
            else if (program == CityBuildingType::MerchantHouse || (program != CityBuildingType::Warehouse && random(id, 41) < .4))
            {
              const double front = dd * .48, wing = ww * .46;
              part(0, 0, ww, front, obj.height);
              part(0, front, wing, dd - front, std::max(3., obj.height - 2));
              obj.courtyard = rect(wing, front, ww - wing, dd - front);
            }
            else
            {
              part(0, 0, ww, dd, obj.height);
            }
          }
          obj.access = access;
          advance = ww;
          break;
        }
        if (obj.kind == CityObjectKind::RejectedGap)
        {
          ++r.rejectedAreas;
        }
        else
        {
          for (auto key : bins(obj.clearance))
          {
            occupiedBins[key].push_back(r.objects.size());
          }
        }
        r.objects.push_back(obj);
        cursor += advance + s.frontageGap;
      }
    }
  }

  return true;
}

} // namespace procedural::world::citylayout
