#include "PlotPlanner.hpp"

#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Offset.hpp"
#include "procedural/geom2/Partition.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/geom2/Quantise.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

using rewrite::Position;

namespace {

geom2::Polygon toPolygon(const std::vector<Position> &ring)
{
  geom2::Polygon p;

  for (const Position &q : ring)
  {
    p.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
  }

  return p;
}

std::vector<Position> toRing(const geom2::Ring &ring, const double y = 0)
{
  std::vector<Position> out;

  for (const geom2::Point &q : ring)
  {
    out.push_back(Position{geom2::toMetres(q.x), y, geom2::toMetres(q.y)});
  }

  return out;
}

PlotGround toGround(const geom2::Polygon &piece)
{
  PlotGround ground;
  ground.outer = toRing(piece.outer);

  for (const geom2::Ring &hole : piece.holes)
  {
    ground.holes.push_back(toRing(hole));
  }

  return ground;
}

const geom2::Polygon *largest(const std::vector<geom2::Polygon> &pieces)
{
  const geom2::Polygon *best = nullptr;

  for (const geom2::Polygon &piece : pieces)
  {
    if (best == nullptr || geom2::areaMetres(piece) > geom2::areaMetres(*best))
    {
      best = &piece;
    }
  }

  return best;
}

double total(const std::vector<geom2::Polygon> &pieces)
{
  double sum = 0;

  for (const geom2::Polygon &piece : pieces)
  {
    sum += geom2::areaMetres(piece);
  }

  return sum;
}

// A rectangle in a local frame (u along the street, v away from it).
struct Rect
{
  double u0, v0, u1, v1;
};

} // namespace

void describeStanding(PlotBrief &b)
{
  switch (b.standing)
  {
  case PlotStanding::Slum:
    b.roofStyle = RoofStyle::Gabled;
    b.houseShare = .78;
    b.gardenShare = 0;
    // Two floors, not one. A slum is dense because it builds upward on a
    // small plot; a quarter of single-storey cottages packed to their
    // boundaries is a row of sheds, and with one floor there is nothing for a
    // jetty to oversail or an arcade to stand under either.
    b.minimumStoreys = 2;
    b.maximumStoreys = 3;
    b.outbuildings = 0;
    b.fenced = false;
    b.setbackMetres = .3;
    b.jettyMetres = .30;
    b.arcadeDepthMetres = 0;
    b.arcadeChance = 0;
    break;

  case PlotStanding::Modest:
    b.roofStyle = RoofStyle::Gabled;
    b.houseShare = .62;
    b.gardenShare = 0;
    b.minimumStoreys = 2;
    b.maximumStoreys = 3;
    b.outbuildings = 0;
    b.fenced = false;
    b.setbackMetres = .6;
    b.jettyMetres = .26;
    b.arcadeDepthMetres = 1.6;
    b.arcadeChance = .18;
    break;

  case PlotStanding::Comfortable:
    b.roofStyle = RoofStyle::CrossGabled;
    b.houseShare = .48;
    b.gardenShare = .5;
    b.minimumStoreys = 2;
    b.maximumStoreys = 2;
    b.outbuildings = 1;
    b.fenced = false;
    b.setbackMetres = 1.0;
    b.jettyMetres = .22;
    b.arcadeDepthMetres = 1.9;
    b.arcadeChance = .30;
    break;

  case PlotStanding::Wealthy:
    b.roofStyle = RoofStyle::CrossGabled;
    b.houseShare = .34;
    b.gardenShare = .7;
    b.minimumStoreys = 2;
    b.maximumStoreys = 3;
    b.outbuildings = 2;
    b.fenced = false;
    b.setbackMetres = 1.5;
    b.jettyMetres = .12;
    b.arcadeDepthMetres = 2.1;
    b.arcadeChance = .25;
    break;

  case PlotStanding::Grand:
    b.roofStyle = RoofStyle::Hipped;
    b.houseShare = .26;
    b.gardenShare = .8;
    b.minimumStoreys = 2;
    b.maximumStoreys = 3;
    b.outbuildings = 3;
    b.fenced = false;
    b.setbackMetres = 2.0;
    b.jettyMetres = 0;
    b.arcadeDepthMetres = 2.4;
    b.arcadeChance = .45;
    break;

  case PlotStanding::Rural:
    b.roofStyle = RoofStyle::Gabled;
    b.houseShare = .22;
    b.gardenShare = 0;
    b.minimumStoreys = 1;
    b.maximumStoreys = 2;
    b.outbuildings = 1;
    b.fenced = true;
    b.setbackMetres = 1.0;
    b.jettyMetres = 0;
    b.arcadeDepthMetres = 0;
    b.arcadeChance = 0;
    break;
  }
}

bool planPlot(const PlotBrief &b, const ComplexDwellingSettings &settings, PlotPlan &out, std::string *error)
{
  // The standing's roof, not the caller's default. Everything else in the
  // settings is how a building of this age is put together; this one is how a
  // building of this standing is, and the brief is where standing lives.
  ComplexDwellingSettings s = settings;
  s.roofStyle = b.roofStyle;

  out = PlotPlan{};

  if (b.lot.size() < 3)
  {
    out.note = "the lot has no outline";

    return true;
  }

  geom2::Polygon lot = toPolygon(b.lot);

  for (const auto &hole : b.holes)
  {
    if (hole.size() >= 3)
    {
      lot.holes.push_back(toPolygon(hole).outer);
    }
  }

  out.lotArea = geom2::areaMetres(lot);

  if (!geom2::polygonFault(lot).empty())
  {
    if (error != nullptr)
    {
      *error = "lot outline: " + geom2::polygonFault(lot);
    }

    return false;
  }

  // ---- the street ----------------------------------------------------------
  //
  // The longest frontage edge is the street the house faces. With none given,
  // the lot's longest edge stands in: a lot drawn on its own is on a lane.
  double ux = 1, uz = 0, fx = 0, fz = 0, longest = -1;

  const auto consider = [&](const double ax, const double az, const double bx, const double bz) {
    const double length = std::hypot(bx - ax, bz - az);

    if (length > longest)
    {
      longest = length;
      ux = (bx - ax) / length;
      uz = (bz - az) / length;
      fx = (ax + bx) * .5;
      fz = (az + bz) * .5;
    }
  };

  for (const auto &edge : b.frontage)
  {
    consider(edge.first.x, edge.first.z, edge.second.x, edge.second.z);
  }

  if (longest < 0)
  {
    for (size_t i = 0; i < b.lot.size(); ++i)
    {
      const Position &p = b.lot[i], &q = b.lot[(i + 1) % b.lot.size()];
      consider(p.x, p.z, q.x, q.z);
    }
  }

  // Inward from the street: the side of the frontage the lot's centre is on.
  geom2::Point centre;
  geom2::centroid(lot, centre);

  double nx = -uz, nz = ux;

  if ((geom2::toMetres(centre.x) - fx) * nx + (geom2::toMetres(centre.y) - fz) * nz < 0)
  {
    nx = -nx;
    nz = -nz;
  }

  // ---- the ground the house may stand on -------------------------------
  std::vector<geom2::Polygon> inset;

  if (!geom2::offsetPolygon(lot, -b.setbackMetres, {}, inset, error))
  {
    return false;
  }

  if (b.frontSetbackMetres > 0 && !inset.empty())
  {
    // Cut a strip off the street side.
    const geom2::Polygon *ground = largest(inset);
    const double reach = std::sqrt(out.lotArea) * 4 + longest;
    const double cut = b.frontSetbackMetres;

    std::vector<geom2::Polygon> parts;
    const geom2::Point a{geom2::toUnit(fx + nx * cut - ux * reach), geom2::toUnit(fz + nz * cut - uz * reach)};
    const geom2::Point c{geom2::toUnit(fx + nx * cut + ux * reach), geom2::toUnit(fz + nz * cut + uz * reach)};

    if (geom2::cutPolygon(*ground, a, c, parts, nullptr) && parts.size() >= 2)
    {
      std::vector<geom2::Polygon> behind;

      for (geom2::Polygon &part : parts)
      {
        geom2::Point pc;
        geom2::centroid(part, pc);

        if ((geom2::toMetres(pc.x) - fx) * nx + (geom2::toMetres(pc.y) - fz) * nz > cut)
        {
          behind.push_back(std::move(part));
        }
      }

      if (!behind.empty())
      {
        inset = std::move(behind);
      }
    }
  }

  const geom2::Polygon *ground = largest(inset);

  if (ground == nullptr || geom2::areaMetres(*ground) < s.minimumRoomMetres * 2)
  {
    out.note = "nothing left to build on after the setback";

    return true;
  }

  // ---- the house -----------------------------------------------------------
  //
  // Composed on the buildable ground, first wing on the street, covering the
  // share of the *lot* the standing says. The share is re-expressed against
  // the buildable ground, which is what the composer sees.
  MassingBrief massing;
  massing.plot = toRing(ground->outer);

  for (const geom2::Ring &hole : ground->holes)
  {
    massing.holes.push_back(toRing(hole));
  }

  massing.seed = b.seed ^ (b.id * 0x9E3779B97F4A7C15ull);
  massing.coverage = std::min(.95, std::max(.15, b.houseShare * out.lotArea / geom2::areaMetres(*ground)));
  massing.foundation = b.foundationMetres;
  massing.floorRise = b.storeyMetres;
  massing.minimumStoreys = b.minimumStoreys;
  massing.maximumStoreys = std::max(b.minimumStoreys, b.maximumStoreys);
  massing.axisX = ux;
  massing.axisZ = uz;
  massing.anchored = true;
  massing.anchorX = fx;
  massing.anchorZ = fz;
  massing.jettyMetres = b.jettyMetres;

  // An arcade is drawn for rather than given to every plot of a standing: a
  // street where every ground floor is a colonnade is a cloister, not a town.
  {
    uint64_t bits = massing.seed * 6364136223846793005ull + 1442695040888963407ull;
    bits ^= bits >> 33;
    bits *= 0xff51afd7ed558ccdull;
    bits ^= bits >> 29;

    if (double(bits >> 11) / 9007199254740992.0 < b.arcadeChance)
    {
      massing.arcadeDepthMetres = b.arcadeDepthMetres;
    }
  }

  massComposedBuilding(massing, s, out.built.wings, out.built.roofs, &out.built.levels, &out.built.pillars);

  if (out.built.wings.empty())
  {
    out.note = "the ground is too small for a wing";

    return true;
  }

  std::vector<geom2::Operand> operands;

  for (const rewrite::DwellingWing &w : out.built.wings)
  {
    operands.push_back(geom2::toOperand(toPolygon(w.footprint)));
    out.storeys = std::max(out.storeys, w.storeys);
  }

  std::vector<geom2::Polygon> merged;

  if (!geom2::unionOf(operands, merged, error) || merged.empty())
  {
    return false;
  }

  const geom2::Polygon house = *largest(merged);
  out.house = toRing(house.outer);

  for (const geom2::Ring &court : house.holes)
  {
    out.courts.push_back(toRing(court));
  }

  out.houseArea = geom2::areaMetres(house);

  // The front door: the point of the outline nearest the street.
  {
    double best = 1e300;

    for (size_t i = 0; i < out.house.size(); ++i)
    {
      const Position &p = out.house[i], &q = out.house[(i + 1) % out.house.size()];
      const double ex = q.x - p.x, ez = q.z - p.z;
      const double length2 = ex * ex + ez * ez;
      const double t = length2 > 1e-12 ? std::min(1.0, std::max(0.0, ((fx - p.x) * ex + (fz - p.z) * ez) / length2)) : 0;
      const double px = p.x + ex * t, pz = p.z + ez * t;
      const double d = std::hypot(px - fx, pz - fz);

      if (d < best)
      {
        best = d;
        out.entrance = Position{px, b.foundationMetres, pz};
      }
    }
  }

  // ---- the open ground -----------------------------------------------------
  std::vector<geom2::Polygon> open;

  if (!geom2::booleanOp({geom2::toOperand(lot), geom2::toOperand(house)}, geom2::Operation::Difference, {}, open, error))
  {
    return false;
  }

  // ---- outbuildings, at the back --------------------------------------------
  //
  // Small rectangles on the house's own axes, placed as far from the street as
  // the ground allows and clear of everything else: the service side of a
  // plot, which is where the people who work there live.
  std::vector<geom2::Polygon> sheds;

  if (b.outbuildings > 0)
  {
    std::vector<geom2::Polygon> room;

    if (geom2::offsetPolygons(open, -1.0, {}, room, nullptr) && !room.empty())
    {
      uint64_t state = massing.seed * 2862933555777941757ull + 3037000493ull;

      const auto random = [&]() {
        state ^= state >> 31;
        state *= 0x9E3779B97F4A7C15ull;
        state ^= state >> 29;

        return double(state >> 11) / 9007199254740992.0;
      };

      const auto inside = [&](const Rect &r) {
        const double us[] = {r.u0, r.u1, r.u0, r.u1, (r.u0 + r.u1) * .5, (r.u0 + r.u1) * .5, r.u0, r.u1};
        const double vs[] = {r.v0, r.v0, r.v1, r.v1, r.v0, r.v1, (r.v0 + r.v1) * .5, (r.v0 + r.v1) * .5};

        for (int i = 0; i < 8; ++i)
        {
          const geom2::Point q{geom2::toUnit(fx + ux * us[i] + nx * vs[i]), geom2::toUnit(fz + uz * us[i] + nz * vs[i])};
          bool held = false;

          for (const geom2::Polygon &piece : room)
          {
            held = held || geom2::contains(piece, q);
          }

          if (!held)
          {
            return false;
          }
        }

        return true;
      };

      std::vector<Rect> placed;

      const auto clear = [&](const Rect &r) {
        for (const Rect &o : placed)
        {
          if (std::min(r.u1, o.u1) - std::max(r.u0, o.u0) > -1.0 && std::min(r.v1, o.v1) - std::max(r.v0, o.v0) > -1.0)
          {
            return false;
          }
        }

        return true;
      };

      // The plot in the street frame.
      double lu0 = 1e300, lu1 = -1e300, lv0 = 1e300, lv1 = -1e300;

      for (const Position &q : b.lot)
      {
        const double u = (q.x - fx) * ux + (q.z - fz) * uz, v = (q.x - fx) * nx + (q.z - fz) * nz;
        lu0 = std::min(lu0, u);
        lu1 = std::max(lu1, u);
        lv0 = std::min(lv0, v);
        lv1 = std::max(lv1, v);
      }

      for (uint32_t k = 0; k < b.outbuildings; ++k)
      {
        // A shed is a fifth of the house, between a privy and a coach house.
        const double area = std::min(36.0, std::max(9.0, out.houseArea * (.14 + .12 * random())));
        const double width = std::sqrt(area * (1.2 + .6 * random()));
        const double depth = area / width;

        bool done = false;

        // Back first, then forward; across the plot at random.
        for (int attempt = 0; attempt < 24 && !done; ++attempt)
        {
          const double v1 = lv1 - double(attempt) * (lv1 - lv0) / 24.0;
          const double u0 = lu0 + (lu1 - lu0 - width) * random();
          const Rect r{u0, v1 - depth, u0 + width, v1};

          if (r.v0 < lv0 || !inside(r) || !clear(r))
          {
            continue;
          }

          placed.push_back(r);
          done = true;
        }
      }

      for (const Rect &r : placed)
      {
        geom2::Polygon shed;
        const double cu[] = {r.u0, r.u1, r.u1, r.u0};
        const double cv[] = {r.v0, r.v0, r.v1, r.v1};

        for (int i = 0; i < 4; ++i)
        {
          shed.outer.push_back({geom2::toUnit(fx + ux * cu[i] + nx * cv[i]), geom2::toUnit(fz + uz * cu[i] + nz * cv[i])});
        }

        if (geom2::polygonFault(shed).empty())
        {
          sheds.push_back(shed);
          out.outbuildings.push_back(toRing(shed.outer));
        }
      }
    }
  }

  // Every outbuilding is massed as a building of its own: a shed with a flat
  // lid reads as a crate, and a shed whose geometry is invented by whatever
  // draws it is the same duplication this layer exists to remove.
  for (const geom2::Polygon &shed : sheds)
  {
    PlotBuilding built;
    MassingBrief small;
    small.plot = toRing(shed.outer);
    small.seed = massing.seed ^ (uint64_t(out.sheds.size() + 1) * 0x27D4EB2F165667C5ull);
    small.coverage = .95;
    small.foundation = b.foundationMetres;
    small.floorRise = std::max(2.2, b.storeyMetres * .8);
    small.minimumStoreys = 1;
    small.maximumStoreys = 1;
    small.axisX = ux;
    small.axisZ = uz;

    massComposedBuilding(small, s, built.wings, built.roofs, &built.levels, nullptr);
    out.sheds.push_back(std::move(built));
  }

  if (!sheds.empty())
  {
    std::vector<geom2::Operand> clips;

    for (const geom2::Polygon &shed : sheds)
    {
      clips.push_back(geom2::toOperand(shed));
    }

    std::vector<geom2::Polygon> rest;
    std::vector<geom2::Operand> subject;

    for (const geom2::Polygon &piece : open)
    {
      subject.push_back(geom2::toOperand(piece));
    }

    std::vector<geom2::Operand> all = subject;
    all.insert(all.end(), clips.begin(), clips.end());

    // In any open piece and in no shed.
    if (geom2::selectFaces(all, [&](const std::vector<int32_t> &winding) {
          bool inOpen = false;

          for (size_t i = 0; i < subject.size(); ++i)
          {
            inOpen = inOpen || winding[i] != 0;
          }

          for (size_t i = subject.size(); i < all.size(); ++i)
          {
            if (winding[i] != 0)
            {
              return false;
            }
          }

          return inOpen;
        }, {}, rest, nullptr))
    {
      open = std::move(rest);
    }
  }

  // ---- the path from the street to the door ---------------------------------
  {
    const double dx = out.entrance.x - fx, dz = out.entrance.z - fz;
    const double length = std::hypot(dx, dz);

    if (length > .8)
    {
      const double px = -dz / length * .6, pz = dx / length * .6;
      geom2::Polygon band;
      band.outer = {
        {geom2::toUnit(fx + px), geom2::toUnit(fz + pz)},
        {geom2::toUnit(fx - px), geom2::toUnit(fz - pz)},
        {geom2::toUnit(out.entrance.x - px), geom2::toUnit(out.entrance.z - pz)},
        {geom2::toUnit(out.entrance.x + px), geom2::toUnit(out.entrance.z + pz)},
      };

      if (geom2::polygonFault(band).empty())
      {
        std::vector<geom2::Operand> ops{geom2::toOperand(band)};

        for (const geom2::Polygon &piece : open)
        {
          ops.push_back(geom2::toOperand(piece));
        }

        std::vector<geom2::Polygon> walk;

        if (ops.size() > 1 && geom2::selectFaces(ops, [&](const std::vector<int32_t> &w) {
              if (w[0] == 0)
              {
                return false;
              }

              for (size_t i = 1; i < w.size(); ++i)
              {
                if (w[i] != 0)
                {
                  return true;
                }
              }

              return false;
            }, {}, walk, nullptr))
        {
          for (const geom2::Polygon &piece : walk)
          {
            out.path.push_back(toGround(piece));
          }
        }
      }
    }
  }

  // ---- garden and yard --------------------------------------------------------
  //
  // Garden is the open ground behind the house; yard is the rest. A plot with
  // no garden share is all yard.
  for (const geom2::Polygon &piece : open)
  {
    out.openArea += geom2::areaMetres(piece);

    geom2::Point pc;
    geom2::centroid(piece, pc);

    const double v = (geom2::toMetres(pc.x) - fx) * nx + (geom2::toMetres(pc.y) - fz) * nz;
    const double houseBack = [&] {
      double back = 0;

      for (const Position &q : out.house)
      {
        back = std::max(back, (q.x - fx) * nx + (q.z - fz) * nz);
      }

      return back;
    }();

    const bool behind = v > houseBack * .5;

    if (b.gardenShare > 0 && behind)
    {
      out.garden.push_back(toGround(piece));
    }
    else
    {
      out.yard.push_back(toGround(piece));
    }
  }

  // ---- the fence ------------------------------------------------------------
  if (b.fenced)
  {
    std::vector<geom2::Polygon> inner;

    if (geom2::offsetPolygon(lot, -.25, {}, inner, nullptr) && !inner.empty())
    {
      std::vector<geom2::Operand> clips;

      for (const geom2::Polygon &piece : inner)
      {
        clips.push_back(geom2::toOperand(piece));
      }

      std::vector<geom2::Polygon> rail;
      std::vector<geom2::Operand> operands{geom2::toOperand(lot)};
      operands.insert(operands.end(), clips.begin(), clips.end());

      if (geom2::booleanOp(operands, geom2::Operation::Difference, {}, rail, nullptr))
      {
        for (const geom2::Polygon &piece : rail)
        {
          out.fence.push_back(toGround(piece));
        }
      }
    }
  }

  return true;
}

} // namespace procedural::world::generator
