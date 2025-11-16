#include "./VulkanRHI.hpp"
#include "os/Logger.hpp"
#include <set>
#include <vulkan/vulkan_format_traits.hpp>

namespace rendering
{
namespace backend
{
namespace vulkan
{

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif

struct VulkanPhysicalDevice
{
  VkPhysicalDevice device;
  DeviceFeatures feature_flags;
  DeviceProperties properties;
};

static VkBufferUsageFlags toVkBufferUsageFlags(BufferUsage usage)
{
  VkBufferUsageFlags flags = 0;

  if (usage & BufferUsage_Uniform)
    flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (usage & BufferUsage_Storage)
    flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (usage & BufferUsage_Vertex)
    flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (usage & BufferUsage_Index)
    flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (usage & BufferUsage_Indirect)
    flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if (usage & BufferUsage_Timestamp)
    flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT; // Typically written by queries
  if (usage & (BufferUsage_Push | BufferUsage_Pull))
    flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  return flags;
}

static VkMemoryPropertyFlags toVkMemoryPropertyFlags(BufferUsage usage, bool persistent)
{
  VkMemoryPropertyFlags flags = 0;

  if (usage & BufferUsage_Push)
    flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  else if (usage & BufferUsage_Pull)
    flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  else
    flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  if (persistent)
  {
    flags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  }
  return flags;
}

static VkImageUsageFlags toVkImageUsageFlags(ImageUsage usage)
{
  VkImageUsageFlags flags = 0;
  if (usage & ImageUsage_Sampled)
    flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (usage & ImageUsage_Storage)
    flags |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (usage & ImageUsage_ColorAttachment)
    flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (usage & ImageUsage_DepthStencilAttachment)
    flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (usage & ImageUsage_TransferSrc)
    flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (usage & ImageUsage_TransferDst)
    flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

  return flags;
}
static VkFormat toVkFormat(Format fmt)
{
  switch (fmt)
  {
  case Format_R8Unorm:
    return VK_FORMAT_R8_UNORM;
  case Format_R8Snorm:
    return VK_FORMAT_R8_SNORM;
  case Format_R8Uint:
    return VK_FORMAT_R8_UINT;
  case Format_R8Sint:
    return VK_FORMAT_R8_SINT;

  case Format_R16Uint:
    return VK_FORMAT_R16_UINT;
  case Format_R16Sint:
    return VK_FORMAT_R16_SINT;
  case Format_R16Float:
    return VK_FORMAT_R16_SFLOAT;

  case Format_RG8Unorm:
    return VK_FORMAT_R8G8_UNORM;
  case Format_RG8Snorm:
    return VK_FORMAT_R8G8_SNORM;
  case Format_RG8Uint:
    return VK_FORMAT_R8G8_UINT;
  case Format_RG8Sint:
    return VK_FORMAT_R8G8_SINT;

  case Format_R32Uint:
    return VK_FORMAT_R32_UINT;
  case Format_R32Sint:
    return VK_FORMAT_R32_SINT;
  case Format_R32Float:
    return VK_FORMAT_R32_SFLOAT;

  case Format_RG16Uint:
    return VK_FORMAT_R16G16_UINT;
  case Format_RG16Sint:
    return VK_FORMAT_R16G16_SINT;
  case Format_RG16Float:
    return VK_FORMAT_R16G16_SFLOAT;

  case Format_RGBA8Unorm:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case Format_RGBA8UnormSrgb:
    return VK_FORMAT_R8G8B8A8_SRGB;
  case Format_RGBA8Snorm:
    return VK_FORMAT_R8G8B8A8_SNORM;
  case Format_RGBA8Uint:
    return VK_FORMAT_R8G8B8A8_UINT;
  case Format_RGBA8Sint:
    return VK_FORMAT_R8G8B8A8_SINT;

  case Format_BGRA8Unorm:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case Format_BGRA8UnormSrgb:
    return VK_FORMAT_B8G8R8A8_SRGB;

  case Format_RGB10A2Uint:
    return VK_FORMAT_A2B10G10R10_UINT_PACK32;
  case Format_RGB10A2Unorm:
    return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  case Format_RG11B10UFloat:
    return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
  case Format_RGB9E5UFloat:
    return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;

  case Format_RG32Uint:
    return VK_FORMAT_R32G32_UINT;
  case Format_RG32Sint:
    return VK_FORMAT_R32G32_SINT;
  case Format_RG32Float:
    return VK_FORMAT_R32G32_SFLOAT;

  case Format_RGBA16Uint:
    return VK_FORMAT_R16G16B16A16_UINT;
  case Format_RGBA16Sint:
    return VK_FORMAT_R16G16B16A16_SINT;
  case Format_RGBA16Float:
    return VK_FORMAT_R16G16B16A16_SFLOAT;

  case Format_RGBA32Uint:
    return VK_FORMAT_R32G32B32A32_UINT;
  case Format_RGBA32Sint:
    return VK_FORMAT_R32G32B32A32_SINT;
  case Format_RGBA32Float:
    return VK_FORMAT_R32G32B32A32_SFLOAT;

  case Format_Stencil8:
    return VK_FORMAT_S8_UINT;
  case Format_Depth16Unorm:
    return VK_FORMAT_D16_UNORM;
  case Format_Depth24Plus:
    return VK_FORMAT_D24_UNORM_S8_UINT; // Approximation
  case Format_Depth24PlusStencil8:
    return VK_FORMAT_D24_UNORM_S8_UINT;
  case Format_Depth32Float:
    return VK_FORMAT_D32_SFLOAT;
  case Format_Depth32FloatStencil8:
    return VK_FORMAT_D32_SFLOAT_S8_UINT;
  case Format_RGB8Unorm:
    return VK_FORMAT_R8G8B8_UNORM;
  case Format_RGB8Snorm:
    return VK_FORMAT_R8G8B8_SNORM;
  case Format_RGB8Uint:
    return VK_FORMAT_R8G8B8_UINT;
  case Format_RGB8Sint:
    return VK_FORMAT_R8G8B8_SINT;

  case Format_RGB16Uint:
    return VK_FORMAT_R16G16B16_UINT;
  case Format_RGB16Sint:
    return VK_FORMAT_R16G16B16_SINT;
  case Format_RGB16Float:
    return VK_FORMAT_R16G16B16_SFLOAT;

  case Format_RGB32Uint:
    return VK_FORMAT_R32G32B32_UINT;
  case Format_RGB32Sint:
    return VK_FORMAT_R32G32B32_SINT;
  case Format_RGB32Float:
    return VK_FORMAT_R32G32B32_SFLOAT;
  default:
    assert(false);
  }

  return VK_FORMAT_UNDEFINED;
}

static Format vkFormatToFormat(VkFormat vkFmt)
{
  switch (vkFmt)
  {
  case VK_FORMAT_R8_UNORM:
    return Format_R8Unorm;
  case VK_FORMAT_R8_SNORM:
    return Format_R8Snorm;
  case VK_FORMAT_R8_UINT:
    return Format_R8Uint;
  case VK_FORMAT_R8_SINT:
    return Format_R8Sint;

  case VK_FORMAT_R16_UINT:
    return Format_R16Uint;
  case VK_FORMAT_R16_SINT:
    return Format_R16Sint;
  case VK_FORMAT_R16_SFLOAT:
    return Format_R16Float;

  case VK_FORMAT_R8G8_UNORM:
    return Format_RG8Unorm;
  case VK_FORMAT_R8G8_SNORM:
    return Format_RG8Snorm;
  case VK_FORMAT_R8G8_UINT:
    return Format_RG8Uint;
  case VK_FORMAT_R8G8_SINT:
    return Format_RG8Sint;

  case VK_FORMAT_R32_UINT:
    return Format_R32Uint;
  case VK_FORMAT_R32_SINT:
    return Format_R32Sint;
  case VK_FORMAT_R32_SFLOAT:
    return Format_R32Float;

  case VK_FORMAT_R16G16_UINT:
    return Format_RG16Uint;
  case VK_FORMAT_R16G16_SINT:
    return Format_RG16Sint;
  case VK_FORMAT_R16G16_SFLOAT:
    return Format_RG16Float;

  case VK_FORMAT_R8G8B8A8_UNORM:
    return Format_RGBA8Unorm;
  case VK_FORMAT_R8G8B8A8_SRGB:
    return Format_RGBA8UnormSrgb;
  case VK_FORMAT_R8G8B8A8_SNORM:
    return Format_RGBA8Snorm;
  case VK_FORMAT_R8G8B8A8_UINT:
    return Format_RGBA8Uint;
  case VK_FORMAT_R8G8B8A8_SINT:
    return Format_RGBA8Sint;

  case VK_FORMAT_B8G8R8A8_UNORM:
    return Format_BGRA8Unorm;
  case VK_FORMAT_B8G8R8A8_SRGB:
    return Format_BGRA8UnormSrgb;

  case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    return Format_RGB10A2Uint;
  case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    return Format_RGB10A2Unorm;
  case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    return Format_RG11B10UFloat;
  case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    return Format_RGB9E5UFloat;

  case VK_FORMAT_R32G32_UINT:
    return Format_RG32Uint;
  case VK_FORMAT_R32G32_SINT:
    return Format_RG32Sint;
  case VK_FORMAT_R32G32_SFLOAT:
    return Format_RG32Float;

  case VK_FORMAT_R16G16B16A16_UINT:
    return Format_RGBA16Uint;
  case VK_FORMAT_R16G16B16A16_SINT:
    return Format_RGBA16Sint;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return Format_RGBA16Float;

  case VK_FORMAT_R32G32B32A32_UINT:
    return Format_RGBA32Uint;
  case VK_FORMAT_R32G32B32A32_SINT:
    return Format_RGBA32Sint;
  case VK_FORMAT_R32G32B32A32_SFLOAT:
    return Format_RGBA32Float;

  case VK_FORMAT_S8_UINT:
    return Format_Stencil8;
  case VK_FORMAT_D16_UNORM:
    return Format_Depth16Unorm;
  case VK_FORMAT_D24_UNORM_S8_UINT:
    return Format_Depth24PlusStencil8; // also used for Depth24Plus
  case VK_FORMAT_D32_SFLOAT:
    return Format_Depth32Float;
  case VK_FORMAT_D32_SFLOAT_S8_UINT:
    return Format_Depth32FloatStencil8;
  case VK_FORMAT_R8G8B8_UNORM:
    return Format_RGB8Unorm;
  case VK_FORMAT_R8G8B8_SNORM:
    return Format_RGB8Snorm;
  case VK_FORMAT_R8G8B8_UINT:
    return Format_RGB8Uint;
  case VK_FORMAT_R8G8B8_SINT:
    return Format_RGB8Sint;

  case VK_FORMAT_R16G16B16_UINT:
    return Format_RGB16Uint;
  case VK_FORMAT_R16G16B16_SINT:
    return Format_RGB16Sint;
  case VK_FORMAT_R16G16B16_SFLOAT:
    return Format_RGB16Float;

  case VK_FORMAT_R32G32B32_UINT:
    return Format_RGB32Uint;
  case VK_FORMAT_R32G32B32_SINT:
    return Format_RGB32Sint;
  case VK_FORMAT_R32G32B32_SFLOAT:
    return Format_RGB32Float;
  default:
    assert(false); // Invalid / unsupported
  }
  return Format_None;
}

static VkImageAspectFlags toVkImageAspectFlags(ImageAspectFlags flags)
{
  VkImageAspectFlags aspect = 0;
  if ((uint32_t)flags & (uint32_t)ImageAspectFlags::Color)
    aspect |= VK_IMAGE_ASPECT_COLOR_BIT;
  if ((uint32_t)flags & (uint32_t)ImageAspectFlags::Depth)
    aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
  if ((uint32_t)flags & (uint32_t)ImageAspectFlags::Stencil)
    aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
  return aspect;
}

static VkFilter toVkFilter(Filter filter)
{
  switch (filter)
  {
  case Filter::Nearest:
    return VK_FILTER_NEAREST;
  case Filter::Linear:
    return VK_FILTER_LINEAR;
  default:
    return VK_FILTER_LINEAR;
  }
}

VkImageLayout toVkImageLayout(ResourceLayout layout)
{
  switch (layout)
  {
  case ResourceLayout::UNDEFINED:
    return VK_IMAGE_LAYOUT_UNDEFINED;
  case ResourceLayout::GENERAL:
    return VK_IMAGE_LAYOUT_GENERAL;
  case ResourceLayout::COLOR_ATTACHMENT:
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  case ResourceLayout::DEPTH_STENCIL_ATTACHMENT:
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  case ResourceLayout::DEPTH_STENCIL_READ_ONLY:
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  case ResourceLayout::SHADER_READ_ONLY:
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  case ResourceLayout::TRANSFER_SRC:
    return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  case ResourceLayout::TRANSFER_DST:
    return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  case ResourceLayout::PREINITIALIZED:
    return VK_IMAGE_LAYOUT_PREINITIALIZED;
  case ResourceLayout::PRESENT_SRC:
    return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  default:
    throw std::runtime_error("Unsupported ResourceLayout");
  }
}

static VkSamplerAddressMode toVkSamplerAddressMode(SamplerAddressMode mode)
{
  switch (mode)
  {
  case SamplerAddressMode::Repeat:
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  case SamplerAddressMode::MirroredRepeat:
    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  case SamplerAddressMode::ClampToEdge:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  case SamplerAddressMode::ClampToBorder:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  default:
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}

static VkShaderStageFlags toVkShaderStageFlags(BindingVisibility vis)
{
  VkShaderStageFlags flags = 0;
  if (vis & BindingVisibility::BindingVisibility_Vertex)
    flags |= VK_SHADER_STAGE_VERTEX_BIT;
  if (vis & BindingVisibility::BindingVisibility_Fragment)
    flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
  if (vis & BindingVisibility::BindingVisibility_Compute)
    flags |= VK_SHADER_STAGE_COMPUTE_BIT;
  return flags;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void *pUserData)
{
  std::cerr << "Validation layer: " << pCallbackData->pMessage << std::endl;
  return VK_FALSE;
}

void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT &createInfo)
{
  createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  createInfo.pfnUserCallback = debugCallback;
}

static uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties, VkPhysicalDevice physicalDevice)
{
  VkPhysicalDeviceMemoryProperties memProperties;
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

  for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
  {
    if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
      return i;
  }

  throw std::runtime_error("Failed to find suitable memory type!");
}

static VkDescriptorSetLayoutBinding bufferEntryToBinding(const BindingGroupLayoutBufferEntry &entry)
{
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = entry.binding;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  binding.descriptorCount = 1;
  binding.stageFlags = toVkShaderStageFlags(entry.visibility);
  binding.pImmutableSamplers = nullptr;
  return binding;
}

static VkDescriptorSetLayoutBinding samplerEntryToBinding(const BindingGroupLayoutSamplerEntry &entry)
{
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = entry.binding;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = toVkShaderStageFlags(entry.visibility);
  binding.pImmutableSamplers = nullptr;
  return binding;
}

static VkDescriptorSetLayoutBinding textureEntryToBinding(const BindingGroupLayoutTextureEntry &entry)
{
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = entry.binding;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  binding.descriptorCount = 1;
  binding.stageFlags = toVkShaderStageFlags(entry.visibility);
  binding.pImmutableSamplers = nullptr;
  return binding;
}

static VkDescriptorSetLayoutBinding storageTextureEntryToBinding(const BindingGroupLayoutStorageTextureEntry &entry)
{
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = entry.binding;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  binding.descriptorCount = 1;
  binding.stageFlags = toVkShaderStageFlags(entry.visibility);
  binding.pImmutableSamplers = nullptr;
  return binding;
}

std::vector<VulkanPhysicalDevice> getMatchingDevices(VkInstance instance, const DeviceRequiredLimits &requiredLimits)
{
  uint32_t deviceCount = 0;
  vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
  if (deviceCount == 0)
  {
    throw std::runtime_error("No Vulkan physical devices found.");
  }

  std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
  vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());

  std::vector<VulkanPhysicalDevice> matchingDevices;

  for (const auto &device : physicalDevices)
  {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);

    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(device, &features);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device, &memProps);

    VkPhysicalDeviceSubgroupProperties subgroupProps{};
    subgroupProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;

    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &subgroupProps;
    vkGetPhysicalDeviceProperties2(device, &props2);

    size_t totalMemory = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i)
    {
      if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      {
        totalMemory += memProps.memoryHeaps[i].size;
      }
    }

    VkPhysicalDeviceShaderAtomicInt64Features atomic64Features = {};
    atomic64Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &atomic64Features;
    vkGetPhysicalDeviceFeatures2(device, &features2);

    DeviceFeatures featureFlags = DeviceFeatures_None;

    featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Atomic32_AllOps);

    if (atomic64Features.shaderBufferInt64Atomics)
    {
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Atomic64_MinMax);
    }
    if (atomic64Features.shaderSharedInt64Atomics)
    {
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Atomic64_AllOps);
    }
    if (features.shaderInt64)
    {
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Atomic64_MinMax);
    }
    if (features.drawIndirectFirstInstance)
    {
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_DrawIndirectFirstInstance);
    }
    if (features.multiDrawIndirect)
    {
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_MultiDrawIndirect);
    }
    if (features.geometryShader)
    {
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_GeometryShader);
    }

    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
    {
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Integrated);
    }
    else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
    {
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Dedicated);
    }

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> properties(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, properties.data());

    bool hasTimestamp = std::any_of(
        properties.begin(),
        properties.end(),
        [](const auto &q)
        {
          return q.timestampValidBits > 0;
        });
    bool hasCompute = std::any_of(
        properties.begin(),
        properties.end(),
        [](const auto &q)
        {
          return q.queueFlags & VK_QUEUE_COMPUTE_BIT;
        });
    bool hasGraphics = std::any_of(
        properties.begin(),
        properties.end(),
        [](const auto &q)
        {
          return q.queueFlags & VK_QUEUE_GRAPHICS_BIT;
        });
    if (hasCompute)
    {
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Compute);
    }
    if (hasGraphics)
    {
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Graphics);
    }
    if (hasTimestamp)
    {
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Timestamp);
    }

    DeviceProperties dprops{};

    dprops.sugroupSize = subgroupProps.subgroupSize;
    dprops.maxMemory = totalMemory;
    dprops.maxComputeSharedMemorySize = props.limits.maxComputeSharedMemorySize;
    dprops.maxComputeWorkGroupInvocations = props.limits.maxComputeWorkGroupInvocations;
    dprops.uniformBufferAlignment = props.limits.minUniformBufferOffsetAlignment;

    if (dprops.maxMemory >= requiredLimits.minimumMemory && dprops.maxComputeSharedMemorySize >= requiredLimits.minimumComputeSharedMemory &&
        dprops.maxComputeWorkGroupInvocations >= requiredLimits.minimumComputeWorkGroupInvocations)
    {
      matchingDevices.push_back((VulkanPhysicalDevice){
        .device = device,
        .feature_flags = featureFlags,
        .properties = dprops,
      });
    }
#ifdef VULKAN_DEVICE_LOG
    os::Logger::logf("VulkanDevice Device name = %s", props.deviceName);

    os::Logger::logf(
        "  Vendor ID: 0x%04x, Device ID: 0x%04x, API Version: %u.%u.%u",
        props.vendorID,
        props.deviceID,
        VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion),
        VK_VERSION_PATCH(props.apiVersion));

    os::Logger::logf("  Features:");
    if (featureFlags & DeviceFeatures_Atomic32_AllOps)
      os::Logger::logf("    - Atomic32_AllOps");
    if (featureFlags & DeviceFeatures_Atomic64_MinMax)
      os::Logger::logf("    - Atomic64_MinMax");
    if (featureFlags & DeviceFeatures_Atomic64_AllOps)
      os::Logger::logf("    - Atomic64_AllOps");
    if (featureFlags & DeviceFeatures_DrawIndirectFirstInstance)
      os::Logger::logf("    - DrawIndirectFirstInstance");
    if (featureFlags & DeviceFeatures_MultiDrawIndirect)
      os::Logger::logf("    - MultiDrawIndirect");
    if (featureFlags & DeviceFeatures_GeometryShader)
      os::Logger::logf("    - GeometryShader");
    if (featureFlags & DeviceFeatures_Compute)
      os::Logger::logf("    - Compute");
    if (featureFlags & DeviceFeatures_Graphics)
      os::Logger::logf("    - Graphics");
    if (featureFlags & DeviceFeatures_Timestamp)
      os::Logger::logf("    - Timestamp");
    if (featureFlags & DeviceFeatures_Dedicated)
      os::Logger::logf("    - Dedicated GPU");
    if (featureFlags & DeviceFeatures_Integrated)
      os::Logger::logf("    - Integrated GPU");

    os::Logger::logf("  Limits:");
    os::Logger::logf("    - Subgroup Size: %zu", dprops.sugroupSize);
    os::Logger::logf("    - Max Memory: %.2f GB", static_cast<double>(dprops.maxMemory) / (1024 * 1024 * 1024));
    os::Logger::logf("    - Max Shared Memory: %.2f KB", static_cast<double>(dprops.maxComputeSharedMemorySize) / 1024);
    os::Logger::logf("    - Max Workgroup Invocations: %zu", dprops.maxComputeWorkGroupInvocations);
#endif
  }

  std::sort(
      matchingDevices.begin(),
      matchingDevices.end(),
      [](const VulkanPhysicalDevice &a, const VulkanPhysicalDevice &b)
      {
        if (a.properties.maxMemory != b.properties.maxMemory)
          return a.properties.maxMemory > b.properties.maxMemory;
        if (a.properties.maxComputeSharedMemorySize != b.properties.maxComputeSharedMemorySize)
          return a.properties.maxComputeSharedMemorySize > b.properties.maxComputeSharedMemorySize;
        return a.properties.maxComputeWorkGroupInvocations > b.properties.maxComputeWorkGroupInvocations;
      });

  return matchingDevices;
}

VkResult createDebugUtilsMessengerEXT(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDebugUtilsMessengerEXT *pDebugMessenger)
{
  auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
  if (func != nullptr)
  {
    return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
  }
  else
  {
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }
}

void VulkanRHI::setupDebugMessenger()
{
  if (!enableValidationLayers)
    return;

  VkDebugUtilsMessengerCreateInfoEXT createInfo;
  populateDebugMessengerCreateInfo(createInfo);

  if (createDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to set up debug messenger!");
  }
}

VulkanRHI::VulkanRHI(VulkanVersion version, DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures, std::vector<std::string> extensions) : RHI()
{

  this->version = version;
  this->requiredLimits = requiredLimits;
  this->requestedFeaturesFlags = requestedFeatures;

  instanceExtensions.push_back(strdup(VK_KHR_SURFACE_EXTENSION_NAME));
  instanceExtensions.push_back(strdup(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME));
  instanceExtensions.push_back(strdup(VK_EXT_DEBUG_UTILS_EXTENSION_NAME));

  std::unordered_set<std::string> uniqueExtensions;
  for (auto instanceExtension : instanceExtensions)
  {
    uniqueExtensions.insert(instanceExtension);
  }

  for (auto extension : extensions)
  {
    if (uniqueExtensions.count(extension) == 0)
    {
      instanceExtensions.push_back(strdup(extension.c_str()));
    }

    uniqueExtensions.insert(extension);
  }

  for (auto extension : instanceExtensions)
  {
#ifdef VULKAN_DEVICE_LOG
    os::Logger::logf("[Vulkan Extension]: %s", extension);
#endif
  }

  initializeInstance(version);
  setupDebugMessenger();
}

VulkanRHI::~VulkanRHI()
{
  // TODO: add cleanup of resources
}

void VulkanRHI::init(std::vector<VkSurfaceKHR> &surfaces)
{
  for (uint32_t i = 0; i < instanceExtensions.size(); i++)
  {
#ifdef VULKAN_DEVICE_LOG
    os::Logger::logf("[Vulkan Extension]: %s", instanceExtensions[i]);
#endif
  }

  for (auto &surface : surfaces)
  {
    VulkanSurface vkSurface;
    vkSurface.surface = surface;
    this->surfaces.push_back(vkSurface);
  }

  initializePhysicalDevice();
  createLogicalDevice();
}

void VulkanRHI::initializePhysicalDevice()
{
  uint32_t deviceCount = 0;

  vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

  if (deviceCount == 0)
  {
    throw std::runtime_error("Failed to find GPUs with Vulkan support");
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

  std::vector<VulkanPhysicalDevice> availablePhysicalDevices = getMatchingDevices(instance, requiredLimits);
  std::vector<VulkanPhysicalDevice> physicalDevices;

  for (auto &physicalDevice : availablePhysicalDevices)
  {
    if ((physicalDevice.feature_flags & requestedFeaturesFlags) == requestedFeaturesFlags)
    {
      physicalDevices.push_back(physicalDevice);
    }
  }
  // DeviceResult dev = getPhysicalDevice(devices, requiredLimits, requestedFeaturesFlags);

  // physicalDevice = dev.device;
  // featureFlags = dev.feature_flags;
  // properties = dev.properties;

  if (physicalDevices.size() == 0)
  {
    throw std::runtime_error("Failed to find a suitable GPU");
  }

  physicalDevice = physicalDevices[0].device;
  properties = physicalDevices[0].properties;
  features = physicalDevices[0].feature_flags;
}

void VulkanRHI::initializeInstance(VulkanVersion version)
{
  if (enableValidationLayers && !checkValidationLayerSupport())
  {
    throw std::runtime_error("validation layers requested, but not available!");
  }

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "RHI Vulkan App";
  appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.pEngineName = "No Engine";
  appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);

  switch (version)
  {
  case Vulkan_1_2:
    appInfo.apiVersion = VK_API_VERSION_1_2;
    break;
  case Vulkan_1_3:
    appInfo.apiVersion = VK_API_VERSION_1_3;
    break;
  }

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
  createInfo.ppEnabledExtensionNames = instanceExtensions.data();

  VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};

  if (enableValidationLayers)
  {
    createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
    createInfo.ppEnabledLayerNames = validationLayers.data();

    populateDebugMessengerCreateInfo(debugCreateInfo);
    createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT *)&debugCreateInfo;
  }
  else
  {
    createInfo.enabledLayerCount = 0;

    createInfo.pNext = nullptr;
  }

  if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create instance!");
  }
}

VulkanQueueFamilyIndices VulkanRHI::findQueueFamilyIndices()
{
  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

  VulkanQueueFamilyIndices indices{};

  indices.hasComputeFamily = false;
  indices.hasGraphicsFamily = false;
  indices.hasTransferFamily = false;

  std::unordered_set<uint32_t> usedIndices;

  for (uint32_t i = 0; i < queueFamilyCount; ++i)
  {
    const auto &props = queueFamilies[i];

    for (auto &surface : surfaces)
    {
      VkBool32 supported = VK_FALSE;

      vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface.surface, &supported);

      if (supported && !surface.hasPresentFamily)
      {
        surface.hasPresentFamily = true;
        surface.presentFamily = i;
      }
    }
  }

  for (uint32_t i = 0; i < queueFamilyCount; ++i)
  {
    if ((queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT))
    {
      indices.transferFamily = i;
      indices.hasTransferFamily = true;
      indices.transferQueueCount = queueFamilies[i].queueCount;
      usedIndices.insert(i);
      break;
    }
  }

  for (uint32_t i = 0; i < queueFamilyCount; ++i)
  {
    if ((queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !usedIndices.count(i))
    {
      indices.computeFamily = i;
      indices.computeQueueCount = queueFamilies[i].queueCount;
      indices.hasComputeFamily = true;
      usedIndices.insert(i);
      break;
    }
  }

  for (uint32_t i = 0; i < queueFamilyCount; ++i)
  {
    if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && !usedIndices.count(i))
    {
      indices.graphicsFamily = i;
      indices.graphicsQueueCount = queueFamilies[i].queueCount;
      indices.hasGraphicsFamily = true;
      usedIndices.insert(i);
      break;
    }
  }

  for (auto &surface : surfaces)
  {
    if (surface.hasPresentFamily && !usedIndices.count(surface.presentFamily))
    {
      usedIndices.insert(surface.presentFamily);
    }
    else if (!surface.hasPresentFamily && indices.hasGraphicsFamily)
    {
      surface.presentFamily = indices.graphicsFamily;
      surface.hasPresentFamily = true;
    }
  }

  for (uint32_t i = 0; i < queueFamilyCount; ++i)
  {
    if (!indices.hasTransferFamily && (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT))
    {
      indices.transferFamily = i;
      indices.transferQueueCount = queueFamilies[i].queueCount;
      indices.hasTransferFamily = true;
    }

    if (!indices.hasComputeFamily && (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
    {
      indices.computeFamily = i;
      indices.computeQueueCount = queueFamilies[i].queueCount;
      indices.hasComputeFamily = true;
    }

    if (!indices.hasGraphicsFamily && (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    {
      indices.graphicsFamily = i;
      indices.graphicsQueueCount = queueFamilies[i].queueCount;
      indices.hasGraphicsFamily = true;
    }
  }

  return indices;
}

void VulkanRHI::createLogicalDevice()
{
  indices = findQueueFamilyIndices();

  if ((features & DeviceFeatures_Graphics) && !indices.hasGraphicsFamily)
  {
    throw std::runtime_error("Missing required queue families");
  }

  if ((features & DeviceFeatures_Compute) && !indices.hasComputeFamily)
  {
    throw std::runtime_error("Missing required queue families");
  }

  std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
  std::set<uint32_t> uniqueFamiles;
  std::unordered_map<uint32_t, uint32_t> familyToCount;

  if (indices.hasComputeFamily)
  {
    familyToCount[indices.computeFamily] = indices.computeQueueCount;
    uniqueFamiles.insert(indices.computeFamily);
  }

  if (indices.hasGraphicsFamily)
  {
    familyToCount[indices.graphicsFamily] = indices.graphicsQueueCount;
    uniqueFamiles.insert(indices.graphicsFamily);
  }

  if (indices.hasTransferFamily)
  {
    familyToCount[indices.transferFamily] = indices.transferQueueCount;
    uniqueFamiles.insert(indices.transferFamily);
  }

  for (auto &surface : surfaces)
  {

    if (surface.hasPresentFamily)
    {
      familyToCount[surface.presentFamily] += 1;
      uniqueFamiles.insert(surface.presentFamily);
    }
  }

  std::vector<std::vector<float>> queuePriorityStorage;

  for (uint32_t familyIndex : uniqueFamiles)
  {
    std::vector<float> priorities(familyToCount[familyIndex], 1.0f);
    queuePriorityStorage.push_back(priorities);

    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = familyIndex;
    queueCreateInfo.queueCount = familyToCount[familyIndex];
    queueCreateInfo.pQueuePriorities = priorities.data();
    queueCreateInfos.push_back(queueCreateInfo);
  }

  VkPhysicalDeviceFeatures deviceFeatures{};

  deviceFeatures.multiDrawIndirect = features & DeviceFeatures_MultiDrawIndirect ? VK_TRUE : VK_FALSE;
  deviceFeatures.drawIndirectFirstInstance = features & DeviceFeatures_DrawIndirectFirstInstance ? VK_TRUE : VK_FALSE;

  VkDeviceCreateInfo createInfo = {};

  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
  createInfo.pQueueCreateInfos = queueCreateInfos.data();
  createInfo.pEnabledFeatures = &deviceFeatures;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
  createInfo.ppEnabledExtensionNames = deviceExtensions.data();

  VkDevice device = VK_NULL_HANDLE;

  if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create logical device!");
  }

  uint32_t graphicsCount = 0;
  uint32_t computeCount = 0;
  uint32_t transferCount = 0;
  uint32_t presentCount = 0;

  for (VkDeviceQueueCreateInfo &info : queueCreateInfos)
  {
    VkQueue queue;
    uint32_t index = 0;

    if (info.queueFamilyIndex == indices.computeFamily)
    {
      index = computeCount;
      computeCount++;
    }

    if (info.queueFamilyIndex == indices.graphicsFamily)
    {
      index = graphicsCount;
      graphicsCount++;
    }

    if (info.queueFamilyIndex == indices.transferFamily)
    {
      index = transferCount;
      transferCount++;
    }

    for (auto &surface : surfaces)
    {
      if (info.queueFamilyIndex == surface.presentFamily)
      {
        index = presentCount;
        presentCount++;
        break;
      }
    }

    vkGetDeviceQueue(device, info.queueFamilyIndex, index, &queue);

    if (info.queueFamilyIndex == indices.computeFamily)
    {
      computeQueue.push_back(queue);
    }

    if (info.queueFamilyIndex == indices.graphicsFamily)
    {
      graphicsQueue.push_back(queue);
    }

    if (info.queueFamilyIndex == indices.transferFamily)
    {
      transferQueue.push_back(queue);
    }

    for (auto &surface : surfaces)
    {
      if (info.queueFamilyIndex == surface.presentFamily)
      {
        surface.presentQueue = queue;
        break;
      }
    }
  }

  this->device = device;
}

VulkanBuffer &VulkanRHI::allocateBuffer(const BufferInfo &info)
{
  VulkanBuffer *vkBuf = new VulkanBuffer();

  vkBuf->info = info;
  vkBuf->size = info.size;

  vkBuf->usageFlags = toVkBufferUsageFlags(info.usage);
  vkBuf->memoryFlags = toVkMemoryPropertyFlags(info.usage, false);

  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = info.size;
  bufferInfo.usage = vkBuf->usageFlags;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateBuffer(device, &bufferInfo, nullptr, &vkBuf->buffer) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create Vulkan buffer!");
  }

  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(device, vkBuf->buffer, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, vkBuf->memoryFlags, physicalDevice);

  if (vkAllocateMemory(device, &allocInfo, nullptr, &vkBuf->memory) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to allocate buffer memory!");
  }

  vkBindBufferMemory(device, vkBuf->buffer, vkBuf->memory, 0);

  if (vkBuf->memoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
  {
    vkMapMemory(device, vkBuf->memory, 0, info.size, 0, &vkBuf->mapped);
  }

  vkBuffers.insert(info.name, vkBuf);
  return *vkBuf;
}

void VulkanRHI::releaseBuffer(VulkanBuffer &buf)
{
  if (buf.mapped)
  {
    vkUnmapMemory(device, buf.memory);
    buf.mapped = nullptr;
  }

  if (buf.buffer != VK_NULL_HANDLE)
  {
    vkDestroyBuffer(device, buf.buffer, nullptr);
    buf.buffer = VK_NULL_HANDLE;
  }

  if (buf.memory != VK_NULL_HANDLE)
  {
    vkFreeMemory(device, buf.memory, nullptr);
    buf.memory = VK_NULL_HANDLE;
  }

  buf.size = 0;
  buf.usageFlags = 0;
  buf.memoryFlags = 0;
  buf.info = BufferInfo{};

  vkBuffers.erase(buf.info.name);
  delete &buf;
}

VulkanTexture &VulkanRHI::allocateTexture(const TextureInfo &info)
{
  VulkanTexture *tex = new VulkanTexture();
  tex->info = info;
  tex->format = toVkFormat(info.format);
  tex->extent = {info.width, info.height, std::max(1u, info.depth)};
  tex->mipLevels = std::max(1u, info.mipLevels);
  tex->usageFlags = toVkImageUsageFlags(info.usage);
  tex->memoryFlags = toVkMemoryPropertyFlags(info.memoryProperties, true);

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = info.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  imageInfo.extent = tex->extent;
  imageInfo.mipLevels = tex->mipLevels;
  imageInfo.arrayLayers = 1;
  imageInfo.format = tex->format;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = tex->usageFlags;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.flags = 0;

  if (vkCreateImage(device, &imageInfo, nullptr, &tex->image) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create Vulkan image!");
  }

  VkMemoryRequirements memRequirements;
  vkGetImageMemoryRequirements(device, tex->image, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, tex->memoryFlags, physicalDevice);

  if (vkAllocateMemory(device, &allocInfo, nullptr, &tex->memory) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to allocate image memory!");
  }

  vkBindImageMemory(device, tex->image, tex->memory, 0);

  tex->currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  vkTextures.insert(info.name, tex);
  return *tex;
}

void VulkanRHI::releaseTexture(VulkanTexture &vkTex)
{
  VulkanTexture *tex = &vkTex;
  if (tex->image != VK_NULL_HANDLE)
  {
    vkDestroyImage(device, tex->image, nullptr);
    tex->image = VK_NULL_HANDLE;
  }

  if (tex->memory != VK_NULL_HANDLE)
  {
    vkFreeMemory(device, tex->memory, nullptr);
    tex->memory = VK_NULL_HANDLE;
  }

  tex->format = VK_FORMAT_UNDEFINED;
  tex->extent = {};
  tex->mipLevels = 1;
  tex->usageFlags = 0;
  tex->memoryFlags = 0;
  tex->currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  vkTextures.erase(tex->info.name);
  delete tex;
}

VulkanTextureView VulkanRHI::createTextureView(const TextureView &view)
{
  VulkanTextureView vkView{};
  const VulkanTexture &tex = getVulkanTexture(view.texture.name);

  vkView.image = tex.image;
  vkView.format = tex.format;
  vkView.viewType = tex.extent.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = tex.image;
  viewInfo.viewType = vkView.viewType;
  viewInfo.format = tex.format;
  viewInfo.subresourceRange.aspectMask = toVkImageAspectFlags(view.flags);
  viewInfo.subresourceRange.baseMipLevel = view.baseMipLevel;
  viewInfo.subresourceRange.levelCount = view.levelCount;
  viewInfo.subresourceRange.baseArrayLayer = view.baseArrayLayer;
  viewInfo.subresourceRange.layerCount = view.layerCount;

  if (vkCreateImageView(device, &viewInfo, nullptr, &vkView.view) != VK_SUCCESS)
    throw std::runtime_error("Failed to create image view!");

  vkView.range = viewInfo.subresourceRange;
  vkView.original = view;

  return vkView;
}

void VulkanRHI::destroyTextureView(VulkanTextureView view)
{
  if (view.view != VK_NULL_HANDLE)
  {
    vkDestroyImageView(device, view.view, nullptr);
    view.view = VK_NULL_HANDLE;
  }

  view.image = VK_NULL_HANDLE;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = VK_FORMAT_UNDEFINED;
  view.range = {};
}

VulkanSampler &VulkanRHI::allocateSampler(const SamplerInfo &info)
{
  VulkanSampler *vkSampler = new VulkanSampler();
  vkSampler->info = info;

  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = toVkFilter(info.magFilter);
  samplerInfo.minFilter = toVkFilter(info.minFilter);
  samplerInfo.addressModeU = toVkSamplerAddressMode(info.addressModeU);
  samplerInfo.addressModeV = toVkSamplerAddressMode(info.addressModeV);
  samplerInfo.addressModeW = toVkSamplerAddressMode(info.addressModeW);
  samplerInfo.anisotropyEnable = info.anisotropyEnable ? VK_TRUE : VK_FALSE;
  samplerInfo.maxAnisotropy = info.anisotropyEnable ? info.maxAnisotropy : 1.0f;
  samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  samplerInfo.unnormalizedCoordinates = VK_FALSE;
  samplerInfo.compareEnable = VK_FALSE;
  samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = info.maxLod;

  if (vkCreateSampler(device, &samplerInfo, nullptr, &vkSampler->sampler) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create Vulkan sampler!");
  }

  vkSamplers.insert(info.name, vkSampler);
  return *vkSampler;
}

void VulkanRHI::releaseSampler(VulkanSampler &sampler)
{
  VulkanSampler *vkSampler = &sampler;
  if (sampler.sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(device, sampler.sampler, nullptr);
    sampler.sampler = VK_NULL_HANDLE;
  }

  vkSamplers.erase(sampler.info.name);
  delete vkSampler;
}

VulkanBindingsLayout &VulkanRHI::allocateBindingsLayout(const BindingsLayoutInfo &info)
{
  VulkanBindingsLayout *vkLayout = new VulkanBindingsLayout();
  vkLayout->name = info.name;
  vkLayout->groups = info.groups;

  for (const BindingGroupLayout &group : info.groups)
  {
    std::vector<VkDescriptorSetLayoutBinding> bindings;

    for (const auto &b : group.buffers)
    {
      bindings.push_back(bufferEntryToBinding(b));
    }

    for (const auto &s : group.samplers)
    {
      bindings.push_back(samplerEntryToBinding(s));
    }

    for (const auto &t : group.textures)
    {
      bindings.push_back(textureEntryToBinding(t));
    }

    for (const auto &st : group.storageTextures)
    {
      bindings.push_back(storageTextureEntryToBinding(st));
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VkDescriptorSetLayout setLayout;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout) != VK_SUCCESS)
    {
      throw std::runtime_error("Failed to create descriptor set layout!");
    }

    vkLayout->setLayouts.push_back(setLayout);
  }

  // const VulkanBindingsLayout& layout = getVulkanBindingsLayout(info.name);

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(vkLayout->setLayouts.size());
  pipelineLayoutInfo.pSetLayouts = vkLayout->setLayouts.data();
  pipelineLayoutInfo.pushConstantRangeCount = 0;
  pipelineLayoutInfo.pPushConstantRanges = nullptr;

  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create pipeline layout!");
  }
  vkLayout->pipelineLayout = pipelineLayout;

  vkBindingsLayout.insert(info.name, vkLayout);
  return *vkLayout;
}

void VulkanRHI::releaseBindingsLayout(VulkanBindingsLayout &layout)
{
  VulkanBindingsLayout *l = &layout;
  for (VkDescriptorSetLayout setLayout : layout.setLayouts)
  {
    if (setLayout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
  }

  layout.setLayouts.clear();
  layout.groups.clear();
  layout.name.clear();

  vkBindingsGroups.erase(layout.name);
  delete l;
}

VulkanBindingGroups &VulkanRHI::allocateBindings(const BindingGroupsInfo &groups, const VulkanBindingsLayout &layout)
{
  std::vector<VulkanBindingGroup> result;

  for (size_t groupIndex = 0; groupIndex < groups.groups.size(); ++groupIndex)
  {
    const GroupInfo &groupInfo = groups.groups[groupIndex];
    // const BindingGroupLayout &groupLayout = layout.groups[groupIndex];

    VulkanBindingGroup vkGroup{};
    vkGroup.info = groupInfo;

    // Count descriptor types
    uint32_t bufferCount = static_cast<uint32_t>(groupInfo.buffers.size());
    uint32_t samplerCount = static_cast<uint32_t>(groupInfo.samplers.size());
    uint32_t textureCount = static_cast<uint32_t>(groupInfo.textures.size());
    uint32_t storageTextureCount = static_cast<uint32_t>(groupInfo.storageTextures.size());

    std::vector<VkDescriptorPoolSize> poolSizes;
    if (bufferCount > 0)
      poolSizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bufferCount});
    if (samplerCount > 0)
      poolSizes.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, samplerCount});
    if (textureCount > 0)
      poolSizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureCount});
    if (storageTextureCount > 0)
      poolSizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storageTextureCount});

    // Create descriptor pool
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1; // one descriptor set per group
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &vkGroup.descriptorPool) != VK_SUCCESS)
      throw std::runtime_error("Failed to create descriptor pool");

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = vkGroup.descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout.setLayouts[groupIndex];

    VkDescriptorSet descriptorSet;
    if (vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet) != VK_SUCCESS)
      throw std::runtime_error("Failed to allocate descriptor set");

    vkGroup.descriptorSets.push_back(descriptorSet);

    // Write descriptors
    std::vector<VkWriteDescriptorSet> descriptorWrites;

    // Buffers
    for (size_t i = 0; i < groupInfo.buffers.size(); ++i)
    {
      const BindingBuffer &binding = groupInfo.buffers[i];
      VulkanBuffer buf = getVulkanBuffer(binding.bufferView.buffer.name);

      VkDescriptorBufferInfo bufferInfo{};
      bufferInfo.buffer = buf.buffer;
      bufferInfo.offset = binding.bufferView.offset;
      bufferInfo.range = binding.bufferView.size;

      VkDescriptorType type = binding.isDynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptorSet;
      write.dstBinding = binding.binding;
      write.dstArrayElement = 0;
      write.descriptorType = type;
      write.descriptorCount = 1;
      write.pBufferInfo = &bufferInfo;

      descriptorWrites.push_back(write);
    }

    // Samplers
    for (size_t i = 0; i < groupInfo.samplers.size(); ++i)
    {
      const BindingSampler &binding = groupInfo.samplers[i];
      const VulkanSampler &sampler = getVulkanSampler(binding.sampler.name);
      const VulkanTextureView &view = createTextureView(binding.view);
      vkGroup.textureViews.push_back(view);

      VkDescriptorImageInfo imageInfo{};
      imageInfo.sampler = sampler.sampler;
      imageInfo.imageView = view.view;
      imageInfo.imageLayout = toVkImageLayout(binding.view.layout);

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptorSet;
      write.dstBinding = binding.binding;
      write.dstArrayElement = 0;
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.descriptorCount = 1;
      write.pImageInfo = &imageInfo;

      descriptorWrites.push_back(write);
    }

    for (size_t i = 0; i < groupInfo.textures.size(); ++i)
    {
      const BindingTextureInfo &binding = groupInfo.textures[i];
      VulkanTextureView view = createTextureView(binding.textureView);
      vkGroup.textureViews.push_back(view);
      VkDescriptorImageInfo imageInfo{};
      imageInfo.sampler = VK_NULL_HANDLE;
      imageInfo.imageView = view.view;
      imageInfo.imageLayout = toVkImageLayout(binding.textureView.layout);

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptorSet;
      write.dstBinding = binding.binding;
      write.dstArrayElement = 0;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.descriptorCount = 1;
      write.pImageInfo = &imageInfo;

      descriptorWrites.push_back(write);
    }

    for (size_t i = 0; i < groupInfo.storageTextures.size(); ++i)
    {
      const BindingStorageTextureInfo &binding = groupInfo.storageTextures[i];
      VulkanTextureView view = createTextureView(binding.textureView);
      vkGroup.textureViews.push_back(view);
      ;
      VkDescriptorImageInfo imageInfo{};
      imageInfo.sampler = VK_NULL_HANDLE;
      imageInfo.imageView = view.view;
      imageInfo.imageLayout = toVkImageLayout(binding.textureView.layout);

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptorSet;
      write.dstBinding = binding.binding;
      write.dstArrayElement = 0;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      write.descriptorCount = 1;
      write.pImageInfo = &imageInfo;

      descriptorWrites.push_back(write);
    }

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);

    result.push_back(vkGroup);
  }

  VulkanBindingGroups *resultGroups = new VulkanBindingGroups();

  resultGroups->groups = result;
  resultGroups->info = groups;

  vkBindingsGroups.insert(groups.name, resultGroups);
  return *resultGroups;
}

void VulkanRHI::releaseBindingGroup(VulkanBindingGroups &groups)
{
  VulkanBindingGroups *g = &groups;
  for (auto &group : groups.groups)
  {
    if (group.descriptorPool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool(device, group.descriptorPool, nullptr);
      group.descriptorPool = VK_NULL_HANDLE;
    }

    group.descriptorSets.clear();
    
    for(auto& view : group.textureViews) {
      destroyTextureView(view);
    }
  }

  groups.groups.clear();

  vkBindingsGroups.erase(groups.info.name);
  delete g;
}

VulkanSwapChainSupportDetails querySwapChainSupport(VkSurfaceKHR surface, VkPhysicalDevice device)
{

  VulkanSwapChainSupportDetails details;

  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

  uint32_t formatCount;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

  if (formatCount != 0)
  {
    details.formats.resize(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
  }

  uint32_t presentModeCount;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);

  if (presentModeCount != 0)
  {
    details.presentModes.resize(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
  }

  return details;
}

VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats)
{
  for (const auto &availableFormat : availableFormats)
  {
    if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    {
      return availableFormat;
    }
  }

  return availableFormats[0];
}

VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR> &availablePresentModes)
{
  for (const auto &availablePresentMode : availablePresentModes)
  {
    if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
    {
      return availablePresentMode;
    }
  }

  return VK_PRESENT_MODE_FIFO_KHR;
}

template <typename T> const T &clamp(const T &value, const T &low, const T &high)
{
  if (value < low)
    return low;
  if (value > high)
    return high;
  return value;
}

VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities, uint32_t width, uint32_t height)
{
  if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
  {
    return capabilities.currentExtent;
  }
  else
  {
    VkExtent2D actualExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    actualExtent.width = clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    actualExtent.height = clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

    return actualExtent;
  }
}

const SwapChain VulkanRHI::createSwapChain(uint32_t surfaceIndex, uint32_t width, uint32_t height)
{
  VulkanSurface &surfaceImp = surfaces[surfaceIndex];

  VulkanSwapChainSupportDetails swapChainSupport = querySwapChainSupport(surfaceImp.surface, physicalDevice);
  VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);

  VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
  VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities, width, height);

  uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
  if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount)
  {
    imageCount = swapChainSupport.capabilities.maxImageCount;
  }

  VkSwapchainCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  createInfo.surface = surfaceImp.surface;

  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = extent;
  createInfo.imageArrayLayers = 1;
  createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

  std::vector<VkSurfaceKHR> surfaces = {surfaceImp.surface};

  uint32_t queueFamilyIndices[2] = {indices.graphicsFamily, surfaceImp.presentFamily};

  if (indices.graphicsFamily != surfaceImp.presentFamily)
  {
    createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = queueFamilyIndices;
  }
  else
  {
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
  createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  createInfo.presentMode = presentMode;
  createInfo.clipped = VK_TRUE;

  VulkanSwapChain *swapChainImp = new VulkanSwapChain();

  if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChainImp->swapChain) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create swap chain!");
  }

  vkGetSwapchainImagesKHR(device, swapChainImp->swapChain, &imageCount, nullptr);
  std::vector<VkImage> images;
  std::vector<VkImageView> imagesViews;

  if (swapChainImp->swapChainImages.size() == 0)
  {
    imagesViews = std::vector<VkImageView>(imageCount);
    images = std::vector<VkImage>(imageCount);
  }

  vkGetSwapchainImagesKHR(device, swapChainImp->swapChain, &imageCount, images.data());

  for (int i = 0; i < images.size(); i++)
  {
    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = images[i];
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = surfaceFormat.format;
    createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = 1;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &createInfo, nullptr, &imagesViews[i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create image views!");
    }
  }

  for (int i = 0; i < images.size(); i++)
  {
    VulkanTextureView *view = new VulkanTextureView();

    // view->fence = VK_NULL_HANDLE;
    // view->achireSemaphore = VK_NULL_HANDLE;
    // view->presentSemaphore = VK_NULL_HANDLE;
    view->view = imagesViews[i];
    // view->renderData.swapChain = swapChainImp;
    // view->renderData.swapChainImageIndex = i;

    // TextureViewInfo info;
    // info.name = "SwapChainImage";
    // info.flags = ImageAspectFlags::Color;
    swapChainImp->swapChainImages.push_back(view);
  }

  swapChainImp->swapChainImageFormat = surfaceFormat.format;
  swapChainImp->swapChainExtent = extent;
  swapChainImp->support = swapChainSupport;
  swapChainImp->presentQueue = surfaceImp.presentQueue;
  swapChainImp->achireSemaphores.resize(images.size());
  swapChainImp->presentSemaphores.resize(images.size());

  for (int i = 0; i < images.size(); i++)
  {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &swapChainImp->achireSemaphores[i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create semaphore for image views!");
    }
  }

  for (int i = 0; i < images.size(); i++)
  {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &swapChainImp->presentSemaphores[i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create semaphore for image views!");
    }
  }

  swapChains.resize(surfaceIndex + 1);
  swapChains[surfaceIndex] = swapChainImp;

  return SwapChain(surfaceIndex);
}

void VulkanRHI::destroySwapChain(SwapChain swapChain)
{
  VulkanSwapChain *swapChainImp = swapChains[(uint32_t)swapChain];

  // vkDeviceWaitIdle(device);

  for (auto &imageView : swapChainImp->swapChainImages)
  {
    vkDestroyImageView(device, imageView->view, nullptr);
    delete imageView;
  }

  // swapChainImp->swapChainImages.clear();

  for (auto &semaphore : swapChainImp->achireSemaphores)
  {
    vkDestroySemaphore(device, semaphore, NULL);
  }

  for (auto &semaphore : swapChainImp->presentSemaphores)
  {
    vkDestroySemaphore(device, semaphore, NULL);
  }

  if (swapChainImp->swapChain != VK_NULL_HANDLE)
  {
    vkDestroySwapchainKHR(device, swapChainImp->swapChain, nullptr);
  }

  delete swapChainImp;
}

const TextureView VulkanRHI::getCurrentSwapChainTextureView(SwapChain swapChainHandle)
{
  VulkanSwapChain *swapChain = swapChains[(uint32_t)swapChainHandle];

  uint32_t index = UINT32_MAX;
  uint32_t current = swapChain->currentPrimitive.fetch_add(1) % swapChain->swapChainImages.size();

  if (vkAcquireNextImageKHR(device, swapChain->swapChain, UINT64_MAX, swapChain->achireSemaphores[current], VK_NULL_HANDLE, &index) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to achire next image, you probably did not submit the commands");
  }

  VulkanTextureView *viewImp = swapChain->swapChainImages[index];

  VkSemaphore expectedAchire = VK_NULL_HANDLE;
  VkSemaphore expectedPresent = VK_NULL_HANDLE;

  // while (!viewImp->achireSemaphore.compare_exchange_strong(expectedAchire, swapChain->achireSemaphores[current]))
  // {
  // }

  // while (!viewImp->presentSemaphore.compare_exchange_strong(expectedPresent, swapChain->presentSemaphores[current]))
  // {
  // }
  TextureView view;
  view.access = AccessPattern::COLOR_ATTACHMENT_WRITE;
  view.layout = ResourceLayout::COLOR_ATTACHMENT;
  view.baseArrayLayer = 0;
  view.baseMipLevel = 0;
  view.layerCount = UINT32_MAX;
  view.levelCount = UINT32_MAX;
  view.flags = ImageAspectFlags::Color;
  view.texture.name = "SwapChainImage[" + std::to_string(index) + "].texture";

  return view;
}

const VulkanTexture &VulkanRHI::getVulkanTexture(const std::string &obj)
{
  VulkanTexture *ptr;

  bool result = vkTextures.find(obj, ptr);

  if (!result)
  {
    throw std::runtime_error("VulkanTexture not found");
  }

  return *ptr;
}

const VulkanSampler &VulkanRHI::getVulkanSampler(const std::string &obj)
{
  VulkanSampler *ptr;

  bool result = vkSamplers.find(obj, ptr);

  if (!result)
  {
    throw std::runtime_error("VulkanSampler not found");
  }

  return *ptr;
}

const VulkanBuffer &VulkanRHI::getVulkanBuffer(const std::string &obj)
{
  VulkanBuffer *ptr;

  bool result = vkBuffers.find(obj, ptr);

  if (!result)
  {
    throw std::runtime_error("VulkanBuffer not found");
  }

  return *ptr;
}

const VulkanBindingsLayout &VulkanRHI::getVulkanBindingsLayout(const std::string &obj)
{
  VulkanBindingsLayout *ptr;

  bool result = vkBindingsLayout.find(obj, ptr);

  if (!result)
  {
    throw std::runtime_error("VulkanBindingsLayout not found");
  }

  return *ptr;
}

const VulkanBindingGroups &VulkanRHI::getVulkanBindingGroups(const std::string &obj)
{
  VulkanBindingGroups *ptr;

  bool result = vkBindingsGroups.find(obj, ptr);

  if (!result)
  {
    throw std::runtime_error("VulkanBindingGroups not found");
  }

  return *ptr;
}

const VulkanGraphicsPipeline &VulkanRHI::getVulkanGraphicsPipeline(const std::string &obj)
{
  VulkanGraphicsPipeline *ptr;

  bool result = vkGraphicsPipeline.find(obj, ptr);

  if (!result)
  {
    throw std::runtime_error("VulkanGraphicsPipeline not found");
  }

  return *ptr;
}

const VulkanComputePipeline &VulkanRHI::getVulkanComputePipeline(const std::string &obj)
{
  VulkanComputePipeline *ptr;

  bool result = vkComputePipeline.find(obj, ptr);

  if (!result)
  {
    throw std::runtime_error("VulkanComputePipeline not found");
  }

  return *ptr;
}

const VulkanShader &VulkanRHI::getVulkanShader(const std::string &obj)
{
  VulkanShader *ptr;

  bool result = vkShaders.find(obj, ptr);

  if (!result)
  {
    throw std::runtime_error("VulkanShader not found");
  }

  return *ptr;
};

void VulkanRHI::bufferMapRead(const Buffer &buffer, const uint64_t offset, const uint64_t size, void **ptr)
{
  const VulkanBuffer &heap = getVulkanBuffer(buffer.name);
  vkMapMemory(device, heap.memory, offset, size, 0, ptr);
}

void VulkanRHI::bufferUnmap(const Buffer &buffer)
{
  const VulkanBuffer &heap = getVulkanBuffer(buffer.name);
  vkUnmapMemory(device, heap.memory);
}

void VulkanRHI::bufferWrite(const Buffer &buffer, const uint64_t offset, const uint64_t size, void *data)
{
  void *ptr;
  const VulkanBuffer &heap = getVulkanBuffer(buffer.name);
  vkMapMemory(device, heap.memory, offset, size, 0, &ptr);
  memcpy((void *)((uint64_t)ptr + offset), data, size);
  vkUnmapMemory(device, heap.memory);
}

size_t GetVkFormatSize(VkFormat format)
{
  return vk::blockSize((vk::Format)format);
}
inline VkAttachmentLoadOp loadOpToVkLoadOp(LoadOp op)
{
  switch (op)
  {
  case LoadOp::LoadOp_Load:
    return VK_ATTACHMENT_LOAD_OP_LOAD;
  case LoadOp::LoadOp_Clear:
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
  case LoadOp::LoadOp_DontCare:
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  default:
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  }
}

inline VkAttachmentStoreOp storeOpToVkStoreOp(StoreOp op)
{
  switch (op)
  {
  case StoreOp::StoreOp_Store:
    return VK_ATTACHMENT_STORE_OP_STORE;
  case StoreOp::StoreOp_DontCare:
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
  }
  return VK_ATTACHMENT_STORE_OP_DONT_CARE;
}
VkRenderPass VulkanRHI::createRenderPass(const ColorAttatchment *attachments, uint32_t attatchmentsCount, DepthAttatchment depth)
{
  std::vector<VkAttachmentDescription> attachmentsDescriptions;
  std::vector<VkAttachmentReference> colorAttachmentRefs;

  // Color attachments
  for (size_t i = 0; i < attatchmentsCount; ++i)
  {
    VkAttachmentDescription colorAttachment{};

    colorAttachment.format = toVkFormat(attachments[i].format);
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = loadOpToVkLoadOp(attachments[i].loadOp);
    colorAttachment.storeOp = storeOpToVkStoreOp(attachments[i].storeOp);
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = attachments[i].loadOp == LoadOp::LoadOp_Clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    attachmentsDescriptions.push_back(colorAttachment);

    VkAttachmentReference colorRef{};
    colorRef.attachment = static_cast<uint32_t>(i);
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachmentRefs.push_back(colorRef);
  }

  // Depth attachment (optional)
  VkAttachmentReference depthAttachmentRef{};
  if (depth.format != Format_None)
  {
    VkAttachmentDescription depthAttachment{};

    switch (depth.format)
    {
    case Format_Depth32Float:
      depthAttachment.format = VK_FORMAT_D32_SFLOAT;
      break;
    case Format_Depth32FloatStencil8:
      depthAttachment.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
      break;
    case Format_Depth24PlusStencil8:
      depthAttachment.format = VK_FORMAT_D24_UNORM_S8_UINT;
      break;
    case Format_Depth16Unorm:
      depthAttachment.format = VK_FORMAT_D16_UNORM;
      break;
    case Format_None:
      depthAttachment.format = VK_FORMAT_UNDEFINED;
      break;
    default:
      abort(); // NOT IMPLEMENTED
    }

    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = loadOpToVkLoadOp(depth.loadOp);
    depthAttachment.storeOp = storeOpToVkStoreOp(depth.storeOp);
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = depth.loadOp == LoadOp::LoadOp_Clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    attachmentsDescriptions.push_back(depthAttachment);

    depthAttachmentRef.attachment = static_cast<uint32_t>(attachmentsDescriptions.size() - 1);
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  }

  // Subpass
  VkSubpassDescription subpass{};

  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentRefs.size());
  subpass.pColorAttachments = colorAttachmentRefs.data();
  bool hasDepth = depth.format != Format_None;
  if (hasDepth)
  {
    subpass.pDepthStencilAttachment = &depthAttachmentRef;
  }

  // Dependencies
  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | (hasDepth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0);
  dependency.srcAccessMask = 0;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | (hasDepth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0);
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | (hasDepth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0);

  // Render pass create info
  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attachmentsDescriptions.size());
  renderPassInfo.pAttachments = attachmentsDescriptions.data();
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 1;
  renderPassInfo.pDependencies = &dependency;

  VkRenderPass renderPass;

  if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create render pass!");
  }

  return renderPass;
}

VulkanGraphicsPipeline &VulkanRHI::allocateGraphicsPipeline(const GraphicsPipelineInfo &info)
{
#ifdef VULKAN_DEVICE_LOG
  os::Logger::logf("VulkanDevice creating (GraphicsPipeline)%s", info.name.c_str());
#endif
  // VkViewport viewport = {0};
  // viewport.x = 0.0f;
  // viewport.y = 0.0f;
  // viewport.width = renderPasses[renderPass].width;
  // viewport.height = renderPasses[renderPass].height;

  // VkRect2D scissor;
  // scissor.offset.x = 0;
  // scissor.offset.y = 0;
  // scissor.extent.width = renderPasses[renderPass].width;
  // scissor.extent.width = renderPasses[renderPass].height;

  std::vector<VkDynamicState> dynamicStates = {
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
  };

  VkPipelineDynamicStateCreateInfo dynamicState;
  dynamicState.dynamicStateCount = dynamicStates.size();
  dynamicState.pDynamicStates = dynamicStates.data();

  VkPipelineViewportStateCreateInfo viewportState;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;
  viewportState.pViewports = nullptr;
  viewportState.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterizer = {};
  rasterizer.depthBiasClamp = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;

  switch (info.vertexStage.cullType)
  {
  case PrimitiveCullType_None:
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    break;
  case PrimitiveCullType_CCW:
    rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT;
    break;
  case PrimitiveCullType_CW:
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    break;
  default:
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    break;
  }
  switch (info.vertexStage.cullType)
  {
  case PrimitiveCullType_CCW:
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    break;
  case PrimitiveCullType_CW:
  default:
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    break;
  }

  rasterizer.depthBiasEnable = VK_FALSE;
  rasterizer.depthBiasSlopeFactor = 1.0f;
  rasterizer.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  multisampling.sampleShadingEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState colorBlendAttachment{};

  colorBlendAttachment.blendEnable = VK_TRUE;
  colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

  colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.logicOp = VK_LOGIC_OP_COPY;
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;

  VkGraphicsPipelineCreateInfo pipelineInfo{};

  std::vector<VkVertexInputAttributeDescription> attributes;
  std::unordered_map<uint32_t, uint32_t> bindingStrideMap;

  for (int i = 0; i < info.vertexStage.vertexLayoutElements.size(); i++)
  {
    VkVertexInputAttributeDescription desc = {};
    desc.format = toVkFormat(typeToFormat(info.vertexStage.vertexLayoutElements[i].type));
    desc.binding = info.vertexStage.vertexLayoutElements[i].binding;
    desc.location = info.vertexStage.vertexLayoutElements[i].location;
    desc.offset = info.vertexStage.vertexLayoutElements[i].offset;
    attributes.push_back(desc);

    uint32_t attributeEndOffset = desc.offset + GetVkFormatSize(desc.format);
    bindingStrideMap[desc.binding] = std::max(bindingStrideMap[desc.binding], attributeEndOffset);
  }

  std::vector<VkVertexInputBindingDescription> bindings;
  for (auto const &[bindingId, stride] : bindingStrideMap)
  {
    VkVertexInputBindingDescription bindDesc = {};
    bindDesc.binding = bindingId;
    bindDesc.stride = stride;
    bindDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings.push_back(bindDesc);
  }

  std::sort(
      bindings.begin(),
      bindings.end(),
      [](const VkVertexInputBindingDescription &a, const VkVertexInputBindingDescription &b)
      {
        return a.binding < b.binding;
      });

  // Set up VkPipelineVertexInputStateCreateInfo
  VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
  vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
  vertexInputInfo.pVertexBindingDescriptions = bindings.data();
  vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
  vertexInputInfo.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};

  switch (info.vertexStage.primitiveType)
  {
  case PrimitiveType_Triangles:
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    break;
  case PrimitiveType_TrianglesFan:
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    break;
  case PrimitiveType_TrianglesStrip:
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    break;
  case PrimitiveType_Points:
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    break;
  case PrimitiveType_Lines:
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    break;
  default:
    abort();
  }

  inputAssembly.primitiveRestartEnable = VK_FALSE;

  const VulkanShader &vertex = getVulkanShader(info.vertexStage.vertexShader.name);
  const VulkanShader &fragment = getVulkanShader(info.fragmentStage.fragmentShader.name);

  if (vertex.shaderModule == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Invalid vertex shader!");
  }
  if (fragment.shaderModule == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Invalid fragment shader!");
  }

  VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
  vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vertShaderStageInfo.module = vertex.shaderModule;
  vertShaderStageInfo.pName = info.vertexStage.shaderEntry.c_str();

  VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
  fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  fragShaderStageInfo.module = fragment.shaderModule;
  fragShaderStageInfo.pName = info.fragmentStage.shaderEntry.c_str();

  VkPipelineShaderStageCreateInfo shaderStages[] = {
    vertShaderStageInfo,
    fragShaderStageInfo,
  };

  // pipelineInfo.pNext = &pipelineRenderingCreateInfo;

  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = shaderStages;

  pipelineInfo.pVertexInputState = &vertexInputInfo;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDynamicState = &dynamicState;
  const VulkanBindingsLayout &layout = getVulkanBindingsLayout(info.layout.name);
  pipelineInfo.layout = layout.pipelineLayout;

  VkRenderPass renderPass = createRenderPass(info.fragmentStage.colorAttatchments.data(), info.fragmentStage.colorAttatchments.size(), info.fragmentStage.depthAttatchment);

  pipelineInfo.renderPass = renderPass;
  VkPipeline pipeline = VK_NULL_HANDLE;

  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create graphics pipeline!");
  }

  VulkanGraphicsPipeline *result = new VulkanGraphicsPipeline();

  result->pipeline = pipeline;
  result->renderPass = renderPass;
  result->info = info;

  vkGraphicsPipeline.insert(info.name, result);
  return *result;
}

void VulkanRHI::releaseGraphicsPipeline(VulkanGraphicsPipeline &handle)
{
  VulkanGraphicsPipeline *pipeline = &handle;
  vkDestroyPipeline(device, handle.pipeline, NULL);
  vkDestroyRenderPass(device, handle.renderPass, NULL);
  vkGraphicsPipeline.erase(handle.info.name);
  delete pipeline;
}

VulkanComputePipeline &VulkanRHI::allocateComputePipeline(const ComputePipelineInfo &info)
{
  VkPipelineShaderStageCreateInfo computeShaderStageInfo{};
  computeShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  computeShaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;

  const VulkanShader &shader = getVulkanShader(info.shader.name);
  if (shader.shaderModule == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Invalid compute shader!");
  }
  computeShaderStageInfo.module = shader.shaderModule;
  computeShaderStageInfo.pName = info.entry;

  const VulkanBindingsLayout &layout = getVulkanBindingsLayout(info.layout.name);
  if (layout.pipelineLayout == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Invalid pipeline layout in ComputePipelineInfo!");
  }

  VkComputePipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipelineInfo.stage = computeShaderStageInfo;
  pipelineInfo.layout = layout.pipelineLayout;
  pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
  pipelineInfo.basePipelineIndex = -1;

  VkPipeline pipeline = VK_NULL_HANDLE;
  if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create compute pipeline!");
  }

  VulkanComputePipeline *result = new VulkanComputePipeline();
  result->pipeline = pipeline;
  result->layout = info.layout;
  result->info = info;

  vkComputePipeline.insert(info.name, result);
  return *result;
}

void VulkanRHI::releaseComputePipeline(VulkanComputePipeline &vkPipeline)
{
  VulkanComputePipeline *pipeline = &vkPipeline;
  if (vkPipeline.pipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(device, vkPipeline.pipeline, nullptr);
  }

  vkComputePipeline.erase(vkPipeline.info.name);
  delete pipeline;
}

} // namespace vulkan
} // namespace backend
} // namespace rendering