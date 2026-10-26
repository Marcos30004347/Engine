#include "virtualgeometry/CityVoxelTraceScene.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace virtualgeometry {
namespace {
bool fail(std::string *error, const char *message) {
  if (error) *error = message;
  return false;
}
uint32_t read32(const std::vector<uint8_t> &bytes, size_t at) {
  return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1u]) << 8u) |
         (uint32_t(bytes[at + 2u]) << 16u) | (uint32_t(bytes[at + 3u]) << 24u);
}
bool opaque(const voxel::svraster::CityVoxelCompiledAppearance &appearance) {
  const auto &bytes = appearance.bytes;
  using Codec = voxel::svraster::CityVoxelCodec;
  if (appearance.codec == Codec::Raw)
  {
    for (size_t at = 0; at < bytes.size(); at += 8u)
      if ((read32(bytes, at) >> 24u) != 255u) return false;
  }
  else if (appearance.codec == Codec::Palette8 || appearance.codec == Codec::Palette16)
  {
    const uint32_t count = read32(bytes, 0u);
    for (uint32_t i = 0u; i < count; ++i)
      if ((read32(bytes, 4ull + uint64_t(i) * 8ull) >> 24u) != 255u) return false;
  }
  else
  {
    if ((read32(bytes, 0u) >> 24u) != 255u) return false;
    const uint32_t count = read32(bytes, 8u);
    for (uint32_t i = 0u; i < count; ++i)
      if ((read32(bytes, 16ull + uint64_t(i) * 12ull) >> 24u) != 255u) return false;
  }
  return true;
}
bool intersect(const CityVoxelTraceScene::Ray &ray,
    const std::array<float, 3> &minimum, const std::array<float, 3> &maximum,
    double &entry) {
  double near = 0.0, far = ray.maximumDistance;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (ray.direction[axis] == 0.0)
    {
      if (ray.origin[axis] < minimum[axis] || ray.origin[axis] >= maximum[axis])
        return false;
      continue;
    }
    const double a = (minimum[axis] - ray.origin[axis]) / ray.direction[axis];
    const double b = (maximum[axis] - ray.origin[axis]) / ray.direction[axis];
    near = std::max(near, std::min(a, b));
    far = std::min(far, std::max(a, b));
    if (!(far > near)) return false;
  }
  entry = near;
  return true;
}
std::array<double, 3> rotateXZ(uint32_t turns, const std::array<double, 3> &p) {
  switch (turns)
  {
    case 1u: return {-p[2], p[1], p[0]};
    case 2u: return {-p[0], p[1], -p[2]};
    case 3u: return {p[2], p[1], -p[0]};
    default: return p;
  }
}
std::array<double, 3> inverseRotateXZ(uint32_t turns, const std::array<double, 3> &p) {
  return rotateXZ((4u - turns) & 3u, p);
}
} // namespace

bool CityVoxelTraceScene::build(const CityVoxelStaticScene &city,
    const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry,
    const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances,
    std::string *error) {
  if (error) error->clear();
  if (geometry.size() != city.models().size() || appearances.empty() ||
      city.placements().size() >= 0x80000000ull)
    return fail(error, "city trace tables do not match the static scene");
  CityVoxelTraceScene next;
  next.city_ = &city;
  next.geometry_ = &geometry;
  next.appearances_ = &appearances;
  next.modelForGeometry_.reserve(geometry.size());
  std::unordered_map<uint64_t, uint32_t> modelById;
  for (size_t i = 0u; i < geometry.size(); ++i)
  {
    const auto &asset = geometry[i];
    if (asset.id != city.models()[i].geometryId ||
        asset.pitchMicrometres != city.pitchMicrometres())
      return fail(error, "city trace geometry identity or pitch differs from raster");
    const auto found = modelById.find(asset.id);
    if (found != modelById.end())
    {
      next.modelForGeometry_.push_back(found->second);
      continue;
    }
    std::vector<std::array<int32_t, 3>> cells;
    if (!voxel::svraster::decodeCityVoxelGeometry(asset, cells, nullptr, error)) return false;
    uint32_t side = 4u;
    for (int32_t extent : asset.extent)
    {
      if (extent <= 0) return fail(error, "city trace geometry extent is invalid");
      while (side < uint32_t(extent) && side < 65536u) side *= 4u;
      if (side < uint32_t(extent))
        return fail(error, "city trace geometry exceeds Tree64 model side");
    }
    Model model{};
    model.geometryId = asset.id;
    model.extent = asset.extent;
    if (!voxel::svraster::buildSparseVoxelTree64ModelCells(side, cells, model.tree, error))
      return false;
    const uint32_t index = uint32_t(next.models_.size());
    modelById.emplace(asset.id, index);
    next.modelForGeometry_.push_back(index);
    next.models_.push_back(std::move(model));
  }
  next.lookup_.resize(appearances.size());
  for (size_t i = 0u; i < appearances.size(); ++i)
  {
    auto found = modelById.find(appearances[i].geometryId);
    if (found == modelById.end())
      return fail(error, "city trace appearance references missing geometry");
    const auto source = std::find_if(geometry.begin(), geometry.end(),
        [&](const auto &value) { return value.id == appearances[i].geometryId; });
    if (!next.lookup_[i].bind(*source, appearances[i], error)) return false;
    if (!opaque(appearances[i]))
      return fail(error, "city trace requires opaque appearance to match raster visibility");
  }
  next.placementBounds_.reserve(city.placements().size());
  for (uint32_t index = 0u; index < city.placements().size(); ++index)
  {
    const auto &placement = city.placements()[index];
    if (placement.geometryIndex >= geometry.size() ||
        placement.appearanceIndex >= appearances.size() ||
        appearances[placement.appearanceIndex].geometryId != geometry[placement.geometryIndex].id)
      return fail(error, "city trace placement geometry or appearance index is invalid");
    next.placementBounds_.push_back(next.placementBounds(index));
  }
  next.placementOrder_.resize(city.placements().size());
  std::iota(next.placementOrder_.begin(), next.placementOrder_.end(), 0u);
  if (!next.placementOrder_.empty()) next.buildNode(0u, uint32_t(next.placementOrder_.size()));
  next.geometryReady_.assign(geometry.size(), 1u);
  next.appearanceReady_.assign(appearances.size(), 1u);
  next.residencyRevision_ = 1u;
  *this = std::move(next);
  return true;
}

bool CityVoxelTraceScene::publishResidency(const std::vector<uint8_t> &geometryReady,
    const std::vector<uint8_t> &appearanceReady, uint64_t revision, std::string *error) {
  if (error) error->clear();
  if (!city_ || geometryReady.size() != geometryReady_.size() ||
      appearanceReady.size() != appearanceReady_.size() || revision <= residencyRevision_)
    return fail(error, "city trace residency transaction is stale or malformed");
  geometryReady_ = geometryReady;
  appearanceReady_ = appearanceReady;
  residencyRevision_ = revision;
  return true;
}

CityVoxelTraceScene::Bounds CityVoxelTraceScene::placementBounds(uint32_t index) const {
  const auto &placement = city_->placements()[index];
  const auto &building = city_->buildings()[placement.buildingIndex];
  const auto &model = models_[modelForGeometry_[placement.geometryIndex]];
  const auto position = city_->worldPosition(placement);
  const double pitch = double(city_->pitchMicrometres()) * 1.0e-6;
  Bounds result{};
  result.minimum.fill(std::numeric_limits<float>::max());
  result.maximum.fill(-std::numeric_limits<float>::max());
  for (uint32_t corner = 0u; corner < 8u; ++corner)
  {
    const std::array<double, 3> local{
        (corner & 1u) ? double(model.extent[0]) * pitch : 0.0,
        (corner & 2u) ? double(model.extent[1]) * pitch : 0.0,
        (corner & 4u) ? double(model.extent[2]) * pitch : 0.0};
    const auto rotated = rotateXZ(building.quarterTurns, local);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const float world = float(double(position[axis]) + rotated[axis]);
      result.minimum[axis] = std::min(result.minimum[axis], world);
      result.maximum[axis] = std::max(result.maximum[axis], world);
    }
  }
  return result;
}

uint32_t CityVoxelTraceScene::buildNode(uint32_t first, uint32_t count) {
  const uint32_t index = uint32_t(tlas_.size());
  tlas_.push_back({});
  Bounds bounds{};
  bounds.minimum.fill(std::numeric_limits<float>::max());
  bounds.maximum.fill(-std::numeric_limits<float>::max());
  for (uint32_t i = first; i < first + count; ++i)
  {
    const auto &box = placementBounds_[placementOrder_[i]];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      bounds.minimum[axis] = std::min(bounds.minimum[axis], box.minimum[axis]);
      bounds.maximum[axis] = std::max(bounds.maximum[axis], box.maximum[axis]);
    }
  }
  tlas_[index].bounds = bounds;
  if (count <= 8u)
  {
    tlas_[index].first = first;
    tlas_[index].count = count;
    return index;
  }
  uint32_t axis = 0u;
  for (uint32_t candidate = 1u; candidate < 3u; ++candidate)
    if (bounds.maximum[candidate] - bounds.minimum[candidate] >
        bounds.maximum[axis] - bounds.minimum[axis]) axis = candidate;
  const uint32_t middle = first + count / 2u;
  std::nth_element(placementOrder_.begin() + first,
                   placementOrder_.begin() + middle,
                   placementOrder_.begin() + first + count,
      [&](uint32_t a, uint32_t b) {
        const auto &x = placementBounds_[a], &y = placementBounds_[b];
        const float centerA = x.minimum[axis] + x.maximum[axis];
        const float centerB = y.minimum[axis] + y.maximum[axis];
        return centerA == centerB ? a < b : centerA < centerB;
      });
  const uint32_t left = buildNode(first, middle - first);
  const uint32_t right = buildNode(middle, first + count - middle);
  tlas_[index].first = left;
  tlas_[index].right = right;
  return index;
}

voxel::svraster::SparseVoxelTree64Ray CityVoxelTraceScene::localRay(
    uint32_t placementIndex, const Ray &ray) const {
  const auto &placement = city_->placements()[placementIndex];
  const auto position = city_->worldPosition(placement);
  const auto turns = city_->buildings()[placement.buildingIndex].quarterTurns;
  std::array<double, 3> delta{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    delta[axis] = ray.origin[axis] - double(position[axis]);
  const auto localOrigin = inverseRotateXZ(turns, delta);
  const auto localDirection = inverseRotateXZ(turns, ray.direction);
  const double inversePitch = 1.0e6 / double(city_->pitchMicrometres());
  voxel::svraster::SparseVoxelTree64Ray result{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    result.origin[axis] = localOrigin[axis] * inversePitch;
    result.direction[axis] = localDirection[axis] * inversePitch;
  }
  result.maximumDistance = ray.maximumDistance;
  return result;
}

std::array<float, 3> CityVoxelTraceScene::worldNormal(uint32_t placementIndex,
    const voxel::svraster::SparseVoxelTree64Ray &local,
    const voxel::svraster::SparseVoxelTree64TraceResult &hit) const {
  uint32_t axis = 0u;
  double best = std::numeric_limits<double>::infinity();
  for (uint32_t candidate = 0u; candidate < 3u; ++candidate)
  {
    if (local.direction[candidate] == 0.0) continue;
    const double coordinate = local.origin[candidate] + hit.distance * local.direction[candidate];
    const double face = double(hit.cell[candidate]) +
        (local.direction[candidate] < 0.0 ? 1.0 : 0.0);
    const double distance = std::abs(coordinate - face);
    if (distance < best) { best = distance; axis = candidate; }
  }
  std::array<double, 3> normal{};
  normal[axis] = local.direction[axis] < 0.0 ? 1.0 : -1.0;
  const auto &placement = city_->placements()[placementIndex];
  const auto rotated = rotateXZ(city_->buildings()[placement.buildingIndex].quarterTurns, normal);
  return {float(rotated[0]), float(rotated[1]), float(rotated[2])};
}

CityVoxelTraceScene::Hit CityVoxelTraceScene::trace(const Ray &ray,
    uint32_t maximumTreeQueries) const {
  Hit output{};
  if (!city_ || !std::isfinite(ray.maximumDistance) || ray.maximumDistance < 0.0)
    return output;
  bool nonzeroDirection = false;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(ray.origin[axis]) || !std::isfinite(ray.direction[axis])) return output;
    nonzeroDirection |= ray.direction[axis] != 0.0;
  }
  if (!nonzeroDirection) return output;
  output.status = Status::Miss;
  double missingDistance = std::numeric_limits<double>::infinity();
  uint32_t missingPlacement = UINT32_MAX;
  if (tlas_.empty()) return output;
  std::vector<uint32_t> stack{0u};
  while (!stack.empty())
  {
    const uint32_t nodeIndex = stack.back(); stack.pop_back();
    const Node &node = tlas_[nodeIndex];
    double nodeEntry = 0.0;
    if (!intersect(ray, node.bounds.minimum, node.bounds.maximum, nodeEntry) ||
        nodeEntry > output.distance && output.status == Status::Hit) continue;
    if (node.count == 0u)
    {
      stack.push_back(node.right);
      stack.push_back(node.first);
      continue;
    }
    for (uint32_t i = node.first; i < node.first + node.count; ++i)
    {
      const uint32_t placementIndex = placementOrder_[i];
      const auto &placement = city_->placements()[placementIndex];
      double entry = 0.0;
      const auto &box = placementBounds_[placementIndex];
      if (!intersect(ray, box.minimum, box.maximum, entry) ||
          (output.status == Status::Hit && entry > output.distance)) continue;
      if (!geometryReady_[placement.geometryIndex] ||
          !appearanceReady_[placement.appearanceIndex])
      {
        if (entry < missingDistance) { missingDistance = entry; missingPlacement = placementIndex; }
        continue;
      }
      const auto local = localRay(placementIndex, ray);
      const auto &model = models_[modelForGeometry_[placement.geometryIndex]];
      const auto result = voxel::svraster::traceSparseVoxelTree64(
          model.tree, local, maximumTreeQueries);
      if (result.status == voxel::svraster::SparseVoxelTree64TraceStatus::QueryLimit)
      {
        output.status = Status::QueryLimit;
        return output;
      }
      if (!result.hit()) continue;
      if (output.status == Status::Hit &&
          (result.distance > output.distance ||
           (result.distance == output.distance && placementIndex < output.placementIndex)))
        continue;
      uint32_t rgba = 0u, profile = 0u;
      if (!lookup_[placement.appearanceIndex].lookupCell(result.cell, rgba, profile))
      {
        output.status = Status::Invalid;
        return output;
      }
      output.status = Status::Hit;
      output.distance = result.distance;
      output.stableId = placement.stableId;
      output.placementIndex = placementIndex;
      output.modelCell = result.cell;
      output.worldNormal = worldNormal(placementIndex, local, result);
      output.rgba = rgba;
      output.materialProfile = profile;
      output.opacity = float(rgba >> 24u) / 255.0f;
    }
  }
  if (missingPlacement != UINT32_MAX &&
      (output.status != Status::Hit || missingDistance <= output.distance))
  {
    output = {};
    output.status = Status::Unavailable;
    output.distance = missingDistance;
    output.placementIndex = missingPlacement;
    output.stableId = city_->placements()[missingPlacement].stableId;
    output.opacity = 1.0f;
  }
  return output;
}

bool CityVoxelTraceScene::shadowOccluded(const Ray &ray) const {
  return trace(ray).status != Status::Miss;
}

void CityVoxelTraceScene::exportTree64Sources(
    std::vector<voxel::svraster::SparseVoxelTree64DynamicModelSource> &models,
    std::vector<voxel::svraster::SparseVoxelTree64DynamicInstanceSource> &instances) const {
  models.clear(); instances.clear();
  if (!city_) return;
  const float pitch = float(double(city_->pitchMicrometres()) * 1.0e-6);
  models.reserve(models_.size());
  for (uint32_t index = 0u; index < models_.size(); ++index)
  {
    const auto &model = models_[index];
    voxel::svraster::SparseVoxelTree64DynamicModelSource source{};
    source.modelId = index;
    source.side = model.tree.side;
    source.localBounds.minimum = {0.0f,0.0f,0.0f};
    const float sideMetres = float(model.tree.side) * pitch;
    source.localBounds.maximum = {sideMetres,sideMetres,sideMetres};
    source.prebuiltNodes = model.tree.nodes;
    source.payloadRevision = model.geometryId;
    models.push_back(std::move(source));
  }
  instances.reserve(city_->placements().size());
  for (uint32_t index = 0u; index < city_->placements().size(); ++index)
  {
    const auto &placement = city_->placements()[index];
    const auto turns = city_->buildings()[placement.buildingIndex].quarterTurns;
    const auto position = city_->worldPosition(placement);
    const std::array<double, 3> inverseX = inverseRotateXZ(turns, {1.0,0.0,0.0});
    const std::array<double, 3> inverseY = inverseRotateXZ(turns, {0.0,1.0,0.0});
    const std::array<double, 3> inverseZ = inverseRotateXZ(turns, {0.0,0.0,1.0});
    const auto translation = inverseRotateXZ(turns,
        {-double(position[0]),-double(position[1]),-double(position[2])});
    voxel::svraster::SparseVoxelTree64DynamicInstanceSource source{};
    source.stableInstanceId = index + 1u;
    source.modelId = modelForGeometry_[placement.geometryIndex];
    source.worldBounds.minimum = placementBounds_[index].minimum;
    source.worldBounds.maximum = placementBounds_[index].maximum;
    for (uint32_t row = 0u; row < 3u; ++row)
    {
      source.worldToModel[row] = float(inverseX[row]);
      source.worldToModel[4u + row] = float(inverseY[row]);
      source.worldToModel[8u + row] = float(inverseZ[row]);
      source.worldToModel[12u + row] = float(translation[row]);
    }
    source.worldToModel[15u] = 1.0f;
    source.flags = turns == 0u ? voxel::svraster::kSparseVoxelTree64DynamicTranslationOnly : 0u;
    instances.push_back(source);
  }
}

uint64_t CityVoxelTraceScene::stableIdForTree64Instance(uint32_t token) const {
  if (!city_ || token == 0u || token > city_->placements().size()) return 0u;
  return city_->placements()[token - 1u].stableId;
}

CityVoxelTraceScene::Stats CityVoxelTraceScene::stats() const {
  Stats result{};
  result.uniqueGeometry = uint32_t(models_.size());
  result.placements = uint32_t(placementBounds_.size());
  result.tlasNodes = uint32_t(tlas_.size());
  result.tlasBytes = placementBounds_.capacity() * sizeof(Bounds) +
      placementOrder_.capacity() * sizeof(uint32_t) + tlas_.capacity() * sizeof(Node);
  for (const auto &model : models_)
    result.treeNodeBytes += model.tree.nodes.capacity() *
        sizeof(voxel::svraster::SparseVoxelTree64NodeGPU);
  for (const auto &lookup : lookup_) result.appearanceScratchBytes += lookup.scratchBytes();
  return result;
}

} // namespace virtualgeometry
