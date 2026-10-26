#include "voxel/svraster/SparseVoxelTree64Pages.hpp"

#include <algorithm>
#include <functional>
#include <set>

namespace voxel::svraster
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

} // namespace

void SparseVoxelTree64PageAllocator::reset()
{
  settings_ = {};
  pages_.clear();
  freePages_.clear();
  retiredPages_.clear();
  stats_ = {};
  completedSubmission_ = 0u;
  configured_ = false;
}

bool SparseVoxelTree64PageAllocator::configure(
    const SparseVoxelTree64PageAllocatorSettings &settings,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (settings.pageCount == 0u)
    return fail(error, "Tree64 page allocator needs at least one page");
  if (settings.nodesPerPage == 0u)
    return fail(error, "Tree64 page allocator needs a non-zero page capacity");

  reset();
  settings_ = settings;
  pages_.resize(settings.pageCount);
  freePages_.reserve(settings.pageCount);
  for (uint32_t slot = settings.pageCount; slot > 0u; --slot)
    freePages_.push_back(slot - 1u);
  configured_ = true;
  refreshStats();
  return true;
}

bool SparseVoxelTree64PageAllocator::reserve(
    SparseVoxelTree64PageHandle &handle,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  handle = {};
  if (!configured_)
    return fail(error, "Tree64 page allocator is not configured");
  if (freePages_.empty())
  {
    ++stats_.failedAllocations;
    return fail(error, "Tree64 page pool is exhausted");
  }

  const uint32_t slot = freePages_.back();
  auto &page = pages_[slot];
  if (page.state != SparseVoxelTree64PageState::Free)
    return fail(error, "Tree64 free-page list contains a live page");
  if (page.generation == std::numeric_limits<uint32_t>::max())
  {
    ++stats_.failedAllocations;
    return fail(error, "Tree64 page generation overflow");
  }

  freePages_.pop_back();
  ++page.generation;
  page.state = SparseVoxelTree64PageState::Reserved;
  handle = {slot, page.generation};
  refreshStats();
  return true;
}

bool SparseVoxelTree64PageAllocator::transition(
    const SparseVoxelTree64PageHandle handle,
    const SparseVoxelTree64PageState expected,
    const SparseVoxelTree64PageState target,
    const char *operation,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!configured_)
    return fail(error, "Tree64 page allocator is not configured");
  if (!isCurrent(handle))
  {
    ++stats_.staleOperations;
    return fail(error, std::string("Tree64 ") + operation + " used a stale page handle");
  }
  auto &page = pages_[handle.slot];
  if (page.state != expected)
    return fail(error, std::string("Tree64 ") + operation + " used a page in the wrong state");
  page.state = target;
  refreshStats();
  return true;
}

bool SparseVoxelTree64PageAllocator::beginUpload(
    const SparseVoxelTree64PageHandle handle,
    std::string *error)
{
  return transition(
      handle, SparseVoxelTree64PageState::Reserved,
      SparseVoxelTree64PageState::Uploading, "upload begin", error);
}

bool SparseVoxelTree64PageAllocator::finishUpload(
    const SparseVoxelTree64PageHandle handle,
    std::string *error)
{
  return transition(
      handle, SparseVoxelTree64PageState::Uploading,
      SparseVoxelTree64PageState::Uploaded, "upload completion", error);
}

bool SparseVoxelTree64PageAllocator::commit(
    const SparseVoxelTree64PageHandle handle,
    std::string *error)
{
  return transition(
      handle, SparseVoxelTree64PageState::Uploaded,
      SparseVoxelTree64PageState::Committed, "publication", error);
}

bool SparseVoxelTree64PageAllocator::publish(
    const std::vector<SparseVoxelTree64PageReplacement> &replacements,
    const uint64_t visibilityCompletion,
    std::string *error)
{
  if (!validatePublish(replacements, visibilityCompletion, error))
    return false;

  // No operation below can fail after the complete batch has validated.
  for (const auto &entry : replacements)
    if (entry.replacement.valid())
      pages_[entry.replacement.slot].state =
          SparseVoxelTree64PageState::Committed;
  for (const auto &entry : replacements)
    if (entry.previous.valid())
    {
      auto &page = pages_[entry.previous.slot];
      if (visibilityCompletion <= completedSubmission_)
      {
        page.state = SparseVoxelTree64PageState::Free;
        freePages_.push_back(entry.previous.slot);
      }
      else
      {
        page.state = SparseVoxelTree64PageState::Retired;
        retiredPages_.push_back({entry.previous, visibilityCompletion});
      }
    }
  sortFreePages();
  refreshStats();
  return true;
}

bool SparseVoxelTree64PageAllocator::validatePublish(
    const std::vector<SparseVoxelTree64PageReplacement> &replacements,
    const uint64_t visibilityCompletion,
    std::string *error) const
{
  if (error != nullptr)
    error->clear();
  if (!configured_)
    return fail(error, "Tree64 page allocator is not configured");
  if (visibilityCompletion < completedSubmission_)
    return fail(error, "Tree64 publication completion precedes completed GPU work");

  std::set<uint32_t> touchedSlots;
  for (const auto &entry : replacements)
  {
    if (!entry.previous.valid() && !entry.replacement.valid())
      return fail(error, "Tree64 page replacement has no owner");
    if (entry.previous.valid())
    {
      if (!isCurrent(entry.previous) ||
          pages_[entry.previous.slot].state !=
              SparseVoxelTree64PageState::Committed)
        return fail(error, "Tree64 page replacement has a stale previous owner");
      if (!touchedSlots.insert(entry.previous.slot).second)
        return fail(error, "Tree64 page replacement touches a slot twice");
    }
    if (entry.replacement.valid())
    {
      if (!isCurrent(entry.replacement) ||
          pages_[entry.replacement.slot].state !=
              SparseVoxelTree64PageState::Uploaded)
        return fail(error, "Tree64 page replacement is not upload-complete");
      if (!touchedSlots.insert(entry.replacement.slot).second)
        return fail(error, "Tree64 page replacement touches a slot twice");
    }
  }
  return true;
}

bool SparseVoxelTree64PageAllocator::release(
    const SparseVoxelTree64PageHandle handle,
    const uint64_t retireAfterCompletion,
    std::string *error)
{
  if (retireAfterCompletion < completedSubmission_)
    return fail(error, "Tree64 page retirement precedes completed GPU work");
  auto &page = pages_[handle.slot];
  if (retireAfterCompletion <= completedSubmission_)
  {
    page.state = SparseVoxelTree64PageState::Free;
    freePages_.push_back(handle.slot);
    sortFreePages();
  }
  else
  {
    page.state = SparseVoxelTree64PageState::Retired;
    retiredPages_.push_back({handle, retireAfterCompletion});
  }
  refreshStats();
  return true;
}

bool SparseVoxelTree64PageAllocator::abort(
    const SparseVoxelTree64PageHandle handle,
    const uint64_t retireAfterCompletion,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!configured_)
    return fail(error, "Tree64 page allocator is not configured");
  if (!isCurrent(handle))
  {
    ++stats_.staleOperations;
    return fail(error, "Tree64 abort used a stale page handle");
  }
  const auto currentState = pages_[handle.slot].state;
  if (currentState != SparseVoxelTree64PageState::Reserved &&
      currentState != SparseVoxelTree64PageState::Uploading &&
      currentState != SparseVoxelTree64PageState::Uploaded)
    return fail(error, "Tree64 abort used a page in the wrong state");
  return release(handle, retireAfterCompletion, error);
}

bool SparseVoxelTree64PageAllocator::abortAll(
    const std::vector<SparseVoxelTree64PageHandle> &handles,
    const uint64_t retireAfterCompletion,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!configured_)
    return fail(error, "Tree64 page allocator is not configured");
  if (retireAfterCompletion < completedSubmission_)
    return fail(error, "Tree64 page rollback precedes completed GPU work");
  std::set<uint32_t> slots;
  for (const auto handle : handles)
  {
    if (!isCurrent(handle))
    {
      ++stats_.staleOperations;
      return fail(error, "Tree64 page rollback contains a stale handle");
    }
    const auto state = pages_[handle.slot].state;
    if ((state != SparseVoxelTree64PageState::Reserved &&
         state != SparseVoxelTree64PageState::Uploading &&
         state != SparseVoxelTree64PageState::Uploaded) ||
        !slots.insert(handle.slot).second)
      return fail(error, "Tree64 page rollback contains an invalid page state");
  }
  for (const auto handle : handles)
  {
    auto &page = pages_[handle.slot];
    if (retireAfterCompletion <= completedSubmission_)
    {
      page.state = SparseVoxelTree64PageState::Free;
      freePages_.push_back(handle.slot);
    }
    else
    {
      page.state = SparseVoxelTree64PageState::Retired;
      retiredPages_.push_back({handle, retireAfterCompletion});
    }
  }
  sortFreePages();
  refreshStats();
  return true;
}

bool SparseVoxelTree64PageAllocator::retire(
    const SparseVoxelTree64PageHandle handle,
    const uint64_t retireAfterCompletion,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!configured_)
    return fail(error, "Tree64 page allocator is not configured");
  if (!isCurrent(handle))
  {
    ++stats_.staleOperations;
    return fail(error, "Tree64 retirement used a stale page handle");
  }
  if (pages_[handle.slot].state != SparseVoxelTree64PageState::Committed)
    return fail(error, "Tree64 retirement requires a committed page");
  return release(handle, retireAfterCompletion, error);
}

bool SparseVoxelTree64PageAllocator::reclaimCompleted(
    const uint64_t completedSubmission,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!configured_)
    return fail(error, "Tree64 page allocator is not configured");
  if (completedSubmission < completedSubmission_)
    return fail(error, "Tree64 completed submission token moved backwards");

  completedSubmission_ = completedSubmission;
  auto iterator = retiredPages_.begin();
  while (iterator != retiredPages_.end())
  {
    if (iterator->completion > completedSubmission_)
    {
      ++iterator;
      continue;
    }
    const auto handle = iterator->handle;
    if (!isCurrent(handle) ||
        pages_[handle.slot].state != SparseVoxelTree64PageState::Retired)
      return fail(error, "Tree64 retirement queue contains a stale page");
    pages_[handle.slot].state = SparseVoxelTree64PageState::Free;
    freePages_.push_back(handle.slot);
    iterator = retiredPages_.erase(iterator);
  }
  sortFreePages();
  refreshStats();
  return true;
}

bool SparseVoxelTree64PageAllocator::configured() const
{
  return configured_;
}

bool SparseVoxelTree64PageAllocator::isCurrent(
    const SparseVoxelTree64PageHandle handle) const
{
  return handle.valid() && handle.slot < pages_.size() &&
      pages_[handle.slot].generation == handle.generation;
}

bool SparseVoxelTree64PageAllocator::isReachable(
    const SparseVoxelTree64PageHandle handle) const
{
  return isCurrent(handle) &&
      pages_[handle.slot].state == SparseVoxelTree64PageState::Committed;
}

std::optional<SparseVoxelTree64PageState>
SparseVoxelTree64PageAllocator::state(
    const SparseVoxelTree64PageHandle handle) const
{
  if (!isCurrent(handle))
    return {};
  return pages_[handle.slot].state;
}

SparseVoxelTree64PageAllocatorStats
SparseVoxelTree64PageAllocator::stats() const
{
  return stats_;
}

uint64_t SparseVoxelTree64PageAllocator::completedSubmission() const
{
  return completedSubmission_;
}

void SparseVoxelTree64PageAllocator::refreshStats()
{
  stats_.pageCount = static_cast<uint32_t>(pages_.size());
  stats_.nodesPerPage = settings_.nodesPerPage;
  stats_.freePages = 0u;
  stats_.reservedPages = 0u;
  stats_.uploadingPages = 0u;
  stats_.uploadedPages = 0u;
  stats_.committedPages = 0u;
  stats_.retiredPages = 0u;
  for (const auto &page : pages_)
  {
    switch (page.state)
    {
    case SparseVoxelTree64PageState::Free:
      ++stats_.freePages;
      break;
    case SparseVoxelTree64PageState::Reserved:
      ++stats_.reservedPages;
      break;
    case SparseVoxelTree64PageState::Uploading:
      ++stats_.uploadingPages;
      break;
    case SparseVoxelTree64PageState::Uploaded:
      ++stats_.uploadedPages;
      break;
    case SparseVoxelTree64PageState::Committed:
      ++stats_.committedPages;
      break;
    case SparseVoxelTree64PageState::Retired:
      ++stats_.retiredPages;
      break;
    }
  }
  stats_.highWatermark = std::max(
      stats_.highWatermark, stats_.pageCount - stats_.freePages);
}

void SparseVoxelTree64PageAllocator::sortFreePages()
{
  std::sort(freePages_.begin(), freePages_.end(), std::greater<uint32_t>());
}

} // namespace voxel::svraster
