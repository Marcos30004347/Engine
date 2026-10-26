#pragma once
#include "WorldCityCompletion.hpp"
#include "procedural/geom2/Polygon.hpp"

namespace procedural::world::rewrite {
enum class CitySurfaceKind : uint8_t { Road, Block, Lot, BuildingPad, Landing, StairTread, Field, Sidewalk, Court };
struct CitySurfacePiece
{
  CitySurfaceKind kind = CitySurfaceKind::Road;
  uint64_t owner = 0;
  uint32_t segment = 0;
  geom2::Polygon polygon;
  double height = 0, bottom = 0;
  bool terrainFollowing = false;
  uint32_t pavingMaterial = 0; // Explicit road finish; zero keeps terrain blending.
};
struct CitySurfaceStructure
{
  // Disjoint arrangement faces, including intersection ownership. All pieces
  // share one quantised network; segmentation never rebuilds road ribbons.
  std::vector<CitySurfacePiece> pieces;
  struct Influence {
    geom2::Polygon polygon;
    uint32_t channel = 1; // Fixed palette: 0 grass, 1 dirt, 2 stone.
    double strength = 1, featherMetres = .9;
    bool road = false;
    double furrowX=0, furrowZ=0; // Metre-space crop rows; zero disables modulation.
    bool frontage=false;
    // Paving reaches the boundary, then feathers into the adjoining ground.
    // Ordinary terrain covers retain their symmetric boundary blend.
    bool paving=false;
  };
  std::vector<Influence> influences;
};
bool planCitySidewalks(CityCompletionPlan &, double width=1.4, std::string *error=nullptr);
bool buildCitySurfaces(const CityCompletionPlan &, CitySurfaceStructure &, std::string *error=nullptr);
// Restore shared collinear boundary nodes before terrain tessellation.
void nodeCitySurfaceBoundaries(CitySurfaceStructure &);
// Shared world-space field: material transitions never alter ownership polygons.
std::array<double,3> cityGroundWeights(const CitySurfaceStructure &, double x, double z);

// The same field, asked a few million times.
//
// The query above walks every influence and recomputes its bounds on each
// call, which is fine for a handful of samples and is quadratic against a
// mesh: the city model asks once per vertex. This is that query with the
// bounds taken once and the influences bucketed into a uniform grid, so a
// sample only sees the influences that can actually reach it.
struct CityGroundWeightIndex
{
  struct Entry { uint32_t influence; double minX, minZ, maxX, maxZ; };
  std::vector<Entry> entries;
  std::vector<std::vector<uint32_t>> cells; // Indices into `entries`.
  // Every influence's ring edges in metres (outer and holes), so a query
  // does not convert units per edge: edges[edgeStart[k]..edgeStart[k+1]).
  struct Edge { double ax, az, dx, dz, inverseLength2, minX, minZ, maxX, maxZ; };
  std::vector<Edge> edges;
  std::vector<uint32_t> edgeStart;
  double originX = 0, originZ = 0, cell = 8.;
  uint32_t columns = 0, rows = 0;
  const CitySurfaceStructure *surfaces = nullptr;
};
CityGroundWeightIndex buildCityGroundWeightIndex(const CitySurfaceStructure &);
std::array<double,3> cityGroundWeights(const CityGroundWeightIndex &, double x, double z);
}
