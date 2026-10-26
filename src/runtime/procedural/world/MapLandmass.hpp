#pragma once

#include "procedural/world/MapGraph.hpp"

#include <string>

namespace procedural::world
{

struct LandmassSettings
{
  double landFraction = 0.35;
  double extentMetres = 361000.0;

  // Target width of one continent, in metres. When positive this decides how
  // much land there is and `landFraction` is ignored.
  //
  // Sizing by span rather than by fraction is what you actually want to say: a
  // 200 km continent is 200 km whatever the world around it is, whereas the
  // same land fraction is a different country in a different world. A compact
  // landmass covers about 0.6 of its bounding square, which is the factor used
  // to turn a span into an area.
  double continentSpanMetres = 0.0;

  // How many separate continents to grow. Each gets its own seed cell; whether
  // they end up joined is left to the growth, because two continents that meet
  // and fuse is a perfectly good map.
  uint32_t continents = 2u;

  // Growth seeds per continent.
  //
  // One seed grows a disc: the cost is accumulated distance, so the front is a
  // distance ball and no amount of noise on top of it changes that it is round.
  // Real continents are several old blocks that fused, and growing from several
  // seeds at once is what produces a lobed outline with gulfs between the
  // lobes. This is the setting that stops the map being a starfish.
  uint32_t cratonsPerContinent = 5u;

  // How far a continent's blocks spread from its seed, as a fraction of its
  // span. Wide gives ranges that reach across the continent; too wide and the
  // growth fragments into lobes with inland seas trapped between them.
  double cratonSpreadFraction = 0.34;

  // Irregularity of the growth front, 0..1. Zero grows a disc from each seed;
  // higher lets the front run further along some directions than others, which
  // is what produces peninsulas, gulfs and a genuinely non-convex outline.
  double raggedness = 0.7;

  // Detail added to the coastline below the scale the continents were grown at,
  // 0..1. The growth decides the shape; this decides how ragged its edge is.
  double coastDetail = 0.6;

  // Land components smaller than this share of the total land are removed.
  //
  // A growth front sheds specks, and a speck is an island nothing can reach and
  // nothing can be done with. Dropping them is cheaper than pretending a
  // two-cell rock is a place.
  double minimumIslandFraction = 0.004;

  uint64_t seed = 0u;
};

// Grows landmasses by selecting cells of the coarse partition, then refines the
// coastline at the finest level.
//
// This exists because the archetype fields cannot make a continent that is not
// convex. They are analytic - a disc, a half-plane, a union of cores - and
// however hard the domain is warped, the result still reads as a smoothed blob.
// Growing over the coarse Voronoi graph instead makes the *shape* a sequence of
// discrete decisions over irregular cells, which is what produces coastlines
// with peninsulas, isthmuses, gulfs and inland seas.
//
// Three stages:
//   1. Grow `continents` fronts over the coarsest level, cheapest cell first,
//      with a per-cell cost that varies so the front is ragged rather than
//      circular. Stops at the requested land area.
//   2. Push the result down to the leaves, then displace the coastline by
//      multi-octave noise measured against the signed distance to it, and
//      re-cut by ranking so the land fraction is still exact.
//   3. Drop land components too small to matter.
//
// Replaces raiseLand for worlds that want a continent rather than an archetype;
// raiseLand is untouched and still serves the eight named shapes.
bool growLandmasses(
    const LandmassSettings &settings,
    MapGraph &graph,
    std::string *error = nullptr);

} // namespace procedural::world
