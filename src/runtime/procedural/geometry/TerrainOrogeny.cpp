#include "procedural/geometry/TerrainOrogeny.hpp"
#include <algorithm>
#include <cmath>

namespace procedural::geometry
{
namespace
{
uint64_t hash(uint64_t x) { x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 27; x *= 0x94d049bb133111ebull; return x ^ (x >> 31); }
double random(uint64_t x) { return double(hash(x) >> 11) / double(uint64_t(1) << 53); }
double smooth(double t) { t = std::clamp(t, 0.0, 1.0); return t * t * (3 - 2 * t); }
constexpr double kTau = 6.283185307179586;

// The four processes, in the order of the enum.
const OrogenyProfile kProfiles[] = {
    // Cordillera - active convergence. The Alps, the Andes, Taiwan, the
    // Southern Alps. Flanks average 20-25 degrees, which is a slope of .36-.47;
    // 4,000 m summits recur every 10-15 km with passes 1,100-1,600 m below them
    // (Great St Bernard 2,469 m, Brenner 1,370 m); and the belt is two
    // sub-parallel lines, as the Andes are Occidental and Oriental.
    {"cordillera", 1.55, .25, 11000.0, .55, 2u, .10, .14, .55, .14},
    // Fold belt - a worn collision. The Appalachian Valley and Ridge, the Jura,
    // the Zagros. Ten or more parallel crests 5-10 km apart, each standing
    // 250-400 m over 2 km of flank, and famously even along its length: a fold
    // belt is interrupted by water gaps, not by cols, so the saddles are
    // shallow and the ridge lines plunge gradually under their foreland.
    {"fold belt", .78, .12, 7000.0, .72, 4u, .07, .10, .50, .22},
    // Massif - an eroded upland block. The Massif Central, the Scottish
    // Highlands, the Harz. Low and broad, with the drainage radial rather than
    // along a crest, so most of the relief is in the spurs and not the spine.
    {"massif", .62, .09, 16000.0, .70, 1u, .12, .42, .78, .28},
    // Volcanic centre - Etna, Fuji, Teide, Mauna Kea. One summit rather than a
    // crest, standing far above what its area would otherwise carry (Hawaii:
    // 4,207 m on 10,430 km^2, against 1,800 m natural), on flanks that average
    // 15-20 degrees near the base and steepen towards the cone.
    {"volcanic", 1.35, .22, 30000.0, .25, 1u, .12, .22, .40, .34},
};

class CordilleraOrogeny final : public OrogenyStrategy
{
public:
  OrogenyKind kind() const override { return OrogenyKind::Cordillera; }
  bool admits(double potential, double spineLength, double extentMetres) const override
  { return potential > .42 && spineLength > extentMetres * .10; }
};

class FoldBeltOrogeny final : public OrogenyStrategy
{
public:
  OrogenyKind kind() const override { return OrogenyKind::FoldBelt; }
  bool admits(double potential, double spineLength, double extentMetres) const override
  { return spineLength > extentMetres * .12; }
  // A fold belt's crest is even along its length and then plunges: the summits
  // of the Valley and Ridge stay within a hundred metres of each other for
  // hundreds of kilometres. So the along-crest variation is damped, and what
  // remains is the plunge at the ends the base class already applies.
  double crestVariation(double along, double spineLength, uint64_t seed) const override
  {
    const double base = OrogenyStrategy::crestVariation(along, spineLength, seed);
    const double even = smooth(std::min(along, spineLength - along)
        / std::max(1.0, profile().endTaperFraction * spineLength));
    return base * .45 + even * .55;
  }
};

class MassifOrogeny final : public OrogenyStrategy
{
public:
  OrogenyKind kind() const override { return OrogenyKind::Massif; }
  bool admits(double, double, double) const override { return true; }   // the fallback
};

class VolcanicOrogeny final : public OrogenyStrategy
{
public:
  OrogenyKind kind() const override { return OrogenyKind::Volcanic; }
  bool admits(double potential, double spineLength, double extentMetres) const override
  { return potential > .52 && spineLength < extentMetres * .20; }
  // One cone, not a crest: the height is a single peak along the line rather
  // than a train of summits, and it falls away in every direction from there.
  double crestVariation(double along, double spineLength, uint64_t seed) const override
  {
    const double at = (.36 + .28 * random(seed + 991u)) * std::max(1.0, spineLength);
    const double d = std::abs(along - at) / std::max(1.0, spineLength * .46);
    return std::pow(std::max(0.0, 1 - d * d), 1.6);
  }
  // A stratovolcano steepens towards the summit, so the flank is concave: the
  // upper cone is near the angle of repose and the apron is much flatter.
  double flankHalfWidth(double rise) const override
  { return rise / profile().flankSlope * (1.0 + .35 * smooth(rise / 2500.0)); }
};
}

double terrainSlopeProvince(Point2 point, double extentMetres, uint64_t seed)
{
  // Three octaves over a third of the world, so a province is tens of
  // kilometres across - the scale a mountain belt actually has - and the
  // boundaries between them are soft enough to read as a front rather than a
  // seam.
  const auto lattice = [&](double x, double y, uint64_t salt)
  {
    const double fx = std::floor(x), fy = std::floor(y);
    const double tx = x - fx, ty = y - fy;
    const auto corner = [&](double cx, double cy)
    {
      return random(salt ^ (uint64_t(int64_t(cx)) * 0x9E3779B97F4A7C15ull)
          ^ (uint64_t(int64_t(cy)) * 0xC2B2AE3D27D4EB4Full));
    };
    const double sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const double a = corner(fx, fy) + (corner(fx + 1, fy) - corner(fx, fy)) * sx;
    const double b = corner(fx, fy + 1) + (corner(fx + 1, fy + 1) - corner(fx, fy + 1)) * sx;
    return a + (b - a) * sy;
  };
  double value = 0, amplitude = 1, total = 0;
  double frequency = 1.0 / std::max(1.0, extentMetres * .33);
  for (int octave = 0; octave < 3; ++octave)
  {
    value += amplitude * lattice(point.x * frequency, point.y * frequency,
        seed ^ (0x70726f76ull + uint64_t(octave)));
    total += amplitude;
    amplitude *= .5;
    frequency *= 2.13;
  }
  const double unit = std::clamp(value / std::max(1e-9, total), 0.0, 1.0);
  // Contrast, not bias. A power curve here makes every province mediocre, which
  // gives a landmass of uniformly low hills - measured at a 430 m maximum where
  // the geography allowed 1,740. Pushing the extremes apart instead is what
  // makes one part of the map mountainous and another part flat.
  return unit * unit * (3 - 2 * unit);
}

const OrogenyProfile &orogenyProfile(OrogenyKind kind)
{
  const auto index = size_t(kind);
  return kProfiles[index < size_t(OrogenyKind::Count) ? index : 0];
}

double naturalSummitMetres(double landAreaSquareKm)
{
  // Below a square kilometre there is no landmass to speak of; the fit is not
  // meaningful there and would return a cliff on a sandbar.
  return 1000.0 * std::pow(std::max(1.0, landAreaSquareKm) / 1000.0, .25);
}

double OrogenyStrategy::crestRise(double summitMetres, double along, double spineLength,
    uint64_t seed) const
{
  return summitMetres * profile().heightGain * crestVariation(along, spineLength, seed);
}

double OrogenyStrategy::crestVariation(double along, double spineLength, uint64_t seed) const
{
  const auto &shape = profile();
  // A range dies into its foreland rather than ending in a wall.
  const double taper = smooth(std::min(along, spineLength - along)
      / std::max(1.0, shape.endTaperFraction * spineLength));
  // Summits at the profile's spacing, with two shorter harmonics so no two
  // summits are alike. This is the whole difference between a row of peaks and
  // a mesa, and the mesa is what a constant rise along a spine produces.
  double wave = 0, amplitude = 1, total = 0, wavelength = std::max(600.0, shape.summitSpacingMetres);
  for (int octave = 0; octave < 3; ++octave)
  {
    wave += amplitude * std::sin(kTau * along / wavelength + random(seed + 17u * uint64_t(octave + 1)) * kTau);
    total += amplitude;
    amplitude *= .5;
    wavelength *= .52;
  }
  const double crest = .5 + .5 * wave / std::max(1e-6, total);
  return taper * (shape.colFraction + (1 - shape.colFraction) * crest);
}

double OrogenyStrategy::flankHalfWidth(double riseMetres) const
{
  return riseMetres / std::max(.02, profile().flankSlope);
}

bool OrogenyStrategy::admits(double, double, double) const { return true; }

std::unique_ptr<OrogenyStrategy> makeOrogeny(OrogenyKind kind)
{
  switch (kind)
  {
    case OrogenyKind::Cordillera: return std::make_unique<CordilleraOrogeny>();
    case OrogenyKind::FoldBelt:   return std::make_unique<FoldBeltOrogeny>();
    case OrogenyKind::Volcanic:   return std::make_unique<VolcanicOrogeny>();
    default:                      return std::make_unique<MassifOrogeny>();
  }
}
}
