#include "Device.hpp"
#include "vulkan/vulkan.hpp"
#include <algorithm>

namespace rhi
{

static const Type formatToTypeTable[Format_Count] = {
  // 8-bit formats
  [Format_R8Unorm] = Type_Uint8,
  [Format_R8Snorm] = Type_Int8,
  [Format_R8Uint] = Type_Uint8,
  [Format_R8Sint] = Type_Int8,

  // 16-bit formats
  [Format_R16Uint] = Type_Uint16,
  [Format_R16Sint] = Type_Int16,
  [Format_R16Float] = Type_Float16,
  [Format_RG8Unorm] = Type_Uint8x2,
  [Format_RG8Snorm] = Type_Int8x2,
  [Format_RG8Uint] = Type_Uint8x2,
  [Format_RG8Sint] = Type_Int8x2,

  // 32-bit single channel
  [Format_R32Uint] = Type_Uint32,
  [Format_R32Sint] = Type_Int32,
  [Format_R32Float] = Type_Float32,

  // 32-bit two channel
  [Format_RG16Uint] = Type_Uint16x2,
  [Format_RG16Sint] = Type_Int16x2,
  [Format_RG16Float] = Type_Float16x2,

  // 32-bit four channel (8-bit each)
  [Format_RGBA8Unorm] = Type_Uint8x4,
  [Format_RGBA8UnormSrgb] = Type_Uint8x4,
  [Format_RGBA8Snorm] = Type_Int8x4,
  [Format_RGBA8Uint] = Type_Uint8x4,
  [Format_RGBA8Sint] = Type_Int8x4,
  [Format_BGRA8Unorm] = Type_Uint8x4,
  [Format_BGRA8UnormSrgb] = Type_Uint8x4,

  // 32-bit packed
  [Format_RGB10A2Uint] = Type_Packed_Uint_2_10_10_10,
  [Format_RGB10A2Unorm] = Type_Packed_Uint_2_10_10_10,
  [Format_RG11B10UFloat] = Type_Packed_UFloat_11_11_10,
  [Format_RGB9E5UFloat] = Type_Packed_UFloat_9995,

  // 64-bit formats
  [Format_RG32Uint] = Type_Uint32x2,
  [Format_RG32Sint] = Type_Int32x2,
  [Format_RG32Float] = Type_Float32x2,

  [Format_RGBA16Uint] = Type_Uint16x4,
  [Format_RGBA16Sint] = Type_Int16x4,
  [Format_RGBA16Float] = Type_Float16x4,

  [Format_RGB8Unorm] = Type_Uint8x3,
  [Format_RGB8Snorm] = Type_Int8x3,
  [Format_RGB8Uint] = Type_Uint8x3,
  [Format_RGB8Sint] = Type_Int8x3,

  [Format_RGB16Uint] = Type_Uint16x3,
  [Format_RGB16Sint] = Type_Int16x3,
  [Format_RGB16Float] = Type_Float16x3,

  [Format_RGB32Uint] = Type_Uint32x3,
  [Format_RGB32Sint] = Type_Int32x3,
  [Format_RGB32Float] = Type_Float32x3,

  // 128-bit formats
  [Format_RGBA32Uint] = Type_Uint32x4,
  [Format_RGBA32Sint] = Type_Int32x4,
  [Format_RGBA32Float] = Type_Float32x4,

  // Depth / stencil
  [Format_Stencil8] = Type_Stencil,
  [Format_Depth16Unorm] = Type_Depth,
  [Format_Depth24Plus] = Type_Depth,
  [Format_Depth24PlusStencil8] = Type_Depth,
  [Format_Depth32Float] = Type_Depth,
  [Format_Depth32FloatStencil8] = Type_Depth,
};

static const Format typeToFormatTable[Type_Count] = {
  [Type_None] = Format_None,

  // Unsigned integers
  [Type_Uint8] = Format_R8Uint,
  [Type_Uint8x2] = Format_RG8Uint,
  [Type_Uint8x3] = Format_RGB8Uint,
  [Type_Uint8x4] = Format_RGBA8Uint,

  [Type_Uint16] = Format_R16Uint,
  [Type_Uint16x2] = Format_RG16Uint,
  [Type_Uint16x3] = Format_RGB16Uint,
  [Type_Uint16x4] = Format_RGBA16Uint,

  [Type_Uint32] = Format_R32Uint,
  [Type_Uint32x2] = Format_RG32Uint,
  [Type_Uint32x3] = Format_RGB32Uint,
  [Type_Uint32x4] = Format_RGBA32Uint,

  // Signed integers
  [Type_Int8] = Format_R8Sint,
  [Type_Int8x2] = Format_RG8Sint,
  [Type_Int8x3] = Format_RGB8Sint,
  [Type_Int8x4] = Format_RGBA8Sint,

  [Type_Int16] = Format_R16Sint,
  [Type_Int16x2] = Format_RG16Sint,
  [Type_Int16x3] = Format_RGB16Sint,
  [Type_Int16x4] = Format_RGBA16Sint,

  [Type_Int32] = Format_R32Sint,
  [Type_Int32x2] = Format_RG32Sint,
  [Type_Int32x3] = Format_RGB32Sint,
  [Type_Int32x4] = Format_RGBA32Sint,

  // Floats
  [Type_Float16] = Format_R16Float,
  [Type_Float16x2] = Format_RG16Float,
  [Type_Float16x3] = Format_RGB16Float,
  [Type_Float16x4] = Format_RGBA16Float,

  [Type_Float32] = Format_R32Float,
  [Type_Float32x2] = Format_RG32Float,
  [Type_Float32x3] = Format_RGB32Float,
  [Type_Float32x4] = Format_RGBA32Float,

  // Packed
  [Type_Packed_Uint_2_10_10_10] = Format_RGB10A2Uint,
  [Type_Packed_UFloat_11_11_10] = Format_RG11B10UFloat,
  [Type_Packed_UFloat_9995] = Format_RGB9E5UFloat,

  // Depth / stencil
  [Type_Depth] = Format_Depth32Float,
  [Type_Stencil] = Format_Stencil8,
};

Type formatToType(Format format)
{
  return formatToTypeTable[format];
}

Format typeToFormat(Type type)
{
  return typeToFormatTable[type];
}


size_t formatPixelSize(Format fmt)
{
  switch (fmt)
  {
  case Format_R8Unorm:
  case Format_R8Snorm:
  case Format_R8Uint:
  case Format_R8Sint:
    return 1;

  case Format_R16Uint:
  case Format_R16Sint:
  case Format_R16Float:
  case Format_RG8Unorm:
  case Format_RG8Snorm:
  case Format_RG8Uint:
  case Format_RG8Sint:
    return 2;

  case Format_R32Uint:
  case Format_R32Sint:
  case Format_R32Float:
  case Format_RG16Uint:
  case Format_RG16Sint:
  case Format_RG16Float:
  case Format_RGBA8Unorm:
  case Format_RGBA8UnormSrgb:
  case Format_RGBA8Snorm:
  case Format_RGBA8Uint:
  case Format_RGBA8Sint:
  case Format_BGRA8Unorm:
  case Format_BGRA8UnormSrgb:
  case Format_RGB10A2Uint:
  case Format_RGB10A2Unorm:
  case Format_RG11B10UFloat:
  case Format_RGB9E5UFloat:
    return 4;

  case Format_RG32Uint:
  case Format_RG32Sint:
  case Format_RG32Float:
  case Format_RGBA16Uint:
  case Format_RGBA16Sint:
  case Format_RGBA16Float:
    return 8;

  case Format_RGBA32Uint:
  case Format_RGBA32Sint:
  case Format_RGBA32Float:
    return 16;

  case Format_Stencil8:
    return 1;
  case Format_Depth16Unorm:
    return 2;
  case Format_Depth24Plus:
  case Format_Depth24PlusStencil8:
    return 4;
  case Format_Depth32Float:
  case Format_Depth32FloatStencil8:
    return 4;

  default:
    return 0;
  }
}


// Device *Device::create(DeviceBackend backend, DeviceRequiredLimits limits, std::vector<DeviceFeatures> features) {
//   switch (backend) {
//   case DeviceBackend_Vulkan_1_2:
//     return new vulkan::VulkanDevice(vulkan::VulkanVersion::Vulkan_1_2, limits, features);
//     break;
//   default:
//     break;
//   }

//   return nullptr;
// }

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
} // namespace rhi