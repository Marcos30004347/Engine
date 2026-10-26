#include "procedural/world/hierarchy/CellElevation.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::hierarchy {

const char *name(const ElevationSide side)
{
  switch (side)
  {
    case ElevationSide::North:
      return "north";
    case ElevationSide::East:
      return "east";
    case ElevationSide::South:
      return "south";
    case ElevationSide::West:
      return "west";
  }

  return "?";
}

Elevation elevationOf(const Solid &solid, const ElevationSide side)
{
  Elevation out;
  out.side = side;

  // The viewer's frame: `across` is the page's x, `toward` the page's depth
  // toward the viewer.
  double ax = 1, az = 0; // across
  double tx = 0, tz = -1; // toward the viewer

  switch (side)
  {
    case ElevationSide::North: // standing south, looking north (+z): across is +x, toward the viewer is -z
      ax = 1; az = 0; tx = 0; tz = -1;
      break;
    case ElevationSide::South: // standing north, looking south (-z): across is -x, toward is +z
      ax = -1; az = 0; tx = 0; tz = 1;
      break;
    case ElevationSide::East: // standing west, looking east (+x): across is +z... reading left to right: -z to +z
      ax = 0; az = 1; tx = -1; tz = 0;
      break;
    case ElevationSide::West: // standing east, looking west (-x): across is -z
      ax = 0; az = -1; tx = 1; tz = 0;
      break;
  }

  out.left = out.bottom = 1e300;
  out.right = out.top = -1e300;

  const auto take = [&](const char *kind, const std::vector<Solid::Face> &group, const bool cull) {
    for (const Solid::Face &face : group)
    {
      if (face.size() < 3)
      {
        continue;
      }

      // The face's normal, from its first three corners, to drop what faces
      // away. Flat pieces (decking, a roof seen edge-on) keep both sides:
      // culling them by a normal that is nearly vertical would be noise.
      double nx = 0, ny = 0, nz = 0;

      {
        const Solid::Vertex &p = face[0];
        const Solid::Vertex &q = face[1];
        const Solid::Vertex &r = face[2];
        const double ux = q.x - p.x, uy = q.y - p.y, uz = q.z - p.z;
        const double vx = r.x - p.x, vy = r.y - p.y, vz = r.z - p.z;
        nx = uy * vz - uz * vy;
        ny = uz * vx - ux * vz;
        nz = ux * vy - uy * vx;
      }

      const double horizontal = std::hypot(nx, nz);

      if (cull && horizontal > std::abs(ny) && nx * tx + nz * tz < 0)
      {
        continue;
      }

      ElevationFace flat;
      flat.kind = kind;
      double depth = 0;

      for (const Solid::Vertex &v : face)
      {
        const double across = v.x * ax + v.z * az;
        flat.outline.emplace_back(across, v.y);
        depth += v.x * tx + v.z * tz;
        out.left = std::min(out.left, across);
        out.right = std::max(out.right, across);
        out.bottom = std::min(out.bottom, v.y);
        out.top = std::max(out.top, v.y);
      }

      flat.depth = depth / double(face.size());

      // Seen edge-on, a face is a line and draws nothing.
      double area = 0;

      for (size_t k = 0; k < flat.outline.size(); ++k)
      {
        const auto &p = flat.outline[k];
        const auto &q = flat.outline[(k + 1) % flat.outline.size()];
        area += p.first * q.second - q.first * p.second;
      }

      if (std::abs(area) < 1e-4)
      {
        continue;
      }

      out.faces.push_back(std::move(flat));
    }
  };

  take("walls", solid.walls, true);
  take("timbers", solid.timbers, true);
  take("reveals", solid.reveals, true);
  take("joinery", solid.joinery, true);
  take("pillars", solid.pillars, true);
  take("slopes", solid.slopes, true);
  take("gables", solid.gables, true);
  take("soffits", solid.soffits, true);
  take("yard", solid.yard, false);
  take("foundation", solid.foundations, false);

  std::stable_sort(out.faces.begin(), out.faces.end(),
                   [](const ElevationFace &p, const ElevationFace &q) { return p.depth < q.depth; });

  if (out.faces.empty())
  {
    out.left = out.right = out.bottom = out.top = 0;
  }

  return out;
}

} // namespace procedural::world::hierarchy
