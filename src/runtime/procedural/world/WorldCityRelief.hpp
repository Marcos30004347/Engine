#pragma once

#include "WorldCityReliefNoise.hpp"
#include "WorldCitySkeleton.hpp"

#include <functional>
#include <memory>
#include <set>
#include <unordered_map>

namespace procedural::world::rewrite {

struct CityTerrainRoutePlan;
struct CityReliefPlan
{
  std::shared_ptr<const CityTerrainRoutePlan> constructionTerrain;
  uint64_t revision = 0;
  // Graded height at every road node, and the profile along every road edge.
  std::unordered_map<uint64_t, double> nodeHeight;
  struct EdgeProfile
  {
    uint64_t edge = 0;
    std::vector<Position> path; // y carries the graded height.
    double halfWidth = 0, grade = 0;
    bool stepped = false;
  };
  std::vector<EdgeProfile> edges;
  struct Stairway {
    uint64_t edge=0;
    Position a,b; // Ascending, including the two end landings.
    double halfWidth=0;
    bool supported=false;
    double foundation=0;
    // End distance and top elevation for each tread or landing.
    std::vector<std::pair<double,double>> bands;
  };
  std::vector<Stairway> stairs;
  struct DeckBridge { Position a,b; double halfWidth=1.3, archRise=1.6, thickness=.4; };
  struct RaisedBlock {
    uint64_t block=0;
    std::vector<Position> boundary; // Includes the public apron around the lots.
    double level=0, foundation=0;
    // Ascending access from a lower street/deck to this deck's boundary.
    Position accessFrom, accessTo;
    double stairHalfWidth=2.;
    std::vector<Position> accessPath; // Optional flights and level turning landings.
    std::vector<DeckBridge> bridges; // Cross a reserved stair court at deck level.
    bool terrainCut=false; // Excavate the footprint beneath the paved terrace.
    bool rockCut=false; // Exposed natural stone below the paved deck.
  };
  std::vector<RaisedBlock> platforms;
  // One owner for the landform, its lots and access reservations. Platforms
  // and stairs are derived structures of this block, not independent parcels.
  struct LandBlock {
    enum class Kind { Natural, Hillside, Retained };
    uint64_t block=0;
    Kind kind=Kind::Natural;
    std::vector<Position> boundary, accessPath;
    std::vector<uint64_t> lots;
    Position centre;
    double slopeX=0, slopeZ=0, blendWidth=0;
    double accessHalfWidth=0; // Reserved corridor around accessPath.
    double minimum=0, maximum=0, maximumGrade=0;
    double minX=0,maxX=0,minZ=0,maxZ=0;
  };
  std::vector<LandBlock> landBlocks;
  struct RockFormation { Position centre; double radius=0,height=0; uint64_t seed=0; };
  std::vector<RockFormation> rocks;
  struct StairLanding { std::vector<Position> boundary; double level=0, foundation=0; };
  std::vector<StairLanding> stairLandings;
  // Road edges whose own ground was steeper than a street can be. They are
  // graded like every other way; this is what they are paved with.
  std::set<uint64_t> steepRoads;
  // Pad level per Block/Lot region id, and the plinth each lot stands on.
  std::unordered_map<uint64_t, double> regionLevel;
  std::unordered_map<uint64_t, double> regionPlinth;
  // How wide the bank around each flattened region has to be to stay walkable.
  std::unordered_map<uint64_t, double> regionSkirt;
  // The published urban surface: graded inside road ribbons, flat on pads,
  // ramped across the retaining bands, terrain elsewhere.
  std::function<double(double, double)> ground;
  // The same surface without the pads: the streets graded down the hill and
  // the ground between them, and nothing finer.
  //
  // A lot pad is a half-metre retaining band carrying up to three metres, and
  // a terrain job stations its edges until a straight interpolation is within
  // ten centimetres of the surface. Handed the full field, it chases two
  // thousand cliffs it has no vertices to spend and its triangulation fails.
  // The cliffs belong to the city model, which cuts the bands as geometry;
  // a terrain tile wants the ground the town sits on, which is this.
  std::function<double(double, double)> openGround;
  // Local excavation under independent access structures; source terrain remains intact.
  std::function<double(double, double)> accessGround;
  double minimumLevel = 0, maximumLevel = 0, fallMetres = 0;
  double maximumGrade = 0, maximumPlinth = 0;
  double padSkirtMetres = 0; // What the flattened regions grade back over.
  // The stair the plan was built with, so anything that has to build a flight
  // of its own - a wall walk, a landing - cuts the same step.
  double stepRiseMetres = 0, stepGoingMetres = 0, maximumWalkGrade = 0;
  uint32_t steppedWays = 0, plinths = 0, terracedLots = 0, groundedLots = 0;
  // Stepped ways whose alignment is too short for the climb: they want
  // switchbacks, and are built as the steepest stair that does fit.
  uint32_t impossibleWays = 0;
  // Lots whose cut or fill to reach their street exceeds the plinth budget.
  // They are still terraced - see the note in the planner - but the slope
  // under them is one the lot plan should have split.
  uint32_t overBudgetLots = 0;
  // Lots with no way near enough to be levelled onto: back land, on its own
  // ground rather than on a pad pinned to a street that does not serve it.
  uint32_t unservedLots = 0;
  std::vector<std::string> notes;
};

// Adds independent masonry decks without changing either terrain height field.
// Requests must contain their lots and fit a full stair with landings.
bool planRaisedCityBlocks(const CitySkeleton &, const std::vector<CityReliefPlan::RaisedBlock> &,
                         const CityReliefSettings &, CityReliefPlan &, std::string *error=nullptr);

// Grades the road network against the surface, terraces every lot off the
// streets it fronts, and publishes the resulting ground field.
bool planCityRelief(
    const CitySkeleton &,
    const std::function<double(double, double)> &surface,
    const CityReliefSettings &,
    CityReliefPlan &,
    std::string *error = nullptr);

} // namespace procedural::world::rewrite
