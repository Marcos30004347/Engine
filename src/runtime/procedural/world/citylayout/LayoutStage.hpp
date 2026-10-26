#pragma once

#include "procedural/world/citylayout/LayoutGeometry.hpp"

#include "procedural/world/WorldPatchMesh.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace procedural::world::citylayout {

using rewrite::CityLayout;
using rewrite::CitySettings;

// Stages of the city layout preamble.
//
// generateCityLayout still owns two thousand lines of work before a housing
// strategy runs: it validates the envelope, facets the wall, plans the wards,
// derives streets from their shared edges, prunes the circulation graph,
// bends the spans, places towers, and finally publishes the ground. Those are
// stages, not paragraphs, and they are being lifted out from the tail forward
// exactly as the settlement fabric was - each one taking the state it shares
// with its neighbours into a named struct on the way out.

// Turning the planned layout into published ground.
//
// The last stage before housing, and the one that decides what the world
// actually sees: the usable land as ground, every street as a road band, the
// junctions between them as platforms, and the wall clearance as pavement.
//
// It is separate from everything above it because it is the only stage that
// writes contours, and because a band is published only when it lies entirely
// within the usable land - a road that crosses a planning seam must not
// publish an invisible cut, and proving that is an area comparison this stage
// owns.
struct GroundPublishInput
{
  const CitySettings &settings;
  CityLayout &layout;

  // Land a building or a road may occupy, as convex pieces with disjoint
  // interiors. The disjointness is what makes the area test below exact.
  const std::vector<Poly> &usable;

  // Masks the wall and its towers occupy; published as pavement so no block
  // forms between the wards and the wall.
  const std::vector<Poly> &wallMasks;

  const std::function<double(double, double)> &terrain;

  // How many of `usable` are interior pieces. Everything at or beyond this
  // index lies outside the wall, where the ground is published under different
  // rules.
  size_t interiorPieces = 0;
};

struct GroundPublishResult
{
  // The road bands actually published. The housing strategies test against
  // these so that nothing is ever placed on a road.
  std::vector<Poly> roads;
};

// Planning the wards beyond the wall.
//
// A settlement with suburb land grows outer wards on it: the enclosure's holes
// are filled first so a lake or a courtyard inside the wall can never be
// reclassified as exterior developable land, then the land outside is cut into
// wards and their shared edges become streets, exactly as the interior was.
//
// Its own stage because it is conditional and self-contained - a settlement
// without suburb land skips it entirely - and because it is the only place
// that appends to the ward, cell and street lists after the interior is final.
struct OuterWardInput
{
  const CitySettings &settings;
  CityLayout &layout;

  // The enclosure the interior wards were planned inside, as the patch mesh
  // domain - outer ring plus holes, because a courtyard inside the wall is a
  // hole and must not become exterior developable land.
  const rewrite::PatchDomain &patch;

  // The layout envelope, and the ground already spoken for.
  const rewrite::CityPolygon &domain;
  const Poly &wallRing;
  const std::vector<Poly> &reserved;

  // Fixed external circulation and the gates it reaches.
  const std::vector<rewrite::CityStreet> &arteries;
  const std::vector<rewrite::CityGate> &gates;

  // The canonical boundary store the interior wards were committed to. Outer
  // wards commit into the same one, so an interior and an exterior ward that
  // share an edge share its identity.
  rewrite::BoundaryRegistry &registry;

  // The layout's own street numbering, so an outer ward's streets cannot
  // collide with an artery id.
  const std::function<uint64_t()> &nextStreetId;

  const std::function<double(uint64_t, uint64_t)> &random;
  const std::function<double(double, double)> &terrain;
};

// Grows outer wards and appends them to the layout.
//
// `usable`, `cells` and the ward-street derivation are carried in and out
// because the interior stages built them and the housing strategies read them;
// this stage extends all three rather than replacing them.
struct OuterWardState
{
  std::vector<Poly> &usable;
  std::vector<Poly> &cells;

  // Ward ids at or beyond this become outer wards. Left at its maximum when
  // there is no suburb land.
  uint64_t &firstOuterWard;

  // Envelope bounds, widened by whatever the outer wards added.
  double &minX;
  double &minZ;
  double &maxX;
  double &maxZ;
};

bool planOuterWards(const OuterWardInput &, const OuterWardState &, std::string *error);

// Publishes the ground and returns the road bands.
//
// Returns false when a band cannot be laid without an engineered platform the
// caller did not allow, which is a hard failure: a street that cannot be built
// is not a street.
bool publishLayoutGround(const GroundPublishInput &, GroundPublishResult &, std::string *error);

} // namespace procedural::world::citylayout
