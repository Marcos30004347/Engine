#pragma once

#include "procedural/Predicates.hpp"
#include "procedural/world/WorldBrief.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world {

constexpr uint32_t kInvalidNode = 0xffffffffu;

// What the layout wants a node to become, before any surface exists.
//
// Both elevation and uplift are carried, and that is the point: elevation makes
// a mountain cell high, and uplift is what keeps it high if erosion is ever run
// against it. Storing only elevation is what lets a later erosion pass quietly
// flatten everything the layout asked for.
struct NodeIntent
{
  double elevationMetres = 0.0;
  double upliftRate = 0.0;
  // Peak-to-trough relief this node should carry internally. Distinguishes a
  // 400 m hill country from a 3000 m range without a separate cell type.
  double reliefAmplitude = 0.0;
  // 0..1, feeds generator parameters (Dendry disturbance, fBm octaves).
  double roughness = 0.5;
};

// Elevation and slope sampled along a shared edge. Children of a node treat
// these as immutable: the parent owns every boundary.
struct EdgeSample
{
  Point2 position;
  double elevationMetres = 0.0;
  double slope = 0.0;
};

struct RiverCrossing
{
  Point2 position;
  double dischargeCumecs = 0.0;
  // Unit direction of flow, pointing downstream.
  Point2 direction;
};

// Fixed by the parent before any child refines. This is the contract that keeps
// independently refined siblings agreeing at their shared edge.
struct BoundaryConditions
{
  std::vector<EdgeSample> edges;
  std::vector<RiverCrossing> rivers;

  bool empty() const
  {
    return edges.empty() && rivers.empty();
  }
};

struct MapNode
{
  uint32_t id = kInvalidNode;
  uint32_t parent = kInvalidNode;
  uint8_t level = 0u;
  std::vector<uint32_t> children;

  // Landform, from the elevation stage: what the ground does here.
  CellType type = CellType::Ocean;
  // Content, from the region stage: what is *on* the ground. Kept separate
  // because they answer different questions - a wood and a desert can sit on
  // the same hills, and the map is coloured by one while the terrain is built
  // from the other.
  CellType content = CellType::Ocean;
  // Region this cell belongs to, or kInvalidNode before the region stage.
  uint32_t region = kInvalidNode;
  // Land type, from the climate stage. A richer vocabulary than `content`,
  // because content has no word for tundra or rainforest. Declared as a
  // uint8_t so MapGraph does not have to know about the climate module.
  uint8_t biome = 0u;
  // Which of the growth's blocks claimed this cell. The seams between them are
  // where the ranges go, so this has to survive the landmass stage rather than
  // being thrown away with it.
  uint16_t craton = 0xffffu;
  NodeIntent intent{};

  // Cell polygon in world metres, counter-clockwise.
  std::vector<Point2> area;
  // Voronoi site, also in metres. Blending weights are measured from this.
  Point2 site{};

  bool land = false;
  bool coastal = false;
  // Water adjacent to land. Kept distinct from coastal so shallow-water
  // rendering and beach profiles can differ.
  bool shallow = false;

  std::vector<uint32_t> neighbours;
  BoundaryConditions boundary;

  bool refined() const
  {
    return !children.empty();
  }
};

// The hierarchical map. Level 0 is generated eagerly; deeper levels are
// produced on demand by refining a node, which is what keeps a multi-gigabyte
// world's cost proportional to what has actually been visited.
class MapGraph
{
public:
  const WorldBrief &brief() const
  {
    return brief_;
  }

  void setBrief(const WorldBrief &brief)
  {
    brief_ = brief;
  }

  uint32_t addNode(MapNode node);

  const MapNode &node(const uint32_t id) const
  {
    return nodes_[id];
  }

  MapNode &node(const uint32_t id)
  {
    return nodes_[id];
  }

  size_t nodeCount() const
  {
    return nodes_.size();
  }

  const std::vector<MapNode> &nodes() const
  {
    return nodes_;
  }

  // Ids of every node at a level. Used by the views and by refinement.
  std::vector<uint32_t> nodesAtLevel(uint8_t level) const;

  // Node whose polygon contains the point, searched at the deepest refined
  // level. kInvalidNode when the point is outside the world.
  uint32_t nodeAt(Point2 position) const;

  void clear();

  // Stable signature of the whole graph. Hashes scalar bit patterns rather than
  // object padding, so a captured baseline survives a compiler change. This is
  // what the determinism gate compares.
  uint64_t signature() const;

  // Structural checks that must hold after every generation stage. Returns
  // false and fills `error` on the first violation. Cheap enough to run in
  // debug builds unconditionally.
  bool validate(std::string *error = nullptr) const;

private:
  WorldBrief brief_{};
  std::vector<MapNode> nodes_;
};

} // namespace procedural::world
