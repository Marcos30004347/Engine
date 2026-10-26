#pragma once
#include "WorldSurfaceArrangement.hpp"
namespace procedural::world::rewrite
{
enum class FieldRecipe
{
  Enclosed,
  Strips
};
struct FarmFieldInput
{
  uint64_t patch = 1, source = 1, revision = 1;
  std::vector<Position> outer;
  std::vector<std::vector<Position>> holes;
  std::vector<Position> access;                    // Connected upstream track; may begin outside patch.
  std::vector<std::vector<Position>> sharedTracks; // Convex upstream track masks, including routes to neighbouring farms.
};
struct FarmFieldSettings
{
  FieldRecipe recipe = FieldRecipe::Enclosed;
  bool deriveGrainFromAccess = true;
  double grainRadians = 0, targetArea = 1500, maximumArea = 3000, minimumArea = 150;
  double minimumWidth = 8, maximumAspect = 4, headland = 3, trackWidth = 4, entranceWidth = 2;
  uint32_t maximumDepth = 8, maximumAttempts = 256;
};
struct FarmField
{
  uint64_t id = 0, patch = 0, source = 0;
  std::vector<Position> boundary, access;
  double area = 0, minimumWidth = 0, minimumAngleDegrees = 0, aspect = 0;
};
struct FarmFieldProduct
{
  uint64_t revision = 0;
  std::vector<FarmField> fields;
  std::vector<SurfaceContour> contours;               // Domain base then crop/track ownership masks.
  std::vector<std::vector<Position>> margins, tracks; // Disjoint accounting pieces.
  std::vector<std::string> rejections;
  uint32_t splitAttempts = 0;
  double patchArea = 0, cropArea = 0, trackArea = 0, marginArea = 0, grainRadians = 0;
};
struct AgriculturalPatch;
FarmFieldInput farmFieldInput(const AgriculturalPatch &, uint64_t revision);
FarmFieldSettings farmFieldPreset(FieldRecipe);
// Polygon-with-holes decomposition retains all components. Convex field masks
// use inward offsets and morphological opening to remove unusable sharp tips.
// Both children must be feasible before a cut commits. Failed terminal fields
// remain noncrop margins. Full-width access is checked in the final crop layout.
// Atomic, bounded CPU compiler. Track grading / terrain suitability are upstream.
bool generateFarmFields(const FarmFieldInput &, const FarmFieldSettings &, FarmFieldProduct &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
