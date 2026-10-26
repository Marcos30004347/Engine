#include "Skeleton.hpp"

// For triangulating a face. A roof is three-dimensional and this file really
// belongs beside the prism rather than in the 2D layer; the include runs the
// right way round (geom3 knows geom2, not the reverse) but the home is wrong.
#include "procedural/geom3/Prism.hpp"
#include "Boolean.hpp"
#include "Polygon.hpp"
#include "Quantise.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace procedural::geom2 {

namespace {

// Footprints use the shared micrometre grid. Intersections need a finer local
// grid: rounding a bisector to a footprint unit can bend a micrometre-wide
// cell into a steep fin. This grid only identifies vertices in this roof; it
// does not change the world-format quantisation.
constexpr double roofUnitsPerMetre = 1e9;

int64_t roofUnit(double x)
{
  return int64_t(std::llround(x * roofUnitsPerMetre));
}

double roofMetres(int64_t x)
{
  return double(x) / roofUnitsPerMetre;
}

// A wall of the plan, and with it the plane of the roof above it.
//
// The roof over a wall is the plane containing that wall and rising inward at
// the pitch. Before the pitch is applied its height at a point is the distance
// from the point to the wall's own line, measured inward - one dot product, and
// the only arithmetic this construction needs.
struct Wall
{
  double ax = 0, az = 0; // Start, in metres.
  double bx = 0, bz = 0; // End.
  double ux = 0, uz = 0; // Unit direction, a to b.
  double nx = 0, nz = 0; // Unit normal, into the building.

  // A wall the roof rises *to* rather than falls from: a gable end, or a party
  // wall shared with a taller mass. It has no face of its own and is simply
  // left out of the competition below, which is the whole of what a gable is
  // here.
  bool raised = false;
  uint32_t source = 0;

  // The walls either side of this one *in its own ring*, which is what the
  // corner tests turn on: a court's last wall is followed by that court's
  // first, never by the outline's.
  size_t next = 0, prev = 0;

  // Which ring, so a run of collinear edges is never collapsed across two of
  // them.
  uint32_t ring = 0;
};

double riseOver(const Wall &w, const double px, const double pz)
{
  return (px - w.ax) * w.nx + (pz - w.az) * w.nz;
}

// Sutherland and Hodgman, clipping a convex polygon by the half plane a.p <= c.
//
// Convex in, convex out, so it is exact and needs no boolean. Everything the
// envelope below asks for is an intersection of half planes, which is convex
// however many of them there are.
void clipTo(std::vector<std::pair<double, double>> &poly, const double ax, const double az, const double c)
{
  if (poly.size() < 3)
  {
    poly.clear();

    return;
  }

  std::vector<std::pair<double, double>> kept;
  kept.reserve(poly.size() + 4);

  for (size_t i = 0; i < poly.size(); ++i)
  {
    const auto &p = poly[i];
    const auto &q = poly[(i + 1) % poly.size()];
    const double dp = ax * p.first + az * p.second - c;
    const double dq = ax * q.first + az * q.second - c;

    if (dp <= 0)
    {
      kept.push_back(p);
    }

    if ((dp < 0 && dq > 0) || (dp > 0 && dq < 0))
    {
      const double t = dp / (dp - dq);

      kept.push_back({p.first + (q.first - p.first) * t, p.second + (q.second - p.second) * t});
    }
  }

  poly.swap(kept);

  if (poly.size() < 3)
  {
    poly.clear();
  }
}

// Hertel and Mehlhorn: given convex pieces over a shared vertex list, drop any
// diagonal whose two pieces merge into a convex one, until none is left.
//
// Used on the plan the roof is about to compete over. It arrives carved into
// triangles by the ear clipper, and that carving is nothing the roof asked for
// - a plain square came out as five facets over four eaves. Convexity is kept
// because everything downstream wants it: Sutherland-Hodgman is exact only on
// convex input, and a fan needs it too.
void mergeConvexPieces(const std::vector<Point> &vertices, std::vector<std::vector<uint32_t>> &loops)
{
  // Twice the signed area, in __int128 because these are roof-grid units: a
  // building is 1e10 of them across and the shoelace terms square that.
  const auto twiceArea = [&](const std::vector<uint32_t> &loop) {
    __int128 twice = 0;
    const Point &origin = vertices[loop.front()];
    for (size_t i = 0; i < loop.size(); ++i)
    {
      const Point &a = vertices[loop[i]], &b = vertices[loop[(i + 1) % loop.size()]];
      twice += __int128(a.x - origin.x) * (b.y - origin.y) - __int128(a.y - origin.y) * (b.x - origin.x);
    }
    return twice;
  };

  const auto convexLoop = [&](const std::vector<uint32_t> &loop) {
    if (loop.size() < 3)
    {
      return false;
    }

    // A ring that visits a corner twice is two lobes, not one face - and it is
    // convex by every turn it makes and has exactly the area of both, so
    // neither of the other two tests sees it.
    std::set<uint32_t> once(loop.begin(), loop.end());
    if (once.size() != loop.size())
    {
      return false;
    }

    for (size_t i = 0; i < loop.size(); ++i)
    {
      const Point &a = vertices[loop[i]];
      const Point &b = vertices[loop[(i + 1) % loop.size()]];
      const Point &c = vertices[loop[(i + 2) % loop.size()]];

      // A repeated point is a hole bridge folding back on itself. Merging
      // across one can leave a ring that is convex by its turns and not simple,
      // so refuse it rather than reason about it.
      if (a == b || orientation(a, b, c) < 0)
      {
        return false;
      }
    }
    return true;
  };

  for (bool merged = true; merged;)
  {
    merged = false;

    // Every directed edge, so a shared diagonal is the one edge that appears in
    // both directions. An edge of the region itself appears once and cannot be
    // found this way, which is exactly right: dropping one would merge a piece
    // with the outside.
    std::map<std::pair<uint32_t, uint32_t>, std::pair<size_t, size_t>> directed;
    for (size_t p = 0; p < loops.size(); ++p)
    {
      for (size_t i = 0; i < loops[p].size(); ++i)
      {
        directed[{loops[p][i], loops[p][(i + 1) % loops[p].size()]}] = {p, i};
      }
    }

    for (const auto &entry : directed)
    {
      const auto twin = directed.find({entry.first.second, entry.first.first});
      if (twin == directed.end() || twin->second.first == entry.second.first)
      {
        continue;
      }

      const size_t p = entry.second.first, i = entry.second.second;
      const size_t q = twin->second.first, j = twin->second.second;
      const std::vector<uint32_t> &P = loops[p], &Q = loops[q];

      std::vector<uint32_t> joined;
      for (size_t k = 0; k < P.size(); ++k)
      {
        joined.push_back(P[(i + 1 + k) % P.size()]);
      }
      for (size_t k = 2; k < Q.size(); ++k)
      {
        joined.push_back(Q[(j + k) % Q.size()]);
      }

      // Convexity is what the rest of the file needs; area is what says the
      // merge is a merge at all.
      //
      // Two pieces can share an edge and still not be two halves of one face -
      // a comb's teeth meet over the same eave at the valley between them, and
      // walking that join gives a ring that is convex by every turn it makes
      // and covers the tooth between them twice. Nothing local to a corner sees
      // it. The union of two disjoint pieces has their areas added, and only
      // that, so ask the area: exact integers, so this is a fact and not a
      // tolerance.
      if (!convexLoop(joined) || twiceArea(joined) != twiceArea(P) + twiceArea(Q))
      {
        continue;
      }

      loops[p] = std::move(joined);
      loops.erase(loops.begin() + long(q));
      merged = true;
      break;
    }
  }
}

// For orthogonal plans the inward wavefront is the boundary's L-infinity
// distance field. A finite eave contributes max(normal distance, distance
// beyond either endpoint); the roof is the minimum over all eaves. These
// continuous piecewise-linear functions need no visibility classification.
// Omitting a gabled eave retains continuity at reflex corners as well.
bool roofSubdivision(const Polygon &ground, std::vector<Wall> &walls, std::vector<std::vector<Polygon>> &pieces)
{
  bool convex = ground.holes.empty();
  for (size_t i = 0; i < ground.outer.size(); ++i)
  {
    convex = convex && orientation(ground.outer[i], ground.outer[(i + 1) % ground.outer.size()], ground.outer[(i + 2) % ground.outer.size()]) >= 0;
  }

  using Loop = std::vector<std::pair<double, double>>;

  struct Cell
  {
    Loop loop;
    size_t plane;
  };

  std::vector<Cell> cells;
  std::vector<std::array<uint32_t, 3>> triangles;
  std::vector<Point> vertices;
  if (!geom3::triangulate(ground, triangles, vertices, nullptr))
  {
    return false;
  }

  // The cells only exist to be clipped, and the clipping needs them convex -
  // nothing here needs them to be *triangles*. Starting from the triangulation
  // and keeping it is what put a diagonal through the middle of a face that had
  // no reason to be cut: a plain square came out as five facets over four
  // eaves, because the ear clipper's diagonal outlived every plane that
  // actually decides the roof, and each of those facets then paid for its own
  // fan.
  //
  // So take the diagonals back out. Hertel and Mehlhorn: drop a diagonal
  // whenever the two pieces either side of it merge into a convex one, and
  // stop. The pieces stay convex, so Sutherland-Hodgman is still exact and
  // every cut below is unchanged - this only decides how coarsely the plan is
  // carved before the roof gets a say.
  std::vector<std::vector<uint32_t>> seeds;
  for (const auto &t : triangles)
  {
    seeds.push_back({t[0], t[1], t[2]});
  }
  mergeConvexPieces(vertices, seeds);

  if (std::getenv("ROOF_CELLS") != nullptr)
  {
    std::fprintf(stderr, "ROOF_SEED %zu triangles -> %zu convex pieces\n", triangles.size(), seeds.size());
  }

  for (const auto &piece : seeds)
  {
    Loop loop;
    for (const uint32_t i : piece)
    {
      loop.emplace_back(toMetres(vertices[i].x), toMetres(vertices[i].y));
    }
    cells.push_back({std::move(loop), size_t(-1)});
  }

  const size_t boundaryCount = walls.size();
  const auto cut = [&](Loop loop, const size_t a, const size_t b) {
    const Wall &p = walls[a], &q = walls[b];
    clipTo(loop, p.nx - q.nx, p.nz - q.nz, p.nx * p.ax + p.nz * p.az - q.nx * q.ax - q.nz * q.az);
    return loop;
  };
  const auto hasArea = [](const Loop &loop) {
    if (loop.size() < 3)
    {
      return false;
    }
    double twice = 0;
    const auto origin = loop.front();
    for (size_t i = 0; i < loop.size(); ++i)
    {
      const auto &a = loop[i], &b = loop[(i + 1) % loop.size()];
      twice += (a.first - origin.first) * (b.second - origin.second) - (a.second - origin.second) * (b.first - origin.first);
    }
    return std::abs(twice) > 1e-12;
  };

  for (size_t e = 0; e < boundaryCount; ++e)
  {
    const Wall w = walls[e];
    if (w.raised)
    {
      continue;
    }
    // A wall's claim is bounded three ways: it never dives below its own eaves
    // (the mirror), and it is cut off square to itself at each end.
    //
    // At a *reflex* corner that square cut is not a guard at all - it is the
    // neighbour's own roof plane, exactly. (Wall e ending at v with tangent u,
    // and its successor f with inward normal n_f: at a square corner n_f = -u,
    // so u.(p-v) and n_f.(p-v) are the same function.) So the face it wins is
    // the neighbour's face, and saying otherwise gives a consumer a facet
    // standing in a plane through an edge it does not name. Only the plan
    // contract above makes this true, which is the second thing that contract
    // buys.
    //
    // The copy is kept rather than reusing the neighbour's index so that a
    // gabled neighbour still bounds this wall without re-entering the
    // competition it was deliberately left out of.
    const auto reflexAt = [&](const Wall &before, const Wall &after) {
      return before.ux * after.uz - before.uz * after.ux < 0;
    };

    const size_t back = walls.size();
    Wall p = w;
    p.raised = false;
    p.nx = -w.nx;
    p.nz = -w.nz;
    walls.push_back(p);

    p = w;
    p.raised = false;
    p.nx = -w.ux;
    p.nz = -w.uz;
    p.source = reflexAt(walls[w.prev], w) ? walls[w.prev].source : w.source;
    walls.push_back(p);

    p = w;
    p.raised = false;
    p.ax = w.bx;
    p.az = w.bz;
    p.nx = w.ux;
    p.nz = w.uz;
    p.source = reflexAt(w, walls[w.next]) ? walls[w.next].source : w.source;
    walls.push_back(p);
    const std::vector<size_t> planes = convex ? std::vector<size_t>{e} : std::vector<size_t>{e, back, back + 1, back + 2};
    std::vector<Cell> next;
    for (const Cell &cell : cells)
    {
      Loop winning = cell.loop;
      if (cell.plane != size_t(-1))
      {
        // If a single branch is already above the old face throughout this
        // cell, the new eave cannot win anywhere. This also resolves coplanar
        // ties without introducing irrelevant subdivision edges.
        bool dominated = false;
        for (const size_t plane : planes)
        {
          bool above = true;
          for (const auto &q : cell.loop)
          {
            above = above && riseOver(walls[plane], q.first, q.second) >= riseOver(walls[cell.plane], q.first, q.second) - 1e-12;
          }
          dominated = dominated || above;
        }
        if (dominated)
        {
          next.push_back(cell);
          continue;
        }
        // The new max-of-planes wins only where every branch is below the
        // old face. Peel off the complementary convex regions one at a time;
        // this partitions the cell instead of independently claiming it.
        for (const size_t plane : planes)
        {
          Loop retained = cut(winning, cell.plane, plane);
          if (hasArea(retained))
          {
            next.push_back({std::move(retained), cell.plane});
          }
          winning = cut(std::move(winning), plane, cell.plane);
          if (!hasArea(winning))
          {
            break;
          }
        }
      }
      if (!hasArea(winning))
      {
        continue;
      }
      for (const size_t plane : planes)
      {
        Loop domain = winning;
        for (const size_t other : planes)
        {
          if (other != plane)
          {
            domain = cut(std::move(domain), other, plane);
          }
        }
        if (hasArea(domain))
        {
          next.push_back({std::move(domain), plane});
        }
      }
    }
    cells.swap(next);
  }
  pieces.resize(walls.size());
  for (const Cell &cell : cells)
  {
    Polygon part;
    for (const auto &q : cell.loop)
    {
      Point p{roofUnit(q.first), roofUnit(q.second)};
      if (part.outer.empty() || p != part.outer.back())
      {
        part.outer.push_back(p);
      }
    }
    if (part.outer.size() > 1 && part.outer.front() == part.outer.back())
    {
      part.outer.pop_back();
    }
    if (part.outer.size() < 3 || doubleSignedArea(part.outer) == 0)
    {
      continue;
    }
    if (!counterClockwise(part.outer))
    {
      reverse(part.outer);
    }
    // Cells were obtained by convex clipping; only a pinched ring introduced
    // by quantisation is invalid here. Coordinates are roof-grid units, not
    // footprint-grid units, so polygonFault's footprint bounds do not apply.
    std::set<Point> unique(part.outer.begin(), part.outer.end());
    if (unique.size() != part.outer.size())
    {
      return false;
    }
    pieces[cell.plane].push_back(std::move(part));
  }

  // ---- one face per plane ------------------------------------------------
  //
  // The competition above leaves a plane's face in pieces, and they are not an
  // accident of rounding - they are the price of keeping every cell convex. The
  // region a new wall wins inside a cell is an intersection of half planes and
  // so convex; the region the *old* face keeps is that subtracted from the
  // cell, which is not. Peeling it off one branch at a time is a convex
  // decomposition of a concave region, and it is correct - but it is a
  // decomposition of one face into slabs, and it compounds: a plan with four
  // convex pieces and nine eaves came out with seventy-six facets, one wall's
  // face arriving as twenty-eight triangles, and every one of those corners is
  // then inserted into each neighbouring face by `conform`, which cannot tell a
  // corner of the roof from a corner of a slab.
  //
  // Aichholzer et al. are unambiguous about what the answer should be: the
  // skeleton "consists of exactly n connected faces", one for each edge. So put
  // the face back together - with the arrangement, not by hand. A hand-rolled
  // merge of adjacent convex pieces was tried first and it cannot do this job:
  // it needs the seam to be one whole edge on both sides, it cannot produce a
  // concave result, and two slabs meeting at a valley pass every local test it
  // can make while covering the ground between them twice.
  // Group the buckets by the plane they actually are, not by which branch of
  // which wall produced them.
  //
  // The paper has one plane per edge - pi(e), bounded by the line of e, leaning
  // in at the pitch - and its Lemma 1 counts one face per edge against exactly
  // that. This construction carries four planes per wall instead, because the
  // L-infinity box distance is a max of four, and one of them is not a plane of
  // its own at all: at a square reflex corner a wall's end cap *is* the
  // neighbour's pi(e), which is why the facet it wins is attributed to the
  // neighbour. So a wall's face arrives in two buckets - its own and the cap of
  // whichever neighbour turns into it - which are the same plane, adjacent, and
  // never offered to the union together. Measured, that is the whole of the
  // difference from one: 212 of 212 split eaves were split across buckets and
  // none within one.
  std::map<size_t, std::vector<size_t>> byPlane;

  for (size_t i = 0; i < pieces.size(); ++i)
  {
    if (pieces[i].empty())
    {
      continue;
    }

    // Which wall's plane is this bucket, really? The bucket names a source; it
    // belongs with that source's own bucket when the two are the same plane.
    //
    // Hashing the plane was tried and is the wrong tool. A cap and the
    // neighbour plane it coincides with are one plane in exact arithmetic and
    // are not computed alike - each normalises by its own wall's length - so
    // they disagree in the last bits. Key them finely and they never group;
    // key them coarsely and, on a rotated plan where a thirty-metre lever arm
    // turns a 1e-7 radian disagreement into micrometres of offset, two planes
    // that are genuinely apart land in one group and the face stops being
    // single-valued. Comparing the two planes directly asks the question
    // instead of encoding it.
    const Wall &w = walls[i];
    const Wall &owner = walls[w.source];

    // Exactly the same plane, to the bit, and no tolerance at all.
    //
    // A tolerance is the wrong idea here even though these planes are "the
    // same": merging two buckets makes one plane's equation answer for the
    // other's ground, so anything the tolerance admits comes straight back as
    // disagreement about the height over a shared vertex. Square to the axes
    // the two are bit-identical - the normals are 0 and +/-1 exactly, and the
    // cap is anchored on the very corner the neighbour starts from, so the
    // offsets are the same arithmetic on the same numbers. Turned, they are
    // not, and a 1e-7 radian difference over a thirty-metre face is
    // micrometres of height that the subdivision is right to reject. So merge
    // where they are one plane and leave them apart where they are two.
    const bool onePlane = w.nx == owner.nx && w.nz == owner.nz
      && w.nx * w.ax + w.nz * w.az == owner.nx * owner.ax + owner.nz * owner.az;

    byPlane[onePlane ? size_t(w.source) : i].push_back(i);
  }

  for (const auto &entry : byPlane)
  {
    std::vector<Polygon> gathered;
    for (const size_t i : entry.second)
    {
      for (Polygon &part : pieces[i])
      {
        gathered.push_back(std::move(part));
      }
    }

    std::vector<Polygon> &plane = pieces[entry.second.front()];

    for (const size_t i : entry.second)
    {
      pieces[i].clear();
    }

    if (gathered.size() < 2)
    {
      plane = std::move(gathered);

      continue;
    }

    std::vector<Operand> parts;
    for (const Polygon &part : gathered)
    {
      parts.push_back(toOperand(part));
    }

    std::vector<Polygon> faces;
    if (!unionOf(parts, faces, nullptr) || faces.empty())
    {
      plane = std::move(gathered); // The slabs are a correct surface; keep them.

      continue;
    }

    // A skeleton face is monotone and simply connected, so a court in one means
    // the union found something this construction does not model. Keep the
    // slabs rather than triangulate a face nobody can vouch for.
    bool sound = true;
    for (const Polygon &face : faces)
    {
      sound = sound && face.holes.empty();
    }

    plane = sound ? std::move(faces) : std::move(gathered);
  }

  return true;
}

} // namespace

bool roofFromSkeleton(const Polygon &ground, const RoofOptions &options, std::vector<RoofTriangle> &out, std::string *error)
{
  out.clear();
  if (error)
  {
    error->clear();
  }
  // A slope of nought is a deck, not a missing argument. A parapet roof is flat
  // by construction and still wants the skeleton, because the skeleton is what
  // knows about the holes, the courts and the walls a storey stands against.
  if (!std::isfinite(options.slope) || !std::isfinite(options.ridgeHeight) || options.ridgeHeight < 0 ||
      options.slope < 0)
  {
    if (error)
    {
      *error = "roof pitch or ridge height is invalid";
    }
    return false;
  }

  if (ground.outer.size() < 3)
  {
    if (error != nullptr)
    {
      *error = "a roof needs at least three edges";
    }

    return false;
  }

  const std::string fault = polygonFault(ground);

  if (!fault.empty())
  {
    if (error != nullptr)
    {
      *error = "the outline is not a simple polygon: " + fault;
    }

    return false;
  }

  if (std::abs(areaMetres(ground)) < 1e-6)
  {
    if (error != nullptr)
    {
      *error = "the outline has no area";
    }

    return false;
  }

  // ---- the walls --------------------------------------------------------
  //
  // Every ring: the outline, and any court the building wraps around. A court's
  // walls are walls, and the roof over a range between an outer wall and a
  // court is those two meeting - which needs no special case at all once both
  // are in the same list.
  std::vector<Wall> walls;

  {
    std::vector<const Ring *> rings{&ground.outer};

    for (const Ring &hole : ground.holes)
    {
      rings.push_back(&hole);
    }

    for (size_t r = 0; r < rings.size(); ++r)
    {
      const Ring *ring = rings[r];
      const size_t ringStart = walls.size();

      for (size_t i = 0; i < ring->size(); ++i)
      {
        const Point &from = (*ring)[i], &to = (*ring)[(i + 1) % ring->size()];

        Wall w;
        w.ax = toMetres(from.x);
        w.az = toMetres(from.y);
        w.bx = toMetres(to.x);
        w.bz = toMetres(to.y);

        const double length = std::hypot(w.bx - w.ax, w.bz - w.az);

        if (length < 1e-6)
        {
          continue;
        }

        w.ux = (w.bx - w.ax) / length;
        w.uz = (w.bz - w.az) / length;
        w.nx = -w.uz; // Counter-clockwise outer, clockwise hole: inside is left of both.
        w.nz = w.ux;

        w.ring = uint32_t(r);
        w.source = uint32_t(walls.size());
        w.next = walls.size() + 1;
        walls.push_back(w);
      }

      // A ring closes on itself, and only on itself: the wall after the last
      // one of a court is that court's first, never the outline's.
      if (walls.size() > ringStart)
      {
        walls.back().next = ringStart;

        for (size_t i = ringStart; i < walls.size(); ++i)
        {
          walls[walls[i].next].prev = i;
        }
      }
    }
  }

  if (walls.size() < 3)
  {
    if (error != nullptr)
    {
      *error = "the outline has no usable edges";
    }

    return false;
  }

  // Gable boundaries carry vertical panels instead of contributing eaves.
  {
    size_t raised = 0;

    for (size_t i = 0; i < walls.size() && i < options.gables.size(); ++i)
    {
      walls[i].raised = options.gables[i];
      raised += options.gables[i] ? 1 : 0;
    }

    if (raised == walls.size())
    {
      if (error)
      {
        *error = "a roof needs at least one non-gabled eave";
      }
      return false;
    }
  }

  // ---- one wall per wall -------------------------------------------------
  //
  // A run of collinear edges is one wall, and has to be made into one before
  // anything else happens.
  //
  // Aichholzer et al. partition an n-gon into n faces, one per edge. A vertex
  // between two collinear edges is not a vertex of the polygon in any sense the
  // skeleton uses - its bisector is the edge itself - so a plan carrying one
  // does not have a skeleton so much as a tie. Two walls on the same line
  // facing the same way both claim the whole face, and whatever breaks that tie
  // (an index, an ordering) cuts one face into two arbitrary pieces, along a
  // seam that is in the roof for no reason and that every neighbouring face
  // then has to be given a vertex for.
  //
  // The eaves arrive from booleans and offsets, so they carry these constantly:
  // one grand-wings plan came in with sixteen planes over a mass that has eight
  // walls, and its long elevation was two coplanar faces side by side.
  //
  // A gable boundary ends a run whatever the geometry does, because a wall that
  // is half party wall and half eaves is two walls in the only sense that
  // matters here.
  {
    // Collinear to a tenth of a milliradian, and - separately - no dropped
    // corner further than a millimetre off the line that replaces it. The
    // angle alone would let a long run drift, and the drift is what would move
    // the roof.
    constexpr double straight = 1e-4;
    constexpr double drift = 1e-3;

    std::vector<Wall> kept;

    for (size_t start = 0; start < walls.size();)
    {
      size_t stop = start;
      while (stop < walls.size() && walls[stop].ring == walls[start].ring)
      {
        ++stop;
      }

      const size_t n = stop - start;
      const auto at = [&](const size_t k) -> const Wall & { return walls[start + k % n]; };
      const auto continues = [&](const Wall &a, const Wall &b) {
        return a.raised == b.raised
          && std::abs(std::atan2(a.ux * b.uz - a.uz * b.ux, a.ux * b.ux + a.uz * b.uz)) < straight;
      };

      // Start the walk at a real corner, or a run that wraps the end of the
      // ring is collapsed as two.
      size_t first = 0;
      while (first < n && continues(at(first + n - 1), at(first)))
      {
        ++first;
      }

      const size_t ringStart = kept.size();

      for (size_t k = 0; k < n && first < n;)
      {
        Wall run = at(first + k);
        size_t length = 1;

        while (k + length < n && continues(at(first + k + length - 1), at(first + k + length)))
        {
          const Wall &next = at(first + k + length);
          const double ex = next.bx - run.ax, ez = next.bz - run.az;
          const double span = std::hypot(ex, ez);
          bool straightEnough = span > 1e-9;

          // Every corner this would drop, against the line that would replace
          // the whole run - not against the step just taken.
          for (size_t back = 0; back <= length && straightEnough; ++back)
          {
            const Wall &dropped = at(first + k + back);
            straightEnough = std::abs((dropped.bx - run.ax) * ez - (dropped.bz - run.az) * ex) / span <= drift;
          }

          if (!straightEnough)
          {
            break;
          }

          run.bx = next.bx;
          run.bz = next.bz;
          ++length;
        }

        const double span = std::hypot(run.bx - run.ax, run.bz - run.az);

        if (span > 1e-6)
        {
          run.ux = (run.bx - run.ax) / span;
          run.uz = (run.bz - run.az) / span;
          run.nx = -run.uz;
          run.nz = run.ux;
          run.next = kept.size() + 1;
          kept.push_back(run);
        }

        k += length;
      }

      // A ring every one of whose corners is collinear has no area, and the
      // outline was measured for area above. Keep it whole rather than reason
      // about it.
      if (first == n)
      {
        for (size_t k = 0; k < n; ++k)
        {
          Wall run = at(k);
          run.next = kept.size() + 1;
          kept.push_back(run);
        }
      }

      if (kept.size() > ringStart)
      {
        kept.back().next = ringStart;

        for (size_t i = ringStart; i < kept.size(); ++i)
        {
          kept[kept[i].next].prev = i;
        }
      }

      start = stop;
    }

    if (std::getenv("ROOF_CELLS") != nullptr && kept.size() != walls.size())
    {
      std::fprintf(stderr, "ROOF_WALLS %zu edges -> %zu walls\n", walls.size(), kept.size());
    }

    walls.swap(kept);
  }

  // ---- and no needles ----------------------------------------------------
  //
  // A pair of walls that doubles back on itself is a slit, not a corner.
  //
  // One church came in with a wall running 3.94 m down and another running
  // 6.45 m back up 6.7 cm away from it - two walls that should be one, left by
  // a boolean - and the reflex corner at the tip turns 179.02 degrees. The
  // corner contract below is right to refuse that, and refusing it costs the
  // building its entire roof. It is also right that this cannot be roofed as
  // it stands: measured against a mitred-offset skeleton, a slit is the worst
  // case there is, and it gets worse as the slit narrows -
  //
  //   slit turn   179d    178d    175d    170d    150d
  //   peak error  2.880   2.853   2.771   2.630   2.073   m per unit pitch
  //
  // - because the true skeleton drives a valley from the slit's tip right
  // across the plan and a construction that cuts each wall's claim off at its
  // own ends has nothing of the sort. So the slit has to go before the roof is
  // asked about it.
  //
  // Width is the measure, not the tip's distance from the chord across it. The
  // eaves are already simplified by chord deviation at 15 cm and it does not
  // catch this: the tip stands 17.2 cm off the chord, because a long thin spike
  // leans away from its own chord far more than it is wide.
  {
    // Width is the test; the angle only says "spike" is the right word for it.
    // The church's slit opens a whole degree - a spike does not have to be
    // arbitrarily sharp to be a spike, it has to be thin - so the band is wide
    // and it is the width that decides. A real notch, a light well or a
    // re-entrant corner, is wider than a wall and keeps its tip.
    constexpr double doubledBack = .2;
    constexpr double sliver = .3;

    for (bool cut = true; cut;)
    {
      cut = false;

      for (size_t i = 0; i < walls.size() && !cut; ++i)
      {
        const Wall &a = walls[i];
        const Wall &b = walls[a.next];

        if (a.next == i || a.raised != b.raised)
        {
          continue;
        }

        const double turn = std::atan2(a.ux * b.uz - a.uz * b.ux, a.ux * b.ux + a.uz * b.uz);
        const double opening = 3.14159265358979323846 - std::abs(turn);

        if (opening > doubledBack)
        {
          continue;
        }

        const double arm = std::min(std::hypot(a.bx - a.ax, a.bz - a.az), std::hypot(b.bx - b.ax, b.bz - b.az));

        if (arm * std::sin(opening) > sliver)
        {
          continue;
        }

        // The tip goes, and the two walls either side of it become the one
        // wall they were always meant to be.
        Wall joined = a;
        joined.bx = b.bx;
        joined.bz = b.bz;

        const double span = std::hypot(joined.bx - joined.ax, joined.bz - joined.az);

        if (span < 1e-6)
        {
          continue; // The slit is the whole of both walls; leave it to the contract.
        }

        joined.ux = (joined.bx - joined.ax) / span;
        joined.uz = (joined.bz - joined.az) / span;
        joined.nx = -joined.uz;
        joined.nz = joined.ux;

        std::vector<Wall> kept;

        for (size_t k = 0; k < walls.size(); ++k)
        {
          if (k == a.next)
          {
            continue;
          }

          kept.push_back(k == i ? joined : walls[k]);
        }

        // The ring links are indices into a vector that just lost an entry.
        for (size_t start = 0; start < kept.size();)
        {
          size_t stop = start;
          while (stop < kept.size() && kept[stop].ring == kept[start].ring)
          {
            ++stop;
          }

          for (size_t k = start; k < stop; ++k)
          {
            kept[k].next = k + 1 < stop ? k + 1 : start;
            kept[kept[k].next].prev = k;
          }

          start = stop;
        }

        walls.swap(kept);
        cut = true;
      }
    }
  }

  // ---- the one thing this construction needs of a plan -------------------
  //
  // The height over a point is the distance to the nearest eave measured in
  // that eave's own frame - the L-infinity distance to the boundary. Aichholzer
  // et al. (1995) give the exact condition under which that *is* the straight
  // skeleton: "if P is rectilinear then the straight skeleton is the medial
  // axis of P for the L-infinity metric". Ahn et al. (2013, Lemma 3) restate it
  // as z(p) = d_inf(p, boundary).
  //
  // What the construction actually needs is weaker than rectilinear, and worth
  // stating exactly, because it is what decides which plans a caller may draw.
  // A wall's claim is cut off at its ends square to itself; a straight skeleton
  // cuts it off on the angular bisector with its neighbour. At a *square*
  // corner those two cuts are the same line, and that is true however the plan
  // is turned. So:
  //
  //   every reflex corner must be a right angle.
  //
  // Convex corners may be any angle at all - the neighbouring plane takes over
  // on the bisector there without being told - and the plan may sit at any
  // rotation. A convex outline is therefore always allowed, which is why a
  // trapezoid, a wedge and a pentagon are still roofed exactly.
  //
  // For an oblique reflex corner there is no local repair. Aichholzer et al.
  // prove it in the same paper: a roof is not the lower envelope of functions
  // defined locally by each edge, because moving a *distant* reflex vertex
  // changes which roof is correct here. That is what the refusal below is for -
  // the alternative is a plausible roof with its ridges in the wrong place, and
  // nothing to tell the caller.
  //
  // The test is on the turn angle, and it is a band rather than an equality,
  // because two different things put a corner off square and they are five
  // orders of magnitude apart. Quantising a *rotated* plan to micrometres
  // leaves its corners off by about 1e-7 radians - noise, and no plan should be
  // refused for it. A plan that came back from a boolean leaves them off by a
  // degree or so, and a plan that is genuinely oblique by five or more.
  //
  // Where to draw the line is a judgement, and worth being honest that it is
  // one: the error is linear in the deviation the whole way, with no cliff. It
  // was measured on an L with one wall tilted off square, against a mitred
  // offset skeleton:
  //
  //   off square    0.25d    0.5d    1d     2d     3d     30d
  //   peak error    0.017    0.034   0.066  0.133  0.186  1.82   m per unit pitch
  //
  // Two degrees, then: about 0.13 m per unit pitch, so under 7 cm at the
  // pitches this generator builds. That takes the near-square plans the eaves
  // booleans actually produce - 0.70 and 0.84 degrees out, measured on
  // mansion-wings - and still refuses the plans where the error runs to metres.
  {
    // Half a degree of turn still counts as straight: two walls that continue
    // each other have no corner between them to bisect, whatever the sign of a
    // cross product that small says.
    constexpr double straight = 1e-4;

    // Two degrees off square is still square enough, per the table above.
    constexpr double toSquare = 2 * 3.14159265358979323846 / 180;

    for (const Wall &w : walls)
    {
      const Wall &after = walls[w.next];
      const double turn = std::atan2(w.ux * after.uz - w.uz * after.ux, w.ux * after.ux + w.uz * after.uz);

      if (turn > -straight)
      {
        continue; // Straight, or a convex corner: free at any angle.
      }

      if (std::abs(std::abs(turn) - 3.14159265358979323846 / 2) > toSquare)
      {
        if (error != nullptr)
        {
          *error = "a reflex corner of the outline is not a right angle, at ("
            + std::to_string(w.bx) + ", " + std::to_string(w.bz) + "): it turns "
            + std::to_string(std::abs(turn) * 180 / 3.14159265358979323846)
            + " degrees. This roof is the L-infinity medial axis, which is the "
              "straight skeleton only where the corners it turns into are square";
        }

        return false;
      }
    }
  }


  std::vector<std::vector<Polygon>> pieces(walls.size());
  if (!roofSubdivision(ground, walls, pieces))
  {
    if (error)
    {
      *error = "roof subdivision failed";
    }
    return false;
  }

  struct Facet
  {
    Polygon shape;
    size_t wall;
  };

  std::vector<Facet> facets;
  double peak = 0;
  for (size_t i = 0; i < pieces.size(); ++i)
  {
    for (Polygon &part : pieces[i])
    {
      for (const Point &q : part.outer)
      {
        peak = std::max(peak, riseOver(walls[i], roofMetres(q.x), roofMetres(q.y)));
      }
      facets.push_back({std::move(part), i});
    }
  }

  if (std::getenv("ROOF_CELLS") != nullptr)
  {
    // Per source eave: how many facets, and across how many distinct plane
    // buckets. The second number is the question - a wall whose face is split
    // across several buckets was never offered to the union as one face.
    std::map<uint32_t, std::pair<size_t, std::set<size_t>>> perSource;
    for (const Facet &f : facets)
    {
      auto &c = perSource[walls[f.wall].source];
      c.first += 1;
      c.second.insert(f.wall);
    }
    size_t facetsTotal = 0, splitBySource = 0, splitByBucket = 0;
    for (const auto &entry : perSource)
    {
      facetsTotal += entry.second.first;
      splitBySource += entry.second.first > 1 ? 1 : 0;
      splitByBucket += entry.second.second.size() > 1 ? 1 : 0;
    }
    std::fprintf(stderr, "ROOF_CELLS %zu facets over %zu eaves; %zu eaves split, %zu of them across buckets:",
                 facetsTotal, perSource.size(), splitBySource, splitByBucket);
    for (const auto &entry : perSource)
    {
      if (entry.second.first > 1)
      {
        std::fprintf(stderr, " e%u=%zuf/%zub", entry.first, entry.second.first, entry.second.second.size());
      }
    }
    std::fprintf(stderr, "\n");
  }

  if (facets.empty())
  {
    if (error != nullptr)
    {
      *error = "the skeleton produced no faces";
    }

    return false;
  }

  const double pitch = options.ridgeHeight > 0 && peak > 1e-6 ? options.ridgeHeight / peak : options.slope;

  // The faces, made to agree about their shared edges before any of them is
  // triangulated.
  //
  // Two faces can share a line without sharing an edge. Where three faces meet,
  // one may run the whole length while the other two end part way along it:
  // every vertex is where it should be, the surface has no gap in it, and it
  // still comes apart when it is drawn, because a rasteriser walks each
  // triangle from its own vertices and two walks along the same line from
  // different endpoints do not land on the same pixels.
  //
  // So each face is given the vertices its neighbours end on. This is not a
  // repair - nothing moves, and the height over every point is what it was,
  // since a vertex inserted on an edge of a plane lies in that plane. It is
  // what makes the faces a subdivision rather than a heap of polygons, and it
  // has to happen before the triangulator sees them, because afterwards the
  // only thing left to do is cut triangles up and hope.
  {
    std::set<std::pair<int64_t, int64_t>> corners;

    for (const Facet &facet : facets)
    {
      for (const Point &q : facet.shape.outer)
      {
        corners.insert({q.x, q.y});
      }

      for (const Ring &hole : facet.shape.holes)
      {
        for (const Point &q : hole)
        {
          corners.insert({q.x, q.y});
        }
      }
    }

    const auto conform = [&](Ring &ring) {
      Ring grown;

      for (size_t i = 0; i < ring.size(); ++i)
      {
        const Point &a = ring[i];
        const Point &b = ring[(i + 1) % ring.size()];

        grown.push_back(a);

        const double ax = roofMetres(a.x), az = roofMetres(a.y);
        const double bx = roofMetres(b.x), bz = roofMetres(b.y);
        const double ex = bx - ax, ez = bz - az;
        const double span = ex * ex + ez * ez;

        if (span < 1e-24)
        {
          continue;
        }

        std::vector<std::pair<double, Point>> on;

        for (const auto &corner : corners)
        {
          if ((corner.first == a.x && corner.second == a.y)
              || (corner.first == b.x && corner.second == b.y))
          {
            continue;
          }

          const double qx = roofMetres(corner.first), qz = roofMetres(corner.second);
          const double at = ((qx - ax) * ex + (qz - az) * ez) / span;

          if (at <= 1e-9 || at >= 1. - 1e-9)
          {
            continue;
          }

          if (std::hypot(qx - (ax + ex * at), qz - (az + ez * at)) > 2e-9)
          {
            continue;
          }

          on.push_back({at, Point{corner.first, corner.second}});
        }

        std::vector<size_t> order(on.size());

        for (size_t k = 0; k < order.size(); ++k)
        {
          order[k] = k;
        }

        std::sort(order.begin(), order.end(),
                  [&](const size_t l, const size_t r) { return on[l].first < on[r].first; });

        for (const size_t k : order)
        {
          grown.push_back(on[k].second);

        }
      }

      ring.clear();
      for (const Point &q : grown)
      {
        if (ring.empty() || q != ring.back())
        {
          ring.push_back(q);
        }
      }
      if (ring.size() > 1 && ring.front() == ring.back())
      {
        ring.pop_back();
      }
    };

    for (Facet &facet : facets)
    {
      conform(facet.shape.outer);

      for (Ring &hole : facet.shape.holes)
      {
        conform(hole);
      }
    }
  }

  // A subdivision vertex has one height. Evaluating adjacent planes
  // independently after XY quantisation can put that height on opposite sides
  // of a rounding boundary. Store it once for all incident cells.
  std::map<Point, double> heights;
  bool continuous = true;
  for (const Facet &facet : facets)
  {
    const auto record = [&](const Ring &ring) {
      for (const Point &q : ring)
      {
        const double height = std::max(0.0, riseOver(walls[facet.wall], roofMetres(q.x), roofMetres(q.y))) * pitch;
        const auto inserted = heights.emplace(q, height);
        continuous = continuous && std::abs(inserted.first->second - height) <= 8e-9 * pitch;
      }
    };
    record(facet.shape.outer);
    for (const Ring &hole : facet.shape.holes)
    {
      record(hole);
    }
  }

  if (!continuous)
  {
    if (error)
    {
      *error = "roof subdivision has inconsistent vertex heights";
    }
    return false;
  }

  for (const Facet &facet : facets)
  {
    // A face is either convex, and a fan from one of its corners is the whole
    // triangulation - two triangles on a quad, inventing no vertex - or it is
    // not, and it needs a real triangulator.
    //
    // Both must keep every boundary vertex. The faces carry collinear ones on
    // purpose: they are where a neighbour ends, and a face that drops one opens
    // a seam against that neighbour which no amount of area accounting will
    // show. The fan keeps them because a fan uses every vertex it walks past;
    // ear clipping keeps them because it consumes vertices rather than
    // discarding them.
    const Ring &ring = facet.shape.outer;
    const auto height = [&](const Point &q) { return heights.at(q); };

    bool convexFace = true;
    for (size_t i = 0; i < ring.size() && convexFace; ++i)
    {
      convexFace = orientation(ring[i], ring[(i + 1) % ring.size()], ring[(i + 2) % ring.size()]) >= 0;
    }

    size_t apex = ring.size();

    if (convexFace)
    {
      for (size_t k = 0; k < ring.size() && apex == ring.size(); ++k)
      {
        bool clean = true;

        for (size_t i = 1; i + 1 < ring.size() && clean; ++i)
        {
          const Point &a = ring[k], &b = ring[(k + i) % ring.size()], &c = ring[(k + i + 1) % ring.size()];

          clean = a != b && b != c && a != c && orientation(a, b, c) > 0;
        }

        apex = clean ? k : apex;
      }
    }

    if (apex < ring.size())
    {
      const Point &a = ring[apex];

      for (size_t i = 1; i + 1 < ring.size(); ++i)
      {
        const Point &b = ring[(apex + i) % ring.size()], &c = ring[(apex + i + 1) % ring.size()];

        out.push_back({roofMetres(a.x), height(a), roofMetres(a.y), roofMetres(b.x), height(b), roofMetres(b.y),
                       roofMetres(c.x), height(c), roofMetres(c.y), walls[facet.wall].source, false});
      }

      continue;
    }

    std::vector<std::array<uint32_t, 3>> parts;
    std::vector<Point> corners;

    if (geom3::triangulate(facet.shape, parts, corners, nullptr))
    {
      for (const auto &t : parts)
      {
        const Point &a = corners[t[0]], &b = corners[t[1]], &c = corners[t[2]];

        out.push_back({roofMetres(a.x), height(a), roofMetres(a.y), roofMetres(b.x), height(b), roofMetres(b.y),
                       roofMetres(c.x), height(c), roofMetres(c.y), walls[facet.wall].source, false});
      }

      continue;
    }

    // Last resort, and reachable only for a face the triangulator refused: a
    // fan from a point inside it, which costs one triangle per edge and a
    // vertex of its own but cannot fail on a convex ring.
    double cx = 0, cz = 0;
    for (const Point &q : ring)
    {
      cx += roofMetres(q.x);
      cz += roofMetres(q.y);
    }
    cx /= ring.size();
    cz /= ring.size();
    const double cy = std::max(0.0, riseOver(walls[facet.wall], cx, cz)) * pitch;
    for (size_t i = 0; i < ring.size(); ++i)
    {
      const Point &a = ring[i], &b = ring[(i + 1) % ring.size()];
      if (a == b)
      {
        continue;
      }
      out.push_back({roofMetres(a.x), height(a), roofMetres(a.y), roofMetres(b.x), height(b), roofMetres(b.y), cx, cy, cz, walls[facet.wall].source, false});
    }
  }

  if (out.empty())
  {
    if (error != nullptr)
    {
      *error = "the skeleton produced no surface";
    }

    return false;
  }

  // A gable is the vertical strip under each exposed slope edge. Reuse
  // its vertices directly so the cap and roof have identical boundaries.
  const size_t slopes = out.size();
  for (const Wall &w : walls)
  {
    if (!w.raised)
    {
      continue;
    }
    const double length = std::hypot(w.bx - w.ax, w.bz - w.az);
    for (size_t i = 0; i < slopes; ++i)
    {
      const RoofTriangle t = out[i];
      const double x[3]{t.ax, t.bx, t.cx}, y[3]{t.ay, t.by, t.cy}, z[3]{t.az, t.bz, t.cz};
      for (int k = 0; k < 3; ++k)
      {
        const int j = (k + 1) % 3;
        const double a = (x[k] - w.ax) * w.ux + (z[k] - w.az) * w.uz;
        const double b = (x[j] - w.ax) * w.ux + (z[j] - w.az) * w.uz;
        if (std::abs(riseOver(w, x[k], z[k])) > 2e-9 || std::abs(riseOver(w, x[j], z[j])) > 2e-9 || std::min(a, b) < -2e-9 || std::max(a, b) > length + 2e-9 || std::abs(a - b) < 1e-9)
        {
          continue;
        }
        if (y[j] > 1e-12)
        {
          out.push_back({x[k], y[k], z[k], x[j], y[j], z[j], x[j], 0, z[j], w.source, true});
        }
        if (y[k] > 1e-12)
        {
          out.push_back({x[k], y[k], z[k], x[j], 0, z[j], x[k], 0, z[k], w.source, true});
        }
      }
    }
  }

  // Match the winding convention of the CCW footprint triangulation: the
  // gable and slope must traverse their common top edge in opposite directions.
  for (size_t i = slopes; i < out.size(); ++i)
  {
    std::swap(out[i].bx, out[i].cx);
    std::swap(out[i].by, out[i].cy);
    std::swap(out[i].bz, out[i].cz);
  }

  return true;
}

bool roofFromSkeleton(const std::vector<std::pair<double, double>> &outline, const RoofOptions &options, std::vector<RoofTriangle> &out, std::string *error)
{
  Polygon ground;

  for (const auto &q : outline)
  {
    if (!representable(q.first) || !representable(q.second))
    {
      out.clear();
      if (error)
      {
        *error = "roof coordinate is outside the geometry grid";
      }
      return false;
    }
    ground.outer.push_back({toUnit(q.first), toUnit(q.second)});
  }

  return roofFromSkeleton(ground, options, out, error);
}


bool roofSummit(const std::vector<RoofTriangle> &triangles, double &x, double &z, double &height)
{
  bool any = false;
  height = 0;

  for (const RoofTriangle &triangle : triangles)
  {
    const double xs[3] = {triangle.ax, triangle.bx, triangle.cx};
    const double ys[3] = {triangle.ay, triangle.by, triangle.cy};
    const double zs[3] = {triangle.az, triangle.bz, triangle.cz};

    for (uint32_t k = 0; k < 3; ++k)
    {
      if (!any || ys[k] > height)
      {
        any = true;
        height = ys[k];
        x = xs[k];
        z = zs[k];
      }
    }
  }

  return any;
}

bool rectilinearReflex(const Polygon &polygon)
{
  const auto check = [](const Ring &ring, const bool outer) {
    if (ring.size() < 3)
    {
      return false;
    }

    // An outer ring is counter-clockwise and a hole clockwise, so the sign of
    // the turn that means "into the solid" is opposite between them.
    const bool wound = counterClockwise(ring);
    const int32_t reflexSign = (outer == wound) ? -1 : 1;

    for (size_t k = 0; k < ring.size(); ++k)
    {
      const Point &a = ring[(k + ring.size() - 1) % ring.size()];
      const Point &b = ring[k];
      const Point &c = ring[(k + 1) % ring.size()];

      const int turn = orientation(a, b, c);

      if (turn == 0)
      {
        continue;
      }

      if (turn != reflexSign)
      {
        continue;
      }

      // Reflex here, so the corner has to be square.
      const double ux = toMetres(b.x - a.x), uz = toMetres(b.y - a.y);
      const double vx = toMetres(c.x - b.x), vz = toMetres(c.y - b.y);
      const double dot = ux * vx + uz * vz;
      const double lengths = std::sqrt((ux * ux + uz * uz) * (vx * vx + vz * vz));

      if (lengths > 0 && std::abs(dot) / lengths > 1e-6)
      {
        return false;
      }
    }

    return true;
  };

  if (!check(polygon.outer, true))
  {
    return false;
  }

  for (const Ring &hole : polygon.holes)
  {
    if (!check(hole, false))
    {
      return false;
    }
  }

  return true;
}

} // namespace procedural::geom2
