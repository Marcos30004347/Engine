#include "procedural/world/hierarchy/Generators.hpp"

#include "procedural/geom2/Offset.hpp"
#include "procedural/geom2/Partition.hpp"

#include <functional>

#include <algorithm>
#include <cmath>

namespace procedural::world::hierarchy {

namespace {

constexpr double kPi = 3.14159265358979323846;

Point pointAt(const double x, const double y)
{
  return {geom2::toUnit(x), geom2::toUnit(y)};
}

struct Extent
{
  double minX = 0;
  double minY = 0;
  double maxX = 0;
  double maxY = 0;

  double width() const
  {
    return maxX - minX;
  }

  double height() const
  {
    return maxY - minY;
  }

  double centreX() const
  {
    return (minX + maxX) * .5;
  }

  double centreY() const
  {
    return (minY + maxY) * .5;
  }

  double diagonal() const
  {
    return std::hypot(width(), height());
  }
};

Extent extentOf(const Polygon &polygon)
{
  const geom2::Box box = geom2::bounds(polygon);

  return {geom2::toMetres(box.minX), geom2::toMetres(box.minY), geom2::toMetres(box.maxX), geom2::toMetres(box.maxY)};
}

Extent extentOf(const Region &region)
{
  return extentOf(region.boundary);
}

Edge widthed(const Point &a, const Point &b, const double halfWidth, const std::string &producer, const uint64_t feature)
{
  Edge edge;
  edge.path = {a, b};
  edge.roles.push_back({EdgeRole::Road, producer, feature});
  edge.bands.push_back({-halfWidth, halfWidth, Cover::Road, "carriageway"});

  return edge;
}

Edge cut(const Point &a, const Point &b, const std::string &producer, const uint64_t feature)
{
  Edge edge;
  edge.path = {a, b};
  edge.roles.push_back({EdgeRole::LotBoundary, producer, feature});

  return edge;
}

} // namespace

Descriptor GridStreets::descriptor() const
{
  return {"streets.grid", Role::Streets, Scope::Region, 1, true};
}

bool GridStreets::propose(const Request &request, std::vector<Edge> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (!(settings_.spacingMetres > 0.0) || !(settings_.halfWidthMetres > 0.0))
  {
    if (error != nullptr)
    {
      *error = "grid streets need a positive spacing and width";
    }

    return false;
  }

  const Extent extent = extentOf(request.region);
  const double reach = extent.diagonal();
  const double cx = extent.centreX();
  const double cy = extent.centreY();

  const double cosine = std::cos(settings_.axisRadians);
  const double sine = std::sin(settings_.axisRadians);

  uint64_t feature = 0;

  for (int axis = 0; axis < 2; ++axis)
  {
    const double dx = axis == 0 ? cosine : -sine;
    const double dy = axis == 0 ? sine : cosine;
    const double nx = -dy;
    const double ny = dx;

    const int steps = int(reach / settings_.spacingMetres);

    for (int k = -steps; k <= steps; ++k)
    {
      const double offset = double(k) * settings_.spacingMetres;
      const double px = cx + nx * offset;
      const double py = cy + ny * offset;

      out.push_back(widthed(pointAt(px - dx * reach, py - dy * reach), pointAt(px + dx * reach, py + dy * reach), settings_.halfWidthMetres, "streets.grid", ++feature));
    }
  }

  if (out.empty())
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "streets.grid: region smaller than one block");
  }

  return true;
}

Descriptor RadialStreets::descriptor() const
{
  return {"streets.radial", Role::Streets, Scope::Region, 1, true};
}

bool RadialStreets::propose(const Request &request, std::vector<Edge> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (settings_.spokes < 2)
  {
    if (error != nullptr)
    {
      *error = "radial streets need at least two spokes";
    }

    return false;
  }

  const Extent extent = extentOf(request.region);
  const double reach = extent.diagonal();
  const double cx = extent.centreX();
  const double cy = extent.centreY();

  for (uint32_t k = 0; k < settings_.spokes; ++k)
  {
    // Jittered by a purpose-addressed draw, not by a counter: shuffling the
    // work list cannot move a spoke.
    const double jitter = (request.context.unitRandom("streets.radial", request.region.id, 1, k) - .5) * .3;
    const double angle = 2.0 * kPi * double(k) / double(settings_.spokes) + jitter;

    out.push_back(widthed(pointAt(cx, cy), pointAt(cx + std::cos(angle) * reach, cy + std::sin(angle) * reach), settings_.halfWidthMetres, "streets.radial", k + 1));
  }

  // A ring road partway out, as a closed chain of widthed edges.
  const double radius = std::max(extent.width(), extent.height()) * .5 * settings_.ringFraction;

  if (radius > settings_.halfWidthMetres * 2.0)
  {
    const uint32_t segments = std::max(8u, settings_.spokes * 3);

    for (uint32_t k = 0; k < segments; ++k)
    {
      const double a0 = 2.0 * kPi * double(k) / double(segments);
      const double a1 = 2.0 * kPi * double(k + 1) / double(segments);

      out.push_back(widthed(pointAt(cx + std::cos(a0) * radius, cy + std::sin(a0) * radius), pointAt(cx + std::cos(a1) * radius, cy + std::sin(a1) * radius), settings_.halfWidthMetres, "streets.radial", 1000 + k));
    }
  }
  else
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "streets.radial: region too small for a ring road");
  }

  return true;
}

Descriptor StripLots::descriptor() const
{
  return {"lots.strip", Role::Lots, Scope::Region, 1, true};
}

bool StripLots::propose(const Request &request, std::vector<Edge> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (!(settings_.targetAreaMetres > 0.0))
  {
    if (error != nullptr)
    {
      *error = "strip lots need a positive target area";
    }

    return false;
  }

  const double area = request.region.areaMetres();

  if (area <= settings_.targetAreaMetres)
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "lots.strip: block already at target area");

    return true;
  }

  // Cuts in both directions, not one.
  //
  // The first version cut only across the block's longest axis, into
  // area / target strips. On a block much larger than one lot in both
  // directions that gives strips of enormous aspect - a 260 m block at a
  // 4,200 m2 target came out as sixteen lots 16 m wide and 260 m deep - which
  // the gallery render showed as hair-thin slivers. A lot has a frontage and a
  // depth, and both are bounded.
  //
  // So: depth from the target area at the frontage ratio, width from the
  // target area at that depth, and cuts at both spacings.
  const double depth = std::sqrt(settings_.targetAreaMetres / std::max(1.2, settings_.frontageRatio));
  const double width = settings_.targetAreaMetres / std::max(depth, 1.0);

  const Extent extent = extentOf(request.region);
  const bool alongX = extent.width() >= extent.height();
  const double reach = extent.diagonal();

  // Rows run across the long axis at the lot depth; lots divide each row along
  // it at the lot width.
  const double rowSpan = alongX ? extent.height() : extent.width();
  const double lotSpan = alongX ? extent.width() : extent.height();

  const int rows = std::max(1, int(std::llround(rowSpan / std::max(depth, 1.0))));
  const int lots = std::max(1, int(std::llround(lotSpan / std::max(width, 1.0))));

  uint64_t feature = 0;

  const auto cutAt = [&](const bool acrossX, const double t, const uint64_t id) {
    const double jitter = (request.context.unitRandom("lots.strip", request.region.id, acrossX ? 2 : 3, id) - .5) * settings_.jitter;

    if (acrossX)
    {
      const double x = extent.minX + extent.width() * std::min(.985, std::max(.015, t + jitter / double(lots + 1)));
      out.push_back(cut(pointAt(x, extent.minY - reach), pointAt(x, extent.maxY + reach), "lots.strip", id));
    }
    else
    {
      const double y = extent.minY + extent.height() * std::min(.985, std::max(.015, t + jitter / double(rows + 1)));
      out.push_back(cut(pointAt(extent.minX - reach, y), pointAt(extent.maxX + reach, y), "lots.strip", id));
    }
  };

  for (int k = 1; k < rows; ++k)
  {
    cutAt(!alongX, double(k) / double(rows), ++feature);
  }

  for (int k = 1; k < lots; ++k)
  {
    cutAt(alongX, double(k) / double(lots), ++feature);
  }

  if (out.empty())
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "lots.strip: block takes one lot whole");
  }

  return true;
}

Descriptor RingLots::descriptor() const
{
  return {"lots.ring", Role::Lots, Scope::Region, 1, true};
}

bool RingLots::propose(const Request &request, std::vector<Edge> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  // The court is the block copied inward by the lot depth. Built with the
  // offset library rather than by moving each edge by hand, so a block whose
  // corner is too sharp for a clean miter, or that is too narrow to hold a
  // court at all, is handled by the same code that handles every other offset.
  std::vector<Polygon> inner;

  if (!geom2::offsetPolygon(request.region.boundary, -settings_.depthMetres, {}, inner, error))
  {
    return false;
  }

  if (inner.empty())
  {
    ledger.add(LedgerKind::Rejected, request.region.id, "lots.ring: block too narrow to hold a court");

    return true;
  }

  uint64_t feature = 0;

  for (const auto &court : inner)
  {
    Edge ring;
    ring.path = court.outer;
    ring.path.push_back(court.outer.front()); // Closed.
    ring.roles.push_back({EdgeRole::LotBoundary, "lots.ring", ++feature});

    out.push_back(std::move(ring));
  }

  // One cut per lot, across the band only.
  //
  // Emitted from the court boundary outward rather than from the centre: a cut
  // that starts at the centre also crosses the court and stars it, which is
  // what the gallery render showed. The court is common ground and must come
  // out as one piece.
  //
  // The centre is the court's own centroid, not the block's bounding box: on a
  // block whose court sits off-centre the two are different, and using the box
  // fans the lots.
  const Polygon &court = inner.front();
  const double band = request.region.areaMetres() - geom2::areaMetres(court);
  const int lots = std::max(3, int(std::llround(band / std::max(settings_.targetAreaMetres, 1.0))));

  geom2::Point centre;

  if (!geom2::centroid(court, centre))
  {
    ledger.add(LedgerKind::Rejected, request.region.id, "lots.ring: the court has no centroid to cut from");

    return true;
  }

  const double cx = geom2::toMetres(centre.x);
  const double cy = geom2::toMetres(centre.y);
  const Extent extent = extentOf(request.region);
  const double reach = extent.diagonal();

  // Where a ray from the centre leaves a ring. Both rings are star shaped
  // about the court's centroid by construction - the court is an inward copy
  // of the block - so the first crossing is the only one.
  const auto rayExit = [&](const Ring &ring, const double angle, double &distance) {
    const double dx = std::cos(angle), dy = std::sin(angle);
    bool found = false;
    distance = 0;

    for (size_t i = 0; i < ring.size(); ++i)
    {
      const double ax = geom2::toMetres(ring[i].x) - cx, ay = geom2::toMetres(ring[i].y) - cy;
      const double bx = geom2::toMetres(ring[(i + 1) % ring.size()].x) - cx, by = geom2::toMetres(ring[(i + 1) % ring.size()].y) - cy;

      const double denominator = (bx - ax) * dy - (by - ay) * dx;

      if (std::abs(denominator) < 1e-12)
      {
        continue;
      }

      const double t = (ax * dy - ay * dx) / -denominator;
      const double along = (ax + (bx - ax) * t) * dx + (ay + (by - ay) * t) * dy;

      if (t >= 0 && t <= 1 && along > 0 && (!found || along < distance))
      {
        found = true;
        distance = along;
      }
    }

    return found;
  };

  for (int k = 0; k < lots; ++k)
  {
    const double angle = 2.0 * kPi * double(k) / double(lots);

    double innerReach = 0, outerReach = 0;

    if (!rayExit(court.outer, angle, innerReach) || !rayExit(request.region.boundary.outer, angle, outerReach) || outerReach <= innerReach)
    {
      continue;
    }

    // A whisker past each end, so the cut certainly meets both boundaries
    // rather than stopping a rounding error short of them.
    const double from = std::max(0.0, innerReach - .05);
    const double to = std::min(reach, outerReach + .05);

    out.push_back(cut(pointAt(cx + std::cos(angle) * from, cy + std::sin(angle) * from), pointAt(cx + std::cos(angle) * to, cy + std::sin(angle) * to), "lots.ring", 100 + uint64_t(k)));
  }

  return true;
}

Descriptor BisectionLots::descriptor() const
{
  return {"lots.bisection", Role::Lots, Scope::Region, 1, true};
}

// The town generator's ward table, quoted. `r * r` rather than `r` is not a
// typo: squaring a uniform draw biases every size range toward its small end,
// which is why most of a quarter is modest and a few plots are not.
BisectionLots::Settings BisectionLots::craftsmen()
{
  return {10 * 6.25, .6, .6, .04, 1.5, 12}; // small to large, moderately regular
}

BisectionLots::Settings BisectionLots::merchant()
{
  return {50 * 6.25, .65, .7, .15, 1.5, 12}; // medium to large, many yards
}

BisectionLots::Settings BisectionLots::patriciate()
{
  return {80 * 6.25, .65, .8, .2, 1.5, 12}; // large plots, a fifth left as garden
}

BisectionLots::Settings BisectionLots::administration()
{
  return {80 * 6.25, .25, .3, .04, 1.5, 12}; // large and deliberately regular
}

BisectionLots::Settings BisectionLots::slum()
{
  return {10 * 6.25, .8, .8, .03, 1.5, 12}; // small and chaotic
}

bool BisectionLots::propose(const Request &request, std::vector<Edge> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (!(settings_.minimumAreaMetres > 0))
  {
    if (error != nullptr)
    {
      *error = "bisection lots need a positive minimum area";
    }

    return false;
  }

  const RegionId region = request.region.id;
  const Context &context = request.context;

  uint64_t draws = 0;

  // One stream, advanced per draw, so the recursion is reproducible without
  // depending on the order two sibling branches happen to be visited in.
  const auto random = [&]() {
    return context.unitRandom("lots.bisection", region, 7, draws++);
  };

  uint32_t cuts = 0;

  // Watabou recurses on polygons and keeps the leaves. We recurse the same way
  // but keep the *cuts*, because in this model the child regions are derived
  // from the edges rather than authored - the arrangement rebuilds exactly the
  // same pieces from the same chords.
  const std::function<void(const Polygon &, uint32_t, bool)> bisect = [&](const Polygon &piece, const uint32_t depth, const bool split) {
    const double area = geom2::areaMetres(piece);

    // Rule 4: the stopping size is log-uniform about the target.
    const double stop = settings_.minimumAreaMetres * std::pow(2.0, 4.0 * settings_.sizeChaos * (random() - .5));

    if (depth >= settings_.maximumDepth || area < stop || piece.outer.size() < 3)
    {
      return;
    }

    // Rule 1: the longest edge, and its start vertex.
    size_t longest = 0;
    double best = -1;

    for (size_t i = 0; i < piece.outer.size(); ++i)
    {
      const geom2::Point &a = piece.outer[i];
      const geom2::Point &b = piece.outer[(i + 1) % piece.outer.size()];
      const double length = std::hypot(geom2::toMetres(b.x - a.x), geom2::toMetres(b.y - a.y));

      if (length > best)
      {
        best = length;
        longest = i;
      }
    }

    const geom2::Point &v0 = piece.outer[longest];
    const geom2::Point &v1 = piece.outer[(longest + 1) % piece.outer.size()];

    // Rule 2: where along it.
    const double spread = .8 * settings_.gridChaos;
    const double ratio = (1 - spread) / 2 + random() * spread;

    // Rule 3: how far off perpendicular - nothing once the piece is small.
    const double angleSpread = (kPi / 6) * settings_.gridChaos * (area < settings_.minimumAreaMetres * 4 ? 0.0 : 1.0);
    const double angle = (random() - .5) * angleSpread;

    const double ax = geom2::toMetres(v0.x), ay = geom2::toMetres(v0.y);
    const double dx = geom2::toMetres(v1.x) - ax, dy = geom2::toMetres(v1.y) - ay;

    const double px = ax + dx * ratio, py = ay + dy * ratio;

    // The cut direction: the edge, turned by the jitter, then rotated a
    // quarter turn - so at zero jitter it is exactly perpendicular.
    const double cosine = std::cos(angle), sine = std::sin(angle);
    const double ex = dx * cosine - dy * sine, ey = dy * cosine + dx * sine;

    double nx = -ey, ny = ex;
    const double length = std::hypot(nx, ny);

    if (!(length > 1e-9))
    {
      return;
    }

    nx /= length;
    ny /= length;

    // The cut is a *chord of this piece*, not a line across the block.
    //
    // Emitting the full-width line is what an earlier version did, and the
    // gallery showed the result: a cut made deep in the recursion sliced every
    // other lot it happened to cross, so a ward with a small minimum area came
    // out as a web of lines rather than nested subdivision. Watabou's
    // Polygon.cut finds the two edges the line crosses; so does this.
    double entry = 0, exit = 0;
    int crossings = 0;

    for (size_t i = 0; i < piece.outer.size() && crossings < 2; ++i)
    {
      const geom2::Point &e0 = piece.outer[i];
      const geom2::Point &e1 = piece.outer[(i + 1) % piece.outer.size()];

      const double ex0 = geom2::toMetres(e0.x), ey0 = geom2::toMetres(e0.y);
      const double sx = geom2::toMetres(e1.x) - ex0, sy = geom2::toMetres(e1.y) - ey0;

      const double denominator = nx * sy - ny * sx;

      if (std::abs(denominator) < 1e-12)
      {
        continue;
      }

      // Where along this edge, and how far along the cut direction.
      //
      // Solving P + t*N = E0 + u*S by Cramer: with
      // denominator = nx*sy - ny*sx and D = E0 - P,
      //   u = (D.x*ny - D.y*nx) / denominator
      // and t is then the projection of (E0 + u*S - P) on N, which is exact
      // because N is a unit vector.
      const double u = ((ex0 - px) * ny - (ey0 - py) * nx) / denominator;

      if (u < 0 || u > 1)
      {
        continue;
      }

      const double t = ((ex0 - px) + sx * u) * nx + ((ey0 - py) + sy * u) * ny;

      (crossings++ == 0 ? entry : exit) = t;
    }

    if (crossings < 2)
    {
      return;
    }

    if (exit < entry)
    {
      std::swap(entry, exit);
    }

    // A whisker past each end, so the chord certainly meets both edges rather
    // than stopping a rounding error short of them.
    entry -= .05;
    exit += .05;

    const geom2::Point from = pointAt(px + nx * entry, py + ny * entry);
    const geom2::Point to = pointAt(px + nx * exit, py + ny * exit);

    Edge edge;
    edge.path = {from, to};
    edge.roles.push_back({EdgeRole::LotBoundary, "lots.bisection", ++cuts});

    // Rule 5: an alley only while the group is large.
    if (split)
    {
      edge.bands.push_back({-settings_.alleyMetres * .5, settings_.alleyMetres * .5, Cover::Road, "alley"});
    }

    out.push_back(std::move(edge));

    // Recurse on the two halves the cut actually makes, so the sizes the rules
    // test are the sizes the arrangement will produce.
    std::vector<Polygon> halves;
    std::string cutError;

    if (!geom2::cutPolygon(piece, from, to, halves, &cutError) || halves.size() < 2)
    {
      return;
    }

    for (const auto &half : halves)
    {
      // Watabou's own condition, verbatim: the next level splits with an alley
      // only while the half is large relative to a product of two draws, which
      // is usually small - so alleys separate groups, not neighbours.
      const double threshold = settings_.minimumAreaMetres / std::max(1e-6, random() * random());

      bisect(half, depth + 1, geom2::areaMetres(half) > threshold);
    }
  };

  bisect(request.region.boundary, 0, true);

  if (out.empty())
  {
    ledger.add(LedgerKind::Omitted, region, "lots.bisection: block already at target size");
  }

  return true;
}

Descriptor HouseFootprints::descriptor() const
{
  return {"buildings.setback", Role::Buildings, Scope::Region, 1, true};
}

bool HouseFootprints::populate(const Request &request, std::vector<Object> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (request.region.cover == Cover::Road || request.region.cover == Cover::Water)
  {
    // Not a mistake to skip: a road is a region like any other and simply
    // holds no houses.
    return true;
  }

  if (request.region.areaMetres() < settings_.minimumAreaMetres)
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "buildings.setback: lot below the minimum area");

    return true;
  }

  // The setback is a minimum, not the whole story: insetting a large lot by a
  // metre and a half leaves a building the size of the lot. A house covers a
  // fraction of its plot and the rest is yard, so the inset grows until the
  // shell is at or below the coverage the settings allow.
  //
  // The gallery render is what showed this: every lot filled edge to edge,
  // which no settlement looks like.
  const double lotArea = request.region.areaMetres();
  double inset = settings_.setbackMetres;

  if (settings_.maximumCoverage > 0 && settings_.maximumCoverage < 1)
  {
    // For a roughly rectangular lot, insetting by d removes about
    // perimeter * d; solving for the target area gives a good first guess, and
    // the loop below corrects it.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
      std::vector<Polygon> trial;

      if (!geom2::offsetPolygon(request.region.boundary, -inset, {}, trial, error))
      {
        return false;
      }

      double covered = 0;

      for (const auto &piece : trial)
      {
        covered += geom2::areaMetres(piece);
      }

      if (covered <= lotArea * settings_.maximumCoverage || covered <= settings_.minimumAreaMetres)
      {
        break;
      }

      inset += std::max(.4, inset * .5);
    }
  }

  std::vector<Polygon> shells;

  if (!geom2::offsetPolygon(request.region.boundary, -inset, {}, shells, error))
  {
    return false;
  }

  if (shells.empty())
  {
    ledger.add(LedgerKind::Rejected, request.region.id, "buildings.setback: nothing left after the setback");

    return true;
  }

  for (auto &shell : shells)
  {
    if (geom2::areaMetres(shell) < settings_.minimumAreaMetres)
    {
      continue;
    }

    const double draw = request.context.unitRandom("buildings.setback", request.region.id, 3, 0);
    const uint32_t storeys = draw < .7 ? 1u : (draw < .95 ? 2u : 3u);

    Object object;
    object.kind = "house";
    object.heightMetres = settings_.storeyMetres * double(storeys);
    object.anchor = shell.outer.front();
    object.footprint = std::move(shell);

    out.push_back(std::move(object));
  }

  return true;
}

Descriptor SlopeField::descriptor() const
{
  return {"terrain.slope", Role::Terrain, Scope::Region, 1, true};
}

bool SlopeField::evaluate(const RegionTree &, const Context &context, std::vector<Region *> &regions, Ledger &ledger, std::string *)
{
  if (!context.terrain)
  {
    for (auto *region : regions)
    {
      if (region != nullptr)
      {
        ledger.add(LedgerKind::Omitted, region->id, "terrain.slope: no terrain supplied");
      }
    }

    return true;
  }

  for (auto *region : regions)
  {
    if (region == nullptr || region->boundary.outer.size() < 3)
    {
      continue;
    }

    // Sampled at the ring vertices, which is enough to say whether a region is
    // buildable without meshing it.
    double lowest = 0;
    double highest = 0;
    bool first = true;

    for (const auto &p : region->boundary.outer)
    {
      const double height = context.terrain(geom2::toMetres(p.x), geom2::toMetres(p.y));

      if (first || height < lowest)
      {
        lowest = height;
      }

      if (first || height > highest)
      {
        highest = height;
      }

      first = false;
    }

    const geom2::Box box = geom2::bounds(region->boundary);
    const double span = std::max(1.0e-6, std::hypot(geom2::toMetres(box.maxX - box.minX), geom2::toMetres(box.maxY - box.minY)));

    region->attributes.elevationMetres = (lowest + highest) * .5;
    region->attributes.slope = (highest - lowest) / span;
    region->attributes.buildable = region->attributes.slope <= settings_.unbuildableSlope;

    if (!region->attributes.buildable)
    {
      ledger.add(LedgerKind::Capacity, region->id, "terrain.slope: too steep to build on");
    }
  }

  return true;
}

} // namespace procedural::world::hierarchy
