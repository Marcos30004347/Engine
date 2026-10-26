#pragma once

#include "procedural/world/MapGraph.hpp"
#include "procedural/world/MapRivers.hpp"

#include <string>

namespace procedural::world {

// Stream-power erosion over the map graph, run against sustained uplift.
//
// This is the planet generator's division of labour brought to the map: the
// graph owns tectonics, plates and climate, and a fluvial solver owns the
// relief. Its comment is explicit that hydraulic erosion should own the valleys
// and the drainage "instead of a separate mountain-curve generator" - and that
// is the point. A range placed analytically and a river routed afterwards are
// two systems that have to be made to agree; uplift opposed by incision is one
// system in which they already do.
//
// The balance is the whole model. Stream power incises at k A^m S^n and uplift
// raises at U, so a channel settles at S = (U / (k A^m))^(1/n): steep where
// uplift is strong or catchment small, gentle where the drainage is large.
// Ranges are what is left standing where uplift outpaces incision, and the
// valleys between them are cut by the very network that drains them.
struct FluvialSettings
{
  // How far a *median* channel moves toward its receiver in one step, from 0
  // to 1. Erodibility is derived from this rather than given directly.
  //
  // A bare k is a scale-dependent constant: it multiplies A^m, and A is an
  // area in square metres, so the same number means something entirely
  // different on a 32 km world and a 128 km one. Calibrating against the map's
  // own median coefficient makes the dial mean the same thing everywhere,
  // which is the only way this survives a change of world size.
  double channelResponse = 0.14;
  double areaExponent = 0.45;

  // Metres of uplift per step where the map's uplift intent is highest.
  // Normalised the same way and for the same reason.
  double upliftScale = 9.0;
  uint32_t iterations = 60u;
  // How often to re-derive the drainage. The surface moves under the network,
  // and a network that is never refreshed carves the valleys of a landscape
  // that no longer exists.
  uint32_t drainageRefreshInterval = 12u;

  // Hillslope diffusion, which is what rounds the ridges that pure stream power
  // leaves as knife edges.
  double diffusion = 0.12;

  // How much of the analytic relief to discard before eroding, 0 to 1.
  //
  // This is the difference between the two models, and it is worth being exact
  // about. At 0 the elevation stage's ranges are kept and the solver merely
  // erodes them - but that stage builds height from the *same* term it uses for
  // uplift, so height and uplift already correlate at 0.99 and erosion has
  // nothing to discover. At 1 the land starts near its base and the relief is
  // built by uplift outpacing incision, which is the planet generator's model:
  // ranges are what uplift defended, not what was drawn and then weathered.
  double flattenBeforeErosion = 0.0;
  // The base the flattening pulls toward, as a fraction of the land relief.
  double flattenTargetFraction = 0.12;

  double seaLevelMetres = 0.0;
  // Never cut below this above sea level, so a basin cannot erode into a hole.
  double minimumLandElevationMetres = 1.0;
};

struct FluvialReport
{
  uint32_t iterations = 0u;
  uint32_t drainageRebuilds = 0u;
  double reliefBefore = 0.0;
  double reliefAfter = 0.0;
  double meanErosionMetres = 0.0;
  double maximumErosionMetres = 0.0;
  // Cells whose height had to be clamped to their receiver's. A large count
  // means the timestep is too aggressive for the erodibility.
  uint32_t inversionsClamped = 0u;
  // The erodibility the response dial resolved to on this world.
  double resolvedStreamPowerK = 0.0;
  // Correlation between uplift intent and final elevation, before and after.
  //
  // This is the measurement that says whether the model did what it claims. If
  // ranges are what uplift defended against incision, then erosion should make
  // height and uplift agree *more*, not less: the solver removes relief that
  // nothing is holding up and leaves relief that something is.
  double upliftCorrelationBefore = 0.0;
  double upliftCorrelationAfter = 0.0;
};

// Erodes the graph in place. `rivers` is a drainage network over the *current*
// surface; the solver refreshes it internally and leaves the caller to rebuild
// the definitive one afterwards, exactly as terrain conditioning already does.
bool erodeFluvial(const FluvialSettings &settings, const RiverSettings &riverSettings, MapGraph &graph, FluvialReport *report = nullptr, std::string *error = nullptr);

} // namespace procedural::world
