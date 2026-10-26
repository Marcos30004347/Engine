#include "procedural/geom2/QuadMesh.hpp"

#include <algorithm>
#include <map>

namespace procedural::geom2 {
namespace {

// The directed edge a side runs along, as a key. Rebuilding the twins is a
// matter of pairing (a,b) with (b,a), and nothing else in this file needs a
// spatial structure.
struct DirectedEdge
{
  uint32_t from = kNoVertex, to = kNoVertex;

  bool operator<(const DirectedEdge &other) const
  {
    return from != other.from ? from < other.from : to < other.to;
  }
};

// Half the sum, rounded away from zero on a tie, matching `toUnit`: the
// midpoint of a segment must not depend on which end it is measured from.
int64_t halfSum(const int64_t a, const int64_t b)
{
  const int64_t sum = a + b;

  if ((sum & 1) == 0)
  {
    return sum / 2;
  }

  return sum > 0 ? (sum + 1) / 2 : (sum - 1) / 2;
}

Point midpoint(const Point &a, const Point &b)
{
  return {halfSum(a.x, b.x), halfSum(a.y, b.y)};
}

// The average of a face's corners, rounded once. Integer arithmetic throughout:
// the sum of four coordinates below 2^41 cannot overflow int64.
Point average(const std::vector<Point> &points)
{
  __int128 sx = 0, sy = 0;

  for (const Point &p : points)
  {
    sx += p.x;
    sy += p.y;
  }

  const __int128 n = __int128(points.size());
  const __int128 hx = sx >= 0 ? (2 * sx + n) / (2 * n) : -((-2 * sx + n) / (2 * n));
  const __int128 hy = sy >= 0 ? (2 * sy + n) / (2 * n) : -((-2 * sy + n) / (2 * n));

  return {int64_t(hx), int64_t(hy)};
}

std::string rebuildTopology(QuadMesh &mesh)
{
  for (QuadSide &s : mesh.sides)
  {
    s.twin = kNoSide;
  }

  mesh.vertexSide.assign(mesh.vertices.size(), kNoSide);

  std::map<DirectedEdge, uint32_t> directed;

  for (uint32_t s = 0; s < mesh.sides.size(); ++s)
  {
    const uint32_t a = mesh.sides[s].origin;
    const uint32_t b = mesh.sides[nextSide(s)].origin;

    if (a >= mesh.vertices.size() || b >= mesh.vertices.size())
    {
      return "side " + std::to_string(s) + " has a vertex index out of range";
    }

    const auto inserted = directed.emplace(DirectedEdge{a, b}, s);

    if (!inserted.second)
    {
      return "sides " + std::to_string(inserted.first->second) + " and " + std::to_string(s) + " run along the same directed edge";
    }

    if (mesh.vertexSide[a] == kNoSide)
    {
      mesh.vertexSide[a] = s;
    }
  }

  for (const auto &entry : directed)
  {
    const auto other = directed.find(DirectedEdge{entry.first.to, entry.first.from});

    if (other != directed.end())
    {
      mesh.sides[entry.second].twin = other->second;
    }
  }

  // A vertex on the boundary must name a boundary side, so that walking the
  // boundary from any vertex is one step rather than a search.
  for (uint32_t s = 0; s < mesh.sides.size(); ++s)
  {
    if (mesh.sides[s].twin == kNoSide)
    {
      mesh.vertexSide[mesh.sides[s].origin] = s;
    }
  }

  return {};
}

void compactVertices(QuadMesh &mesh)
{
  std::vector<uint32_t> remap(mesh.vertices.size(), kNoVertex);
  std::vector<Point> kept;

  kept.reserve(mesh.vertices.size());

  for (QuadSide &s : mesh.sides)
  {
    if (remap[s.origin] == kNoVertex)
    {
      remap[s.origin] = uint32_t(kept.size());
      kept.push_back(mesh.vertices[s.origin]);
    }

    s.origin = remap[s.origin];
  }

  mesh.vertices.swap(kept);
}

} // namespace

void QuadMesh::corners(const uint32_t face, Point out[4]) const
{
  for (uint32_t i = 0; i < 4; ++i)
  {
    out[i] = vertices[sides[sideOf(face, i)].origin];
  }
}

Point QuadMesh::centroid(const uint32_t face) const
{
  Point c[4];
  corners(face, c);

  return average({c[0], c[1], c[2], c[3]});
}

double QuadMesh::areaMetres(const uint32_t face) const
{
  Point c[4];
  corners(face, c);

  const Ring ring = {c[0], c[1], c[2], c[3]};

  return geom2::areaMetres(ring);
}

std::string quadMeshFault(const QuadMesh &mesh)
{
  if (mesh.sides.size() % 4 != 0)
  {
    return "side count " + std::to_string(mesh.sides.size()) + " is not a multiple of four";
  }

  if (mesh.vertexSide.size() != mesh.vertices.size())
  {
    return "vertexSide has " + std::to_string(mesh.vertexSide.size()) + " entries for " + std::to_string(mesh.vertices.size()) + " vertices";
  }

  for (uint32_t s = 0; s < mesh.sides.size(); ++s)
  {
    if (mesh.sides[s].origin >= mesh.vertices.size())
    {
      return "side " + std::to_string(s) + " has a vertex index out of range";
    }

    const uint32_t t = mesh.sides[s].twin;

    if (t == kNoSide)
    {
      continue;
    }

    if (t >= mesh.sides.size())
    {
      return "side " + std::to_string(s) + " has a twin index out of range";
    }

    if (mesh.sides[t].twin != s)
    {
      return "twin of side " + std::to_string(s) + " is not involutive";
    }

    if (mesh.sides[t].origin != mesh.destination(s) || mesh.destination(t) != mesh.sides[s].origin)
    {
      return "twin of side " + std::to_string(s) + " does not reverse its endpoints";
    }
  }

  for (uint32_t v = 0; v < mesh.vertices.size(); ++v)
  {
    const uint32_t s = mesh.vertexSide[v];

    if (s == kNoSide)
    {
      return "vertex " + std::to_string(v) + " records no outgoing side";
    }

    if (s >= mesh.sides.size() || mesh.sides[s].origin != v)
    {
      return "vertexSide of vertex " + std::to_string(v) + " does not leave it";
    }
  }

  for (uint32_t f = 0; f < mesh.faces(); ++f)
  {
    Point c[4];
    mesh.corners(f, c);

    for (uint32_t i = 0; i < 4; ++i)
    {
      for (uint32_t j = i + 1; j < 4; ++j)
      {
        if (c[i] == c[j])
        {
          return "face " + std::to_string(f) + " repeats a corner";
        }
      }
    }

    const Ring ring = {c[0], c[1], c[2], c[3]};

    if (doubleSignedArea(ring) <= 0)
    {
      return "face " + std::to_string(f) + " is not strictly counter-clockwise";
    }

    if (segmentsProperlyCross(c[0], c[1], c[2], c[3]) || segmentsProperlyCross(c[1], c[2], c[3], c[0]))
    {
      return "face " + std::to_string(f) + " is self-intersecting";
    }
  }

  return {};
}

void weld(QuadMesh &mesh)
{
  std::map<Point, uint32_t> seen;
  std::vector<uint32_t> remap(mesh.vertices.size(), kNoVertex);
  std::vector<Point> kept;

  kept.reserve(mesh.vertices.size());

  for (uint32_t v = 0; v < mesh.vertices.size(); ++v)
  {
    const auto inserted = seen.emplace(mesh.vertices[v], uint32_t(kept.size()));

    if (inserted.second)
    {
      kept.push_back(mesh.vertices[v]);
    }

    remap[v] = inserted.first->second;
  }

  for (QuadSide &s : mesh.sides)
  {
    s.origin = remap[s.origin];
  }

  mesh.vertices.swap(kept);
  compactVertices(mesh);
  rebuildTopology(mesh);
}

void dropFaces(QuadMesh &mesh, const std::vector<bool> &keep)
{
  std::vector<QuadSide> sides;

  sides.reserve(mesh.sides.size());

  for (uint32_t f = 0; f < mesh.faces(); ++f)
  {
    if (f < keep.size() && !keep[f])
    {
      continue;
    }

    for (uint32_t i = 0; i < 4; ++i)
    {
      sides.push_back({mesh.sides[sideOf(f, i)].origin, kNoSide});
    }
  }

  mesh.sides.swap(sides);
  compactVertices(mesh);
  rebuildTopology(mesh);
}

bool orthoSubdivide(const PolyMesh &in, QuadMesh &out, std::string *error)
{
  out = {};

  if (in.faces.empty())
  {
    if (error)
    {
      *error = "no faces to subdivide";
    }

    return false;
  }

  out.vertices = in.vertices;

  std::map<std::pair<uint32_t, uint32_t>, uint32_t> midpoints;

  const auto edgeMidpoint = [&](const uint32_t a, const uint32_t b) {
    const std::pair<uint32_t, uint32_t> key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
    const auto found = midpoints.find(key);

    if (found != midpoints.end())
    {
      return found->second;
    }

    const uint32_t index = uint32_t(out.vertices.size());
    out.vertices.push_back(midpoint(in.vertices[a], in.vertices[b]));
    midpoints.emplace(key, index);

    return index;
  };

  for (uint32_t f = 0; f < in.faces.size(); ++f)
  {
    const std::vector<uint32_t> &loop = in.faces[f];

    if (loop.size() < 3)
    {
      if (error)
      {
        *error = "face " + std::to_string(f) + " has fewer than three corners";
      }

      return false;
    }

    std::vector<Point> corners;
    corners.reserve(loop.size());

    for (const uint32_t v : loop)
    {
      if (v >= in.vertices.size())
      {
        if (error)
        {
          *error = "face " + std::to_string(f) + " names a vertex out of range";
        }

        return false;
      }

      corners.push_back(in.vertices[v]);
    }

    const uint32_t centre = uint32_t(out.vertices.size());
    out.vertices.push_back(average(corners));

    const uint32_t n = uint32_t(loop.size());

    for (uint32_t i = 0; i < n; ++i)
    {
      const uint32_t here = loop[i];
      const uint32_t ahead = edgeMidpoint(here, loop[(i + 1) % n]);
      const uint32_t behind = edgeMidpoint(loop[(i + n - 1) % n], here);

      out.sides.push_back({here, kNoSide});
      out.sides.push_back({ahead, kNoSide});
      out.sides.push_back({centre, kNoSide});
      out.sides.push_back({behind, kNoSide});
    }
  }

  compactVertices(out);

  const std::string fault = rebuildTopology(out);

  if (!fault.empty())
  {
    if (error)
    {
      *error = fault;
    }

    return false;
  }

  return true;
}

bool refineFaces(QuadMesh &mesh,
                 const std::vector<bool> &refine,
                 std::vector<uint32_t> *parentOfFace,
                 std::vector<EdgeSplit> *edgeRemap,
                 std::string *error)
{
  QuadMesh out;
  out.vertices = mesh.vertices;

  std::vector<uint32_t> parents;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> midpoints;

  const auto edgeMidpoint = [&](const uint32_t a, const uint32_t b) {
    const std::pair<uint32_t, uint32_t> key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
    const auto found = midpoints.find(key);

    if (found != midpoints.end())
    {
      return found->second;
    }

    const uint32_t index = uint32_t(out.vertices.size());
    out.vertices.push_back(midpoint(mesh.vertices[a], mesh.vertices[b]));
    midpoints.emplace(key, index);

    return index;
  };

  // For every side generated, which input side it lies along and which half of
  // it: the only way an attribute attached to an edge can survive refinement.
  std::vector<uint32_t> sideParent;
  std::vector<uint8_t> sideHalf;

  for (uint32_t f = 0; f < mesh.faces(); ++f)
  {
    const bool split = f < refine.size() && refine[f];

    if (!split)
    {
      for (uint32_t i = 0; i < 4; ++i)
      {
        out.sides.push_back({mesh.sides[sideOf(f, i)].origin, kNoSide});
        sideParent.push_back(sideOf(f, i));
        sideHalf.push_back(2); // Whole.
      }

      parents.push_back(f);

      continue;
    }

    Point ignored[4];
    (void)ignored;

    uint32_t corner[4];

    for (uint32_t i = 0; i < 4; ++i)
    {
      corner[i] = mesh.sides[sideOf(f, i)].origin;
    }

    std::vector<Point> points;
    points.reserve(4);

    for (uint32_t i = 0; i < 4; ++i)
    {
      points.push_back(mesh.vertices[corner[i]]);
    }

    const uint32_t centre = uint32_t(out.vertices.size());
    out.vertices.push_back(average(points));

    for (uint32_t i = 0; i < 4; ++i)
    {
      const uint32_t here = corner[i];
      const uint32_t aheadSide = sideOf(f, i);
      const uint32_t behindSide = sideOf(f, (i + 3) % 4);
      const uint32_t ahead = edgeMidpoint(here, corner[(i + 1) % 4]);
      const uint32_t behind = edgeMidpoint(corner[(i + 3) % 4], here);

      out.sides.push_back({here, kNoSide});
      sideParent.push_back(aheadSide);
      sideHalf.push_back(0);

      out.sides.push_back({ahead, kNoSide});
      sideParent.push_back(kNoSide);
      sideHalf.push_back(2);

      out.sides.push_back({centre, kNoSide});
      sideParent.push_back(kNoSide);
      sideHalf.push_back(2);

      out.sides.push_back({behind, kNoSide});
      sideParent.push_back(behindSide);
      sideHalf.push_back(1);

      parents.push_back(f);
    }
  }

  std::vector<uint32_t> vertexRemap(out.vertices.size(), kNoVertex);
  {
    std::vector<Point> kept;
    kept.reserve(out.vertices.size());

    for (QuadSide &s : out.sides)
    {
      if (vertexRemap[s.origin] == kNoVertex)
      {
        vertexRemap[s.origin] = uint32_t(kept.size());
        kept.push_back(out.vertices[s.origin]);
      }

      s.origin = vertexRemap[s.origin];
    }

    out.vertices.swap(kept);
  }

  const std::string fault = rebuildTopology(out);

  if (!fault.empty())
  {
    if (error)
    {
      *error = fault;
    }

    return false;
  }

  if (edgeRemap)
  {
    struct Cover
    {
      uint32_t half[2] = {kNoSide, kNoSide};
      uint32_t whole = kNoSide;
    };

    std::map<uint32_t, Cover> byParent;

    for (uint32_t s = 0; s < out.sides.size(); ++s)
    {
      if (sideParent[s] == kNoSide)
      {
        continue;
      }

      const uint32_t parentEdge = edgeOf(mesh, sideParent[s]);
      Cover &cover = byParent[parentEdge];
      const uint32_t child = edgeOf(out, s);
      const uint8_t half = sideHalf[s];

      if (half == 2)
      {
        cover.whole = child;

        continue;
      }

      // Which half a side covers is named from the parent edge's own direction,
      // not from the visiting face's, so the two halves come back in a stable
      // order however the faces happened to be ordered.
      const bool forward = sideParent[s] == parentEdge;

      cover.half[forward == (half == 0) ? 0 : 1] = child;
    }

    edgeRemap->clear();
    edgeRemap->reserve(byParent.size());

    for (const auto &entry : byParent)
    {
      EdgeSplit split;
      split.parent = entry.first;

      if (entry.second.half[0] == kNoSide)
      {
        split.child[0] = entry.second.whole;
        split.count = 1;
      }
      else
      {
        split.child[0] = entry.second.half[0];
        split.child[1] = entry.second.half[1];
        split.count = 2;

        if (entry.second.whole != kNoSide)
        {
          split.child[2] = entry.second.whole;
          split.count = 3;
        }
      }

      edgeRemap->push_back(split);
    }
  }

  if (parentOfFace)
  {
    *parentOfFace = parents;
  }

  mesh = std::move(out);

  return true;
}

FaceAdjacency faceAdjacency(const QuadMesh &mesh)
{
  FaceAdjacency out;
  const uint32_t count = mesh.faces();

  out.offset.assign(count + 1, 0);

  for (uint32_t f = 0; f < count; ++f)
  {
    uint32_t found = 0;

    for (uint32_t i = 0; i < 4; ++i)
    {
      if (mesh.sides[sideOf(f, i)].twin != kNoSide)
      {
        ++found;
      }
    }

    out.offset[f + 1] = out.offset[f] + found;
  }

  out.neighbour.resize(out.offset[count]);
  out.side.resize(out.offset[count]);

  for (uint32_t f = 0; f < count; ++f)
  {
    uint32_t at = out.offset[f];

    for (uint32_t i = 0; i < 4; ++i)
    {
      const uint32_t t = mesh.sides[sideOf(f, i)].twin;

      if (t == kNoSide)
      {
        continue;
      }

      out.neighbour[at] = faceOf(t);
      out.side[at] = uint8_t(i);
      ++at;
    }
  }

  return out;
}

std::vector<std::vector<uint32_t>> boundaryCycles(const QuadMesh &mesh)
{
  std::map<uint32_t, uint32_t> outgoing; // Vertex -> its boundary side.

  for (uint32_t s = 0; s < mesh.sides.size(); ++s)
  {
    if (mesh.sides[s].twin == kNoSide)
    {
      outgoing.emplace(mesh.sides[s].origin, s);
    }
  }

  std::vector<bool> visited(mesh.sides.size(), false);
  std::vector<std::vector<uint32_t>> cycles;

  for (uint32_t s = 0; s < mesh.sides.size(); ++s)
  {
    if (mesh.sides[s].twin != kNoSide || visited[s])
    {
      continue;
    }

    std::vector<uint32_t> chain;
    uint32_t walk = s;

    while (!visited[walk])
    {
      visited[walk] = true;
      chain.push_back(mesh.sides[walk].origin);

      const auto next = outgoing.find(mesh.destination(walk));

      if (next == outgoing.end())
      {
        break;
      }

      walk = next->second;
    }

    if (chain.size() >= 3)
    {
      cycles.push_back(std::move(chain));
    }
  }

  // Outer cycles carry positive area and holes negative, because a boundary
  // side keeps its face on the left. Largest first, so `cycles.front()` is the
  // outline of the largest component.
  std::stable_sort(cycles.begin(), cycles.end(), [&](const std::vector<uint32_t> &a, const std::vector<uint32_t> &b) {
    Ring ra, rb;

    for (const uint32_t v : a)
    {
      ra.push_back(mesh.vertices[v]);
    }

    for (const uint32_t v : b)
    {
      rb.push_back(mesh.vertices[v]);
    }

    return doubleSignedArea(ra) > doubleSignedArea(rb);
  });

  return cycles;
}

} // namespace procedural::geom2
