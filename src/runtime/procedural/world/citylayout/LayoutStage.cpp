#include "procedural/world/citylayout/LayoutStage.hpp"

#include "procedural/world/generator/WardPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace procedural::world::citylayout {

using namespace ::procedural::world::rewrite;

bool planOuterWards(const OuterWardInput &in, const OuterWardState &state, std::string *error)
{
  const CitySettings &s = in.settings;
  CityLayout &r = in.layout;
  const auto &patch = in.patch;
  const auto &domain = in.domain;
  const auto &wallRing = in.wallRing;
  const auto &reserved = in.reserved;
  const auto &arteries = in.arteries;
  const auto &gates = in.gates;
  auto &registry = in.registry;
  const auto &nextStreetId = in.nextStreetId;
  const auto &random = in.random;
  const auto &terrain = in.terrain;

  auto &usable = state.usable;
  auto &cells = state.cells;
  auto &firstOuterWard = state.firstOuterWard;
  auto &minX = state.minX;
  auto &minZ = state.minZ;
  auto &maxX = state.maxX;
  auto &maxZ = state.maxZ;

  const auto fail = [&](const char *m) {
    if (error != nullptr)
    {
      *error = m;
    }

    return false;
  };

  (void)domain;
  (void)wallRing;
  (void)reserved;
  (void)arteries;
  (void)gates;
  (void)registry;
  (void)nextStreetId;
  (void)random;
  (void)terrain;

  // Fill the envelope holes for this exclusion: a lake/courtyard inside the
  // enclosure must never be reclassified as exterior developable land.
  auto enclosureDomain = patch;
  enclosureDomain.holes.clear();
  PatchMesh enclosureMesh;
  if (!compilePatchMesh(
          enclosureDomain,
          registry,
          {},
          [](double, double) {
            return 0.;
          },
          enclosureMesh,
          error))
  {
    return false;
  }
  std::vector<Poly> enclosure;
  for (const auto &t : enclosureMesh.triangles)
  {
    Poly p;
    for (auto index : t)
    {
      p.push_back(enclosureMesh.vertices[index].position);
    }
    if (area(p) < 0)
    {
      std::reverse(p.begin(), p.end());
    }
    enclosure.push_back(std::move(p));
  }
  merge(enclosure);
  auto remove = [&](std::vector<Poly> pieces, const std::vector<Poly> &masks) {
    for (const auto &mask : masks)
    {
      std::vector<Poly> next;
      for (const auto &p : pieces)
      {
        auto rest = intersection(p, mask).empty() ? std::vector<Poly>{p} : subtract(p, mask);
        next.insert(next.end(), rest.begin(), rest.end());
      }
      pieces = std::move(next);
    }
    return pieces;
  };
  // Stable gate order keeps IDs and frontage subdivision independent of input order.
  auto orderedGates = gates;
  std::sort(orderedGates.begin(), orderedGates.end(), [](const auto &a, const auto &b) {
    return a.id < b.id;
  });
  uint64_t wardID = 0;
  for (const auto &ward : r.wards)
  {
    wardID = std::max(wardID, ward.id);
  }
  std::vector<CityStreet> approaches;
  for (const auto &gate : orderedGates)
  {
    const auto &artery = *std::find_if(arteries.begin(), arteries.end(), [&](const auto &road) {
      return road.id == gate.externalRoad;
    });
    double length = dist(artery.a, artery.b);
    Position direction{(artery.b.x - artery.a.x) / length, 0, (artery.b.z - artery.a.z) / length};
    auto along = [&](double t) {
      return Position{gate.position.x + direction.x * t, 0, gate.position.z + direction.z * t};
    };
    auto enclosed = [&](Position p) {
      return std::any_of(enclosure.begin(), enclosure.end(), [&](const auto &q) {
        return inside(q, p);
      });
    };
    bool forward = enclosed(along(.1)), backward = enclosed(along(-.1));
    if (forward == backward)
    {
      continue; // Tangential arrival cannot seed an exterior road.
    }
    if (forward)
    {
      direction.x = -direction.x;
      direction.z = -direction.z;
    }
    // Clip the outward road to continuous supplied land. Do not jump a water,
    // terrain or reservation gap, even when buildable land exists beyond it.
    auto available = remove(s.suburbLand, reserved);
    double reach = 0;
    for (double candidate = 1; candidate <= s.suburbRoadLength; candidate += 1)
    {
      auto band = ribbon({0, gate.position, along(candidate), artery.halfWidth, false});
      auto exteriorRoad = remove({band}, enclosure);
      if (!remove(exteriorRoad, available).empty())
      {
        break;
      }
      reach = candidate;
    }
    if (std::getenv("CITY_DEBUG"))
    {
      std::fprintf(stderr, "suburb gate %llu reach=%.1f (needs %.1f) available=%zu\n", (unsigned long long)gate.id, reach, s.wallSetback + 18, available.size());
    }
    if (reach < s.wallSetback + 18)
    {
      continue;
    }
    auto end = along(reach);
    approaches.push_back({nextStreetId(), gate.position, end, artery.halfWidth, false, {gate.position, end}, 0});
  }
  if (!approaches.empty())
  {
    std::vector<Poly> available;
    for (const auto &mask : s.suburbLand)
    {
      auto unique = remove({mask}, available);
      available.insert(available.end(), unique.begin(), unique.end());
    }
    available = remove(std::move(available), reserved);
    available = remove(std::move(available), enclosure);
    const double wallBand = s.suburbCellSpacing * 1.8;
    double x0 = minX - wallBand, x1 = maxX + wallBand, z0 = minZ - wallBand, z1 = maxZ + wallBand;
    for (const auto &road : approaches)
    {
      for (auto p : {road.a, road.b})
      {
        x0 = std::min(x0, p.x - s.suburbGrowthRadius);
        x1 = std::max(x1, p.x + s.suburbGrowthRadius);
        z0 = std::min(z0, p.z - s.suburbGrowthRadius);
        z1 = std::max(z1, p.z + s.suburbGrowthRadius);
      }
    }
    auto wallDistance = [&](Position p) {
      double nearest = 1e30;
      for (size_t k = 0; k < domain.outer.size(); ++k)
      {
        nearest = std::min(nearest, segmentDistance(p, domain.outer[k], domain.outer[(k + 1) % domain.outer.size()]));
      }
      return nearest;
    };
    // Watabou's outskirts grow along the roads, never along the wall: each
    // gate road sends side roads out at a third and two thirds of its
    // length, clear of the wall band, clipped to continuous supplied land.
    {
      std::vector<CityStreet> branches;
      for (const auto &road : approaches)
      {
        double len = dist(road.a, road.b);
        if (len < 1e-6)
        {
          continue;
        }
        Position dir{(road.b.x - road.a.x) / len, 0, (road.b.z - road.a.z) / len}, side{-dir.z, 0, dir.x};
        for (double t : {.38, .72})
        {
          if (t * len < wallBand + 16)
          {
            continue;
          }
          Position origin = mix(road.a, road.b, t);
          for (int sign : {-1, 1})
          {
            if (random(road.id * 7 + uint64_t(t * 100) * 3 + uint64_t(sign + 1), 94) < .25)
            {
              continue;
            }
            double reach = 0;
            for (double candidate = 1; candidate <= s.suburbGrowthRadius; candidate += 1)
            {
              Position end{origin.x + side.x * sign * candidate, 0, origin.z + side.z * sign * candidate};
              if (wallDistance(end) < wallBand)
              {
                break;
              }
              auto band = ribbon({0, origin, end, 1.6, false, {}, 0});
              if (!remove({band}, available).empty())
              {
                break;
              }
              reach = candidate;
            }
            if (reach < s.suburbCellSpacing)
            {
              continue;
            }
            Position end{origin.x + side.x * sign * reach, 0, origin.z + side.z * sign * reach};
            branches.push_back({nextStreetId(), origin, end, 1.6, false, {origin, end}, 0});
            x0 = std::min(x0, end.x - s.suburbGrowthRadius);
            x1 = std::max(x1, end.x + s.suburbGrowthRadius);
            z0 = std::min(z0, end.z - s.suburbGrowthRadius);
            z1 = std::max(z1, end.z + s.suburbGrowthRadius);
          }
        }
      }
      approaches.insert(approaches.end(), branches.begin(), branches.end());
    }
    auto roadDistance = [&](Position p) {
      double nearest = 1e30;
      for (const auto &road : approaches)
      {
        nearest = std::min(nearest, segmentDistance(p, road.a, road.b));
      }
      return nearest;
    };
    // A jittered site lattice yields a bounded, deterministic tessellation
    // across the outer planning domain; settlement selection happens afterward.
    std::vector<Position> outerSites;
    uint64_t key = 0;
    for (double z = z0; z < z1 && s.outerWardCells.empty(); z += s.suburbCellSpacing)
    {
      for (double x = x0; x < x1; x += s.suburbCellSpacing)
      {
        ++key;
        Position p{x + s.suburbCellSpacing * (.2 + .6 * random(key, 91)), 0, z + s.suburbCellSpacing * (.2 + .6 * random(key, 92))};
        if (roadDistance(p) > s.suburbGrowthRadius + s.suburbCellSpacing && wallDistance(p) > s.suburbCellSpacing * 1.8)
        {
          continue;
        }
        outerSites.push_back(p);
      }
    }
    if (outerSites.size() > 512)
    {
      return fail("outer Voronoi site budget exceeded");
    }

    struct OuterCell
    {
      Poly boundary;
      Position site;
      std::vector<Poly> pieces;
      bool selected = false;
    };

    std::vector<OuterCell> outerCells;
    // Given outer cells (the settlement's planning cells beyond the wall)
    // are the outer wards; otherwise a jittered lattice tessellates the land.
    std::vector<Poly> outerFaces;
    for (const auto &cell : s.outerWardCells)
    {
      if (cell.size() >= 3 && area(cell) > 20)
      {
        outerFaces.push_back(area(cell) < 0 ? Poly(cell.rbegin(), cell.rend()) : cell);
      }
    }
    for (size_t i = 0; i < outerSites.size(); ++i)
    {
      Poly face{{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}};
      auto a = outerSites[i];
      for (size_t j = 0; j < outerSites.size() && !face.empty(); ++j)
      {
        if (i != j)
        {
          auto b = outerSites[j];
          face = clip(face, b.x - a.x, b.z - a.z, (b.x * b.x + b.z * b.z - a.x * a.x - a.z * a.z) * .5);
        }
      }
      outerFaces.push_back(face);
    }
    for (const auto &face : outerFaces)
    {
      const bool given = !s.outerWardCells.empty();
      Position a = generator::vertexMean(face);
      OuterCell cell;
      cell.boundary = face;
      cell.site = a;
      if (given || roadDistance(a) <= s.suburbGrowthRadius || wallDistance(a) <= s.suburbCellSpacing * 1.4)
      {
        for (const auto &mask : available)
        {
          auto p = intersection(face, mask);
          if (area(p) > 1e-6)
          {
            cell.pieces.push_back(std::move(p));
          }
        }
      }
      // Nothing is built within the defensive clearance of the wall: the
      // band around the envelope (its edges pushed out by the setback plus
      // one lot depth) is taken off every outer cell.
      if (s.walls && !s.outerWardCells.empty() && wallRing.size() >= 3)
      {
        // Given outer cells: every chord the piece reaches pushes it out by
        // the setback plus one lot depth (the ring is not convex, so only
        // the chords in reach apply, each as a half-plane on the piece).
        // Watabou's outer patches touch the wall: only the setback is kept clear.
        const double orientation = area(wallRing) < 0 ? -1 : 1, push = s.wallSetback + 1;
        std::vector<Poly> kept;
        for (auto piece : cell.pieces)
        {
          for (size_t i = 0; i < wallRing.size() && !piece.empty(); ++i)
          {
            const auto a = wallRing[i], b = wallRing[(i + 1) % wallRing.size()];
            const double l = dist(a, b);
            if (l < 1e-6)
            {
              continue;
            }
            const double nx = (b.z - a.z) / l * orientation, nz = (a.x - b.x) / l * orientation;
            bool reaches = false;
            for (auto v : piece)
            {
              const double t = ((v.x - a.x) * (b.x - a.x) + (v.z - a.z) * (b.z - a.z)) / (l * l);
              const double outward = nx * (v.x - a.x) + nz * (v.z - a.z);
              reaches |= t > -.05 && t < 1.05 && outward > -1 && outward < push + 2;
            }
            if (reaches)
            {
              piece = clip(piece, -nx, -nz, -(nx * a.x + nz * a.z + push));
            }
          }
          if (piece.size() >= 3 && area(piece) > 20)
          {
            kept.push_back(piece);
          }
        }
        cell.pieces = std::move(kept);
      }
      else if (s.walls && !s.wardCells.empty() && convex(domain.outer))
      {
        Poly band = domain.outer;
        const double push = s.wallSetback + 12;
        Poly pushed;
        for (size_t k = 0; k < band.size(); ++k)
        {
          const auto prev = band[(k + band.size() - 1) % band.size()], p = band[k], next = band[(k + 1) % band.size()];
          const double la = dist(prev, p), lb = dist(p, next);
          Position n0{(p.z - prev.z) / la, 0, -(p.x - prev.x) / la}, n1{(next.z - p.z) / lb, 0, -(next.x - p.x) / lb};
          const double denominator = 1 + n0.x * n1.x + n0.z * n1.z;
          const double miter = push / std::max(.4, denominator);
          pushed.push_back({p.x + (n0.x + n1.x) * miter, 0, p.z + (n0.z + n1.z) * miter});
        }
        if (pushed.size() >= 3 && convex(pushed))
        {
          cell.pieces = remove(cell.pieces, {pushed});
        }
      }
      merge(cell.pieces);
      outerCells.push_back(std::move(cell));
    }
    // Return actual covered intervals of a line, never bridge an excluded gap.
    auto intervals = [&](Position a, Position b, const std::vector<Poly> &land) {
      std::vector<std::pair<double, double>> result;
      for (const auto &p : land)
      {
        double lo = 0, hi = 1;
        for (size_t k = 0; k < p.size(); ++k)
        {
          double da = cross(p[k], p[(k + 1) % p.size()], a), db = cross(p[k], p[(k + 1) % p.size()], b);
          if (da < -1e-7 && db < -1e-7)
          {
            hi = -1;
            break;
          }
          if (da < -1e-7)
          {
            lo = std::max(lo, da / (da - db));
          }
          if (db < -1e-7)
          {
            hi = std::min(hi, da / (da - db));
          }
        }
        if (hi - lo > 1e-6)
        {
          result.emplace_back(lo, hi);
        }
      }
      std::sort(result.begin(), result.end());
      std::vector<std::pair<double, double>> joined;
      for (auto span : result)
      {
        if (!joined.empty() && span.first <= joined.back().second + 1e-7)
        {
          joined.back().second = std::max(joined.back().second, span.second);
        }
        else
        {
          joined.push_back(span);
        }
      }
      return joined;
    };
    std::vector<OuterCell> connectedCells;
    for (const auto &cell : outerCells)
    {
      std::vector<bool> visited(cell.pieces.size());
      for (size_t start = 0; start < cell.pieces.size(); ++start)
      {
        if (!visited[start])
        {
          OuterCell component;
          component.boundary = cell.boundary;
          component.site = cell.site;
          std::vector<size_t> queue{start};
          visited[start] = true;
          for (size_t q = 0; q < queue.size(); ++q)
          {
            const auto &piece = cell.pieces[queue[q]];
            component.pieces.push_back(piece);
            for (size_t j = 0; j < cell.pieces.size(); ++j)
            {
              if (!visited[j])
              {
                bool joins = false;
                for (size_t k = 0; k < piece.size(); ++k)
                {
                  for (auto [lo, hi] : intervals(piece[k], piece[(k + 1) % piece.size()], {cell.pieces[j]}))
                  {
                    joins |= (hi - lo) * dist(piece[k], piece[(k + 1) % piece.size()]) >= 4;
                  }
                }
                if (joins)
                {
                  visited[j] = true;
                  queue.push_back(j);
                }
              }
            }
          }
          connectedCells.push_back(std::move(component));
        }
      }
    }
    outerCells = std::move(connectedCells);
    // Outer wards are the cells the roads cross, nearest gates first; nothing
    // grows along the wall and no ward edge becomes a lane (Watabou's
    // outskirts: houses front the gate roads and their side roads only).
    std::vector<std::pair<double, size_t>> crossed;
    for (size_t i = 0; i < outerCells.size(); ++i)
    {
      if (outerCells[i].pieces.empty())
      {
        continue;
      }
      bool onRoad = false;
      for (const auto &road : approaches)
      {
        onRoad |= !intervals(road.a, road.b, outerCells[i].pieces).empty();
      }
      if (!onRoad)
      {
        continue;
      }
      double nearest = 1e30;
      for (const auto &gate : orderedGates)
      {
        nearest = std::min(nearest, dist(gate.position, outerCells[i].site));
      }
      crossed.emplace_back(nearest, i);
    }
    std::sort(crossed.begin(), crossed.end());
    size_t selected = 0;
    for (auto [distance, i] : crossed)
    {
      (void)distance;
      if (selected >= s.maximumSuburbWards)
      {
        break;
      }
      outerCells[i].selected = true;
      ++selected;
    }
    // Given outer cells are all wards (the outskirts are patches like the
    // core, their shared edges lanes), whether or not a gate road crosses them.
    if (!s.outerWardCells.empty())
    {
      for (auto &cell : outerCells)
      {
        cell.selected = !cell.pieces.empty();
      }
    }
    if (std::getenv("CITY_DEBUG"))
    {
      size_t withPieces = 0, chosen = 0;
      for (const auto &cell : outerCells)
      {
        withPieces += !cell.pieces.empty();
        chosen += cell.selected;
      }
      std::fprintf(
          stderr,
          "outer wards: %zu given cells, %zu faces, %zu cells, %zu with land, %zu selected, %zu available masks, %zu approaches\n",
          s.outerWardCells.size(),
          outerFaces.size(),
          outerCells.size(),
          withPieces,
          chosen,
          available.size(),
          approaches.size());
    }
    firstOuterWard = wardID + 1;
    for (auto &cell : outerCells)
    {
      if (cell.selected)
      {
        r.wards.push_back({++wardID, cell.pieces});
        r.suburbs.insert(r.suburbs.end(), cell.pieces.begin(), cell.pieces.end());
      }
    }
    if (r.suburbs.size() > 512)
    {
      return fail("outer ward piece budget exceeded");
    }
    r.streets.insert(r.streets.end(), approaches.begin(), approaches.end());
    // Outer patches share no lanes (Watabou: only the roads leave the gates);
    // their buildings thin out with the distance from those roads.
    usable.insert(usable.end(), r.suburbs.begin(), r.suburbs.end());
    for (const auto &p : r.suburbs)
    {
      for (auto v : p)
      {
        minX = std::min(minX, v.x);
        maxX = std::max(maxX, v.x);
        minZ = std::min(minZ, v.z);
        maxZ = std::max(maxZ, v.z);
      }
    }
  }

  return true;
}

bool publishLayoutGround(const GroundPublishInput &in, GroundPublishResult &result, std::string *error)
{
  const CitySettings &s = in.settings;
  CityLayout &r = in.layout;
  const auto &usable = in.usable;
  const auto &wallMasks = in.wallMasks;
  const auto &terrain = in.terrain;
  const size_t interiorPieces = in.interiorPieces;
  (void)interiorPieces;

  const auto fail = [&](const char *m) {
    if (error != nullptr)
    {
      *error = m;
    }

    return false;
  };

  std::vector<Poly> roads;
  uint64_t cid = 1;
  // Ward cells are planning ownership, not visible ground seams. Publish the
  // usable ground once; actual streets define the surface blocks downstream.
  for (size_t ground = 0; ground < usable.size(); ++ground)
  {
    const auto &p = usable[ground];
    SurfaceContour c;
    c.id = cid++;
    c.material = ground < interiorPieces ? 11 : 16; // Distinct exterior ownership across the wall.
    c.loop = p;
    r.contours.push_back(c);
  }
  auto publishRoad = [&](const Poly &band, double height, bool join = false) {
    std::vector<Poly> covered;
    double coveredArea = 0;
    for (const auto &p : usable)
    {
      auto q = intersection(p, band);
      if (!q.empty())
      {
        coveredArea += area(q);
        covered.push_back(std::move(q));
      }
    }
    // Usable pieces have disjoint interiors. Exact area coverage proves that a
    // band can cross their planning seams without publishing invisible cuts.
    if (std::abs(coveredArea - area(band)) < 1e-7)
    {
      covered = {band};
    }
    for (auto &q : covered)
    {
      bool alreadyCovered = std::any_of(roads.begin(), roads.end(), [&](const Poly &existing) {
        return std::all_of(q.begin(), q.end(), [&](Position v) {
          return inside(existing, v);
        });
      });
      if (join && !alreadyCovered)
      {
        // A shoulder wedge can straddle two existing ribbons while remaining
        // completely covered by their union. Do not emit those hidden seams.
        std::vector<Poly> uncovered{q};
        for (const auto &existing : roads)
        {
          std::vector<Poly> next;
          for (const auto &piece : uncovered)
          {
            if (!cityPolygonsOverlap(piece, existing))
            {
              next.push_back(piece);
            }
            else
            {
              auto rest = subtract(piece, existing);
              next.insert(next.end(), rest.begin(), rest.end());
            }
          }
          uncovered = std::move(next);
          if (uncovered.empty())
          {
            break;
          }
        }
        alreadyCovered = uncovered.empty();
      }
      SurfaceContour c;
      c.id = cid++;
      c.material = 1;
      c.role = SurfaceRole::Road;
      c.terrainSurface = false;
      c.plane.constant = height;
      if (!std::isfinite(height))
      {
        return false;
      }
      for (size_t k = 0; k < q.size(); ++k)
      {
        auto u = q[k], v = q[(k + 1) % q.size()];
        int steps = std::max(1, int(std::ceil(dist(u, v))));
        for (int j = 0; j <= steps; ++j)
        {
          auto sample = mix(u, v, double(j) / steps);
          double y = terrain(sample.x, sample.z);
          if (!std::isfinite(y) || std::abs(y - height) > 1e-5)
          {
            return false;
          }
        }
      }
      // Covered join wedges add no street surface. Publishing them creates
      // microscopic overlapping edges that can pinch the downstream arrangement.
      if (alreadyCovered)
      {
        continue;
      }
      roads.push_back(q);
      c.loop = q;
      r.contours.push_back(c);
    }
    return true;
  };

  struct Ray
  {
    Position direction;
    double width, length;
  };

  std::map<std::pair<int64_t, int64_t>, std::pair<Position, std::vector<Ray>>> junctions;
  for (auto &street : r.streets)
  {
    double height = terrain(street.a.x, street.a.z);
    for (size_t i = 1; i < street.path.size(); ++i)
    {
      auto a = street.path[i - 1], b = street.path[i];
      double length = dist(a, b);
      if (length < 1e-6)
      {
        continue;
      }
      if (!publishRoad(ribbon({street.id, a, b, street.halfWidth, street.arterial}), height))
      {
        return fail("city streets require an engineered level platform");
      }
      for (auto pair : std::vector<std::pair<Position, Position>>{{a, b}, {b, a}})
      {
        auto p = pair.first, q = pair.second;
        auto &node = junctions[{std::llround(p.x * 1e6), std::llround(p.z * 1e6)}];
        node.first = p;
        node.second.push_back({{(q.x - p.x) / length, 0, (q.z - p.z) / length}, street.halfWidth, length});
      }
    }
  }
  for (auto &[key, node] : junctions)
  {
    auto p = node.first;
    auto rays = node.second;
    std::sort(rays.begin(), rays.end(), [](const Ray &a, const Ray &b) {
      return std::atan2(a.direction.z, a.direction.x) < std::atan2(b.direction.z, b.direction.x);
    });
    for (size_t i = 0; i < rays.size(); ++i)
    {
      auto a = rays[i], b = rays[(i + 1) % rays.size()];
      double den = a.direction.x * b.direction.z - a.direction.z * b.direction.x, dot = a.direction.x * b.direction.x + a.direction.z * b.direction.z, angle = std::atan2(den, dot);
      if (angle <= 0)
      {
        angle += 2 * 3.141592653589793;
      }
      if (angle > 3.141592653589793 + 1e-5 && angle < 6.27)
      {
        Poly wedge{p};
        double start = std::atan2(a.direction.x, -a.direction.z);
        for (int k = 0; k <= 4; ++k)
        {
          double t = k / 4., alpha = start + (angle - 3.141592653589793) * t, width = a.width * (1 - t) + b.width * t;
          wedge.push_back({p.x + width * std::cos(alpha), 0, p.z + width * std::sin(alpha)});
        }
        if (area(wedge) > 1e-7 && !publishRoad(wedge, terrain(p.x, p.z), true))
        {
          return fail("curved shoulder requires engineered platform");
        }
        continue;
      }
      if (angle < .25 || angle > 3.10)
      {
        continue;
      }
      auto offsetIntersection = [&](double radius) {
        Position q{p.x - a.direction.z * (a.width + radius), 0, p.z + a.direction.x * (a.width + radius)}, r{p.x + b.direction.z * (b.width + radius), 0, p.z - b.direction.x * (b.width + radius)};
        double t = ((r.x - q.x) * b.direction.z - (r.z - q.z) * b.direction.x) / den;
        return Position{q.x + a.direction.x * t, 0, q.z + a.direction.z * t};
      };
      auto corner = offsetIntersection(0);
      double alongA = (corner.x - p.x) * a.direction.x + (corner.z - p.z) * a.direction.z, alongB = (corner.x - p.x) * b.direction.x + (corner.z - p.z) * b.direction.z;
      double radius = std::min(s.junctionRadius, std::max(0., std::min(a.length * .8 - alongA, b.length * .8 - alongB) * std::tan(angle * .5)));
      if (radius < .05)
      {
        continue;
      }
      auto center = offsetIntersection(radius);
      double start = std::atan2(-a.direction.x, a.direction.z);
      Position last{center.x + radius * std::cos(start), 0, center.z + radius * std::sin(start)};
      for (int j = 1; j <= 4; ++j)
      {
        double alpha = start + (angle - 3.141592653589793) * j / 4;
        Position next{center.x + radius * std::cos(alpha), 0, center.z + radius * std::sin(alpha)};
        Poly triangle{corner, last, next};
        if (area(triangle) < 0)
        {
          std::reverse(triangle.begin(), triangle.end());
        }
        if (area(triangle) > 1e-7 && !publishRoad(triangle, terrain(p.x, p.z), true))
        {
          return fail("junction requires an engineered platform");
        }
        last = next;
      }
    }
  }
  // The wall clearance is pavement: no block forms between the wards and the wall.
  for (const auto &mask : wallMasks)
  {
    if (!publishRoad(mask, terrain(mask[0].x, mask[0].z)))
    {
      return fail("wall clearance requires an engineered platform");
    }
  }

  result.roads = std::move(roads);

  return true;
}

} // namespace procedural::world::citylayout
