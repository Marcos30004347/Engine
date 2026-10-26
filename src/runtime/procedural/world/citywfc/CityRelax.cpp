#include "procedural/world/citywfc/CityRelax.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace procedural::world::citywfc {
namespace {

using geom2::kNoSide;
using geom2::sideOf;
using geom2::toMetres;
using geom2::toUnit;

struct Vec2
{
  double x = 0, y = 0;
};

// The smallest turn a cell corner may have: about one and a quarter degrees.
// Far below anything the quality gate accepts, and far enough above zero that
// quantising the coordinates cannot turn the corner inside out.
constexpr double kMinimumCornerSine = .022;

// Quarter turns, exactly: no trigonometry, and no rounding beyond what the
// coordinates already carry.
Vec2 turn(const Vec2 &v, const uint32_t quarters)
{
  switch (quarters & 3u)
  {
  case 1:
    return {-v.y, v.x};
  case 2:
    return {-v.x, -v.y};
  case 3:
    return {v.y, -v.x};
  default:
    return v;
  }
}

// The closest-square fit, with the size optionally pulled toward a target.
//
// `v` points at corner zero of the ideal square and its length is that square's
// circumradius, so rescaling it is how the size is steered: a square of side s
// has circumradius s / sqrt 2.
Vec2 squareOffset(const Vec2 corner[4], const Vec2 &centre, const double wantedRadius, const double areaWeight)
{
  Vec2 v{0, 0};

  for (uint32_t i = 0; i < 4; ++i)
  {
    const Vec2 rotated = turn({corner[i].x - centre.x, corner[i].y - centre.y}, 4 - i);
    v.x += rotated.x * .25;
    v.y += rotated.y * .25;
  }

  if (areaWeight <= 0 || wantedRadius <= 0)
  {
    return v;
  }

  const double lengthSquared = v.x * v.x + v.y * v.y;

  if (lengthSquared <= 0)
  {
    return v;
  }

  const double scale = (1.0 - areaWeight) + areaWeight * wantedRadius / std::sqrt(lengthSquared);

  return {v.x * scale, v.y * scale};
}

// The nearest point of the outline to q, over every ring, and how far it is.
// Squared distances throughout; the caller compares against a squared radius.
struct Projection
{
  Vec2 point;
  double distanceSquared = std::numeric_limits<double>::max();
};

Projection nearestOnRegion(const Polygon &region, const Vec2 &q)
{
  Projection best;

  const auto against = [&](const geom2::Ring &ring) {
    for (size_t i = 0; i < ring.size(); ++i)
    {
      const geom2::Point &pa = ring[i];
      const geom2::Point &pb = ring[(i + 1) % ring.size()];

      const double ax = toMetres(pa.x), ay = toMetres(pa.y);
      const double dx = toMetres(pb.x) - ax, dy = toMetres(pb.y) - ay;
      const double denominator = dx * dx + dy * dy;

      double t = 0;

      if (denominator > 0)
      {
        t = std::clamp(((q.x - ax) * dx + (q.y - ay) * dy) / denominator, 0.0, 1.0);
      }

      const Vec2 on{ax + dx * t, ay + dy * t};
      const double ex = q.x - on.x, ey = q.y - on.y;
      const double distance = ex * ex + ey * ey;

      if (distance < best.distanceSquared)
      {
        best.distanceSquared = distance;
        best.point = on;
      }
    }
  };

  against(region.outer);

  for (const geom2::Ring &hole : region.holes)
  {
    against(hole);
  }

  return best;
}

// The state a guarded relaxation needs: positions in metres, which faces each
// vertex touches, and which vertices are on the mesh boundary.
struct Working
{
  std::vector<Vec2> position;
  std::vector<std::vector<uint32_t>> facesOfVertex;
  std::vector<bool> onBoundary;
  std::vector<double> areaBefore;
  double minimumArea = 0;
  uint32_t refused = 0;

  const QuadMesh *mesh = nullptr;

  void attach(const QuadMesh &m)
  {
    mesh = &m;
    position.resize(m.vertices.size());
    facesOfVertex.assign(m.vertices.size(), {});
    onBoundary.assign(m.vertices.size(), false);

    for (size_t v = 0; v < m.vertices.size(); ++v)
    {
      position[v] = {toMetres(m.vertices[v].x), toMetres(m.vertices[v].y)};
    }

    for (uint32_t f = 0; f < m.faces(); ++f)
    {
      for (uint32_t i = 0; i < 4; ++i)
      {
        const uint32_t side = sideOf(f, i);
        facesOfVertex[m.sides[side].origin].push_back(f);

        if (m.sides[side].twin == kNoSide)
        {
          onBoundary[m.sides[side].origin] = true;
          onBoundary[m.destination(side)] = true;
        }
      }
    }
  }

  // Whether a quad is a usable cell: simple, and wound counter-clockwise.
  //
  // Positive area alone is not enough, and the gap is not theoretical - a
  // bowtie has two corners turning each way and its two lobes can leave the
  // total positive, so a guard that checks only the sign lets through exactly
  // the shape it exists to prevent. For a quadrilateral the complete test is
  // cheap: positive area, and at most one corner turning the other way.
  // The usable area of a cell: its own area if it is a cell at all, and zero if
  // it is not. Zero rather than a negative number, so that a caller comparing
  // against "the area before the move" cannot be fooled by a sign.
  //
  // Three ways a quad stops being a cell, and the third is the one that cost a
  // day. It can wind backwards. It can be a bowtie - two corners turning each
  // way, whose two lobes can leave the total area positive, so the sign alone
  // lets through exactly the shape this guard exists to prevent. And it can
  // grow a spike: two edges at one corner turning back along each other, which
  // in floating point is a turn of plus or minus a billionth and reads as a
  // perfectly ordinary concave corner.
  //
  // That last one matters because this guard judges in doubles what
  // `quadMeshFault` validates in integers. A corner whose turn is 1e-9 is
  // accepted here and is a self-intersection once the coordinates are
  // quantised. So the test is not "does this corner turn the right way" but
  // "does it turn by a measurable amount" - the cross product against the
  // product of the two edge lengths, which is the sine of the corner. Well
  // below anything the quality gate would accept, and far enough from zero that
  // rounding cannot cross it.
  double areaOf(const uint32_t f) const
  {
    Vec2 c[4];

    for (uint32_t i = 0; i < 4; ++i)
    {
      c[i] = position[mesh->sides[sideOf(f, i)].origin];
    }

    double twice = 0;
    uint32_t reflex = 0;

    for (uint32_t i = 0; i < 4; ++i)
    {
      const Vec2 &a = c[i];
      const Vec2 &b = c[(i + 1) % 4];
      const Vec2 &d = c[(i + 2) % 4];

      twice += a.x * b.y - b.x * a.y;

      const double ux = b.x - a.x, uy = b.y - a.y;
      const double vx = d.x - b.x, vy = d.y - b.y;
      const double cross = ux * vy - uy * vx;
      const double scale = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));

      if (scale <= 0 || std::abs(cross) < kMinimumCornerSine * scale)
      {
        return 0.0;
      }

      if (cross < 0)
      {
        ++reflex;
      }
    }

    return reflex <= 1 ? twice * .5 : 0.0;
  }

  // Takes the move if every face the vertex touches stays a usable cell,
  // halving it until it does and refusing it if it never does.
  //
  // This is the whole fold-prevention story. With it, no parameterisation of
  // the outline is needed and a boundary vertex can simply be told where to go.
  void move(const uint32_t v, const Vec2 &wanted)
  {
    const Vec2 before = position[v];

    areaBefore.clear();

    for (const uint32_t f : facesOfVertex[v])
    {
      areaBefore.push_back(areaOf(f));
    }

    for (uint32_t attempt = 0; attempt < 6; ++attempt)
    {
      const double share = 1.0 / double(1u << attempt);
      position[v] = {before.x + (wanted.x - before.x) * share, before.y + (wanted.y - before.y) * share};

      bool usable = true;

      for (size_t k = 0; k < facesOfVertex[v].size(); ++k)
      {
        const double after = areaOf(facesOfVertex[v][k]);

        // Above the floor, or at least no worse than it already was. The second
        // clause is what keeps a cell that starts out thin from freezing every
        // vertex around it.
        usable = usable && after > 0 && (after >= minimumArea || after >= areaBefore[k]);
      }

      if (usable)
      {
        return;
      }
    }

    position[v] = before;
    ++refused;
  }

  void writeBack(QuadMesh &m) const
  {
    for (size_t v = 0; v < position.size(); ++v)
    {
      m.vertices[v] = {toUnit(position[v].x), toUnit(position[v].y)};
    }
  }
};

void snapBoundary(Working &work, const Polygon &region, const double snapRadius)
{
  if (snapRadius <= 0)
  {
    return;
  }

  const double radiusSquared = snapRadius * snapRadius;

  for (uint32_t v = 0; v < uint32_t(work.position.size()); ++v)
  {
    if (!work.onBoundary[v])
    {
      continue;
    }

    const Projection near = nearestOnRegion(region, work.position[v]);

    if (near.distanceSquared > radiusSquared)
    {
      continue;
    }

    work.move(v, near.point);
  }
}

} // namespace

bool snapBoundaryToRegion(QuadMesh &mesh, const Polygon &region, const double snapRadiusMetres, const double minimumAreaMetres, uint32_t *refused)
{
  Working work;
  work.attach(mesh);
  work.minimumArea = minimumAreaMetres;

  snapBoundary(work, region, snapRadiusMetres);
  work.writeBack(mesh);

  if (refused)
  {
    *refused = work.refused;
  }

  return true;
}

bool relaxQuadMesh(QuadMesh &mesh, const Polygon &region, const CityRelaxSettings &settings, CityRelaxReport &report, std::string *error)
{
  report = {};

  if (mesh.faces() == 0)
  {
    if (error)
    {
      *error = "nothing to relax";
    }

    return false;
  }

  Working work;
  work.attach(mesh);
  work.minimumArea = settings.minimumAreaFraction * settings.targetEdgeMetres * settings.targetEdgeMetres;

  // A square of side s has circumradius s / sqrt 2.
  const double wantedRadius = settings.targetEdgeMetres * 0.70710678118654752440;

  std::vector<Vec2> accumulated(mesh.vertices.size());
  std::vector<uint32_t> incident(mesh.vertices.size());

  const auto residualOf = [&]() {
    double total = 0;

    for (uint32_t f = 0; f < mesh.faces(); ++f)
    {
      Vec2 corner[4];
      Vec2 centre{0, 0};

      for (uint32_t i = 0; i < 4; ++i)
      {
        corner[i] = work.position[mesh.sides[sideOf(f, i)].origin];
        centre.x += corner[i].x * .25;
        centre.y += corner[i].y * .25;
      }

      const Vec2 v = squareOffset(corner, centre, wantedRadius, settings.areaWeight);

      for (uint32_t i = 0; i < 4; ++i)
      {
        const Vec2 ideal = turn(v, i);
        const double dx = corner[i].x - centre.x - ideal.x;
        const double dy = corner[i].y - centre.y - ideal.y;
        total += dx * dx + dy * dy;
      }
    }

    return total;
  };

  report.initialResidual = residualOf();
  report.residuals.push_back(report.initialResidual);

  double previous = report.initialResidual;

  for (uint32_t pass = 0; pass < settings.iterations; ++pass)
  {
    std::fill(accumulated.begin(), accumulated.end(), Vec2{0, 0});
    std::fill(incident.begin(), incident.end(), 0u);

    for (uint32_t f = 0; f < mesh.faces(); ++f)
    {
      uint32_t index[4];
      Vec2 corner[4];
      Vec2 centre{0, 0};

      for (uint32_t i = 0; i < 4; ++i)
      {
        index[i] = mesh.sides[sideOf(f, i)].origin;
        corner[i] = work.position[index[i]];
        centre.x += corner[i].x * .25;
        centre.y += corner[i].y * .25;
      }

      const Vec2 v = squareOffset(corner, centre, wantedRadius, settings.areaWeight);

      for (uint32_t i = 0; i < 4; ++i)
      {
        const Vec2 ideal = turn(v, i);
        accumulated[index[i]].x += centre.x + ideal.x;
        accumulated[index[i]].y += centre.y + ideal.y;
        ++incident[index[i]];
      }
    }

    // In index order and against the array as it evolves, so a later vertex is
    // checked against where its neighbours have actually gone. Index order is
    // fixed, so this is Gauss-Seidel without being traversal-dependent.
    for (uint32_t v = 0; v < uint32_t(work.position.size()); ++v)
    {
      if (incident[v] == 0)
      {
        continue;
      }

      const double inverse = 1.0 / double(incident[v]);
      const Vec2 mean{accumulated[v].x * inverse, accumulated[v].y * inverse};

      work.move(v, {work.position[v].x + settings.lambda * (mean.x - work.position[v].x),
                    work.position[v].y + settings.lambda * (mean.y - work.position[v].y)});
    }

    snapBoundary(work, region, settings.snapRadiusMetres);

    const double residual = residualOf();
    report.residuals.push_back(residual);
    report.monotonic = report.monotonic && residual <= previous * (1.0 + 1e-9);
    previous = residual;
    report.iterations = pass + 1;

    if (report.initialResidual > 0 && residual <= report.initialResidual * settings.convergenceRatio)
    {
      break;
    }
  }

  report.finalResidual = previous;
  report.refusedMoves = work.refused;
  work.writeBack(mesh);

  return true;
}

} // namespace procedural::world::citywfc
