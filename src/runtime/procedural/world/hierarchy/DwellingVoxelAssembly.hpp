#pragma once

#include "procedural/geom2/CellGrid.hpp"
#include "procedural/world/hierarchy/CellSkeleton.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world::generator { struct CellHouse; }

namespace procedural::world::hierarchy {

// A compact, source-indexed intermediate representation. The caller retains
// the Skeleton until the offline voxel compiler consumes this assembly; no
// building-sized copy of structure polygons is made here.
struct DwellingVoxelGridSpec
{
  geom2::CellFrame frame{};
  uint32_t columns = 0;
  uint32_t rows = 0;
  uint32_t voxelsPerCell = 0;
  double voxelMetres = 0;
  double baseMetres = 0; // Outer building transform; source coordinates are floor-relative.
  std::array<int32_t, 3> minimumVoxel{}; // Inclusive source geometry bounds.
  std::array<int32_t, 3> maximumVoxel{}; // Exclusive source geometry bounds.
  std::vector<uint32_t> partyEdges;
  std::vector<double> partyGaps;
};

enum class DwellingVoxelPartSource : uint8_t
{
  Piece,
  Facade,
  UnassignedFace,
};

struct DwellingVoxelPart
{
  uint64_t geometryKey = 0; // Shape and dimensions, excluding materials.
  uint64_t appearanceKey = 0; // Semantic face materials; exact voxels follow.
  uint64_t variationSeed = 0;
  DwellingVoxelPartSource source = DwellingVoxelPartSource::Piece;
  uint32_t sourceIndex = 0;
  uint32_t sourceFaceFirst = 0;
  uint32_t sourceFaceCount = 0;
  uint32_t supportPiece = UINT32_MAX;
  uint32_t segment = UINT32_MAX;
  std::array<int32_t, 3> gridPosition{};
  std::array<double, 3> residualMetres{};
  uint8_t quarterTurn = 0;
  bool requiresUniqueOrientation = false;
  bool requiresBakedGeometry = false; // Offset or angle belongs in the asset key.
  bool partyWall = false; // Resolved against CityVoxelPartySeam after all lots are accepted.
  uint16_t connectorMask = 0; // Facade and detail placements expose a prop socket.
  uint16_t occupancyPriority = 0; // Higher-priority parts own coincident 10 cm cells.
  bool omitFromOpaqueScene = false; // Glass waits for alpha-aware traversal.
  bool omitWhenCovered = false; // Static structure with no visible 10 cm cells may be culled.
  bool appearanceOverlay = false; // Sub-voxel door relief paints its leaf's exact cells.
  bool doorLeafSurface = false;
  double localAngleRadians = 0;
};

struct DwellingVoxelConnector
{
  uint32_t pieceA = 0, pieceB = UINT32_MAX;
  ContactKind kind = ContactKind::Meets;
  PieceFace faceA = PieceFace::Body, faceB = PieceFace::Body;
  std::array<double,3> localA{}, localB{}; // Contact coordinates in each piece frame.
  std::array<int32_t, 3> gridPosition{};
  std::array<double, 3> residualMetres{};
  // Semantic interfaces. The compiler clips coincident occupied cells and
  // removes hidden faces only after all connected parts have been placed.
  uint16_t maskA = 0, maskB = 0;
};

enum DwellingVoxelConnectorMask : uint16_t
{
  VoxelWall = 1u << 0,
  VoxelSlab = 1u << 1,
  VoxelRoof = 1u << 2,
  VoxelStair = 1u << 3,
  VoxelOpening = 1u << 4,
  VoxelPropSocket = 1u << 5,
  VoxelOutside = 1u << 6,
  VoxelCap = 1u << 7,
};

uint16_t dwellingVoxelConnectorMask(PieceKind, PieceFace, ContactKind);
uint16_t dwellingVoxelConnectorMask(PieceKind, FaceKind, PieceFace, ContactKind);

struct DwellingVoxelOpening
{
  uint32_t sourceIndex = 0, floor = 0;
  uint64_t wall = 0, room = 0;
  rewrite::OpeningKind kind = rewrite::OpeningKind::Door;
  bool exterior = false;
  std::array<int32_t,3> startVoxel{}, endVoxel{};
  std::array<double,3> startResidual{}, endResidual{};
  double base = 0, top = 0, sill = 0, head = 0;
  uint16_t mask = VoxelOpening;
};

struct DwellingVoxelRoomPath
{
  uint32_t sourceIndex = 0, roomA = UINT32_MAX, roomB = UINT32_MAX;
  RoomConnectionKind kind = RoomConnectionKind::Door;
  std::array<int32_t,3> gridPosition{};
  std::array<double,3> residualMetres{};
};

struct DwellingVoxelAssembly
{
  uint64_t dwellingId = 0;
  uint64_t seed = 0;
  DwellingVoxelGridSpec grid{};
  std::vector<DwellingVoxelPart> parts;
  std::vector<uint32_t> sourceFaces; // Indices into the accompanying Skeleton.
  std::vector<DwellingVoxelConnector> connectors;
  std::vector<DwellingVoxelOpening> openings;
  std::vector<DwellingVoxelRoomPath> roomPaths;
  uint32_t sourcePieceCount = 0;
  uint32_t sourceFacadeCount = 0;
  uint32_t sourceFaceCount = 0;
  uint32_t sourceRoomCount = 0;
  uint32_t sourceRoomConnectionCount = 0;
  uint32_t plannedRoomCount = 0;
  uint32_t plannedOpeningCount = 0;
  uint32_t plannedExteriorWindowCount = 0;
  uint32_t plannedStairCount = 0;
  uint32_t plannedRoofCount = 0;
  uint32_t architecturalAgeYears = 0;
  double weathering = 0;
  uint32_t unalignedPlacementCount = 0;
  uint32_t uniqueOrientationCount = 0;
  uint64_t legacyTriangleFirst = 0, legacyTriangleCount = 0;
  uint64_t legacyPanelFirst = 0, legacyPanelCount = 0;
};

bool buildDwellingVoxelAssembly(const generator::CellHouse &, const rewrite::DwellingPlan &,
                                const Skeleton &, uint64_t dwellingId, uint64_t seed,
                                DwellingVoxelAssembly &, std::string *error = nullptr);
bool auditDwellingVoxelAssembly(const DwellingVoxelAssembly &, const Skeleton &, std::string *error = nullptr);

} // namespace procedural::world::hierarchy
