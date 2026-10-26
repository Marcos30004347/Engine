#include "voxel/svraster/SparseVoxelBrickSurfaceMesh.hpp"

#include <algorithm>
#include <cmath>

namespace voxel::svraster
{
namespace
{

// Bit (x + 8 * y) of a z slab.
// C++17: no <bit>. These are the two intrinsics the mesher needs.
uint32_t countBits(const uint64_t value)
{
  return static_cast<uint32_t>(__builtin_popcountll(value));
}

uint32_t lowestSetBit(const uint32_t value)
{
  return static_cast<uint32_t>(__builtin_ctz(value));
}

constexpr uint64_t kMaskMinimumX = 0x0101010101010101ull;
constexpr uint64_t kMaskMaximumX = 0x8080808080808080ull;
constexpr uint64_t kMaskMinimumY = 0x00000000000000ffull;
constexpr uint64_t kMaskMaximumY = 0xff00000000000000ull;

SparseVoxelBrickOccupancyPlanes planesOrEmpty(
    const SparseVoxelBrickOccupancyGPU *occupancy)
{
  if (occupancy == nullptr)
    return {};
  return sparseVoxelBrickOccupancyPlanes(*occupancy);
}

uint32_t populationCount(const uint32_t value)
{
  return static_cast<uint32_t>(__builtin_popcount(value));
}

// Cell coordinates of the face at (slice, u, v) for one face axis.
std::array<uint32_t, 3> cellForSlice(
    const uint32_t axis,
    const uint32_t slice,
    const uint32_t u,
    const uint32_t v)
{
  const auto axes = sparseVoxelBrickFaceAxes(axis << 1u);
  std::array<uint32_t, 3> cell{};
  cell[axis] = slice;
  cell[axes[0]] = u;
  cell[axes[1]] = v;
  return cell;
}

// 8x8 slice of a direction's exposed faces, one byte per v row, bit per u.
std::array<uint8_t, kSparseVoxelBrickSide> sliceRows(
    const SparseVoxelBrickOccupancyPlanes &exposed,
    const uint32_t axis,
    const uint32_t slice)
{
  std::array<uint8_t, kSparseVoxelBrickSide> rows{};
  switch (axis)
  {
  case 0u:
    // slice = x, u = y, v = z
    for (uint32_t z = 0u; z < kSparseVoxelBrickSide; ++z)
    {
      uint8_t row = 0u;
      for (uint32_t y = 0u; y < kSparseVoxelBrickSide; ++y)
        if ((exposed[z] & (uint64_t{1u} << (slice + kSparseVoxelBrickSide * y))) != 0u)
          row |= static_cast<uint8_t>(1u << y);
      rows[z] = row;
    }
    break;
  case 1u:
    // slice = y, u = z, v = x
    for (uint32_t x = 0u; x < kSparseVoxelBrickSide; ++x)
    {
      uint8_t row = 0u;
      for (uint32_t z = 0u; z < kSparseVoxelBrickSide; ++z)
        if ((exposed[z] & (uint64_t{1u} << (x + kSparseVoxelBrickSide * slice))) != 0u)
          row |= static_cast<uint8_t>(1u << z);
      rows[x] = row;
    }
    break;
  default:
    // slice = z, u = x, v = y
    for (uint32_t y = 0u; y < kSparseVoxelBrickSide; ++y)
      rows[y] = static_cast<uint8_t>(
          (exposed[slice] >> (kSparseVoxelBrickSide * y)) & 0xffull);
    break;
  }
  return rows;
}

uint32_t linearCell(const std::array<uint32_t, 3> &cell)
{
  return cell[0] + kSparseVoxelBrickSide * cell[1] +
      kSparseVoxelBrickSide * kSparseVoxelBrickSide * cell[2];
}

} // namespace

SparseVoxelBrickOccupancyPlanes sparseVoxelBrickOccupancyPlanes(
    const SparseVoxelBrickOccupancyGPU &occupancy)
{
  SparseVoxelBrickOccupancyPlanes planes{};
  for (uint32_t z = 0u; z < kSparseVoxelBrickPlanes; ++z)
    planes[z] = static_cast<uint64_t>(occupancy.cells[2u * z].bits) |
        (static_cast<uint64_t>(occupancy.cells[2u * z + 1u].bits) << 32u);
  return planes;
}

uint32_t sparseVoxelBrickExposedFaces(
    const SparseVoxelBrickOccupancyGPU &occupancy,
    const SparseVoxelBrickNeighbourOccupancy &neighbours,
    SparseVoxelBrickFaceMasks &exposed)
{
  const SparseVoxelBrickOccupancyPlanes self =
      sparseVoxelBrickOccupancyPlanes(occupancy);
  std::array<SparseVoxelBrickOccupancyPlanes, kSparseVoxelBrickFaceDirections>
      neighbourPlanes{};
  for (uint32_t direction = 0u; direction < kSparseVoxelBrickFaceDirections;
       ++direction)
    neighbourPlanes[direction] = planesOrEmpty(neighbours.neighbours[direction]);

  uint32_t faces = 0u;
  for (uint32_t z = 0u; z < kSparseVoxelBrickPlanes; ++z)
  {
    const uint64_t plane = self[z];
    if (plane == 0u)
    {
      for (uint32_t direction = 0u;
           direction < kSparseVoxelBrickFaceDirections;
           ++direction)
        exposed[direction][z] = 0u;
      continue;
    }

    // Cell across the face, gathered from this slab where the neighbour is
    // inside the brick and from the adjacent brick where it is not.
    const uint64_t acrossMinusX = ((plane << 1u) & ~kMaskMinimumX) |
        ((neighbourPlanes[0][z] & kMaskMaximumX) >> 7u);
    const uint64_t acrossPlusX = ((plane >> 1u) & ~kMaskMaximumX) |
        ((neighbourPlanes[1][z] & kMaskMinimumX) << 7u);
    const uint64_t acrossMinusY = ((plane << kSparseVoxelBrickSide) & ~kMaskMinimumY) |
        ((neighbourPlanes[2][z] & kMaskMaximumY) >> 56u);
    const uint64_t acrossPlusY = ((plane >> kSparseVoxelBrickSide) & ~kMaskMaximumY) |
        ((neighbourPlanes[3][z] & kMaskMinimumY) << 56u);
    const uint64_t acrossMinusZ = z == 0u
        ? neighbourPlanes[4][kSparseVoxelBrickPlanes - 1u]
        : self[z - 1u];
    const uint64_t acrossPlusZ = z + 1u == kSparseVoxelBrickPlanes
        ? neighbourPlanes[5][0]
        : self[z + 1u];

    exposed[0][z] = plane & ~acrossMinusX;
    exposed[1][z] = plane & ~acrossPlusX;
    exposed[2][z] = plane & ~acrossMinusY;
    exposed[3][z] = plane & ~acrossPlusY;
    exposed[4][z] = plane & ~acrossMinusZ;
    exposed[5][z] = plane & ~acrossPlusZ;

    for (uint32_t direction = 0u;
         direction < kSparseVoxelBrickFaceDirections;
         ++direction)
      faces += countBits(exposed[direction][z]);
  }
  return faces;
}

uint32_t sparseVoxelBrickClosedBlockFaces(
    const SparseVoxelBrickOccupancyGPU &occupancy, const uint32_t blockCells,
    SparseVoxelBrickFaceMasks &exposed)
{
  sparseVoxelBrickExposedFaces(occupancy, {}, exposed);
  const auto planes = sparseVoxelBrickOccupancyPlanes(occupancy);
  uint64_t minX = 0u, maxX = 0u, minY = 0u, maxY = 0u;
  const uint32_t side = std::clamp(blockCells, 1u, kSparseVoxelBrickSide);
  for (uint32_t y = 0u; y < kSparseVoxelBrickSide; ++y)
    for (uint32_t x = 0u; x < kSparseVoxelBrickSide; ++x)
    {
      const uint64_t bit = uint64_t{1u} << (x + kSparseVoxelBrickSide * y);
      if (x % side == 0u) minX |= bit;
      if (x % side == side - 1u) maxX |= bit;
      if (y % side == 0u) minY |= bit;
      if (y % side == side - 1u) maxY |= bit;
    }
  uint32_t faces = 0u;
  for (uint32_t z = 0u; z < kSparseVoxelBrickPlanes; ++z)
  {
    exposed[0][z] |= planes[z] & minX;
    exposed[1][z] |= planes[z] & maxX;
    exposed[2][z] |= planes[z] & minY;
    exposed[3][z] |= planes[z] & maxY;
    if (z % side == 0u) exposed[4][z] |= planes[z];
    if (z % side == side - 1u) exposed[5][z] |= planes[z];
    for (const auto &direction : exposed)
      faces += countBits(direction[z]);
  }
  return faces;
}

bool sparseVoxelBrickCellMaterials(
    const SparseVoxelBrickOccupancyGPU &occupancy,
    const std::vector<uint8_t> &materialIndices,
    const uint64_t materialBase,
    std::array<uint8_t, kSparseVoxelBrickVoxelCount> &cellMaterials)
{
  cellMaterials.fill(kSparseVoxelBrickEmptyCellMaterial);
  for (uint32_t word = 0u; word < kSparseVoxelBrickOccupancyWords; ++word)
  {
    uint32_t bits = occupancy.cells[word].bits;
    const uint32_t prefix = occupancy.cells[word].prefix;
    while (bits != 0u)
    {
      const uint32_t bit = lowestSetBit(bits);
      const uint32_t preceding = bit == 0u ? 0u : (occupancy.cells[word].bits &
                                                   ((uint32_t{1u} << bit) - 1u));
      const uint32_t cell = word * 32u + bit;
      if (materialIndices.empty())
      {
        cellMaterials[cell] = 0u;
      }
      else
      {
        const uint64_t address =
            materialBase + prefix + populationCount(preceding);
        if (address >= materialIndices.size())
          return false;
        cellMaterials[cell] = materialIndices[static_cast<size_t>(address)];
      }
      bits &= bits - 1u;
    }
  }
  return true;
}

uint32_t sparseVoxelBrickGreedyQuads(
    const SparseVoxelBrickFaceMasks &exposed,
    const std::array<uint8_t, kSparseVoxelBrickVoxelCount> &cellMaterials,
    const uint32_t blockCells,
    const bool mergeSameMaterial,
    std::vector<SparseVoxelBrickSurfaceQuad> &quads)
{
  const uint32_t block =
      std::clamp(blockCells, 1u, static_cast<uint32_t>(kSparseVoxelBrickSide));
  const size_t before = quads.size();

  for (uint32_t direction = 0u; direction < kSparseVoxelBrickFaceDirections;
       ++direction)
  {
    const uint32_t axis = direction >> 1u;
    for (uint32_t slice = 0u; slice < kSparseVoxelBrickSide; ++slice)
    {
      std::array<uint8_t, kSparseVoxelBrickSide> rows =
          sliceRows(exposed[direction], axis, slice);
      bool any = false;
      for (const uint8_t row : rows)
        any = any || row != 0u;
      if (!any)
        continue;

      for (uint32_t v = 0u; v < kSparseVoxelBrickSide; ++v)
      {
        while (rows[v] != 0u)
        {
          const uint32_t u = lowestSetBit(rows[v]);
          const uint8_t material =
              cellMaterials[linearCell(cellForSlice(axis, slice, u, v))];

          // Grow along u, then along v, never crossing a block boundary so the
          // quad stays inside exactly one octree-aligned block.
          const uint32_t uLimit = (u / block + 1u) * block;
          uint32_t width = 1u;
          while (u + width < uLimit &&
                 (rows[v] & (1u << (u + width))) != 0u &&
                 (!mergeSameMaterial ||
                  cellMaterials[linearCell(cellForSlice(axis, slice, u + width, v))] ==
                      material))
            ++width;

          const uint32_t runMask = ((1u << width) - 1u) << u;
          const uint32_t vLimit = (v / block + 1u) * block;
          uint32_t height = 1u;
          while (v + height < vLimit && (rows[v + height] & runMask) == runMask)
          {
            bool sameMaterial = true;
            if (mergeSameMaterial)
              for (uint32_t offset = 0u; offset < width && sameMaterial; ++offset)
                sameMaterial = cellMaterials[linearCell(
                                   cellForSlice(axis, slice, u + offset, v + height))] ==
                    material;
            if (!sameMaterial)
              break;
            ++height;
          }

          for (uint32_t row = 0u; row < height; ++row)
            rows[v + row] = static_cast<uint8_t>(rows[v + row] & ~runMask);

          SparseVoxelBrickSurfaceQuad quad{};
          const auto cell = cellForSlice(axis, slice, u, v);
          quad.origin = {
              static_cast<uint8_t>(cell[0]),
              static_cast<uint8_t>(cell[1]),
              static_cast<uint8_t>(cell[2])};
          quad.direction = static_cast<uint8_t>(direction);
          quad.width = static_cast<uint8_t>(width);
          quad.height = static_cast<uint8_t>(height);
          quad.material = material;
          quads.push_back(quad);
        }
      }
    }
  }
  return static_cast<uint32_t>(quads.size() - before);
}

uint32_t sparseVoxelBrickGreedyGeometryQuads(
    const SparseVoxelBrickFaceMasks &exposed, const uint32_t blockCells,
    std::vector<SparseVoxelBrickSurfaceQuad> &quads)
{
  std::array<uint8_t, kSparseVoxelBrickVoxelCount> unused{};
  unused.fill(kSparseVoxelBrickEmptyCellMaterial);
  return sparseVoxelBrickGreedyQuads(exposed, unused, blockCells, false, quads);
}

std::array<uint32_t, 3> sparseVoxelBrickSourceCell(
    const SparseVoxelBrickSurfaceQuad &quad, const float localU, const float localV)
{
  const auto axes = sparseVoxelBrickFaceAxes(quad.direction);
  std::array<uint32_t, 3> cell{quad.origin[0], quad.origin[1], quad.origin[2]};
  const auto coordinate = [](const float uv, const uint32_t first,
                             const uint32_t count) {
    if (!std::isfinite(uv) || uv <= float(first)) return first;
    if (uv >= float(first + count)) return first + count - 1u;
    return std::min(uint32_t(std::floor(uv)), first + count - 1u);
  };
  cell[axes[0]] = coordinate(localU, quad.origin[axes[0]], quad.width);
  cell[axes[1]] = coordinate(localV, quad.origin[axes[1]], quad.height);
  return cell;
}

std::array<std::array<uint32_t, 3>, 4> sparseVoxelBrickQuadCorners(
    const SparseVoxelBrickSurfaceQuad &quad)
{
  const uint32_t axis = quad.direction >> 1u;
  const bool positive = (quad.direction & 1u) != 0u;
  const auto axes = sparseVoxelBrickFaceAxes(quad.direction);

  std::array<uint32_t, 3> base{
      quad.origin[0], quad.origin[1], quad.origin[2]};
  base[axis] += positive ? 1u : 0u;

  const std::array<std::array<uint32_t, 2>, 4> offsets{{
      {{0u, 0u}},
      {{quad.width, 0u}},
      {{quad.width, quad.height}},
      {{0u, quad.height}},
  }};

  std::array<std::array<uint32_t, 3>, 4> corners{};
  for (uint32_t corner = 0u; corner < 4u; ++corner)
  {
    corners[corner] = base;
    corners[corner][axes[0]] += offsets[corner][0];
    corners[corner][axes[1]] += offsets[corner][1];
  }
  // cross(u, v) is the positive axis, so the u/v order already winds a positive
  // face counter-clockwise from outside. A negative face is the mirror.
  if (!positive)
    std::swap(corners[1], corners[3]);
  return corners;
}


} // namespace voxel::svraster
