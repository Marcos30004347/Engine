#include "rhi.hpp"
#include "vulkan/vulkan.hpp"
#include <algorithm>

using namespace rhi;

Device *Device::create(DeviceBackend backend, DeviceRequiredLimits limits, std::vector<DeviceFeatures> features) {
  switch (backend) {
  case DeviceBackend_Vulkan_1_2:
    return new vulkan::VulkanDevice(vulkan::VulkanVersion::Vulkan_1_2, limits, features);
    break;
  default:
    break;
  }

  return nullptr;
}

/*
BufferHeapAllocator::BufferHeapAllocator(Device *device, size_t totalSize, BufferUsage usage) : device(device), totalSize(totalSize), usage(usage) {
  buffer = device->createBuffer(totalSize, usage, nullptr);
  freeRegions.emplace_back(0, totalSize);
}

BufferHeapAllocator::~BufferHeapAllocator() { device->destroyBuffer(buffer); }

BufferSlice BufferHeapAllocator::allocate(size_t size, size_t alignment) {
  for (auto it = freeRegions.begin(); it != freeRegions.end(); ++it) {
    size_t alignedOffset = alignUp(it->first, alignment);
    size_t padding = alignedOffset - it->first;

    if (it->second >= size + padding) {
      BufferSlice slice{buffer, alignedOffset, size};

      size_t remaining = it->second - (size + padding);
      size_t nextOffset = alignedOffset + size;

      if (remaining > 0) {
        *it = {nextOffset, remaining};
      } else {
        freeRegions.erase(it);
      }

      return slice;
    }
  }

  BufferSlice nullslice;
  nullslice.size = 0;
  nullslice.offset = -1;
  nullslice.handle = static_cast<BufferHandle>(-1);

  return nullslice;
}

void BufferHeapAllocator::free(const BufferSlice &slice) {
  freeRegions.emplace_back(slice.offset, slice.size);
  mergeFreeRegions();
}

size_t BufferHeapAllocator::alignUp(size_t value, size_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

void BufferHeapAllocator::mergeFreeRegions() {
  std::sort(freeRegions.begin(), freeRegions.end());

  std::vector<std::pair<size_t, size_t>> merged;
  for (const auto &region : freeRegions) {
    if (!merged.empty() && merged.back().first + merged.back().second == region.first) {
      merged.back().second += region.second;
    } else {
      merged.push_back(region);
    }
  }

  freeRegions = std::move(merged);
}

BufferStackAllocator::BufferStackAllocator(Device *device, size_t totalSize, BufferUsage usage)
    : device(device), totalSize(totalSize), usage(usage), top(0) {
  buffer = device->createBuffer(totalSize, usage, nullptr);
}

BufferStackAllocator::~BufferStackAllocator() { device->destroyBuffer(buffer); }

BufferSlice BufferStackAllocator::allocate(size_t size, size_t alignment) {
  size_t alignedTop = alignUp(top, alignment);
  if (alignedTop + size > totalSize) {
    BufferSlice nullslice;
    nullslice.size = 0;
    nullslice.offset = -1;
    nullslice.handle = static_cast<BufferHandle>(-1);

    return nullslice;
  }

  BufferSlice slice{buffer, alignedTop, size};
  allocationStack.push_back(top);
  top = alignedTop + size;
  return slice;
}


void BufferStackAllocator::reset() {
  top = 0;
  allocationStack.clear();
}

size_t BufferStackAllocator::alignUp(size_t value, size_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
*/