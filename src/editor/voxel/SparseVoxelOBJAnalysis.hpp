#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace voxel::svraster
{

enum class SparseVoxelLengthUnit : uint8_t
{
  Meters,
  Centimeters,
  Millimeters,
  Inches,
  Feet,
  Custom,
};

struct SparseVoxelBoundsD
{
  std::array<double, 3> min{};
  std::array<double, 3> max{};
  std::array<double, 3> center{};
  std::array<double, 3> extent{};
  double diagonal = 0.0;
};

struct SparseVoxelOBJAnalysis
{
  std::filesystem::path sourcePath;
  uint64_t sourceContentHash = 0u;
  SparseVoxelBoundsD sourceBounds{};
  uint64_t vertexCount = 0u;
  uint64_t triangleCount = 0u;
  uint64_t shapeCount = 0u;
  uint64_t materialCount = 0u;
  uint64_t degenerateTriangleCount = 0u;
  std::string warning;
};

struct SparseVoxelOBJImportSettings
{
  SparseVoxelLengthUnit sourceUnit = SparseVoxelLengthUnit::Meters;
  double metersPerSourceUnit = 1.0;
  double voxelSizeMeters = 0.05;
  uint32_t maximumLevel = 16u;
  uint64_t maximumEstimatedCells = uint64_t{1} << 31u;
  uint64_t maximumEstimatedTemporaryBytes = uint64_t{4} << 30u;
  uint32_t estimatedBytesPerCell = 32u;
  uint32_t targetPageBytes = 64u * 1024u;
  // Emit one BLAS and one instance per OBJ shape rather than folding the whole
  // file into a single model. Each model then gets a root cube tight to its own
  // geometry, which is what gives the TLAS something to cull - a single model's
  // root spans the entire scene, so instance culling can never reject anything.
  bool splitByShape = false;
  // Emit one BLAS and one instance per OBJ *material* instead. Faces are
  // filtered by material id across every shape, so one instance holds exactly
  // the geometry that shares an appearance.
  //
  // This is what makes a per-instance colour palette accurate. A palette
  // covering a whole file has to span every material at once - on Sponza that
  // is 25 materials with distinct diffuse maps, and 256 entries across all of
  // them quantises visibly. One instance per material gives each its own 256
  // entries over a single material's colour range, where the residual error is
  // negligible.
  //
  // Mutually exclusive with splitByShape; splitByMaterial wins if both are set.
  bool splitByMaterial = false;
};

struct SparseVoxelOBJGridEstimate
{
  SparseVoxelBoundsD meterBounds{};
  std::array<double, 3> rootMinMeters{};
  std::array<double, 3> rootCenterMeters{};
  std::array<uint64_t, 3> occupiedCellsPerAxis{};
  double rootExtentMeters = 0.0;
  double effectiveVoxelSizeMeters = 0.0;
  uint32_t maximumLevel = 0u;
  uint64_t resolution = 0u;
  uint64_t conservativeCellCount = 0u;
  uint64_t conservativeTemporaryBytes = 0u;
  uint64_t conservativePageCount = 0u;
};

double metersPerSourceUnit(SparseVoxelLengthUnit unit, double customMetersPerSourceUnit = 1.0);
const char *sparseVoxelLengthUnitName(SparseVoxelLengthUnit unit);

bool analyzeSparseVoxelOBJ(
    const std::filesystem::path &sourcePath,
    SparseVoxelOBJAnalysis &analysis,
    std::string *error = nullptr,
    const std::atomic_bool *cancelled = nullptr);

bool deriveSparseVoxelOBJGrid(
    const SparseVoxelOBJAnalysis &analysis,
    const SparseVoxelOBJImportSettings &settings,
    SparseVoxelOBJGridEstimate &estimate,
    std::string *error = nullptr);

} // namespace voxel::svraster
