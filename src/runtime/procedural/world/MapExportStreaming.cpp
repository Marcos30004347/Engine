#include "procedural/world/MapExportStreaming.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <tuple>

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

bool keyLess(const MapExportStreamKey &left, const MapExportStreamKey &right)
{
  return std::tie(left.field, left.address.level, left.address.x, left.address.y) < std::tie(right.field, right.address.level, right.address.x, right.address.y);
}

struct StreamUnit
{
  std::string name;
  MapExportPageAddress logical{};
  MapExportPageAddress resolved{};
  std::vector<MapExportStreamKey> pages;
  size_t bytes = 0u;
  double distance = 0.0;
  bool root = false;
};

bool sameUnit(const StreamUnit &left, const StreamUnit &right)
{
  return left.name == right.name && left.resolved == right.resolved;
}

std::vector<std::vector<const MapExportField *>> requestedGroups(const MapExportBundle &bundle, const std::vector<std::string> &names, std::string *error)
{
  std::set<std::string> requested;
  for (const std::string &name : names)
  {
    const MapExportField *field = bundle.field(name);
    if (field == nullptr || !field->available)
    {
      if (error != nullptr)
        *error = "stream request names a missing or unavailable field: " + name;
      return {};
    }
    requested.insert(name);
    if (!field->samplingGroup.empty())
      for (const MapExportField &candidate : bundle.fields)
        if (candidate.available && candidate.samplingGroup == field->samplingGroup)
          requested.insert(candidate.name);
  }

  std::vector<std::vector<const MapExportField *>> groups;
  std::set<std::string> consumed;
  for (const std::string &name : requested)
  {
    if (consumed.find(name) != consumed.end())
      continue;
    const MapExportField *field = bundle.field(name);
    std::vector<const MapExportField *> group;
    if (field->samplingGroup.empty())
      group.push_back(field);
    else
      for (const std::string &member : requested)
      {
        const MapExportField *candidate = bundle.field(member);
        if (candidate->samplingGroup == field->samplingGroup)
          group.push_back(candidate);
      }
    std::sort(
        group.begin(),
        group.end(),
        [](const MapExportField *left, const MapExportField *right)
        {
          return left->name < right->name;
        });
    for (const MapExportField *member : group)
      consumed.insert(member->name);
    groups.push_back(std::move(group));
  }
  return groups;
}

bool resolveUnit(const std::vector<const MapExportField *> &fields, MapExportPageAddress logical, StreamUnit &unit)
{
  unit.logical = logical;
  unit.name = fields.front()->samplingGroup.empty() ? fields.front()->name : fields.front()->samplingGroup;
  for (;;)
  {
    bool complete = true;
    for (const MapExportField *field : fields)
      complete = complete && field->page(logical) != nullptr;
    if (complete)
      break;
    if (logical.level == 0u)
      return false;
    logical = mapExportPageParent(logical);
  }
  unit.resolved = logical;
  for (const MapExportField *field : fields)
  {
    const MapExportPage *page = field->page(logical);
    if (page == nullptr)
      return false;
    unit.pages.push_back({field->name, logical});
    unit.bytes += page->data.empty() ? static_cast<size_t>(page->width) * page->height * mapExportTexelBytes(field->format) : page->data.size();
  }
  return true;
}

} // namespace

bool operator==(const MapExportStreamKey &left, const MapExportStreamKey &right)
{
  return left.field == right.field && left.address == right.address;
}

bool MapExportStreamingCache::initialize(const MapExportBundle &bundle, std::string *error)
{
  clear();
  if (bundle.pageSizePixels == 0u || bundle.maximumPageLevel > 31u)
    return fail(error, "streaming cache received an invalid map bundle");
  bundle_ = &bundle;
  return true;
}

void MapExportStreamingCache::clear()
{
  bundle_ = nullptr;
  resident_.clear();
  residentBytes_ = 0u;
}

bool MapExportStreamingCache::update(const MapExportStreamRequest &request, MapExportStreamUpdate &update, std::string *error)
{
  update = {};
  if (bundle_ == nullptr)
    return fail(error, "streaming cache is not initialized");
  if (request.fields.empty() || request.byteBudget == 0u || request.level > bundle_->maximumPageLevel || request.level > 31u)
    return fail(error, "stream request has no fields, budget, or valid LOD");
  for (const double bound : request.normalizedBounds)
    if (!std::isfinite(bound))
      return fail(error, "stream request bounds must be finite");

  std::string groupError;
  const std::vector<std::vector<const MapExportField *>> groups = requestedGroups(*bundle_, request.fields, &groupError);
  if (groups.empty())
    return fail(error, groupError.empty() ? "stream request resolved no fields" : groupError);

  const double minimumU = std::clamp(std::min(request.normalizedBounds[0], request.normalizedBounds[2]), 0.0, 1.0);
  const double minimumV = std::clamp(std::min(request.normalizedBounds[1], request.normalizedBounds[3]), 0.0, 1.0);
  const double maximumU = std::clamp(std::max(request.normalizedBounds[0], request.normalizedBounds[2]), 0.0, 1.0);
  const double maximumV = std::clamp(std::max(request.normalizedBounds[1], request.normalizedBounds[3]), 0.0, 1.0);
  const uint32_t dimension = uint32_t{1u} << request.level;
  const auto firstPage = [dimension](const double value)
  {
    return std::min(static_cast<uint32_t>(value * dimension), dimension - 1u);
  };
  const auto lastPage = [dimension](const double value)
  {
    const double scaled = value * dimension;
    const uint32_t exclusive = static_cast<uint32_t>(std::ceil(scaled));
    return std::min(exclusive == 0u ? 0u : exclusive - 1u, dimension - 1u);
  };
  uint32_t firstX = firstPage(minimumU);
  uint32_t firstY = firstPage(minimumV);
  uint32_t lastX = std::max(firstX, lastPage(maximumU));
  uint32_t lastY = std::max(firstY, lastPage(maximumV));
  const uint32_t guard = std::min(request.guardPages, dimension - 1u);
  firstX = firstX > guard ? firstX - guard : 0u;
  firstY = firstY > guard ? firstY - guard : 0u;
  lastX = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(lastX) + guard, dimension - 1u));
  lastY = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(lastY) + guard, dimension - 1u));
  const double centreX = (minimumU + maximumU) * 0.5 * dimension - 0.5;
  const double centreY = (minimumV + maximumV) * 0.5 * dimension - 0.5;

  std::vector<StreamUnit> units;
  size_t ancestorRequests = 0u;
  for (const std::vector<const MapExportField *> &group : groups)
  {
    StreamUnit root{};
    if (!resolveUnit(group, {0u, 0u, 0u}, root))
      return fail(error, "streaming field group has no common root page");
    root.root = true;
    units.push_back(std::move(root));
    for (uint32_t y = firstY; y <= lastY; ++y)
      for (uint32_t x = firstX; x <= lastX; ++x)
      {
        StreamUnit unit{};
        if (!resolveUnit(group, {request.level, x, y}, unit))
          return fail(error, "streaming field group has no resident ancestor");
        ancestorRequests += unit.resolved.level < unit.logical.level ? 1u : 0u;
        const double dx = static_cast<double>(x) - centreX;
        const double dy = static_cast<double>(y) - centreY;
        unit.distance = dx * dx + dy * dy;
        if (std::none_of(
                units.begin(),
                units.end(),
                [&](const StreamUnit &candidate)
                {
                  return sameUnit(candidate, unit);
                }))
          units.push_back(std::move(unit));
      }
  }
  update.requestedUnits = units.size();
  update.ancestorUnits = ancestorRequests;
  std::stable_sort(
      units.begin(),
      units.end(),
      [](const StreamUnit &left, const StreamUnit &right)
      {
        if (left.root != right.root)
          return left.root;
        if (left.distance != right.distance)
          return left.distance < right.distance;
        if (left.name != right.name)
          return left.name < right.name;
        return std::tie(left.resolved.level, left.resolved.x, left.resolved.y) < std::tie(right.resolved.level, right.resolved.x, right.resolved.y);
      });

  std::vector<MapExportStreamKey> target;
  size_t targetBytes = 0u;
  for (const StreamUnit &unit : units)
  {
    if (unit.bytes > request.byteBudget - targetBytes)
    {
      if (unit.root)
        return fail(error, "stream byte budget cannot hold all pinned roots");
      ++update.deferredUnits;
      continue;
    }
    targetBytes += unit.bytes;
    target.insert(target.end(), unit.pages.begin(), unit.pages.end());
  }
  std::sort(target.begin(), target.end(), keyLess);
  target.erase(std::unique(target.begin(), target.end()), target.end());
  std::vector<MapExportStreamKey> previous = resident_;
  std::sort(previous.begin(), previous.end(), keyLess);
  std::set_difference(target.begin(), target.end(), previous.begin(), previous.end(), std::back_inserter(update.loads), keyLess);
  std::set_difference(previous.begin(), previous.end(), target.begin(), target.end(), std::back_inserter(update.evictions), keyLess);
  resident_ = std::move(target);
  residentBytes_ = targetBytes;
  update.residentBytes = residentBytes_;
  update.residentPages = resident_.size();
  return true;
}

bool MapExportStreamingCache::resident(const MapExportStreamKey &key) const
{
  return std::find(resident_.begin(), resident_.end(), key) != resident_.end();
}

const MapExportPage *MapExportStreamingCache::page(const MapExportStreamKey &key) const
{
  if (bundle_ == nullptr || !resident(key))
    return nullptr;
  const MapExportField *field = bundle_->field(key.field);
  return field == nullptr ? nullptr : field->page(key.address);
}

const MapExportPage *MapExportStreamingCache::bestResidentPage(const std::string &fieldName, MapExportPageAddress address) const
{
  if (bundle_ == nullptr)
    return nullptr;
  for (;;)
  {
    const MapExportStreamKey key{fieldName, address};
    if (const MapExportPage *found = page(key); found != nullptr)
      return found;
    if (address.level == 0u)
      return nullptr;
    address = mapExportPageParent(address);
  }
}

} // namespace procedural::world
