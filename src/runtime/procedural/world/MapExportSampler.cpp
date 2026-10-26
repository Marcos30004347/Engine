#include "procedural/world/MapExportSampler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace procedural::world
{

namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

uint16_t loadU16(const uint8_t *bytes)
{
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8u;
}

uint32_t loadU32(const uint8_t *bytes)
{
  return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8u | static_cast<uint32_t>(bytes[2]) << 16u | static_cast<uint32_t>(bytes[3]) << 24u;
}

float halfToFloat(const uint16_t value)
{
  const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16u;
  uint32_t exponent = (value >> 10u) & 0x1fu;
  uint32_t mantissa = value & 0x03ffu;
  uint32_t bits = 0u;
  if (exponent == 0u)
  {
    if (mantissa == 0u)
      bits = sign;
    else
    {
      int32_t unbiased = -14;
      while ((mantissa & 0x0400u) == 0u)
      {
        mantissa <<= 1u;
        --unbiased;
      }
      mantissa &= 0x03ffu;
      bits = sign | (static_cast<uint32_t>(unbiased + 127) << 23u) | (mantissa << 13u);
    }
  }
  else if (exponent == 31u)
    bits = sign | 0x7f800000u | (mantissa << 13u);
  else
    bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
  float result = 0.0f;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

bool finiteCoordinates(const double u, const double v)
{
  return std::isfinite(u) && std::isfinite(v);
}

double unitCoordinate(const double value)
{
  return std::clamp(value, 0.0, 1.0);
}

bool resolvePage(const MapExportField &field, MapExportPageAddress requested, const MapExportPage *&page)
{
  page = field.bestPage(requested);
  return page != nullptr;
}

bool resolveBiomePages(const MapExportField &ids, const MapExportField &weights, MapExportPageAddress requested, const MapExportPage *&idPage, const MapExportPage *&weightPage)
{
  for (;;)
  {
    idPage = ids.page(requested);
    weightPage = weights.page(requested);
    if (idPage != nullptr && weightPage != nullptr)
      return true;
    if (requested.level == 0u)
      return false;
    requested = mapExportPageParent(requested);
  }
}

struct FilterCoordinates
{
  uint32_t x0 = 0u;
  uint32_t x1 = 0u;
  uint32_t y0 = 0u;
  uint32_t y1 = 0u;
  double tx = 0.0;
  double ty = 0.0;
};

FilterCoordinates filterCoordinates(const MapExportBundle &bundle, const MapExportPage &page, const double u, const double v)
{
  const uint32_t dimension = uint32_t{1u} << page.address.level;
  const uint32_t rasterSide = bundle.pageSizePixels * dimension;
  const auto axis = [&](const double coordinate, const uint32_t pageAxis)
  {
    const double raster = unitCoordinate(coordinate) * rasterSide - 0.5;
    const double bounded = std::clamp(raster, 0.0, static_cast<double>(rasterSide - 1u));
    const uint32_t low = static_cast<uint32_t>(std::floor(bounded));
    const uint32_t high = std::min(low + 1u, rasterSide - 1u);
    const uint32_t pageOrigin = pageAxis * bundle.pageSizePixels;
    const uint32_t localLow = static_cast<uint32_t>(static_cast<int64_t>(low) - pageOrigin + bundle.gutterPixels);
    const uint32_t localHigh = static_cast<uint32_t>(static_cast<int64_t>(high) - pageOrigin + bundle.gutterPixels);
    return std::array<double, 3>{static_cast<double>(localLow), static_cast<double>(localHigh), bounded - std::floor(bounded)};
  };
  const std::array<double, 3> x = axis(u, page.address.x);
  const std::array<double, 3> y = axis(v, page.address.y);
  return {static_cast<uint32_t>(x[0]), static_cast<uint32_t>(x[1]), static_cast<uint32_t>(y[0]), static_cast<uint32_t>(y[1]), x[2], y[2]};
}

bool validPagePayload(const MapExportField &field, const MapExportPage &page)
{
  const uint32_t texelBytes = mapExportTexelBytes(field.format);
  return texelBytes != 0u && page.width != 0u && page.height != 0u && page.data.size() == static_cast<size_t>(page.width) * page.height * texelBytes;
}

const uint8_t *texel(const MapExportField &field, const MapExportPage &page, const uint32_t x, const uint32_t y)
{
  if (x >= page.width || y >= page.height)
    return nullptr;
  return page.data.data() + (static_cast<size_t>(y) * page.width + x) * mapExportTexelBytes(field.format);
}

std::array<double, 4> decodeTexel(const MapExportField &field, const uint8_t *bytes)
{
  std::array<double, 4> decoded{};
  switch (field.format)
  {
  case MapExportFormat::R8Uint:
    decoded[0] = bytes[0];
    break;
  case MapExportFormat::R16Float:
    decoded[0] = halfToFloat(loadU16(bytes));
    break;
  case MapExportFormat::R16Unorm:
    decoded[0] = loadU16(bytes) / 65535.0;
    break;
  case MapExportFormat::R32Uint:
    decoded[0] = loadU32(bytes);
    break;
  case MapExportFormat::RG16Float:
    decoded[0] = halfToFloat(loadU16(bytes));
    decoded[1] = halfToFloat(loadU16(bytes + 2u));
    break;
  case MapExportFormat::RGBA8Uint:
    for (size_t component = 0u; component < 4u; ++component)
      decoded[component] = bytes[component];
    break;
  case MapExportFormat::RGBA8Unorm:
    for (size_t component = 0u; component < 4u; ++component)
      decoded[component] = bytes[component] / 255.0;
    break;
  }
  for (size_t component = 0u; component < mapExportComponentCount(field.format); ++component)
    decoded[component] = decoded[component] * field.scale[component] + field.offset[component];
  return decoded;
}

double bilinearWeight(const size_t sample, const double tx, const double ty)
{
  const double x = (sample & 1u) != 0u ? tx : 1.0 - tx;
  const double y = (sample & 2u) != 0u ? ty : 1.0 - ty;
  return x * y;
}

} // namespace

bool mapExportPageAt(const MapExportBundle &bundle, const double u, const double v, const uint8_t level, MapExportPageAddress &address)
{
  if (!finiteCoordinates(u, v) || bundle.pageSizePixels == 0u || bundle.maximumPageLevel > 31u || level > bundle.maximumPageLevel || level > 31u)
    return false;
  const uint32_t dimension = uint32_t{1u} << level;
  const uint32_t x = std::min(static_cast<uint32_t>(unitCoordinate(u) * dimension), dimension - 1u);
  const uint32_t y = std::min(static_cast<uint32_t>(unitCoordinate(v) * dimension), dimension - 1u);
  address = {level, x, y};
  return true;
}

bool sampleMapExportField(const MapExportBundle &bundle, const std::string &fieldName, const double u, const double v, const uint8_t requestedLevel, MapExportFieldSample &sample, std::string *error)
{
  MapExportFieldCursor cursor{};
  return sampleMapExportFieldCached(
      bundle, fieldName, cursor, u, v, requestedLevel, sample, error);
}

bool sampleMapExportFieldCached(const MapExportBundle &bundle, const std::string &fieldName, MapExportFieldCursor &cursor, const double u, const double v, const uint8_t requestedLevel, MapExportFieldSample &sample, std::string *error)
{
  sample = {};
  MapExportPageAddress requested{};
  if (!mapExportPageAt(bundle, u, v, requestedLevel, requested))
    return fail(error, "invalid map export sample coordinates or LOD");
  if (!cursor.resolved)
  {
    cursor.field = bundle.field(fieldName);
    cursor.resolved = true;
  }
  const MapExportField *field = cursor.field;
  if (field == nullptr)
    return fail(error, "map export field does not exist: " + fieldName);
  if (!field->available)
    return fail(error, "map export field is unavailable: " + fieldName);
  if (field->filter == MapExportFilter::SparseMixture)
    return fail(error, "sparse mixture fields require paired biome sampling");

  const MapExportPage *page = nullptr;
  if (cursor.page != nullptr && cursor.requested == requested)
    page = cursor.page;
  else if (!resolvePage(*field, requested, page) || page == nullptr)
    return fail(error, "map export field has no resident page or ancestor");
  if (!validPagePayload(*field, *page))
    return fail(error, "map export page has an invalid decoded payload");
  cursor.page = page;
  cursor.requested = requested;

  sample.requestedPage = requested;
  sample.resolvedPage = page->address;
  sample.componentCount = mapExportComponentCount(field->format);
  const FilterCoordinates coordinates = filterCoordinates(bundle, *page, u, v);
  if (field->filter == MapExportFilter::Nearest)
  {
    const uint32_t x = coordinates.tx < 0.5 ? coordinates.x0 : coordinates.x1;
    const uint32_t y = coordinates.ty < 0.5 ? coordinates.y0 : coordinates.y1;
    const uint8_t *value = texel(*field, *page, x, y);
    if (value == nullptr)
      return fail(error, "nearest map export sample fell outside its page");
    sample.values = decodeTexel(*field, value);
    return true;
  }

  const std::array<const uint8_t *, 4> values{
    texel(*field, *page, coordinates.x0, coordinates.y0), texel(*field, *page, coordinates.x1, coordinates.y0), texel(*field, *page, coordinates.x0, coordinates.y1), texel(*field, *page, coordinates.x1, coordinates.y1)};
  if (std::any_of(
          values.begin(),
          values.end(),
          [](const uint8_t *value)
          {
            return value == nullptr;
          }))
    return fail(error, "linear map export sample exceeded its page gutter");
  for (size_t tap = 0u; tap < values.size(); ++tap)
  {
    const std::array<double, 4> decoded = decodeTexel(*field, values[tap]);
    const double weight = bilinearWeight(tap, coordinates.tx, coordinates.ty);
    for (size_t component = 0u; component < sample.componentCount; ++component)
      sample.values[component] += decoded[component] * weight;
  }
  return true;
}

bool sampleMapExportCategoryAtWorld(const MapExportBundle &bundle, const std::string &fieldName,
    MapExportFieldCursor &cursor, const Point2 position, const uint8_t requestedLevel,
    MapExportCategorySample &sample, std::string *error)
{
  sample = {};
  // Resolve and validate with the ordinary cached sampler, including ancestor
  // fallback. Its nearest label remains available to other semantic consumers.
  MapExportFieldSample nearest{};
  if (!sampleMapExportFieldAtWorld(bundle, fieldName, cursor, position, requestedLevel, nearest, error))
    return false;
  const auto &field = *cursor.field;
  const auto &page = *cursor.page;
  if (field.format != MapExportFormat::R8Uint)
    return fail(error, "category mixture requires an R8_UINT field");
  const auto c = filterCoordinates(bundle, page, position.x / bundle.extentMetres, position.y / bundle.extentMetres);
  const double tx = c.tx*c.tx*(3.0-2.0*c.tx), ty = c.ty*c.ty*(3.0-2.0*c.ty);
  const std::array<const uint8_t *, 4> taps{
      texel(field, page, c.x0, c.y0), texel(field, page, c.x1, c.y0),
      texel(field, page, c.x0, c.y1), texel(field, page, c.x1, c.y1)};
  for (size_t tap = 0u; tap < taps.size(); ++tap)
  {
    if (!taps[tap]) return fail(error, "category mixture exceeded its page gutter");
    const double weight = bilinearWeight(tap, tx, ty);
    if (weight == 0.0) continue;
    const uint8_t id = *taps[tap];
    uint32_t slot = 0u;
    while (slot < sample.count && sample.ids[slot] != id) ++slot;
    if (slot == sample.count) sample.ids[sample.count++] = id;
    sample.weights[slot] += weight;
  }
  return true;
}

bool sampleMapExportBiome(const MapExportBundle &bundle, const double u, const double v, const uint8_t requestedLevel, MapExportBiomeSample &sample, std::string *error)
{
  MapExportBiomeCursor cursor{};
  return sampleMapExportBiomeCached(
      bundle, cursor, u, v, requestedLevel, sample, error);
}

bool sampleMapExportBiomeCached(const MapExportBundle &bundle, MapExportBiomeCursor &cursor, const double u, const double v, const uint8_t requestedLevel, MapExportBiomeSample &sample, std::string *error)
{
  sample = {};
  MapExportPageAddress requested{};
  if (!mapExportPageAt(bundle, u, v, requestedLevel, requested))
    return fail(error, "invalid biome sample coordinates or LOD");
  if (!cursor.resolved)
  {
    cursor.ids = bundle.field("biome_mix_ids");
    cursor.weights = bundle.field("biome_mix_weights");
    cursor.resolved = true;
  }
  const MapExportField *ids = cursor.ids;
  const MapExportField *weights = cursor.weights;
  if (ids == nullptr || weights == nullptr || !ids->available || !weights->available || ids->format != MapExportFormat::RGBA8Uint || weights->format != MapExportFormat::RGBA8Unorm ||
      ids->filter != MapExportFilter::SparseMixture || weights->filter != MapExportFilter::SparseMixture || ids->samplingGroup.empty() || ids->samplingGroup != weights->samplingGroup)
    return fail(error, "map export has no valid paired biome mixture");

  const MapExportPage *idPage = nullptr;
  const MapExportPage *weightPage = nullptr;
  if (cursor.idPage != nullptr && cursor.weightPage != nullptr &&
      cursor.requested == requested)
  {
    idPage = cursor.idPage;
    weightPage = cursor.weightPage;
  }
  else if (!resolveBiomePages(*ids, *weights, requested, idPage, weightPage) || idPage == nullptr || weightPage == nullptr)
    return fail(error, "biome mixture has no common resident ancestor");
  cursor.idPage = idPage;
  cursor.weightPage = weightPage;
  cursor.requested = requested;
  if (!(idPage->address == weightPage->address) || idPage->width != weightPage->width || idPage->height != weightPage->height || !validPagePayload(*ids, *idPage) || !validPagePayload(*weights, *weightPage))
    return fail(error, "paired biome pages do not have matching payloads");

  sample.requestedPage = requested;
  sample.resolvedPage = idPage->address;
  const FilterCoordinates coordinates = filterCoordinates(bundle, *idPage, u, v);
  const std::array<uint32_t, 4> xs{coordinates.x0, coordinates.x1, coordinates.x0, coordinates.x1};
  const std::array<uint32_t, 4> ys{coordinates.y0, coordinates.y0, coordinates.y1, coordinates.y1};
  std::array<double, 256> scores{};
  std::array<double, 256> priorities{};
  for (size_t tap = 0u; tap < 4u; ++tap)
  {
    const uint8_t *idTexel = texel(*ids, *idPage, xs[tap], ys[tap]);
    const uint8_t *weightTexel = texel(*weights, *weightPage, xs[tap], ys[tap]);
    if (idTexel == nullptr || weightTexel == nullptr)
      return fail(error, "biome sample exceeded its paired page gutter");
    const double spatial = bilinearWeight(tap, coordinates.tx, coordinates.ty);
    for (size_t slot = 0u; slot < 4u; ++slot)
    {
      const uint8_t id = idTexel[slot];
      if (id < static_cast<uint8_t>(Biome::Count))
      {
        const double contribution = spatial * (weightTexel[slot] / 255.0);
        scores[id] += contribution;
        // Preserve the encoded order when quantization made two stored
        // weights equal. Across taps this becomes a weighted preference for
        // contributors that were dominant in more of the footprint.
        priorities[id] += contribution * static_cast<double>(4u - slot);
      }
    }
  }

  struct Candidate
  {
    uint8_t id = 0u;
    double weight = 0.0;
    double priority = 0.0;
  };
  std::vector<Candidate> candidates;
  for (uint32_t id = 0u; id < static_cast<uint32_t>(Biome::Count); ++id)
    if (scores[id] > std::numeric_limits<double>::epsilon())
      candidates.push_back({static_cast<uint8_t>(id), scores[id], priorities[id]});
  std::stable_sort(
      candidates.begin(),
      candidates.end(),
      [](const Candidate &left, const Candidate &right)
      {
        if (left.weight != right.weight)
          return left.weight > right.weight;
        if (left.priority != right.priority)
          return left.priority > right.priority;
        return left.id < right.id;
      });
  if (candidates.size() > sample.ids.size())
    candidates.resize(sample.ids.size());
  double total = 0.0;
  for (const Candidate &candidate : candidates)
    total += candidate.weight;
  if (!(total > 0.0) || !std::isfinite(total))
    return fail(error, "biome sample has no finite contributors");
  sample.count = static_cast<uint32_t>(candidates.size());
  for (size_t slot = 0u; slot < candidates.size(); ++slot)
  {
    sample.ids[slot] = candidates[slot].id;
    sample.weights[slot] = candidates[slot].weight / total;
  }
  return true;
}

bool sampleMapExportFieldAtWorld(const MapExportBundle &bundle, const std::string &fieldName, const Point2 position, const uint8_t requestedLevel, MapExportFieldSample &sample, std::string *error)
{
  MapExportFieldCursor cursor{};
  return sampleMapExportFieldAtWorld(
      bundle, fieldName, cursor, position, requestedLevel, sample, error);
}

bool sampleMapExportFieldAtWorld(const MapExportBundle &bundle, const std::string &fieldName, MapExportFieldCursor &cursor, const Point2 position, const uint8_t requestedLevel, MapExportFieldSample &sample, std::string *error)
{
  if (!(bundle.extentMetres > 0.0) || !std::isfinite(bundle.extentMetres))
    return fail(error, "map export has no valid world extent");
  return sampleMapExportFieldCached(bundle, fieldName, cursor, position.x / bundle.extentMetres, position.y / bundle.extentMetres, requestedLevel, sample, error);
}

bool sampleMapExportBiomeAtWorld(const MapExportBundle &bundle, const Point2 position, const uint8_t requestedLevel, MapExportBiomeSample &sample, std::string *error)
{
  MapExportBiomeCursor cursor{};
  return sampleMapExportBiomeAtWorld(
      bundle, cursor, position, requestedLevel, sample, error);
}

bool sampleMapExportBiomeAtWorld(const MapExportBundle &bundle, MapExportBiomeCursor &cursor, const Point2 position, const uint8_t requestedLevel, MapExportBiomeSample &sample, std::string *error)
{
  if (!(bundle.extentMetres > 0.0) || !std::isfinite(bundle.extentMetres))
    return fail(error, "map export has no valid world extent");
  return sampleMapExportBiomeCached(bundle, cursor, position.x / bundle.extentMetres, position.y / bundle.extentMetres, requestedLevel, sample, error);
}

} // namespace procedural::world
