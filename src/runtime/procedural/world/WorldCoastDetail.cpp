#include "WorldCoastDetail.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace procedural::world::rewrite {
bool detailLandCoasts(const LandProduct &input, const CoastDetailSettings &settings, LandProduct &output, std::string *error)
{
  auto fail = [&](const char *s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (!input.spec.validate(error))
  {
    return false;
  }
  if (input.cells.empty() || input.boundaries.records().empty())
  {
    return fail("Empty land product");
  }
  if (!std::isfinite(settings.amplitude) || settings.amplitude < 0 || settings.amplitude >= 1 || settings.subdivisions < 2 || settings.subdivisions > 64)
  {
    return fail("Invalid coast detail budget");
  }

  struct Incident
  {
    uint32_t cell;
    Position centre;
  };

  std::map<uint64_t, std::vector<Incident>> incidents;
  for (uint32_t c = 0; c < input.cells.size(); ++c)
  {
    const auto &cell = input.cells[c];
    std::vector<Position> ring;
    Position centre{};
    if (!cell.patch.holes.empty())
    {
      return fail("Coast detail requires convex planning cells");
    }
    for (auto use : cell.patch.outer)
    {
      auto e = input.boundaries.find(use.edgeId);
      if (!e || e->vertices.size() != 2)
      {
        return fail("Coast detail requires unsplit planning edges");
      }
      ring.push_back((use.reversed ? e->vertices.back() : e->vertices.front()).position);
      centre.x += ring.back().x;
      centre.z += ring.back().z;
    }
    if (ring.size() < 3)
    {
      return fail("Invalid planning ring");
    }
    centre.x /= ring.size();
    centre.z /= ring.size();
    for (size_t j = 0; j < ring.size(); ++j)
    {
      auto a = ring[j], b = ring[(j + 1) % ring.size()], d = ring[(j + 2) % ring.size()];
      if ((b.x - a.x) * (d.z - b.z) - (b.z - a.z) * (d.x - b.x) < -1e-8)
      {
        return fail("Coast detail requires convex planning cells");
      }
    }
    for (auto use : cell.patch.outer)
    {
      incidents[use.edgeId].push_back({c, centre});
    }
  }
  uint64_t nextEdge = 0, nextVertex = 0;
  for (auto &e : input.boundaries.records())
  {
    if (e.vertices.size() != 2)
    {
      return fail("Coast detail requires unsplit planning edges");
    }
    nextEdge = std::max(nextEdge, e.id);
    for (auto v : e.vertices)
    {
      if (v.owner != 0)
      {
        return fail("Coast detail must precede boundary sampling");
      }
      nextVertex = std::max(nextVertex, v.index);
    }
  }
  if (nextEdge > UINT64_MAX - 10000000 || nextVertex > UINT64_MAX - 10000000)
  {
    return fail("Boundary identity capacity exceeded");
  }
  std::vector<BoundaryInput> edges;
  std::map<uint64_t, std::vector<BoundaryUse>> replacement;
  for (auto &edge : input.boundaries.records())
  {
    const auto &uses = incidents[edge.id];
    auto a = edge.vertices.front(), b = edge.vertices.back();
    if (uses.empty() || uses.size() > 2)
    {
      return fail("Invalid coast incidence");
    }
    bool coast = uses.size() == 2 && input.cells[uses[0].cell].land != input.cells[uses[1].cell].land;
    if (!coast || settings.amplitude == 0)
    {
      edges.push_back({edge.id, {a.index, a.position}, {b.index, b.position}, {}});
      replacement[edge.id] = {{edge.id, false}};
      continue;
    }
    double dx = b.position.x - a.position.x, dz = b.position.z - a.position.z, length = std::hypot(dx, dz);
    if (length <= 0)
    {
      return fail("Collapsed coast edge");
    }
    double nx = -dz / length, nz = dx / length;
    auto width = [&](Position centre, double t) {
      double u = ((centre.x - a.position.x) * dx + (centre.z - a.position.z) * dz) / (length * length);
      double h = (centre.x - a.position.x) * nx + (centre.z - a.position.z) * nz;
      // Intersect the perpendicular at t with the two non-base triangle edges.
      double w = 0;
      if (u != 0)
      {
        double s = t / u;
        if (s >= 0 && s <= 1)
        {
          w = std::max(w, std::abs(s * h));
        }
      }
      if (u != 1)
      {
        double s = (t - 1) / (u - 1);
        if (s >= 0 && s <= 1)
        {
          w = std::max(w, std::abs(s * h));
        }
      }
      return std::pair<double, double>{h, w};
    };
    std::vector<BoundaryEndpoint> points{{a.index, a.position}};
    for (uint32_t j = 1; j < settings.subdivisions; ++j)
    {
      double t = double(j) / settings.subdivisions;
      double random = 2 * double(featureSeed(input.spec.seed, input.spec.generatorVersion, edge.id, 0x434f4153, j) >> 11) / 9007199254740992.0 - 1;
      double limit = 0;
      for (auto use : uses)
      {
        auto [sign, w] = width(use.centre, t);
        if (sign * random >= 0)
        {
          limit = w;
        }
      }
      double offset = random * settings.amplitude * limit;
      points.push_back({++nextVertex, {a.position.x + t * dx + nx * offset, input.spec.seaLevelMetres, a.position.z + t * dz + nz * offset}});
    }
    points.push_back({b.index, b.position});
    for (size_t j = 1; j < points.size(); ++j)
    {
      uint64_t id = j == 1 ? edge.id : ++nextEdge;
      edges.push_back({id, points[j - 1], points[j], {}});
      replacement[edge.id].push_back({id, points[j - 1].id > points[j].id});
    }
  }
  LandProduct next = input;
  if (!next.boundaries.commit(edges, error))
  {
    return false;
  }
  auto remap = [&](const std::vector<BoundaryUse> &old) {
    std::vector<BoundaryUse> out;
    for (auto use : old)
    {
      auto chain = replacement.at(use.edgeId);
      if (use.reversed)
      {
        std::reverse(chain.begin(), chain.end());
        for (auto &s : chain)
        {
          s.reversed = !s.reversed;
        }
      }
      out.insert(out.end(), chain.begin(), chain.end());
    }
    return out;
  };
  next.landAreaMetres2 = next.totalAreaMetres2 = 0;
  for (auto &cell : next.cells)
  {
    cell.patch.outer = remap(cell.patch.outer);
    PatchMesh mesh;
    if (!compilePatchMesh(
            cell.patch,
            next.boundaries,
            {},
            [&](double, double) {
              return input.spec.seaLevelMetres;
            },
            mesh,
            error))
    {
      return false;
    }
    cell.areaMetres2 = mesh.domainArea;
    next.totalAreaMetres2 += cell.areaMetres2;
    if (cell.land)
    {
      next.landAreaMetres2 += cell.areaMetres2;
    }
  }
  for (auto &loop : next.coasts)
  {
    loop.edges = remap(loop.edges);
    loop.signedAreaMetres2 = 0;
    for (auto use : loop.edges)
    {
      const auto &e = *next.boundaries.find(use.edgeId);
      auto a = e.vertices.front().position, b = e.vertices.back().position;
      if (use.reversed)
      {
        std::swap(a, b);
      }
      loop.signedAreaMetres2 += (a.x * b.z - b.x * a.z) * .5;
    }
  }
  if (std::abs(next.totalAreaMetres2 - input.totalAreaMetres2) > std::max(1e-4, input.totalAreaMetres2 * 1e-10))
  {
    return fail("Coast refinement changed world coverage");
  }
  output = std::move(next);
  if (error)
  {
    error->clear();
  }
  return true;
}
} // namespace procedural::world::rewrite
