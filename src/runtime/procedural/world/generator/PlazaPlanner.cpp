#include "PlazaPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace procedural::world::generator {
namespace {

using rewrite::CityPlazaBay;
using rewrite::CityPlazaEntry;
using rewrite::CityPlazaTerrace;

constexpr double pi = 3.14159265358979323846;

double signedArea(const std::vector<Position> &ring)
{
  double twice = 0;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const Position &a = ring[i], &b = ring[(i + 1) % ring.size()];
    twice += a.x * b.z - b.x * a.z;
  }

  return twice * .5;
}

double length(const Position &a, const Position &b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

Position along(const Position &a, const Position &b, const double t)
{
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

// The inward unit normal of edge a->b on a counter-clockwise ring.
Position inwardOf(const Position &a, const Position &b)
{
  const double run = length(a, b);

  if (run < 1e-9)
  {
    return {0, 0, 0};
  }

  return {-(b.z - a.z) / run, 0, (b.x - a.x) / run};
}

double segmentDistance(const Position &p, const Position &a, const Position &b)
{
  const double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
  const double t = l2 > 1e-12 ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / l2, 0., 1.) : 0.;

  return std::hypot(p.x - (a.x + dx * t), p.z - (a.z + dz * t));
}

Position centroidOf(const std::vector<Position> &ring)
{
  double twice = 0, cx = 0, cz = 0;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const Position &a = ring[i], &b = ring[(i + 1) % ring.size()];
    const double w = a.x * b.z - b.x * a.z;
    twice += w;
    cx += (a.x + b.x) * w;
    cz += (a.z + b.z) * w;
  }

  if (std::abs(twice) < 1e-9)
  {
    Position mean{};

    for (const Position &p : ring)
    {
      mean.x += p.x / double(ring.size());
      mean.z += p.z / double(ring.size());
    }

    return mean;
  }

  return {cx / (3. * twice), 0, cz / (3. * twice)};
}

bool insideRing(const std::vector<Position> &ring, const Position &p)
{
  bool in = false;

  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
  {
    const Position &a = ring[i], &b = ring[j];

    if ((a.z > p.z) != (b.z > p.z) && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x)
    {
      in = !in;
    }
  }

  return in;
}

// Every edge moved inward by `inset`, the moved lines re-intersected at the
// corners. Exact for a convex ring; a ring whose inset folds over keeps its
// original outline, which only means the arcade stands on the boundary.
std::vector<Position> insetRing(const std::vector<Position> &ring, const double inset)
{
  const size_t n = ring.size();
  std::vector<Position> out(n);

  for (size_t i = 0; i < n; ++i)
  {
    const Position &p = ring[(i + n - 1) % n], &q = ring[i], &r = ring[(i + 1) % n];
    const Position n0 = inwardOf(p, q), n1 = inwardOf(q, r);
    const Position d0{q.x - p.x, 0, q.z - p.z}, d1{r.x - q.x, 0, r.z - q.z};
    const Position a{p.x + n0.x * inset, 0, p.z + n0.z * inset};
    const Position c{q.x + n1.x * inset, 0, q.z + n1.z * inset};
    const double den = d0.x * d1.z - d0.z * d1.x;

    if (std::abs(den) < 1e-9)
    {
      out[i] = {q.x + n1.x * inset, 0, q.z + n1.z * inset};

      continue;
    }

    const double t = ((c.x - a.x) * d1.z - (c.z - a.z) * d1.x) / den;
    out[i] = {a.x + d0.x * t, 0, a.z + d0.z * t};
  }

  if (signedArea(out) < signedArea(ring) * .3)
  {
    return ring;
  }

  return out;
}

std::vector<Position> circle(const Position &centre, const double radius, const uint32_t sides, const double y = 0)
{
  std::vector<Position> ring;

  for (uint32_t k = 0; k < sides; ++k)
  {
    const double angle = 2. * pi * double(k) / double(sides);
    ring.push_back({centre.x + std::cos(angle) * radius, y, centre.z + std::sin(angle) * radius});
  }

  return ring;
}

uint64_t hashOf(uint64_t h, const uint64_t v)
{
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  h *= 0xbf58476d1ce4e5b9ULL;
  h ^= h >> 31;

  return h;
}

// A run of trade down one edge of the enclosure, kept off the gates.
void shamblesAlong(const PlazaBrief &b, const Position &a, const Position &c, const std::vector<std::pair<double, double>> &reserved, const double setIn, std::vector<std::vector<Position>> &stalls)
{
  const double run = length(a, c);

  if (run < b.shamblesFrontageMetres * 3.)
  {
    return;
  }

  const Position u{(c.x - a.x) / run, 0, (c.z - a.z) / run};
  const Position n = inwardOf(a, c);

  // The middle two thirds, cut where a gate's approach crosses it, and each
  // remaining stretch filled with whole bays.
  std::vector<std::pair<double, double>> free{{run / 6., run * 5. / 6.}};

  for (const auto &[lo, hi] : reserved)
  {
    std::vector<std::pair<double, double>> next;

    for (const auto &[s0, s1] : free)
    {
      if (hi <= s0 || lo >= s1)
      {
        next.push_back({s0, s1});

        continue;
      }

      if (lo > s0)
      {
        next.push_back({s0, lo});
      }

      if (hi < s1)
      {
        next.push_back({hi, s1});
      }
    }

    free = std::move(next);
  }

  for (const auto &[s0, s1] : free)
  {
    const uint32_t bays = uint32_t(std::floor((s1 - s0) / b.shamblesFrontageMetres));

    if (bays < 2)
    {
      continue;
    }

    const double width = (s1 - s0) / double(bays);

    for (uint32_t k = 0; k < bays; ++k)
    {
      const double t0 = s0 + width * double(k), t1 = s0 + width * double(k + 1);
      const double deep = setIn + b.shamblesDepthMetres;
      stalls.push_back({{a.x + u.x * t0 + n.x * setIn, 0, a.z + u.z * t0 + n.z * setIn},
                        {a.x + u.x * t1 + n.x * setIn, 0, a.z + u.z * t1 + n.z * setIn},
                        {a.x + u.x * t1 + n.x * deep, 0, a.z + u.z * t1 + n.z * deep},
                        {a.x + u.x * t0 + n.x * deep, 0, a.z + u.z * t0 + n.z * deep}});
    }
  }
}

} // namespace

PlazaPlanner::PlazaPlanner(uint64_t domain) : Generator("city.plaza", domain, "city.plaza-brief", "city.plaza", GenerationScope::Patch)
{
}

bool PlazaPlanner::run(const PlazaBrief &input, CityPlazaPlan &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

bool PlazaPlanner::plan(const PlazaBrief &b, CityPlazaPlan &out, Ledger &ledger, std::string *error)
{
  const auto fail = [&](const std::string &why) {
    if (error != nullptr)
    {
      *error = "plaza " + std::to_string(b.id) + ": " + why;
    }

    ledger.add(LedgerKind::Rejected, b.id, why);

    return false;
  };

  if (b.ring.size() < 3)
  {
    return fail("a square needs a boundary");
  }

  for (const Position &p : b.ring)
  {
    if (!std::isfinite(p.x) || !std::isfinite(p.z))
    {
      return fail("non-finite boundary vertex");
    }
  }

  CityPlazaPlan plan;
  plan.id = b.id;
  plan.level = b.level;
  plan.ring = b.ring;

  for (Position &p : plan.ring)
  {
    p.y = 0;
  }

  if (signedArea(plan.ring) < 0)
  {
    std::reverse(plan.ring.begin(), plan.ring.end());
  }

  const double area = signedArea(plan.ring);

  if (area < 60.)
  {
    return fail("a square under sixty square metres is a yard");
  }

  plan.centre = centroidOf(plan.ring);
  plan.centre.y = 0;

  double inradius = 1e300;

  for (size_t i = 0; i < plan.ring.size(); ++i)
  {
    inradius = std::min(inradius, segmentDistance(plan.centre, plan.ring[i], plan.ring[(i + 1) % plan.ring.size()]));
  }

  // --- the enclosure line and the gates ---------------------------------
  //
  // The arcade stands a little inside the boundary, so the streets round
  // the square keep their width and the piers are not in the carriageway.
  const std::vector<Position> line = b.enclosure ? insetRing(plan.ring, b.insetMetres) : plan.ring;
  const size_t n = line.size();

  std::vector<CityPlazaEntry> entries = b.entries;

  if (entries.empty())
  {
    for (size_t i = 0; i < n; ++i)
    {
      const Position &a = line[i], &c = line[(i + 1) % n];

      // A side takes a gate when the gate and a pier each side fit on it.
      if (length(a, c) >= b.gateClearMetres + 2. * b.pierMetres + 1.)
      {
        entries.push_back({along(a, c, .5), inwardOf(a, c), b.gateClearMetres});
      }
    }
  }

  // Each entry onto the nearest stretch of the enclosure, as a distance
  // along that edge. An entry on a corner goes to whichever edge can carry
  // a gate.
  struct Gate
  {
    size_t edge;
    double at, width;
    uint32_t entry;
  };

  std::vector<Gate> gates;

  for (uint32_t e = 0; e < entries.size(); ++e)
  {
    CityPlazaEntry &entry = entries[e];
    size_t best = n;
    double nearest = 1e300;

    for (size_t i = 0; i < n; ++i)
    {
      const double d = segmentDistance(entry.at, line[i], line[(i + 1) % n]);

      if (d < nearest)
      {
        nearest = d;
        best = i;
      }
    }

    if (best == n)
    {
      continue;
    }

    const Position &a = line[best], &c = line[(best + 1) % n];
    const double run = length(a, c);
    const double width = std::clamp(entry.widthMetres, 2.4, std::max(2.4, run - 2. * b.pierMetres - .4));
    const double margin = width * .5 + b.pierMetres + .2;
    double at = ((entry.at.x - a.x) * (c.x - a.x) + (entry.at.z - a.z) * (c.z - a.z)) / run;

    if (run < 2. * margin)
    {
      continue;
    }

    at = std::clamp(at, margin, run - margin);
    entry.at = along(a, c, at / run);
    entry.inward = inwardOf(a, c);
    entry.widthMetres = width;
    gates.push_back({best, at, width, e});
  }

  plan.entries = entries;

  // --- the bays -----------------------------------------------------------
  if (b.enclosure)
  {
    for (size_t i = 0; i < n; ++i)
    {
      const Position &a = line[i], &c = line[(i + 1) % n];
      const double run = length(a, c);
      const Position inward = inwardOf(a, c);
      plan.corners.push_back(a);

      // The gates on this edge, by pier centre, then the stretches between.
      std::vector<Gate> here;

      for (const Gate &gate : gates)
      {
        if (gate.edge == i)
        {
          here.push_back(gate);
        }
      }

      std::sort(here.begin(), here.end(), [](const Gate &x, const Gate &y) { return x.at < y.at; });

      double from = 0;

      const auto fill = [&](const double s0, const double s1) {
        const double free = s1 - s0;

        if (free < b.pierMetres + .8)
        {
          return;
        }

        const uint32_t bays = std::max(1u, uint32_t(std::lround(free / b.bayMetres)));
        const double width = free / double(bays);

        for (uint32_t k = 0; k < bays; ++k)
        {
          CityPlazaBay bay;
          bay.a = along(a, c, (s0 + width * double(k)) / run);
          bay.b = along(a, c, (s0 + width * double(k + 1)) / run);
          bay.inward = inward;
          bay.pierMetres = b.pierMetres;
          bay.springingMetres = b.springingMetres;
          bay.apexMetres = b.springingMetres + (width - b.pierMetres) * .5;
          bay.topMetres = std::max(b.topMetres, bay.apexMetres + .35);
          bay.parapetMetres = b.parapetMetres;
          plan.bays.push_back(bay);
        }
      };

      for (const Gate &gate : here)
      {
        const double half = gate.width * .5 + b.pierMetres * .5;

        if (gate.at - half < from - 1e-6)
        {
          continue;
        }

        fill(from, gate.at - half);

        CityPlazaBay bay;
        bay.a = along(a, c, (gate.at - half) / run);
        bay.b = along(a, c, (gate.at + half) / run);
        bay.inward = inward;
        bay.pierMetres = b.pierMetres;
        bay.springingMetres = b.gateSpringingMetres;
        bay.apexMetres = b.gateSpringingMetres + gate.width * .5;
        bay.topMetres = std::max(b.gateTopMetres, bay.apexMetres + .6);
        bay.parapetMetres = 0;
        bay.gate = true;
        bay.entry = gate.entry;
        plan.bays.push_back(bay);
        from = gate.at + half;
      }

      fill(from, run);
    }
  }

  // --- the houses ----------------------------------------------------------
  //
  // A row behind the arcade on every side that has the depth for it. The
  // front of the row stands on the back line of the walk under the arcade,
  // so the arcade is the row's colonnade; the back of the row is the lot's
  // depth further in. A gate's lane runs through the row; the corners go
  // to whichever side comes first. An inn takes the first corner of the
  // longest side, a pub the first corner of the side across, a bakehouse
  // the end of the next; the rest are shop-houses, attached.
  std::vector<bool> housed(n, false);
  const double front = b.insetMetres + b.pierMetres * .5 + b.arcadeWalkMetres; // The row's front, from the boundary.
  const double back = front + b.lotDepthMetres;

  if (b.houses && b.enclosure && n >= 3)
  {
    size_t longest = 0;
    double best = 0;

    for (size_t i = 0; i < n; ++i)
    {
      const double run = length(line[i], line[(i + 1) % n]);

      if (run > best)
      {
        best = run;
        longest = i;
      }
    }

    if (inradius >= b.housedInradiusMetres)
    {
      housed.assign(n, true);
    }
    else if (inradius >= b.twoSidedInradiusMetres && n >= 4)
    {
      housed[longest] = true;
      housed[(longest + n / 2) % n] = true;
    }

    const size_t across = (longest + n / 2) % n;

    for (size_t i = 0; i < n; ++i)
    {
      if (!housed[i])
      {
        continue;
      }

      const Position &a = plan.ring[i], &c = plan.ring[(i + 1) % n];
      const double run = length(a, c);
      const Position u{(c.x - a.x) / run, 0, (c.z - a.z) / run};
      const Position in = inwardOf(a, c);

      // The free stretches: from past the previous side's corner lot to
      // before this side's own corner, less every gate's lane.
      // A side owns the corner at its end; the side after it starts past
      // the corner lot's back.
      std::vector<std::pair<double, double>> free{{housed[(i + n - 1) % n] ? back + .3 : front, run - front}};

      // The civic hall takes the middle of the side across from the inn's,
      // or stands beside the gate when the gate is in the middle.
      if (b.civicHall && i == across && run >= b.civicHallWidthMetres + 2. * back)
      {
        const double half = b.civicHallWidthMetres * .5;
        const auto fits = [&](const double mid) {
          if (mid - half < free.front().first || mid + half > free.front().second)
          {
            return false;
          }

          for (const Gate &gate : gates)
          {
            if (gate.edge == i && mid + half > gate.at - gate.width * .5 - .6 && mid - half < gate.at + gate.width * .5 + .6)
            {
              return false;
            }
          }

          return true;
        };

        double mid = run * .5;
        bool placed = fits(mid);

        for (const double shift : {b.gateClearMetres * .5 + half + 1.2, -(b.gateClearMetres * .5 + half + 1.2)})
        {
          if (!placed && fits(run * .5 + shift))
          {
            mid = run * .5 + shift;
            placed = true;
          }
        }

        if (placed)
        {
          const double deep = front + b.civicHallDepthMetres;
          rewrite::CityPlazaLot hall;
          hall.side = uint32_t(i);
          hall.role = "civic-hall";
          hall.attached = false;
          hall.ring = {{a.x + u.x * (mid - half) + in.x * front, 0, a.z + u.z * (mid - half) + in.z * front},
                       {a.x + u.x * (mid + half) + in.x * front, 0, a.z + u.z * (mid + half) + in.z * front},
                       {a.x + u.x * (mid + half) + in.x * deep, 0, a.z + u.z * (mid + half) + in.z * deep},
                       {a.x + u.x * (mid - half) + in.x * deep, 0, a.z + u.z * (mid - half) + in.z * deep}};
          plan.lots.push_back(std::move(hall));
          free = {{free.front().first, mid - half - .3}, {mid + half + .3, free.front().second}};
        }
      }

      for (const Gate &gate : gates)
      {
        if (gate.edge != i)
        {
          continue;
        }

        const double lo = gate.at - gate.width * .5 - .6, hi = gate.at + gate.width * .5 + .6;
        std::vector<std::pair<double, double>> next;

        for (const auto &[s0, s1] : free)
        {
          if (hi <= s0 || lo >= s1)
          {
            next.push_back({s0, s1});

            continue;
          }

          if (lo > s0)
          {
            next.push_back({s0, lo});
          }

          if (hi < s1)
          {
            next.push_back({hi, s1});
          }
        }

        free = std::move(next);
      }

      bool first = true;

      for (const auto &[s0, s1] : free)
      {
        const uint32_t count = uint32_t(std::floor((s1 - s0) / b.frontageMetres));

        if (count == 0)
        {
          continue;
        }

        const double width = (s1 - s0) / double(count);

        for (uint32_t k = 0; k < count; ++k)
        {
          const double t0 = s0 + width * double(k), t1 = t0 + width;
          rewrite::CityPlazaLot lot;
          lot.side = uint32_t(i);
          lot.ring = {{a.x + u.x * t0 + in.x * front, 0, a.z + u.z * t0 + in.z * front},
                      {a.x + u.x * t1 + in.x * front, 0, a.z + u.z * t1 + in.z * front},
                      {a.x + u.x * t1 + in.x * back, 0, a.z + u.z * t1 + in.z * back},
                      {a.x + u.x * t0 + in.x * back, 0, a.z + u.z * t0 + in.z * back}};
          lot.role = first && i == longest ? "inn" : first && i == across ? "pub" : k + 1 == count && i == (longest + 1) % n ? "bakehouse" : "shop-house";
          // One occupied watch house looks over the public approaches. It is
          // a real lot with rooms and stairs, built by the dwelling pipeline.
          if (first && i == (longest + n - 1) % n && run >= 30.) lot.role = "watch-house";
          lot.attached = count > 1;
          plan.lots.push_back(std::move(lot));
          first = false;
        }
      }
    }

    // A bay with a house behind it is the house's covered walk: open.
    for (CityPlazaBay &bay : plan.bays)
    {
      const Position probe{(bay.a.x + bay.b.x) * .5 + bay.inward.x * (b.arcadeWalkMetres + 1.), 0,
                           (bay.a.z + bay.b.z) * .5 + bay.inward.z * (b.arcadeWalkMetres + 1.)};

      for (const auto &lot : plan.lots)
      {
        if (insideRing(lot.ring, probe))
        {
          bay.parapetMetres = 0;
        }
      }
    }
  }

  // --- the dais ------------------------------------------------------------
  //
  // Concentric steps up to a platform at the centre, each a tread wide and a
  // riser high. The platform is a share of the inradius, capped, and it
  // leaves the stalls' depth and a walk behind them untouched.
  const bool anyHoused = std::any_of(housed.begin(), housed.end(), [](const bool h) { return h; });
  const double trade = anyHoused ? 0. : b.insetMetres + b.pierMetres + 1.2 + (b.shambles ? b.shamblesDepthMetres : 0.) + 2.5;

  // What is left inside the rows.
  double interior = 1e300;

  for (size_t i = 0; i < n; ++i)
  {
    interior = std::min(interior, segmentDistance(plan.centre, plan.ring[i], plan.ring[(i + 1) % n]) - (housed[i] ? back + .8 : 0.));
  }

  double dais = 0;

  if (b.terraces && b.daisSteps > 0)
  {
    // Between rows of houses the square is the dais and its walk round;
    // the dais takes a larger share of what is left.
    dais = std::min({interior * (anyHoused ? std::max(b.daisShare, .42) : b.daisShare), b.daisMaximumMetres, interior - trade});

    if (dais >= b.daisMinimumMetres)
    {
      for (uint32_t k = 0; k < b.daisSteps; ++k)
      {
        const double radius = dais + b.stepTreadMetres * double(b.daisSteps - 1 - k);
        plan.terraces.push_back({circle(plan.centre, radius, 32), b.stepRiseMetres * double(k + 1)});
      }
    }
    else
    {
      dais = 0;
    }
  }

  // --- the monument --------------------------------------------------------
  CityPlazaMonument monument = b.monument;

  if (b.monumentAuto)
  {
    const CityPlazaMonument choices[] = {CityPlazaMonument::Fountain, CityPlazaMonument::Cross, CityPlazaMonument::Well};
    monument = choices[hashOf(b.seed, b.id) % 3];
  }

  const auto footprint = [](const CityPlazaMonument kind) {
    switch (kind)
    {
      case CityPlazaMonument::Fountain: return 3.3;
      case CityPlazaMonument::Cross: return 1.6;
      case CityPlazaMonument::Well: return 1.5;
      default: return 0.;
    }
  };

  const double room = dais > 0 ? dais - 2.2 : interior - trade;

  if (monument != CityPlazaMonument::None && footprint(monument) > room)
  {
    monument = room >= footprint(CityPlazaMonument::Cross) ? CityPlazaMonument::Cross : CityPlazaMonument::None;
  }

  plan.monument = monument;
  plan.monumentRadius = footprint(monument);
  plan.monumentHeight = monument == CityPlazaMonument::Fountain ? 2.7 : monument == CityPlazaMonument::Cross ? 5.4 : monument == CityPlazaMonument::Well ? 3.1 : 0.;

  // --- the shambles and the hall -------------------------------------------
  //
  // Inside the arcade now, so the row backs onto the parapet, with the
  // gates' approaches left clear.
  {
    size_t longest = 0;
    double best = 0;

    for (size_t i = 0; i < n; ++i)
    {
      const double run = length(line[i], line[(i + 1) % n]);

      if (run > best)
      {
        best = run;
        longest = i;
      }
    }

    const double setIn = b.pierMetres + 1.0;

    const auto reservedOn = [&](const size_t edge) {
      std::vector<std::pair<double, double>> reserved;

      for (const Gate &gate : gates)
      {
        if (gate.edge == edge)
        {
          reserved.push_back({gate.at - gate.width * .5 - 2.5, gate.at + gate.width * .5 + 2.5});
        }
      }

      return reserved;
    };

    if (b.shambles && !anyHoused)
    {
      shamblesAlong(b, line[longest], line[(longest + 1) % n], reservedOn(longest), setIn, plan.stalls);
    }

    // The hall: across from the shambles for choice, but on any side that
    // has room for it between the gates, and narrower than it would like
    // before it is left out. A gate in the middle of a 46 m side leaves no
    // fifteen metres on either hand; twelve, or ten, usually fit.
    if (b.marketHall && !anyHoused && n >= 3)
    {
      bool placed = false;

      for (size_t step = 1; step < n && !placed; ++step)
      {
        const size_t across = (longest + (step == 1 ? n / 2 : step)) % n;

        if (across == longest)
        {
          continue;
        }

        const Position &c = line[across], &d = line[(across + 1) % n];
        const double run = length(c, d);
        const auto reserved = reservedOn(across);

        for (const double width : {b.marketHallWidthMetres, b.marketHallWidthMetres * .8, 10.})
        {
          if (placed || run < width + 4.)
          {
            continue;
          }

          const double half = width * .5;

          const auto clear = [&](const double mid) {
            if (mid - half < b.pierMetres + .6 || mid + half > run - b.pierMetres - .6)
            {
              return false;
            }

            for (const auto &[lo, hi] : reserved)
            {
              if (mid + half > lo && mid - half < hi)
              {
                return false;
              }
            }

            return true;
          };

          double mid = run * .5;
          placed = clear(mid);

          for (const double shift : {b.gateClearMetres * .5 + half + 3., -(b.gateClearMetres * .5 + half + 3.)})
          {
            if (!placed && clear(run * .5 + shift))
            {
              mid = run * .5 + shift;
              placed = true;
            }
          }

          if (placed)
          {
            const Position u{(d.x - c.x) / run, 0, (d.z - c.z) / run};
            const Position in = inwardOf(c, d);
            const double deep = setIn + b.marketHallDepthMetres;
            plan.halls.push_back({{c.x + u.x * (mid - half) + in.x * setIn, 0, c.z + u.z * (mid - half) + in.z * setIn},
                                  {c.x + u.x * (mid + half) + in.x * setIn, 0, c.z + u.z * (mid + half) + in.z * setIn},
                                  {c.x + u.x * (mid + half) + in.x * deep, 0, c.z + u.z * (mid + half) + in.z * deep},
                                  {c.x + u.x * (mid - half) + in.x * deep, 0, c.z + u.z * (mid - half) + in.z * deep}});
          }
        }
      }
    }
  }

  // Nothing of the trade may stand on the dais.
  if (dais > 0)
  {
    for (const auto *buildings : {&plan.stalls, &plan.halls})
    {
      for (const auto &ring : *buildings)
      {
        for (const Position &p : ring)
        {
          if (std::hypot(p.x - plan.centre.x, p.z - plan.centre.z) < dais + b.stepTreadMetres * double(b.daisSteps) + .5)
          {
            return fail("the market buildings reach the dais");
          }
        }
      }
    }
  }

  // Reserve complete roof envelopes before adding any furniture. A conservative
  // circular clearance also leaves room to walk round the market counters.
  const double pavilionRadius = 4.2;
  const double daisRadius = plan.terraces.empty() ? plan.monumentRadius :
      length(plan.centre, plan.terraces.front().ring.front());
  for (size_t edge = 0; edge < n && plan.pavilions.size() < 3; ++edge)
  {
    const Position a = line[edge], d = line[(edge + 1) % n];
    const double run = length(a, d);
    if (run < 12.) continue;
    const Position u{(d.x-a.x)/run, 0, (d.z-a.z)/run};
    for (double t : {.28, .72, .5})
    {
      Position at = along(plan.centre, along(a, d, t), .62);
      bool clear = insideRing(plan.ring, at) && length(at, plan.centre) > daisRadius + pavilionRadius + 1.5;
      for (size_t k=0; clear && k<plan.ring.size(); ++k)
        clear = segmentDistance(at, plan.ring[k], plan.ring[(k+1)%plan.ring.size()]) > pavilionRadius + 1.5;
      for (const auto &entry : plan.entries)
        if (segmentDistance(at, entry.at, plan.centre) < pavilionRadius + entry.widthMetres*.5 + .8) clear=false;
      const auto avoids = [&](const std::vector<Position> &ring) {
        if (insideRing(ring, at)) return false;
        for (size_t k=0;k<ring.size();++k)
          if (segmentDistance(at,ring[k],ring[(k+1)%ring.size()]) < pavilionRadius+1.5) return false;
        return true;
      };
      for (const auto &lot:plan.lots) if (!avoids(lot.ring)) clear=false;
      for (const auto &ring:plan.stalls) if (!avoids(ring)) clear=false;
      for (const auto &ring:plan.halls) if (!avoids(ring)) clear=false;
      for (const auto &other:plan.pavilions) if (length(at,other.centre)<2*pavilionRadius+1.5) clear=false;
      if (clear) { plan.pavilions.push_back({at,u,5.4,3.6}); break; }
    }
  }

  // Seating islands define an inner walking circuit. Keep their complete
  // footprint out of every gate approach and all planned trading buildings.
  for (unsigned k=0;k<12 && plan.gardens.size()<6;++k) {
    const double angle=(k+.5)*2*pi/12;
    const Position at{plan.centre.x+(daisRadius+5.)*std::cos(angle),0,
                      plan.centre.z+(daisRadius+5.)*std::sin(angle)};
    bool clear=insideRing(plan.ring,at);
    const auto avoids=[&](const std::vector<Position> &ring) {
      if(insideRing(ring,at))return false;
      for(size_t j=0;j<ring.size();++j)
        if(segmentDistance(at,ring[j],ring[(j+1)%ring.size()])<3.6)return false;
      return true;
    };
    for(size_t j=0;j<plan.ring.size();++j)
      if(segmentDistance(at,plan.ring[j],plan.ring[(j+1)%plan.ring.size()])<4.)clear=false;
    for(const auto &entry:plan.entries)
      if(segmentDistance(at,entry.at,plan.centre)<2.4+entry.widthMetres*.5+1.)clear=false;
    for(const auto &lot:plan.lots)if(!avoids(lot.ring))clear=false;
    for(const auto &r:plan.stalls)if(!avoids(r))clear=false;
    for(const auto &r:plan.halls)if(!avoids(r))clear=false;
    for(const auto &p:plan.pavilions)if(length(at,p.centre)<7.6)clear=false;
    for(const auto &p:plan.gardens)if(length(at,p)<6.)clear=false;
    if(clear)plan.gardens.push_back(at);
  }

  uint32_t gateBays = 0;

  for (const CityPlazaBay &bay : plan.bays)
  {
    gateBays += bay.gate ? 1 : 0;
  }

  plan.notes.push_back("a square of " + std::to_string(int(area)) + " m2: " + std::to_string(plan.bays.size()) + " arcade bays, " +
                       std::to_string(gateBays) + " gates, " + std::to_string(plan.terraces.size()) + " steps up to the dais, " +
                       std::to_string(plan.lots.size()) + " houses behind the arcade, " +
                       std::to_string(plan.stalls.size()) + " shambles bays, " + std::to_string(plan.halls.size()) + " market hall, " +
                       std::to_string(plan.pavilions.size()) + " covered market pavilions");
  ledger.add(LedgerKind::Repaired, b.id, plan.notes.back());
  out = std::move(plan);

  return true;
}

// ---------------------------------------------------------------------------
// The mesh.
// ---------------------------------------------------------------------------

namespace {

using hierarchy::Solid;

// Surface ids past the enum are the city's texture manifest, which is what
// the city model writes for every face: the courtyard paving and the
// fountain water live there and nowhere else.
constexpr Solid::SurfaceMaterial kStone = Solid::SurfaceMaterial::Stone;
constexpr Solid::SurfaceMaterial kDarkStone = Solid::SurfaceMaterial::DarkStone;
constexpr Solid::SurfaceMaterial kCarved = Solid::SurfaceMaterial::CarvedStone;
constexpr Solid::SurfaceMaterial kTile = Solid::SurfaceMaterial::RoofTile;
constexpr Solid::SurfaceMaterial kPaving = Solid::SurfaceMaterial(19);
constexpr Solid::SurfaceMaterial kWater = Solid::SurfaceMaterial(23);

struct Mesher
{
  Solid &out;

  // One face, wound to look along `outward`, with metric UVs: along the face
  // and up for a standing face, the plan for a lying one.
  void face(std::vector<Solid::Face> &group, std::vector<Position> ring, const Position outward, const Solid::SurfaceMaterial material)
  {
    if (ring.size() < 3)
    {
      return;
    }

    double nx = 0, ny = 0, nz = 0;

    for (size_t k = 0; k < ring.size(); ++k)
    {
      const Position &a = ring[k], &b = ring[(k + 1) % ring.size()];
      nx += (a.y - b.y) * (a.z + b.z);
      ny += (a.z - b.z) * (a.x + b.x);
      nz += (a.x - b.x) * (a.y + b.y);
    }

    if (nx * outward.x + ny * outward.y + nz * outward.z < 0)
    {
      std::reverse(ring.begin(), ring.end());
    }

    const bool lying = std::abs(ny) >= std::hypot(nx, nz);
    Position u{1, 0, 0};

    if (!lying)
    {
      const double run = std::hypot(nx, nz);
      u = {-nz / run, 0, nx / run};
    }

    Solid::Face face;

    for (const Position &p : ring)
    {
      Solid::Vertex v;
      v.x = p.x;
      v.y = p.y;
      v.z = p.z;
      v.u = lying ? p.x : p.x * u.x + p.z * u.z;
      v.v = lying ? p.z : p.y;
      v.material = material;
      v.authoredUV = true;
      face.push_back(v);
    }

    group.push_back(std::move(face));
  }

  // A prism over a ring, standing from `base` to `top`: sides and the top.
  void prism(std::vector<Solid::Face> &sides, std::vector<Solid::Face> &caps, const std::vector<Position> &ring, const double base, const double top, const Solid::SurfaceMaterial side, const Solid::SurfaceMaterial cap, const bool withTop = true)
  {
    if (ring.size() < 3 || top <= base)
    {
      return;
    }

    const Position centre = centroidOf(ring);

    for (size_t k = 0; k < ring.size(); ++k)
    {
      Position a = ring[k], b = ring[(k + 1) % ring.size()];
      a.y = b.y = base;
      Position c = b, d = a;
      c.y = d.y = top;
      const Position mid{(a.x + b.x) * .5, 0, (a.z + b.z) * .5};
      face(sides, {a, b, c, d}, {mid.x - centre.x, 0, mid.z - centre.z}, side);
    }

    if (withTop)
    {
      std::vector<Position> lid = ring;

      for (Position &p : lid)
      {
        p.y = top;
      }

      face(caps, lid, {0, 1, 0}, cap);
    }
  }

  // An axis-aligned box in the frame (u, n): `u` along, `n` across.
  void box(const Position &centre, const Position &u, const Position &n, const double along, const double across, const double base, const double top, const Solid::SurfaceMaterial material, std::vector<Solid::Face> &sides, std::vector<Solid::Face> &caps)
  {
    const double ha = along * .5, hn = across * .5;
    const std::vector<Position> ring{{centre.x - u.x * ha - n.x * hn, 0, centre.z - u.z * ha - n.z * hn},
                                     {centre.x + u.x * ha - n.x * hn, 0, centre.z + u.z * ha - n.z * hn},
                                     {centre.x + u.x * ha + n.x * hn, 0, centre.z + u.z * ha + n.z * hn},
                                     {centre.x - u.x * ha + n.x * hn, 0, centre.z - u.z * ha + n.z * hn}};
    prism(sides, caps, ring, base, top, material, material);
  }
};

} // namespace

void meshPlaza(const CityPlazaPlan &plan, Solid &out)
{
  out = Solid{};
  Mesher m{out};

  // --- the dais ------------------------------------------------------------
  //
  // Each step is a riser round its ring and a tread between its ring and
  // the next one in; the last ring carries the platform.
  double below = 0;

  for (size_t k = 0; k < plan.terraces.size(); ++k)
  {
    const CityPlazaTerrace &step = plan.terraces[k];
    m.prism(out.foundations, out.decking, step.ring, below, step.level, kDarkStone, Solid::SurfaceMaterial(32), k + 1 == plan.terraces.size());

    if (k + 1 < plan.terraces.size() && plan.terraces[k + 1].ring.size() == step.ring.size())
    {
      const std::vector<Position> &inner = plan.terraces[k + 1].ring;

      for (size_t i = 0; i < step.ring.size(); ++i)
      {
        const size_t j = (i + 1) % step.ring.size();
        Position a = step.ring[i], b = step.ring[j], c = inner[j], d = inner[i];
        a.y = b.y = c.y = d.y = step.level;
        m.face(out.decking, {a, b, c, d}, {0, 1, 0}, kPaving);
      }
    }

    below = step.level;
  }

  // A contrasting cut-stone border and radial joints articulate the dais.
  // These are shallow raised bands, not coplanar decals.
  if (!plan.terraces.empty())
  {
    const auto &deck = plan.terraces.back();
    const double radius = length(plan.centre, deck.ring.front());
    const auto outer = circle(plan.centre, radius-.12, 32, deck.level+.012);
    const auto inner = circle(plan.centre, radius-.42, 32, deck.level+.012);
    for (size_t i=0;i<32;++i)
    {
      const size_t j=(i+1)%32;
      m.face(out.decking,{outer[i],outer[j],inner[j],inner[i]},{0,1,0},kCarved);
      if (i%4==0 && radius>plan.monumentRadius+1.)
      {
        const double angle=2*pi*double(i)/32;
        const Position u{std::cos(angle),0,std::sin(angle)}, v{-u.z,0,u.x};
        const double start=plan.monumentRadius+.3, end=radius-.42;
        const Position mid{plan.centre.x+u.x*(start+end)*.5,0,plan.centre.z+u.z*(start+end)*.5};
        m.box(mid,u,v,end-start,.12,deck.level,deck.level+.012,kCarved,out.walls,out.decking);
      }
    }
  }

  for(const auto &at:plan.gardens) {
    const double angle=std::atan2(at.z-plan.centre.z,at.x-plan.centre.x);
    const Position u{-std::sin(angle),0,std::cos(angle)},v{-u.z,0,u.x};
    // Hollow octagonal planter with a limestone rim and recessed soil.
    const auto outer=circle(at,1.25,8),inner=circle(at,1.02,8);
    for(size_t i=0;i<8;++i) {
      const size_t j=(i+1)%8;
      m.prism(out.walls,out.decking,{outer[i],outer[j],inner[j],inner[i]},0,.55,kStone,kCarved);
    }
    m.face(out.decking,circle(at,1.02,8,.43),{0,1,0},Solid::SurfaceMaterial(12));
    m.prism(out.pillars,out.decking,circle(at,.14,8),.43,2.5,Solid::SurfaceMaterial::Wood,Solid::SurfaceMaterial::Wood);
    // Tapered foliage tiers use sloped sides instead of stacked boxes.
    for(unsigned tier=0;tier<3;++tier) {
      const double bottom=1.8+tier*.65,radius=1.3-tier*.28;
      const auto lo=circle(at,radius,10,bottom),hi=circle(at,radius*.55,10,bottom+.95);
      for(size_t i=0;i<10;++i) {
        const size_t j=(i+1)%10;
        m.face(out.walls,{lo[i],lo[j],hi[j],hi[i]},
          {lo[i].x-at.x,.4,lo[i].z-at.z},Solid::SurfaceMaterial(24));
      }
      m.face(out.decking,hi,{0,1,0},Solid::SurfaceMaterial(24));
      m.face(out.soffits,lo,{0,-1,0},Solid::SurfaceMaterial(24));
    }
    for(double side:{-1.,1.}) {
      const Position seat{at.x+v.x*side*1.75,0,at.z+v.z*side*1.75};
      for(double end:{-.72,.72}) {
        Position foot{seat.x+u.x*end,0,seat.z+u.z*end};
        m.box(foot,u,v,.25,.5,0,.42,kCarved,out.pillars,out.decking);
      }
      m.box(seat,u,v,2.,.55,.42,.55,Solid::SurfaceMaterial::Wood,out.walls,out.decking);
    }
  }

  // Public pavilions have a usable open interior: plinth, columns, capitals,
  // continuous lintels, a closed roof/soffit and counters on the short sides.
  for (const auto &p:plan.pavilions)
  {
    const Position u=p.along, v{-u.z,0,u.x};
    const auto at=[&](double x,double z,double y=0.) {return Position{p.centre.x+u.x*x+v.x*z,y,p.centre.z+u.z*x+v.z*z};};
    m.box(p.centre,u,v,p.width+.4,p.depth+.4,0,.14,kStone,out.foundations,out.decking);
    m.box(p.centre,u,v,p.width,p.depth,.14,.17,Solid::SurfaceMaterial(32),out.foundations,out.decking);
    for (double x:{-p.width*.5,0.,p.width*.5}) for (double z:{-p.depth*.5,p.depth*.5})
    {
      const Position foot=at(x,z);
      m.box(foot,u,v,.62,.62,.14,.36,kCarved,out.pillars,out.decking);
      m.box(foot,u,v,.34,.34,.36,2.65,kStone,out.pillars,out.decking);
      m.box(foot,u,v,.58,.58,2.65,2.85,kCarved,out.pillars,out.decking);
    }
    for(double z:{-p.depth*.5,p.depth*.5})
      m.box(at(0,z),u,v,p.width+.65,.48,2.85,3.12,kCarved,out.walls,out.decking);
    for(double x:{-p.width*.5,p.width*.5})
    {
      m.box(at(x,0),u,v,.48,p.depth,2.85,3.12,kCarved,out.walls,out.decking);
      m.box(at(x,0),u,v,.55,p.depth-1.2,.17,.86,kDarkStone,out.walls,out.decking);
      m.box(at(x,0),u,v,.8,p.depth-.95,.86,.98,kCarved,out.walls,out.decking);
    }
    const double w=p.width*.5+.55,d=p.depth*.5+.55;
    const Position a=at(-w,-d,3.12),b=at(w,-d,3.12),c=at(w,d,3.12),e=at(-w,d,3.12);
    const Position r0=at(-w,0,4.35),r1=at(w,0,4.35);
    m.face(out.slopes,{a,b,r1,r0},{-v.x,1,-v.z},kTile);
    m.face(out.slopes,{e,r0,r1,c},{v.x,1,v.z},kTile);
    m.face(out.gables,{a,r0,e},{-u.x,0,-u.z},kStone);
    m.face(out.gables,{b,c,r1},{u.x,0,u.z},kStone);
    m.face(out.soffits,{a,e,c,b},{0,-1,0},kCarved);
    m.box(at(0,0),u,v,p.width+1.2,.22,4.32,4.48,kTile,out.walls,out.decking);
  }

  // --- the monument --------------------------------------------------------
  const double top = plan.terraces.empty() ? 0. : plan.terraces.back().level;
  const Position c = plan.centre;

  switch (plan.monument)
  {
    case CityPlazaMonument::Fountain:
    {
      m.prism(out.walls, out.decking, circle(c, 3.3, 24), top, top + .18, kStone, kStone);
      const std::vector<Position> inner = circle(c, 2.8, 24), outer = circle(c, 3.1, 24);

      for (size_t k = 0; k < 24; ++k)
      {
        const size_t q = (k + 1) % 24;
        m.prism(out.walls, out.decking, {outer[k], outer[q], inner[q], inner[k]}, top + .18, top + .78, kStone, kStone);
      }

      m.prism(out.walls, out.decking, circle(c, 2.8, 24), top + .18, top + .62, kStone, kWater);
      m.prism(out.pillars, out.decking, circle(c, .5, 12), top + .62, top + 2.4, kCarved, kCarved);
      m.prism(out.walls, out.decking, circle(c, 1.3, 16), top + 1.55, top + 1.8, kCarved, kWater);
      m.prism(out.walls, out.decking, circle(c, .75, 12), top + 2.35, top + 2.7, kCarved, kWater);

      break;
    }
    case CityPlazaMonument::Cross:
    {
      double level = top;

      for (const double radius : {1.6, 1.2, .8})
      {
        m.prism(out.foundations, out.decking, circle(c, radius, 8), level, level + .2, kDarkStone, kStone);
        level += .2;
      }

      m.prism(out.pillars, out.decking, circle(c, .22, 8), level, level + 4., kCarved, kCarved);
      m.prism(out.pillars, out.decking, circle(c, .4, 8), level + 4., level + 4.3, kCarved, kCarved);
      m.box(Position{c.x, 0, c.z}, {1, 0, 0}, {0, 0, 1}, .18, .18, level + 4.3, level + 5.4, kCarved, out.pillars, out.decking);
      m.box(Position{c.x, 0, c.z}, {1, 0, 0}, {0, 0, 1}, .8, .18, level + 4.75, level + 4.93, kCarved, out.pillars, out.decking);

      break;
    }
    case CityPlazaMonument::Well:
    {
      const std::vector<Position> inner = circle(c, 1.0, 16), outer = circle(c, 1.4, 16);

      for (size_t k = 0; k < 16; ++k)
      {
        const size_t q = (k + 1) % 16;
        m.prism(out.walls, out.decking, {outer[k], outer[q], inner[q], inner[k]}, top, top + .95, kStone, kStone);
      }

      m.face(out.decking, circle(c, 1.0, 16, top + .2), {0, 1, 0}, kWater);

      for (const double side : {-1.25, 1.25})
      {
        m.box(Position{c.x + side, 0, c.z}, {1, 0, 0}, {0, 0, 1}, .18, .18, top + .95, top + 2.5, kStone, out.timbers, out.decking);
      }

      m.box(Position{c.x, 0, c.z}, {1, 0, 0}, {0, 0, 1}, 2.68, .18, top + 2.5, top + 2.68, kStone, out.timbers, out.decking);
      const std::vector<Position> eaves{{c.x - 1.6, top + 2.68, c.z - 1.0}, {c.x + 1.6, top + 2.68, c.z - 1.0}, {c.x + 1.6, top + 2.68, c.z + 1.0}, {c.x - 1.6, top + 2.68, c.z + 1.0}};
      const Position ridgeA{c.x - 1.6, top + 3.1, c.z}, ridgeB{c.x + 1.6, top + 3.1, c.z};
      m.face(out.slopes, {eaves[0], eaves[1], ridgeB, ridgeA}, {0, 1, -1}, kStone);
      m.face(out.slopes, {eaves[3], eaves[2], ridgeB, ridgeA}, {0, 1, 1}, kStone);
      m.face(out.gables, {eaves[0], eaves[3], ridgeA}, {-1, 0, 0}, kStone);
      m.face(out.gables, {eaves[1], eaves[2], ridgeB}, {1, 0, 0}, kStone);

      break;
    }
    default: break;
  }

  // --- the arcade ----------------------------------------------------------
  //
  // A bay is two piers, an arch between them and the attic over it. Each is
  // built once: the pier at a bay's start belongs to it, the pier at its end
  // to the next bay or to the corner, and a gate owns both of its piers in
  // case nothing follows it before the corner.
  std::set<std::tuple<long, long>> piers;

  // A pier is a shaft on a plinth with an impost band where the arches
  // spring: three boxes, the outer two a hand wider than the shaft.
  const auto pierAt = [&](const Position &at, const Position &u, const Position &n, const double side, const double springing, const double height, const Solid::SurfaceMaterial material) {
    if (!piers.insert({std::lround(at.x * 100.), std::lround(at.z * 100.)}).second)
    {
      return;
    }

    m.box(at, u, n, side + .16, side + .16, 0, .45, kDarkStone, out.pillars, out.decking);
    m.box(at, u, n, side, side, .45, height, material, out.pillars, out.decking);
    m.box(at, u, n, side + .12, side + .12, springing - .22, springing, kCarved, out.pillars, out.decking);
  };

  for (const Position &corner : plan.corners)
  {
    piers.insert({std::lround(corner.x * 100.), std::lround(corner.z * 100.)});
  }

  for (size_t k = 0; k < plan.corners.size(); ++k)
  {
    const Position &corner = plan.corners[k];
    const Position &next = plan.corners[(k + 1) % plan.corners.size()];
    const double run = length(corner, next);
    const Position u = run > 1e-6 ? Position{(next.x - corner.x) / run, 0, (next.z - corner.z) / run} : Position{1, 0, 0};
    const Position n{-u.z, 0, u.x};
    double height = 0;

    for (const CityPlazaBay &bay : plan.bays)
    {
      height = std::max(height, bay.topMetres);
    }

    m.box(corner, u, n, 1.06, 1.06, 0, .5, kDarkStone, out.pillars, out.decking);
    m.box(corner, u, n, .9, .9, .5, height + .3, kStone, out.pillars, out.decking);
    m.box(corner, u, n, 1.02, 1.02, height + .3, height + .48, kCarved, out.pillars, out.decking);

    // A pyramid cap.
    const double h = height + .48;
    const std::vector<Position> foot{{corner.x - u.x * .51 - n.x * .51, h, corner.z - u.z * .51 - n.z * .51},
                                     {corner.x + u.x * .51 - n.x * .51, h, corner.z + u.z * .51 - n.z * .51},
                                     {corner.x + u.x * .51 + n.x * .51, h, corner.z + u.z * .51 + n.z * .51},
                                     {corner.x - u.x * .51 + n.x * .51, h, corner.z - u.z * .51 + n.z * .51}};
    const Position apex{corner.x, h + .55, corner.z};

    for (size_t k = 0; k < 4; ++k)
    {
      const Position &a = foot[k], &c = foot[(k + 1) % 4];
      const Position mid{(a.x + c.x) * .5 - corner.x, .3, (a.z + c.z) * .5 - corner.z};
      m.face(out.slopes, {a, c, apex}, mid, kCarved);
    }
  }

  for (const CityPlazaBay &bay : plan.bays)
  {
    const double run = length(bay.a, bay.b);

    if (run < bay.pierMetres + .3)
    {
      continue;
    }

    const Position u{(bay.b.x - bay.a.x) / run, 0, (bay.b.z - bay.a.z) / run};
    const Position n = bay.inward;
    const Solid::SurfaceMaterial masonry = bay.gate ? kCarved : kStone;
    const double thickness = bay.gate ? .7 : .5;
    const double clear = run - bay.pierMetres;
    const double radius = clear * .5;
    const double x0 = bay.pierMetres * .5, x1 = run - bay.pierMetres * .5, mid = run * .5;

    const double pier = std::max(bay.pierMetres, thickness + .1);
    pierAt(bay.a, u, n, pier, bay.springingMetres, bay.topMetres, masonry);

    if (bay.gate)
    {
      pierAt(bay.b, u, n, pier, bay.springingMetres, bay.topMetres, masonry);
    }

    const auto at = [&](const double s, const double y, const double side) {
      return Position{bay.a.x + u.x * s + n.x * side, y, bay.a.z + u.z * s + n.z * side};
    };

    // The arch, from the left springing over the crown to the right one.
    const uint32_t segments = 12;
    std::vector<std::pair<double, double>> arch;

    for (uint32_t k = 0; k <= segments; ++k)
    {
      const double angle = pi - pi * double(k) / double(segments);
      arch.push_back({mid + std::cos(angle) * radius, bay.springingMetres + std::sin(angle) * radius});
    }

    for (const double side : {-thickness * .5, thickness * .5})
    {
      std::vector<Position> spandrel;
      spandrel.push_back(at(x0, bay.springingMetres, side));

      for (const auto &[s, y] : arch)
      {
        spandrel.push_back(at(s, y, side));
      }

      spandrel.push_back(at(x1, bay.springingMetres, side));
      spandrel.push_back(at(x1, bay.topMetres, side));
      spandrel.push_back(at(x0, bay.topMetres, side));
      m.face(out.walls, spandrel, {n.x * side, 0, n.z * side}, masonry);
    }

    // The intrados: the underside of the arch between the two skins.
    for (size_t k = 0; k + 1 < arch.size(); ++k)
    {
      const auto [s0, y0] = arch[k];
      const auto [s1, y1] = arch[k + 1];
      const Position a = at(s0, y0, -thickness * .5), b = at(s1, y1, -thickness * .5), c2 = at(s1, y1, thickness * .5), d = at(s0, y0, thickness * .5);
      const Position midpoint{(a.x + b.x + c2.x + d.x) * .25, (a.y + b.y) * .5, (a.z + b.z + c2.z + d.z) * .25};
      const Position crown = at(mid, bay.springingMetres, 0);
      m.face(out.soffits, {a, b, c2, d}, {crown.x - midpoint.x, crown.y - midpoint.y, crown.z - midpoint.z}, masonry);
    }

    // The archivolt: a ring of carved stone a hand proud of each skin,
    // following the arch from springing to springing.
    for (const double side : {-thickness * .5, thickness * .5})
    {
      const double proud = side < 0 ? -.07 : .07;

      for (size_t k = 0; k + 1 < arch.size(); ++k)
      {
        const auto [s0, y0] = arch[k];
        const auto [s1, y1] = arch[k + 1];
        const auto ring = [&](const double s, const double y, const double grow) {
          // Points pushed out from the arch's centre by `grow`.
          const double dx = s - mid, dy = y - bay.springingMetres, len = std::hypot(dx, dy);
          return at(s + dx / len * grow, y + dy / len * grow, side + proud);
        };
        const Position a = ring(s0, y0, 0), c = ring(s1, y1, 0), d = ring(s1, y1, .22), e = ring(s0, y0, .22);
        m.face(out.walls, {a, c, d, e}, {n.x * side, 0, n.z * side}, kCarved);
        const Position mid2{(a.x + c.x) * .5 - at(mid, bay.springingMetres, side).x, (a.y + c.y) * .5 - bay.springingMetres, (a.z + c.z) * .5 - at(mid, bay.springingMetres, side).z};
        m.face(out.walls, {at(s0, y0, side), at(s1, y1, side), c, a}, {-mid2.x, -mid2.y, -mid2.z}, kCarved);
        m.face(out.walls, {ring(s0, y0, .22), ring(s1, y1, .22), at(s1 + (s1 - mid) / std::hypot(s1 - mid, y1 - bay.springingMetres) * .22, y1 + (y1 - bay.springingMetres) / std::hypot(s1 - mid, y1 - bay.springingMetres) * .22, side), at(s0 + (s0 - mid) / std::hypot(s0 - mid, y0 - bay.springingMetres) * .22, y0 + (y0 - bay.springingMetres) / std::hypot(s0 - mid, y0 - bay.springingMetres) * .22, side)}, mid2, kCarved);
      }
    }

    // The attic's top, with a cornice a hand proud along both faces.
    m.face(out.decking, {at(x0, bay.topMetres, -thickness * .5), at(x1, bay.topMetres, -thickness * .5), at(x1, bay.topMetres, thickness * .5), at(x0, bay.topMetres, thickness * .5)}, {0, 1, 0}, kStone);
    m.box(at(mid, 0, 0), u, n, clear, thickness + .2, bay.topMetres - .22, bay.topMetres, kCarved, out.walls, out.decking);

    // A gate's attic has a narrow light either side of its crown.
    if (bay.gate)
    {
      for (const double s : {mid - clear * .25, mid + clear * .25})
      {
        for (const double side : {-thickness * .5, thickness * .5})
        {
          m.face(out.joinery, {at(s - .12, bay.apexMetres + .5, side + (side < 0 ? -.01 : .01)), at(s + .12, bay.apexMetres + .5, side + (side < 0 ? -.01 : .01)),
                               at(s + .12, bay.topMetres - .5, side + (side < 0 ? -.01 : .01)), at(s - .12, bay.topMetres - .5, side + (side < 0 ? -.01 : .01))},
                 {n.x * side, 0, n.z * side}, kDarkStone);
        }
      }
    }

    // A closed bay is a wall under its arch, with a string course on top of
    // the wall; the arch above it is an open gallery. An open bay is a
    // house's covered walk and has nothing under the arch.
    if (bay.parapetMetres > 0)
    {
      m.box(at(mid, 0, 0), u, n, clear, .45, 0, bay.parapetMetres, kStone, out.walls, out.decking);
      m.box(at(mid, 0, 0), u, n, clear, .6, bay.parapetMetres, bay.parapetMetres + .12, kCarved, out.walls, out.decking);
    }

    // A tiled coping rides the attic: two slopes to a ridge down the
    // middle of the wall.
    if (!bay.gate)
    {
      const double eaveOut = thickness * .5 + .15, ridge = bay.topMetres + .32;
      const Position e0 = at(x0, bay.topMetres, -eaveOut), e1 = at(x1, bay.topMetres, -eaveOut);
      const Position f0 = at(x0, bay.topMetres, eaveOut), f1 = at(x1, bay.topMetres, eaveOut);
      const Position r0 = at(x0, ridge, 0), r1 = at(x1, ridge, 0);
      m.face(out.slopes, {e0, e1, r1, r0}, {-n.x, 1, -n.z}, kTile);
      m.face(out.slopes, {f0, f1, r1, r0}, {n.x, 1, n.z}, kTile);
      m.face(out.gables, {e0, r0, f0}, {-u.x, 0, -u.z}, kStone);
      m.face(out.gables, {e1, r1, f1}, u, kStone);
    }

    // The gatehouse: a storey over the arch with two windows and a string
    // course, a hipped tiled roof, and a turret either side carrying a
    // pinnacle, the way a town gate stands apart from the wall it is in.
    if (bay.gate)
    {
      const double storeyFloor = bay.apexMetres + .45;
      m.box(at(mid, 0, 0), u, n, clear + pier, thickness + .24, storeyFloor - .12, storeyFloor, kCarved, out.walls, out.decking);

      for (const double s : {mid - clear * .25, mid + clear * .25})
      {
        for (const double side : {-thickness * .5, thickness * .5})
        {
          const double lip = side < 0 ? -.012 : .012;
          m.face(out.joinery, {at(s - .45, storeyFloor + .8, side + lip), at(s + .45, storeyFloor + .8, side + lip),
                               at(s + .45, bay.topMetres - .6, side + lip), at(s - .45, bay.topMetres - .6, side + lip)},
                 {n.x * side, 0, n.z * side}, kDarkStone);
        }
      }

      const double eaveOut = thickness * .5 + .35, ridge = bay.topMetres + 1.4;
      const Position e0 = at(x0, bay.topMetres, -eaveOut), e1 = at(x1, bay.topMetres, -eaveOut);
      const Position f0 = at(x0, bay.topMetres, eaveOut), f1 = at(x1, bay.topMetres, eaveOut);
      const Position r0 = at(x0 + .9, ridge, 0), r1 = at(x1 - .9, ridge, 0);
      m.face(out.slopes, {e0, e1, r1, r0}, {-n.x, 1, -n.z}, kTile);
      m.face(out.slopes, {f0, f1, r1, r0}, {n.x, 1, n.z}, kTile);
      m.face(out.slopes, {e0, r0, f0}, {-u.x, 1, -u.z}, kTile);
      m.face(out.slopes, {e1, r1, f1}, {u.x, 1, u.z}, kTile);

      for (const Position &foot : {bay.a, bay.b})
      {
        const double side = pier + .5, top = bay.topMetres + 1.9;
        m.box(foot, u, n, side + .2, side + .2, 0, .6, kDarkStone, out.pillars, out.decking);
        m.box(foot, u, n, side, side, .6, top, masonry, out.pillars, out.decking);
        m.box(foot, u, n, side + .16, side + .16, top, top + .22, kCarved, out.pillars, out.decking);
        const double h = top + .22, half = side * .5 + .08;
        const std::vector<Position> base{{foot.x - u.x * half - n.x * half, h, foot.z - u.z * half - n.z * half},
                                         {foot.x + u.x * half - n.x * half, h, foot.z + u.z * half - n.z * half},
                                         {foot.x + u.x * half + n.x * half, h, foot.z + u.z * half + n.z * half},
                                         {foot.x - u.x * half + n.x * half, h, foot.z - u.z * half + n.z * half}};
        const Position apex{foot.x, h + 1.1, foot.z};

        for (size_t k = 0; k < 4; ++k)
        {
          const Position &p = base[k], &q = base[(k + 1) % 4];
          m.face(out.slopes, {p, q, apex}, {(p.x + q.x) * .5 - foot.x, .4, (p.z + q.z) * .5 - foot.z}, kTile);
        }

        m.prism(out.pillars, out.decking, circle(Position{foot.x, 0, foot.z}, .16, 8), h + 1.0, h + 1.75, kCarved, kCarved);
      }
    }
  }
}

} // namespace procedural::world::generator
