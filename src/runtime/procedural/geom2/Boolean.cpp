#include "procedural/geom2/Boolean.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>

namespace procedural::geom2 {

namespace {

bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

bool insideInput(const int32_t winding, const FillRule rule)
{
  return rule == FillRule::NonZero ? winding != 0 : (winding & 1) != 0;
}

// Drops vertices that lie exactly on the line between their neighbours.
//
// The arrangement introduces a vertex wherever anything met anything, so the
// union of two abutting squares comes out with a vertex in the middle of the
// joined edge. Removing a strictly collinear vertex from a simple ring cannot
// make it non-simple, so this is safe as well as tidier: it keeps a lot count
// from depending on what the shape happened to be cut against.
void simplifyCollinear(Ring &ring)
{
  bool changed = true;

  while (changed && ring.size() > 3)
  {
    changed = false;

    for (size_t i = 0; i < ring.size() && ring.size() > 3;)
    {
      const Point &previous = ring[i == 0 ? ring.size() - 1 : i - 1];
      const Point &current = ring[i];
      const Point &next = ring[i + 1 < ring.size() ? i + 1 : 0];

      if (orientation(previous, current, next) == 0)
      {
        ring.erase(ring.begin() + long(i));
        changed = true;
      }
      else
      {
        ++i;
      }
    }
  }
}

} // namespace

Operand toOperand(const Polygon &polygon)
{
  Operand operand;
  operand.rings.push_back(polygon.outer);

  for (const auto &hole : polygon.holes)
  {
    operand.rings.push_back(hole);
  }

  return operand;
}

Operand toOperand(const Ring &ring)
{
  Operand operand;
  operand.rings.push_back(ring);

  return operand;
}

namespace {

// Traces the boundary of an explicit set of faces into canonical polygons.
// Shared by the whole-selection and per-piece entry points below.
bool traceSelection(const Arrangement &arrangement, const std::vector<bool> &selected, std::vector<Polygon> &out, std::string *error)
{
  out.clear();

  const size_t n = arrangement.halfEdges.size();
  std::vector<bool> boundary(n, false);

  for (uint32_t h = 0; h < n; ++h)
  {
    boundary[h] = selected[arrangement.halfEdges[h].face] && !selected[arrangement.halfEdges[arrangement.halfEdges[h].twin].face];
  }

  // Trace the selected region's boundary. Inside is on the left of every
  // boundary half-edge, so an outer ring comes out counter-clockwise and a hole
  // clockwise, with no orientation fixing needed afterwards.
  //
  // Where the next edge of the face is not itself on the boundary, the two
  // faces either side of it are both selected: the edge is an internal seam and
  // is dissolved by stepping across it into the neighbour and carrying on.
  // That is what stops a decomposition seam from becoming an output edge.
  std::vector<bool> used(n, false);
  std::vector<Ring> rings;

  // A traced walk can pass through one vertex twice: the union of two squares
  // meeting at a corner is a single closed walk that pinches there. A pinched
  // ring is not a simple polygon, and the canonical form promises simple ones,
  // so a revisited vertex closes a sub-loop which is emitted as its own ring
  // and popped off the walk. That turns the corner-touching union into the two
  // squares it visibly is.
  const auto emit = [&rings](Ring ring) {
    simplifyCollinear(ring);

    if (ring.size() >= 3 && doubleSignedArea(ring) != 0)
    {
      rings.push_back(std::move(ring));
    }
  };

  for (uint32_t start = 0; start < n; ++start)
  {
    if (!boundary[start] || used[start])
    {
      continue;
    }

    Ring walk;
    std::vector<uint32_t> walkVertices;
    std::map<uint32_t, size_t> positionOf;
    uint32_t current = start;
    size_t guard = 0;

    do
    {
      if (++guard > n + 1)
      {
        return fail(error, "boundary trace did not close");
      }

      used[current] = true;

      const uint32_t vertex = arrangement.halfEdges[current].origin;
      const auto seen = positionOf.find(vertex);

      if (seen != positionOf.end())
      {
        const size_t from = seen->second;

        emit(Ring(walk.begin() + long(from), walk.end()));

        for (size_t k = from; k < walkVertices.size(); ++k)
        {
          positionOf.erase(walkVertices[k]);
        }

        walk.resize(from);
        walkVertices.resize(from);
      }

      positionOf[vertex] = walk.size();
      walk.push_back(arrangement.vertices[vertex]);
      walkVertices.push_back(vertex);

      uint32_t next = arrangement.halfEdges[current].next;
      size_t innerGuard = 0;

      while (!boundary[next])
      {
        if (++innerGuard > n + 1)
        {
          return fail(error, "boundary trace stalled on a dissolved seam");
        }

        next = arrangement.halfEdges[arrangement.halfEdges[next].twin].next;
      }

      current = next;
    } while (current != start);

    emit(std::move(walk));
  }

  // Nest holes in their containing outer ring, smallest first so a hole inside
  // an island inside a lake lands in the island.
  std::vector<size_t> outers;
  std::vector<size_t> holes;

  for (size_t i = 0; i < rings.size(); ++i)
  {
    if (counterClockwise(rings[i]))
    {
      outers.push_back(i);
    }
    else
    {
      holes.push_back(i);
    }
  }

  std::vector<Polygon> polygons(outers.size());
  std::vector<size_t> slotOf(rings.size(), 0);

  for (size_t s = 0; s < outers.size(); ++s)
  {
    polygons[s].outer = rings[outers[s]];
    slotOf[outers[s]] = s;
  }

  for (const size_t h : holes)
  {
    bool placed = false;
    size_t best = 0;
    double bestArea = 0.0;

    for (size_t s = 0; s < outers.size(); ++s)
    {
      const Ring &candidate = rings[outers[s]];

      // A ring may touch its container, so a vertex on the boundary answers
      // nothing; take the verdict from the first vertex that is unambiguous.
      bool contained = false;
      bool decided = false;

      for (const auto &vertex : rings[h])
      {
        if (onBoundary(candidate, vertex))
        {
          continue;
        }

        contained = windingNumber(candidate, vertex) != 0;
        decided = true;

        break;
      }

      if (!decided || !contained)
      {
        continue;
      }

      const double area = areaMetres(candidate);

      if (!placed || area < bestArea)
      {
        placed = true;
        best = s;
        bestArea = area;
      }
    }

    if (!placed)
    {
      return fail(error, "boundary trace produced a hole with no containing ring");
    }

    polygons[best].holes.push_back(rings[h]);
  }

  out = std::move(polygons);

  return true;
}

// Which faces the predicate selects, with the unbounded face never selected
// whatever its winding says.
std::vector<bool> selectionOf(const Arrangement &arrangement, const std::function<bool(const std::vector<int32_t> &)> &inside)
{
  std::vector<bool> selected(arrangement.faces.size(), false);

  for (uint32_t f = 0; f < arrangement.faces.size(); ++f)
  {
    selected[f] = !arrangement.faces[f].unbounded && inside(arrangement.faces[f].winding);
  }

  return selected;
}

} // namespace

bool extractFaces(const Arrangement &arrangement, const std::function<bool(const std::vector<int32_t> &)> &inside, std::vector<Polygon> &out, std::string *error)
{
  out.clear();

  if (!inside)
  {
    return fail(error, "face selection needs a predicate");
  }

  return traceSelection(arrangement, selectionOf(arrangement, inside), out, error);
}

bool extractRegions(const Arrangement &arrangement, const std::function<bool(const std::vector<int32_t> &)> &inside, std::vector<Polygon> &out, std::string *error)
{
  out.clear();

  if (!inside)
  {
    return fail(error, "region selection needs a predicate");
  }

  const std::vector<bool> selected = selectionOf(arrangement, inside);

  // Flood the selected faces, crossing only edges that are not hard. Each
  // component is traced on its own, so a cut line keeps its two sides apart
  // even though nothing about their winding differs.
  constexpr uint32_t none = 0xffffffffu;
  std::vector<uint32_t> component(arrangement.faces.size(), none);
  uint32_t components = 0;

  for (uint32_t seed = 0; seed < arrangement.faces.size(); ++seed)
  {
    if (!selected[seed] || component[seed] != none)
    {
      continue;
    }

    const uint32_t label = components++;
    std::vector<uint32_t> queue{seed};
    component[seed] = label;

    while (!queue.empty())
    {
      const uint32_t face = queue.back();
      queue.pop_back();

      for (const uint32_t cycle : arrangement.faces[face].cycles)
      {
        uint32_t current = cycle;

        do
        {
          const HalfEdge &halfEdge = arrangement.halfEdges[current];
          const uint32_t neighbour = arrangement.halfEdges[halfEdge.twin].face;

          if (!halfEdge.hard && selected[neighbour] && component[neighbour] == none)
          {
            component[neighbour] = label;
            queue.push_back(neighbour);
          }

          current = halfEdge.next;
        } while (current != cycle);
      }
    }
  }

  for (uint32_t label = 0; label < components; ++label)
  {
    std::vector<bool> piece(arrangement.faces.size(), false);

    for (uint32_t f = 0; f < arrangement.faces.size(); ++f)
    {
      piece[f] = component[f] == label;
    }

    std::vector<Polygon> polygons;

    if (!traceSelection(arrangement, piece, polygons, error))
    {
      return false;
    }

    for (auto &polygon : polygons)
    {
      out.push_back(std::move(polygon));
    }
  }

  return true;
}

bool selectFaces(const std::vector<Operand> &operands, const std::function<bool(const std::vector<int32_t> &)> &inside, const BooleanOptions &options, std::vector<Polygon> &out, std::string *error)
{
  out.clear();

  if (operands.empty())
  {
    return fail(error, "boolean needs at least one operand");
  }

  std::vector<InputRing> rings;

  for (uint32_t i = 0; i < operands.size(); ++i)
  {
    for (const auto &ring : operands[i].rings)
    {
      rings.push_back({ring, i});
    }
  }

  if (rings.empty())
  {
    // Every operand empty is a legitimate question with an empty answer, not
    // an error: an intersection with nothing is nothing.
    return true;
  }

  Arrangement arrangement;

  if (!buildArrangement(rings, options.limits, arrangement, error))
  {
    return false;
  }

  return extractFaces(arrangement, inside, out, error);
}

bool booleanOp(const std::vector<Operand> &operands, const Operation operation, const BooleanOptions &options, std::vector<Polygon> &out, std::string *error)
{
  const FillRule rule = options.rule;

  const auto predicate = [operation, rule](const std::vector<int32_t> &winding) {
    switch (operation)
    {
    case Operation::Union:
      for (const int32_t w : winding)
      {
        if (insideInput(w, rule))
        {
          return true;
        }
      }

      return false;

    case Operation::Intersection:
      for (const int32_t w : winding)
      {
        if (!insideInput(w, rule))
        {
          return false;
        }
      }

      return true;

    case Operation::Difference:
      if (winding.empty() || !insideInput(winding[0], rule))
      {
        return false;
      }

      for (size_t i = 1; i < winding.size(); ++i)
      {
        if (insideInput(winding[i], rule))
        {
          return false;
        }
      }

      return true;

    case Operation::Xor:
    default: {
      size_t count = 0;

      for (const int32_t w : winding)
      {
        if (insideInput(w, rule))
        {
          ++count;
        }
      }

      return count % 2 == 1;
    }
    }
  };

  return selectFaces(operands, predicate, options, out, error);
}

bool unionOf(const std::vector<Operand> &operands, std::vector<Polygon> &out, std::string *error)
{
  return booleanOp(operands, Operation::Union, {}, out, error);
}

bool intersectionOf(const std::vector<Operand> &operands, std::vector<Polygon> &out, std::string *error)
{
  return booleanOp(operands, Operation::Intersection, {}, out, error);
}

bool differenceOf(const Operand &subject, const std::vector<Operand> &clips, std::vector<Polygon> &out, std::string *error)
{
  std::vector<Operand> operands{subject};

  for (const auto &clip : clips)
  {
    operands.push_back(clip);
  }

  return booleanOp(operands, Operation::Difference, {}, out, error);
}

} // namespace procedural::geom2
