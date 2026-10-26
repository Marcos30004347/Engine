#include "procedural/world/MapExportPageCache.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

namespace procedural::world {

namespace {

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
  {
    *error = message;
  }
  return false;
}

struct KeyLess
{
  bool operator()(const MapExportStreamKey &left, const MapExportStreamKey &right) const
  {
    return std::tie(left.field, left.address.level, left.address.x, left.address.y) < std::tie(right.field, right.address.level, right.address.x, right.address.y);
  }
};

struct UnitKey
{
  std::string name;
  MapExportPageAddress address{};
};

struct UnitLess
{
  bool operator()(const UnitKey &left, const UnitKey &right) const
  {
    if (left.address.level != right.address.level)
    {
      return left.address.level < right.address.level;
    }
    return std::tie(left.name, left.address.x, left.address.y) < std::tie(right.name, right.address.x, right.address.y);
  }
};

std::string unitName(const MapExportBundle &bundle, const MapExportStreamKey &key)
{
  const MapExportField *field = bundle.field(key.field);
  return field == nullptr || field->samplingGroup.empty() ? key.field : field->samplingGroup;
}

} // namespace

bool operator==(const MapExportPhysicalMapping &left, const MapExportPhysicalMapping &right)
{
  return left.key == right.key && left.slot == right.slot;
}

struct MapExportPageCache::State
{
  struct Work
  {
    uint64_t revision = 0u;
    UnitKey unit;
    std::vector<MapExportStreamKey> pages;
  };

  struct Prepared
  {
    uint64_t revision = 0u;
    UnitKey unit;
    std::vector<MapExportPhysicalUpload> pages;
    size_t bytes = 0u;
  };

  const MapExportBundle *bundle = nullptr;
  MapExportPageCacheConfig config{};
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable prepared;
  bool stopping = false;
  uint64_t targetRevision = 0u;
  size_t preparingUnits = 0u;
  std::string preparationError;
  std::deque<Work> work;
  std::deque<Prepared> ready;
  std::set<MapExportStreamKey, KeyLess> target;
  std::map<MapExportStreamKey, uint32_t, KeyLess> resident;
  std::vector<std::optional<MapExportStreamKey>> slots;
  std::thread worker;

  ~State()
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      work.clear();
    }
    wake.notify_all();
    if (worker.joinable())
    {
      worker.join();
    }
  }

  void run()
  {
    for (;;)
    {
      Work job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [&] {
          return stopping || !work.empty();
        });
        if (stopping)
        {
          return;
        }
        job = std::move(work.front());
        work.pop_front();
      }

      Prepared result{};
      result.revision = job.revision;
      result.unit = job.unit;
      std::string jobError;
      for (const MapExportStreamKey &key : job.pages)
      {
        const MapExportField *field = bundle->field(key.field);
        const MapExportPage *page = field == nullptr ? nullptr : field->page(key.address);
        MapExportPhysicalUpload upload{};
        upload.key = key;
        if (field == nullptr || page == nullptr)
        {
          jobError = "streaming worker could not resolve a requested page";
          break;
        }
        if (config.directory.empty())
        {
          upload.data = page->data;
        }
        else if (!readMapExportPage(config.directory, *field, *page, upload.data, &jobError))
        {
          break;
        }
        if (upload.data.empty())
        {
          jobError = "streaming worker prepared an empty page";
          break;
        }
        result.bytes += upload.data.size();
        result.pages.push_back(std::move(upload));
      }

      {
        std::lock_guard<std::mutex> lock(mutex);
        if (job.revision == targetRevision)
        {
          if (!jobError.empty() && preparationError.empty())
          {
            preparationError = std::move(jobError);
          }
          else if (jobError.empty())
          {
            ready.push_back(std::move(result));
          }
          if (preparingUnits != 0u)
          {
            --preparingUnits;
          }
        }
      }
      prepared.notify_all();
    }
  }
};

MapExportPageCache::MapExportPageCache() = default;
MapExportPageCache::~MapExportPageCache() = default;

bool MapExportPageCache::initialize(const MapExportBundle &bundle, const MapExportPageCacheConfig &config, std::string *error)
{
  clear();
  if (config.slotCapacity == 0u || bundle.pageSizePixels == 0u)
  {
    return fail(error, "physical page cache configuration is invalid");
  }
  state_ = std::make_unique<State>();
  state_->bundle = &bundle;
  state_->config = config;
  state_->slots.resize(config.slotCapacity);
  state_->worker = std::thread([state = state_.get()] {
    state->run();
  });
  return true;
}

void MapExportPageCache::clear()
{
  state_.reset();
}

bool MapExportPageCache::setTarget(const std::vector<MapExportStreamKey> &requested, std::string *error)
{
  if (state_ == nullptr)
  {
    return fail(error, "physical page cache is not initialized");
  }
  std::set<MapExportStreamKey, KeyLess> target(requested.begin(), requested.end());
  if (target.empty())
  {
    return fail(error, "physical page cache target is empty");
  }

  std::map<UnitKey, std::vector<MapExportStreamKey>, UnitLess> units;
  for (const MapExportStreamKey &key : target)
  {
    const MapExportField *field = state_->bundle->field(key.field);
    if (field == nullptr || !field->available || field->page(key.address) == nullptr)
    {
      return fail(error, "physical page cache target contains an invalid page");
    }
    units[{unitName(*state_->bundle, key), key.address}].push_back(key);
  }

  std::set<std::string> rootUnits;
  size_t rootPages = 0u;
  for (const auto &[unit, pages] : units)
  {
    const MapExportField *firstField = state_->bundle->field(pages.front().field);
    if (!firstField->samplingGroup.empty())
    {
      size_t expectedMembers = 0u;
      for (const MapExportField &field : state_->bundle->fields)
      {
        expectedMembers += field.available && field.samplingGroup == firstField->samplingGroup ? 1u : 0u;
      }
      if (pages.size() != expectedMembers)
      {
        return fail(error, "physical page cache target splits a semantic sampling group");
      }
    }
    if (unit.address.level == 0u)
    {
      rootUnits.insert(unit.name);
      rootPages += pages.size();
    }
  }
  for (const auto &[unit, _] : units)
  {
    if (rootUnits.find(unit.name) == rootUnits.end())
    {
      return fail(error, "physical page cache target does not pin every root");
    }
  }
  if (rootPages > state_->config.slotCapacity)
  {
    return fail(error, "physical page cache has too few slots for pinned roots");
  }
  if (target.size() > state_->config.slotCapacity)
  {
    return fail(error, "physical page cache target exceeds its physical slot capacity");
  }

  std::lock_guard<std::mutex> lock(state_->mutex);
  ++state_->targetRevision;
  state_->target = std::move(target);
  state_->preparationError.clear();
  state_->work.clear();
  state_->ready.clear();
  state_->preparingUnits = 0u;
  for (const auto &[unit, pages] : units)
  {
    const bool complete = std::all_of(pages.begin(), pages.end(), [&](const MapExportStreamKey &key) {
      return state_->resident.find(key) != state_->resident.end();
    });
    if (complete)
    {
      continue;
    }
    state_->work.push_back({state_->targetRevision, unit, pages});
    ++state_->preparingUnits;
  }
  state_->wake.notify_one();
  return true;
}

bool MapExportPageCache::waitForPreparation(const std::chrono::milliseconds timeout, std::string *error)
{
  if (state_ == nullptr)
  {
    return fail(error, "physical page cache is not initialized");
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (!state_->prepared.wait_for(lock, timeout, [&] {
        return state_->preparingUnits == 0u || !state_->preparationError.empty();
      }))
  {
    return fail(error, "timed out waiting for streaming page preparation");
  }
  if (!state_->preparationError.empty())
  {
    return fail(error, state_->preparationError);
  }
  return true;
}

bool MapExportPageCache::publish(const size_t maximumUploadBytes, MapExportPagePublication &publication, std::string *error)
{
  publication = {};
  if (state_ == nullptr)
  {
    return fail(error, "physical page cache is not initialized");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!state_->preparationError.empty())
  {
    return fail(error, state_->preparationError);
  }
  publication.revision = state_->targetRevision;

  for (auto resident = state_->resident.begin(); resident != state_->resident.end();)
  {
    if (state_->target.find(resident->first) != state_->target.end())
    {
      ++resident;
      continue;
    }
    publication.evictions.push_back({resident->first, resident->second});
    state_->slots[resident->second].reset();
    resident = state_->resident.erase(resident);
  }

  auto prepared = state_->ready.begin();
  while (prepared != state_->ready.end())
  {
    if (prepared->revision != state_->targetRevision)
    {
      prepared = state_->ready.erase(prepared);
      continue;
    }
    const bool alreadyResident = std::all_of(prepared->pages.begin(), prepared->pages.end(), [&](const auto &page) {
      return state_->resident.find(page.key) != state_->resident.end();
    });
    if (alreadyResident)
    {
      prepared = state_->ready.erase(prepared);
      continue;
    }
    const size_t freeSlots = static_cast<size_t>(std::count_if(state_->slots.begin(), state_->slots.end(), [](const auto &slot) {
      return !slot.has_value();
    }));
    const bool exceedsUploadBudget = prepared->bytes > maximumUploadBytes - publication.uploadedBytes;
    if (prepared->pages.size() > freeSlots || exceedsUploadBudget)
    {
      ++publication.deferredUnits;
      ++prepared;
      continue;
    }
    for (MapExportPhysicalUpload &page : prepared->pages)
    {
      const auto free = std::find_if(state_->slots.begin(), state_->slots.end(), [](const auto &slot) {
        return !slot;
      });
      const uint32_t slot = static_cast<uint32_t>(std::distance(state_->slots.begin(), free));
      page.slot = slot;
      state_->slots[slot] = page.key;
      state_->resident[page.key] = slot;
      publication.uploadedBytes += page.data.size();
      publication.uploads.push_back(std::move(page));
    }
    prepared = state_->ready.erase(prepared);
  }

  for (const auto &[key, slot] : state_->resident)
  {
    publication.pageTable.push_back({key, slot});
  }
  publication.residentPages = state_->resident.size();
  publication.occupiedSlots = state_->resident.size();
  publication.preparingUnits = state_->preparingUnits;
  publication.preparedUnits = state_->ready.size();
  return true;
}

std::optional<uint32_t> MapExportPageCache::residentSlot(const MapExportStreamKey &key) const
{
  if (state_ == nullptr)
  {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto found = state_->resident.find(key);
  return found == state_->resident.end() ? std::nullopt : std::optional<uint32_t>(found->second);
}

std::optional<uint32_t> MapExportPageCache::bestResidentSlot(const std::string &field, MapExportPageAddress address) const
{
  for (;;)
  {
    if (const auto slot = residentSlot({field, address}); slot.has_value())
    {
      return slot;
    }
    if (address.level == 0u)
    {
      return std::nullopt;
    }
    address = mapExportPageParent(address);
  }
}

uint64_t MapExportPageCache::revision() const
{
  if (state_ == nullptr)
  {
    return 0u;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->targetRevision;
}

} // namespace procedural::world
