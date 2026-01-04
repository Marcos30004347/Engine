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

VulkanRHI::VulkanRHI(VulkanVersion version, DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures, std::vector<std::string> extensions)
    : RHI(), commandBuffersAllocated(0), eventLoop(VulkanAsyncHandler::getStatus)
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
bool VulkanRHI::checkValidationLayerSupport()
{
  uint32_t layerCount;
  vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

  std::vector<VkLayerProperties> availableLayers(layerCount);
  vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

  for (const auto &layer : availableLayers)
  {
    printf("Vulkan Layer available: %s\n", layer.layerName);
  }

  for (const char *layerName : validationLayers)
  {
    bool layerFound = false;

    for (const auto &layerProperties : availableLayers)
    {

      if (strcmp(layerName, layerProperties.layerName) == 0)
      {
        layerFound = true;
        break;
      }
    }

    if (!layerFound)
    {
      return false;
    }
  }

  return true;
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

  vkBuffers.remove(buf.info.name);
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

  vkTextures.remove(tex->info.name);
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

  vkSamplers.remove(sampler.info.name);
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

  vkBindingsGroups.remove(layout.name);
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

    for (auto &view : group.textureViews)
    {
      destroyTextureView(view);
    }
  }

  groups.groups.clear();

  vkBindingsGroups.remove(groups.info.name);
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

  auto swapChainImp = swapChains.insert(
      (SwapChain)surfaceIndex,
      VulkanSwapChain{
        .swapChain = VK_NULL_HANDLE,
      });

  if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &(swapChainImp->swapChain)) != VK_SUCCESS)
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

  return SwapChain(surfaceIndex);
}

void VulkanRHI::destroySwapChain(SwapChain swapChain)
{
  auto swapChainImp = swapChains[swapChain];

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

  swapChains.remove(swapChain);
}

const uint32_t VulkanRHI::getSwapChainImagesCount(SwapChain swapChainHandle)
{
  auto swapChain = swapChains[swapChainHandle];
  return swapChain->swapChainImages.size();
}

const TextureView VulkanRHI::getCurrentSwapChainTextureView(SwapChain swapChainHandle, uint32_t imageIndex)
{
  auto swapChain = swapChains[swapChainHandle];

  uint32_t index = UINT32_MAX;

  if (vkAcquireNextImageKHR(device, swapChain->swapChain, UINT64_MAX, swapChain->achireSemaphores[imageIndex], VK_NULL_HANDLE, &index) != VK_SUCCESS)
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

  view.texture.name = "_SwapChainImage[" + std::to_string((uint64_t)swapChainHandle) + "," + std::to_string(index) + "].texture";

  return view;
}

const VulkanTexture &VulkanRHI::getVulkanTexture(const std::string &obj)
{
  auto result = vkTextures.find(obj);

  if (result == vkTextures.end())
  {
    throw std::runtime_error("VulkanTexture not found");
  }

  return *result.value(); //->value();
}

const VulkanSampler &VulkanRHI::getVulkanSampler(const std::string &obj)
{
  auto result = vkSamplers.find(obj);

  if (result == vkSamplers.end())
  {
    throw std::runtime_error("VulkanSampler not found");
  }

  return *result.value();
}

const VulkanBuffer &VulkanRHI::getVulkanBuffer(const std::string &obj)
{
  auto result = vkBuffers.find(obj);

  if (result == vkBuffers.end())
  {
    throw std::runtime_error("VulkanBuffer not found");
  }

  return *result.value();
}

const VulkanBindingsLayout &VulkanRHI::getVulkanBindingsLayout(const std::string &obj)
{
  auto result = vkBindingsLayout.find(obj);

  if (result == vkBindingsLayout.end())
  {
    throw std::runtime_error("VulkanBindingsLayout not found");
  }

  return *result.value();
}

const VulkanBindingGroups &VulkanRHI::getVulkanBindingGroups(const std::string &obj)
{
  auto result = vkBindingsGroups.find(obj);

  if (result == vkBindingsGroups.end())
  {
    throw std::runtime_error("VulkanBindingGroups not found");
  }

  return *result.value();
}

const VulkanGraphicsPipeline &VulkanRHI::getVulkanGraphicsPipeline(const std::string &obj)
{
  auto result = vkGraphicsPipeline.find(obj);

  if (result == vkGraphicsPipeline.end())
  {
    throw std::runtime_error("VulkanGraphicsPipeline not found");
  }

  return *result.value();
}

const VulkanComputePipeline &VulkanRHI::getVulkanComputePipeline(const std::string &obj)
{
  auto result = vkComputePipeline.find(obj);

  if (result == vkComputePipeline.end())
  {
    throw std::runtime_error("VulkanComputePipeline not found");
  }

  return *result.value();
}

const VulkanShader &VulkanRHI::getVulkanShader(const std::string &obj)
{
  auto result = vkShaders.find(obj);

  if (result == vkShaders.end())
  {
    throw std::runtime_error("VulkanShader not found");
  }

  return *result.value();
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
  vkGraphicsPipeline.remove(handle.info.name);
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

  vkComputePipeline.remove(vkPipeline.info.name);
  delete pipeline;
}

VulkanCommandPool VulkanRHI::allocateCommandPool(uint32_t queueFamilyIndex)
{
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.queueFamilyIndex = queueFamilyIndex;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // Optional

  VkCommandPool commandPool;
  if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create command pool");
  }
  VulkanCommandPool pool;
  pool.commandPool = commandPool;
  return pool;
}

void VulkanRHI::releaseCommandPool(VulkanCommandPool &pool)
{
  vkDestroyCommandPool(device, pool.commandPool, nullptr);
}

std::vector<CommandBuffer> VulkanRHI::allocateCommandBuffers(VulkanCommandPool &commandPool, uint32_t count, VkCommandBufferLevel level)
{
  VkCommandBufferAllocateInfo allocInfo{};

  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = commandPool.commandPool;
  allocInfo.level = level;
  allocInfo.commandBufferCount = count;

  std::vector<VkCommandBuffer> cmbs(count);

  if (vkAllocateCommandBuffers(device, &allocInfo, cmbs.data()) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to allocate command buffers");
  }

  std::vector<CommandBuffer> buffers;

  for (uint32_t i = 0; i < count; i++)
  {
    auto index = commandBuffersAllocated.fetch_add(1);

    auto commandBufferData = commandBuffers.insert(
        (CommandBuffer)index,
        VulkanCommandBuffer{
          .commandBuffer = cmbs[i],
          .commandPool = commandPool.commandPool,
          .boundComputePipeline = nullptr,
          .boundGraphicsPipeline = nullptr,
          .renderPasses = std::vector<VulkanCommandBufferRenderPass>(),
        });

    buffers.push_back((CommandBuffer)index);
  }

  return buffers;
}

void VulkanRHI::releaseCommandBuffer(std::vector<CommandBuffer> &buffers)
{
  for (auto &buff : buffers)
  {
    auto commandbuffer = commandBuffers[buff];

    for (auto renderPassData : commandbuffer->renderPasses)
    {
      for (auto view : renderPassData.views)
      {
        vkDestroyImageView(device, view.view, nullptr);
      }

      vkDestroyFramebuffer(device, renderPassData.frameBuffer, nullptr);
    }

    vkFreeCommandBuffers(device, commandbuffer->commandPool, 1, &(commandbuffer->commandBuffer));
  }
}

void VulkanRHI::beginCommandBuffer(CommandBuffer handle)
{
  auto cmd = commandBuffers[handle];

  VkCommandBufferBeginInfo beginInfo{
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
  };

  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  beginInfo.pInheritanceInfo = nullptr;

  VkResult r = vkBeginCommandBuffer(cmd->commandBuffer, &beginInfo);
  if (r != VK_SUCCESS)
    throw std::runtime_error("vkBeginCommandBuffer failed");
}

void VulkanRHI::endCommandBuffer(CommandBuffer handle)
{
  auto cmd = commandBuffers[handle];

  VkResult r = vkEndCommandBuffer(cmd->commandBuffer);
  if (r != VK_SUCCESS)
    throw std::runtime_error("vkEndCommandBuffer failed");
}

void VulkanRHI::cmdCopyBuffer(CommandBuffer cmdBuffer, Buffer src, Buffer dst, uint32_t srcOffset, uint32_t dstOffset, uint32_t size)
{
  auto cmd = commandBuffers[cmdBuffer];
  VkCommandBuffer vkCmd = cmd->commandBuffer;

  VkBufferCopy copyRegion{};

  copyRegion.srcOffset = srcOffset;
  copyRegion.dstOffset = dstOffset;

  copyRegion.size = size;
  auto srcBuffer = getVulkanBuffer(src.name);
  auto dstBuffer = getVulkanBuffer(dst.name);

  vkCmdCopyBuffer(vkCmd, srcBuffer.buffer, dstBuffer.buffer, 1, &copyRegion);
}

static VkImageLayout toVulkanLayout(ResourceLayout layout)
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
    assert(false && "Invalid resource layout");
    return VK_IMAGE_LAYOUT_UNDEFINED;
  }
}

struct SwapChainInfo
{
  uint64_t handle;
  uint32_t index;
};

bool parseSwapChainString(const std::string &input, SwapChainInfo &outInfo)
{
  try
  {
    if (input.rfind("_SwapChainImage", 0) == 0)
    {
      return false;
    }

    size_t startBracket = input.find('[');
    size_t comma = input.find(',');
    size_t endBracket = input.find(']');

    if (startBracket == std::string::npos || comma == std::string::npos || endBracket == std::string::npos)
    {
      return false;
    }

    // Extract substrings
    std::string handleStr = input.substr(startBracket + 1, comma - startBracket - 1);
    std::string indexStr = input.substr(comma + 1, endBracket - comma - 1);

    // Convert strings to numbers
    outInfo.handle = std::stoull(handleStr);
    outInfo.index = static_cast<uint32_t>(std::stoul(indexStr));

    return true;
  }
  catch (...)
  {
    // Handle cases where stoull might fail due to non-numeric content
    return false;
  }
}

void VulkanRHI::cmdBindGraphicsPipeline(CommandBuffer handle, GraphicsPipeline pipelineHandle)
{
  auto commandBuffer = commandBuffers[handle]; // einterpret_cast<VulkanCommandBuffer *>(handle.get());
  VkCommandBuffer cmd = commandBuffer->commandBuffer;
  auto pipeline = getVulkanGraphicsPipeline(pipelineHandle.name); // reinterpret_cast<VulkanGraphicsPipeline *>(pipelineHandle.get())->pipeline;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);

  if (commandBuffer->boundComputePipeline || commandBuffer->boundGraphicsPipeline)
  {
    throw std::runtime_error("pipeline already binded to command buffer");
  }

  commandBuffer->boundGraphicsPipeline = &pipeline; // reinterpret_cast<VulkanGraphicsPipeline *>(pipelineHandle.get());
}

void VulkanRHI::cmdBindComputePipeline(CommandBuffer handle, ComputePipeline pipelineHandle)
{
  auto commandBuffer = commandBuffers[handle]; // einterpret_cast<VulkanCommandBuffer *>(handle.get());
  VkCommandBuffer cmd = commandBuffer->commandBuffer;
  // VkPipeline pipeline = reinterpret_cast<VulkanComputePipeline *>(pipelineHandle.get())->pipeline;

  auto pipeline = getVulkanComputePipeline(pipelineHandle.name); // reinterpret_cast<VulkanGraphicsPipeline *>(pipelineHandle.get())->pipeline;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);

  if (commandBuffer->boundComputePipeline || commandBuffer->boundGraphicsPipeline)
  {
    throw std::runtime_error("pipeline already binded to command buffer");
  }

  commandBuffer->boundComputePipeline = &pipeline; // reinterpret_cast<VulkanComputePipeline *>(pipelineHandle);
}

void VulkanRHI::cmdBeginRenderPass(CommandBuffer cmdHandle, const RenderPassInfo &rpInfo)
{
  // Map your CommandBufferHandle to VkCommandBuffer

  auto commandBuffer = commandBuffers[cmdHandle]; // reinterpret_cast<VulkanCommandBuffer *>(cmdHandle.get());

  if (!commandBuffer->boundGraphicsPipeline)
  {
    throw std::runtime_error("no pipeline was bound");
  }

  VulkanGraphicsPipeline *pipelineData = commandBuffer->boundGraphicsPipeline;

  if (pipelineData->renderPass == VK_NULL_HANDLE)
  {
    throw std::runtime_error("no render pass");
  }

  if (pipelineData->pipeline == VK_NULL_HANDLE)
  {
    throw std::runtime_error("no pipeline");
  }

  VkCommandBuffer cmdBuffer = commandBuffer->commandBuffer;

  VkFramebufferCreateInfo framebufferInfo{};
  framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebufferInfo.renderPass = pipelineData->renderPass;

  std::vector<VulkanTextureView> views;
  std::vector<VkImageView> attachments;
  std::vector<VkSemaphore> achireSemaphores;
  std::vector<VkSemaphore> presentSemaphores;

  if (rpInfo.colorAttachments.size() != pipelineData->info.fragmentStage.colorAttatchments.size())
  {
    throw std::runtime_error("render pass color attatchments count does not match pipeline");
  }

  // if (rpInfo.depthStencilAttachment.clearDepth != NULL && pipelineData->info.fragmentStage.depthAttatchment.storeOp == StoreOp::StoreOp_DontCare)
  // {
  //   throw std::runtime_error("render pass depth attatchment not configured given pipeline");
  // }

  for (int i = 0; i < rpInfo.colorAttachments.size(); i++)
  {
    // if (rpInfo.colorAttachments[i].view == NULL)
    // {
    //   throw std::runtime_error("color attatchment view not setup");
    // }

    auto view = createTextureView(rpInfo.colorAttachments[i].view);
    views.push_back(view);

    // commandBuffer->renderPasses

    attachments.push_back(view.view);
    SwapChainInfo scinfo;
    if (parseSwapChainString(rpInfo.colorAttachments[i].view.texture.name, scinfo))
    {
      auto swapChain = swapChains[(SwapChain)(scinfo.handle)];

      achireSemaphores.push_back(swapChain->achireSemaphores[scinfo.index]);
      presentSemaphores.push_back(swapChain->presentSemaphores[scinfo.index]);
    }

    // if (texture->achireSemaphore != VK_NULL_HANDLE)
    // {
    //   achireSemaphores.push_back(texture->achireSemaphore);
    // }

    // if (texture->presentSemaphore != VK_NULL_HANDLE)
    // {
    //   presentSemaphores.push_back(texture->presentSemaphore);
    // }
  }

  if (rpInfo.depthStencilAttachment.clearDepth)
  {
    auto depthTexture = createTextureView(rpInfo.depthStencilAttachment.view);
    attachments.push_back(depthTexture.view);
    views.push_back(depthTexture);
  }

  framebufferInfo.attachmentCount = attachments.size();
  framebufferInfo.pAttachments = attachments.data();
  framebufferInfo.width = rpInfo.viewport.width;
  framebufferInfo.height = rpInfo.viewport.height;
  framebufferInfo.layers = 1;

  VkFramebuffer frameBuffer;

  if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &frameBuffer) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create framebuffer!");
  }

  VulkanCommandBufferRenderPass commandBufferFrameData;
  commandBufferFrameData.frameBuffer = frameBuffer;
  commandBufferFrameData.renderPass = pipelineData->renderPass;
  commandBufferFrameData.achireSemaphores = std::move(achireSemaphores);
  commandBufferFrameData.presentSemaphores = std::move(presentSemaphores);
  commandBufferFrameData.views = std::move(views);

  for (int i = 0; i < rpInfo.colorAttachments.size(); i++)
  {
    // auto view = createTextureView(rpInfo.colorAttachments[i].view);
    SwapChainInfo nameinfo;

    if (parseSwapChainString(rpInfo.colorAttachments[i].view.texture.name, nameinfo))
    {
      VulkanAttatchment info = {};

      info.presentQueue = swapChains[(SwapChain)nameinfo.handle]->presentQueue; // view->renderData.swapChain->presentQueue.queue;
      info.swapChain = (SwapChain)nameinfo.handle;
      info.swapChainImageIndex = nameinfo.index;

      commandBufferFrameData.attatchments.push_back(info);
    }
  }

  commandBuffer->renderPasses.push_back(commandBufferFrameData);

  // Build clear values array (color + optional depth)
  std::vector<VkClearValue> clearValues;

  for (int i = 0; i < rpInfo.colorAttachments.size(); i++)
  {
    VkClearValue clearColor{};

    clearColor.color.float32[0] = rpInfo.colorAttachments[i].clearValue.R;
    clearColor.color.float32[1] = rpInfo.colorAttachments[i].clearValue.G;
    clearColor.color.float32[2] = rpInfo.colorAttachments[i].clearValue.B;
    clearColor.color.float32[3] = rpInfo.colorAttachments[i].clearValue.A;

    clearValues.push_back(clearColor);
  }

  if (rpInfo.depthStencilAttachment.clearDepth)
  {
    VkClearValue clearDepth{};
    clearDepth.depthStencil.depth = rpInfo.depthStencilAttachment.clearDepth;
    clearDepth.depthStencil.stencil = rpInfo.depthStencilAttachment.clearStencil;
    clearValues.push_back(clearDepth);
  }

  // Begin info
  VkRenderPassBeginInfo rpBeginInfo{};
  rpBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpBeginInfo.renderPass = pipelineData->renderPass;
  rpBeginInfo.framebuffer = frameBuffer;
  rpBeginInfo.renderArea.offset = {static_cast<int32_t>(rpInfo.scissor.x), static_cast<int32_t>(rpInfo.scissor.y)};
  rpBeginInfo.renderArea.extent = {rpInfo.scissor.width, rpInfo.scissor.height};
  rpBeginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
  rpBeginInfo.pClearValues = clearValues.data();

  vkCmdBeginRenderPass(cmdBuffer, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = static_cast<float>(rpInfo.viewport.width);
  viewport.height = static_cast<float>(rpInfo.viewport.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = {static_cast<int32_t>(rpInfo.scissor.x), static_cast<int32_t>(rpInfo.scissor.y)};
  scissor.extent = {rpInfo.scissor.width, rpInfo.scissor.height};

  vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);
}

void VulkanRHI::cmdEndRenderPass(CommandBuffer cmdHandle)
{
  auto cmdBuffer = commandBuffers[cmdHandle]; // reinterpret_cast<VulkanCommandBuffer *>(cmdHandle.get())->commandBuffer;
  vkCmdEndRenderPass(cmdBuffer->commandBuffer);
}

void VulkanRHI::cmdBindBindingGroups(CommandBuffer cmdBuffer, BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount)
{
  auto commandBuffer = commandBuffers[cmdBuffer]; // reinterpret_cast<VulkanCommandBuffer *>(cmdBuffer.get());
  VkPipelineLayout layout = VK_NULL_HANDLE;

  if (commandBuffer->boundComputePipeline)
  {
    layout = getVulkanBindingsLayout(commandBuffer->boundComputePipeline->layout.name).pipelineLayout;
  }
  else if (commandBuffer->boundGraphicsPipeline)
  {
    layout = getVulkanBindingsLayout(commandBuffer->boundGraphicsPipeline->layout.name).pipelineLayout;
  }
  else
  {
    throw std::runtime_error("No bound pipeline");
  }

  auto vkGroups = getVulkanBindingGroups(groups.name);

  VkPipelineBindPoint point = VK_PIPELINE_BIND_POINT_MAX_ENUM;

  if (commandBuffer->boundGraphicsPipeline)
  {
    point = VK_PIPELINE_BIND_POINT_GRAPHICS;
  }
  if (commandBuffer->boundComputePipeline)
  {
    point = VK_PIPELINE_BIND_POINT_COMPUTE;
  }
  if (point == VK_PIPELINE_BIND_POINT_MAX_ENUM)
  {
    throw std::runtime_error("Invalid pipeline bind point");
  }

  // commandBuffer->boundGroups->groups = vkGroups;
  std::vector<VkDescriptorSet> sets;

  for (auto &g : vkGroups.groups)
  {
    for (auto &s : g.descriptorSets)
    {
      sets.push_back(s);
    }
  }

  vkCmdBindDescriptorSets(commandBuffer->commandBuffer, point, layout, 0, static_cast<uint32_t>(sets.size()), sets.data(), dynamicOffsetsCount, dynamicOffsets);
}

void VulkanRHI::cmdBindVertexBuffer(CommandBuffer handle, uint32_t slot, Buffer buffer, uint64_t offfset)
{
  auto cmd = commandBuffers[handle]; // reinterpret_cast<VulkanCommandBuffer *>(cmdBuffer.get());

  auto heap = getVulkanBuffer(buffer.name); // reinterpret_cast<VulkanBuffer *>(bufferHandle.buffer.get());
  VkBuffer vkBuf = heap.buffer;
  VkDeviceSize vkOffset = static_cast<VkDeviceSize>(offfset);

  vkCmdBindVertexBuffers(cmd->commandBuffer, slot, 1, &vkBuf, &vkOffset);
}

void VulkanRHI::cmdBindIndexBuffer(CommandBuffer handle, Buffer buffer, Type type, uint64_t offset)
{
  auto cmd = commandBuffers[handle];        // reinterpret_cast<VulkanCommandBuffer *>(cmdBuffer.get());
  auto heap = getVulkanBuffer(buffer.name); // reinterpret_cast<VulkanBuffer *>(bufferHandle.buffer.get());

  VkBuffer vkBuf = heap.buffer;

  VkDeviceSize vkOffset = static_cast<VkDeviceSize>(offset);

  VkIndexType vkIndexType = VK_INDEX_TYPE_UINT32;
  if (type == Type_Uint16)
    vkIndexType = VK_INDEX_TYPE_UINT16;
  else if (type == Type_Uint32)
    vkIndexType = VK_INDEX_TYPE_UINT32;
  vkCmdBindIndexBuffer(cmd->commandBuffer, vkBuf, vkOffset, vkIndexType);
}

void VulkanRHI::cmdDraw(CommandBuffer handle, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
  auto cmd = commandBuffers[handle];
  vkCmdDraw(cmd->commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanRHI::cmdDrawIndexed(CommandBuffer handle, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
  auto cmd = commandBuffers[handle];
  vkCmdDrawIndexed(cmd->commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VulkanRHI::cmdDrawIndexedIndirect(CommandBuffer handle, Buffer indirectBuffer, size_t offset, uint32_t drawCount, uint32_t stride)
{
  auto cmd = commandBuffers[handle];
  auto heap = getVulkanBuffer(indirectBuffer.name);
  VkBuffer vkBuf = heap.buffer;

  VkDeviceSize vkOffset = static_cast<VkDeviceSize>(offset);

  vkCmdDrawIndexedIndirect(cmd->commandBuffer, vkBuf, vkOffset, drawCount, stride);
}

void VulkanRHI::cmdDispatch(CommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
  auto cmd = commandBuffers[commandBuffer];

  if (cmd->boundComputePipeline == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Attempted to dispatch with no compute pipeline bound!");
  }

  vkCmdDispatch(cmd->commandBuffer, groupCountX, groupCountY, groupCountZ);
}

static VkPipelineStageFlags toVulkanStage(PipelineStage stage)
{
  switch (stage)
  {
  case PipelineStage::TOP_OF_PIPE:
    return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  case PipelineStage::VERTEX_INPUT:
    return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
  case PipelineStage::VERTEX_SHADER:
    return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    //   case PipelineStage::TESSELLATION_CONTROL_SHADER:
    //     return VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT;
    //   case PipelineStage::TESSELLATION_EVALUATION_SHADER:
    //     return VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    //   case PipelineStage::GEOMETRY_SHADER:
    //     return VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
  case PipelineStage::FRAGMENT_SHADER:
    return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    //   case PipelineStage::EARLY_FRAGMENT_TESTS:
    //     return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    //   case PipelineStage::LATE_FRAGMENT_TESTS:
    //     return VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    //   case PipelineStage::COLOR_ATTACHMENT_OUTPUT:
    return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  case PipelineStage::COMPUTE_SHADER:
    return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  case PipelineStage::TRANSFER:
    return VK_PIPELINE_STAGE_TRANSFER_BIT;
  case PipelineStage::BOTTOM_OF_PIPE:
    return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  case PipelineStage::ALL_GRAPHICS:
    return VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
  case PipelineStage::ALL_COMMANDS:
    return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  case PipelineStage::HOST:
    return VK_PIPELINE_STAGE_HOST_BIT;
  default:
    assert(false && "Invalid pipeline stage");
    return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  }
}

static VkAccessFlags toVulkanAccess(AccessPattern access)
{
  switch (access)
  {
  case AccessPattern::NONE:
    return 0;
  case AccessPattern::VERTEX_ATTRIBUTE_READ:
    return VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
  case AccessPattern::INDEX_READ:
    return VK_ACCESS_INDEX_READ_BIT;
  case AccessPattern::UNIFORM_READ:
    return VK_ACCESS_UNIFORM_READ_BIT;
  case AccessPattern::SHADER_READ:
    return VK_ACCESS_SHADER_READ_BIT;
  case AccessPattern::SHADER_WRITE:
    return VK_ACCESS_SHADER_WRITE_BIT;
  case AccessPattern::COLOR_ATTACHMENT_READ:
    return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
  case AccessPattern::COLOR_ATTACHMENT_WRITE:
    return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  case AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ:
    return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
  case AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE:
    return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  case AccessPattern::TRANSFER_READ:
    return VK_ACCESS_TRANSFER_READ_BIT;
  case AccessPattern::TRANSFER_WRITE:
    return VK_ACCESS_TRANSFER_WRITE_BIT;
  case AccessPattern::INDIRECT_COMMAND_READ:
    return VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
  case AccessPattern::MEMORY_READ:
    return VK_ACCESS_MEMORY_READ_BIT;
  case AccessPattern::MEMORY_WRITE:
    return VK_ACCESS_MEMORY_WRITE_BIT;
  default:
    assert(false && "Invalid access pattern");
    return 0;
  }
}

static VkBufferMemoryBarrier createBufferBarrier(
    VkBuffer buffer,
    PipelineStage src_stage,
    PipelineStage dst_stage,
    AccessPattern src_access,
    AccessPattern dst_access,
    VkDeviceSize offset = 0,
    VkDeviceSize size = VK_WHOLE_SIZE,
    uint32_t src_queue_family = VK_QUEUE_FAMILY_IGNORED,
    uint32_t dst_queue_family = VK_QUEUE_FAMILY_IGNORED)
{
  VkBufferMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  barrier.srcAccessMask = toVulkanAccess(src_access);
  barrier.dstAccessMask = toVulkanAccess(dst_access);
  barrier.srcQueueFamilyIndex = src_queue_family;
  barrier.dstQueueFamilyIndex = dst_queue_family;
  barrier.buffer = buffer;
  barrier.offset = offset;
  barrier.size = size;
  return barrier;
}

static VkImageMemoryBarrier createImageBarrier(
    VkImage image,
    PipelineStage src_stage,
    PipelineStage dst_stage,
    AccessPattern src_access,
    AccessPattern dst_access,
    ResourceLayout old_layout,
    ResourceLayout new_layout,
    VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT,
    uint32_t base_mip_level = 0,
    uint32_t level_count = VK_REMAINING_MIP_LEVELS,
    uint32_t base_array_layer = 0,
    uint32_t layer_count = VK_REMAINING_ARRAY_LAYERS,
    uint32_t src_queue_family = VK_QUEUE_FAMILY_IGNORED,
    uint32_t dst_queue_family = VK_QUEUE_FAMILY_IGNORED)
{
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  barrier.srcAccessMask = toVulkanAccess(src_access);
  barrier.dstAccessMask = toVulkanAccess(dst_access);
  barrier.oldLayout = toVulkanLayout(old_layout);
  barrier.newLayout = toVulkanLayout(new_layout);
  barrier.srcQueueFamilyIndex = src_queue_family;
  barrier.dstQueueFamilyIndex = dst_queue_family;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = aspect_mask;
  barrier.subresourceRange.baseMipLevel = base_mip_level;
  barrier.subresourceRange.levelCount = level_count;
  barrier.subresourceRange.baseArrayLayer = base_array_layer;
  barrier.subresourceRange.layerCount = layer_count;
  return barrier;
}

static VkMemoryBarrier createMemoryBarrier(AccessPattern src_access, AccessPattern dst_access)
{
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  barrier.srcAccessMask = toVulkanAccess(src_access);
  barrier.dstAccessMask = toVulkanAccess(dst_access);
  return barrier;
}

void VulkanRHI::cmdBufferBarrier(
    CommandBuffer cmd,
    Buffer b,
    PipelineStage src_stage,
    PipelineStage dst_stage,
    AccessPattern src_access,
    AccessPattern dst_access,
    uint32_t offset,
    uint32_t size,
    uint32_t src_queue_family,
    uint32_t dst_queue_family)
{
  auto commandBuffer = commandBuffers[cmd];
  auto buffer = getVulkanBuffer(b.name);

  VkBufferMemoryBarrier barrier = createBufferBarrier(buffer.buffer, src_stage, dst_stage, src_access, dst_access, offset, size, src_queue_family, dst_queue_family);

  vkCmdPipelineBarrier(commandBuffer->commandBuffer, toVulkanStage(src_stage), toVulkanStage(dst_stage), 0, 0, nullptr, 1, &barrier, 0, nullptr);
}

inline bool hasFlag(ImageAspectFlags value, ImageAspectFlags flag)
{
  return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

inline VkImageAspectFlags imageAspectFlagsToVkImageAspectFlags(ImageAspectFlags flags)
{
  VkImageAspectFlags vkFlags = 0;
  if (hasFlag(flags, ImageAspectFlags::Color))
    vkFlags |= VK_IMAGE_ASPECT_COLOR_BIT;
  if (hasFlag(flags, ImageAspectFlags::Depth))
    vkFlags |= VK_IMAGE_ASPECT_DEPTH_BIT;
  if (hasFlag(flags, ImageAspectFlags::Stencil))
    vkFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;
  return vkFlags;
}

void VulkanRHI::cmdImageBarrier(
    CommandBuffer cmd,
    Texture image,
    PipelineStage src_stage,
    PipelineStage dst_stage,
    AccessPattern src_access,
    AccessPattern dst_access,
    ResourceLayout old_layout,
    ResourceLayout new_layout,
    ImageAspectFlags aspect_mask,
    uint32_t base_mip_level,
    uint32_t level_count,
    uint32_t base_array_layer,
    uint32_t layer_count,
    uint32_t src_queue_family,
    uint32_t dst_queue_family)
{
  auto commandBuffer = commandBuffers[cmd];

  auto vkImage = getVulkanTexture(image.name);
  VkImageMemoryBarrier barrier = createImageBarrier(
      vkImage.image,
      src_stage,
      dst_stage,
      src_access,
      dst_access,
      old_layout,
      new_layout,
      imageAspectFlagsToVkImageAspectFlags(aspect_mask),
      base_mip_level,
      level_count,
      base_array_layer,
      layer_count,
      src_queue_family,
      dst_queue_family);

  vkCmdPipelineBarrier(commandBuffer->commandBuffer, toVulkanStage(src_stage), toVulkanStage(dst_stage), 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanRHI::cmdMemoryBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access)
{
  auto commandBuffer = commandBuffers[cmd];
  VkMemoryBarrier barrier = createMemoryBarrier(src_access, dst_access);
  vkCmdPipelineBarrier(commandBuffer->commandBuffer, toVulkanStage(src_stage), toVulkanStage(dst_stage), 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void VulkanRHI::cmdPipelineBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access)
{
  auto commandBuffer = commandBuffers[cmd];

  VkMemoryBarrier barrier{};

  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = toVulkanAccess(src_access);
  barrier.dstAccessMask = toVulkanAccess(dst_access);

  vkCmdPipelineBarrier(commandBuffer->commandBuffer, toVulkanStage(src_stage), toVulkanStage(dst_stage), 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

static VkFence createFence(VkDevice device, bool signaled = false)
{
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fi.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
  VkFence fence;
  if (vkCreateFence(device, &fi, nullptr, &fence) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create fence");
  }
  return fence;
}

VkFence VulkanRHI::getFence()
{
  VkFence data;

  if (fences.dequeue(data))
  {
    vkResetFences(device, 1, &data);
  }
  else
  {
    data = createFence(device, false);
  }

  return data;
}

VkSemaphore VulkanRHI::getSemaphore()
{
  VkSemaphore data;
  if (semaphores.dequeue(data))
  {
  }
  else
  {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &data) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create semaphore for image views!");
    }
  }

  return data;
}

void VulkanRHI::cleanupSubmitCallback(VulkanRHI::VulkanAsyncHandler &future)
{
  future.device->fences.enqueue(future.fence);
  future.device->semaphores.enqueue(future.semaphore);

  future.fence = VK_NULL_HANDLE;

  for (auto fb : future.framebuffers)
  {
    vkDestroyFramebuffer(future.device->device, fb, NULL);
  }

  for (auto &view : future.views)
  {
    // view.fence = VK_NULL_HANDLE;
    // view->presentSemaphore = VK_NULL_HANDLE;
    // view->achireSemaphore = VK_NULL_HANDLE;
  }

  future.framebuffers.clear();
  future.views.clear();
}

VulkanRHI::VulkanAsyncHandler::VulkanAsyncHandler(VulkanRHI *device, VkFence fence, VkSemaphore semaphore, std::vector<VkFramebuffer> &fb, std::vector<VulkanTextureView> &views)
{
  this->device = device;
  this->fence = fence;
  this->views = std::move(views);
  this->framebuffers = std::move(fb);
  this->semaphore = semaphore;
}

FenceStatus VulkanRHI::VulkanAsyncHandler::getStatus(VulkanAsyncHandler &future)
{
  switch (vkGetFenceStatus(future.device->device, future.fence))
  {
  case VK_SUCCESS:
    return FenceStatus::FINISHED;
  case VK_NOT_READY:
    return FenceStatus::PENDING;
  default:
    return FenceStatus::ERROR;
  }
  return FenceStatus::ERROR;
}

// VulkanRHI::VulkanFuture::VulkanFuture(AsyncEvent<VulkanAsyncHandler> &&handler) : handler(std::forward<AsyncEvent<VulkanAsyncHandler>>(handler))
// {
// }

VkQueue VulkanRHI::getQueueHandle(Queue queueType)
{
  switch (queueType)
  {
  case Queue::Graphics:
    if (graphicsQueue.empty())
      throw std::runtime_error("Graphics queue not initialized");
    return graphicsQueue[0];

  case Queue::Compute:
    // Fallback to graphics if a dedicated compute queue isn't available
    if (!computeQueue.empty())
      return computeQueue[0];
    return graphicsQueue[0];

  case Queue::Transfer:
    // Fallback to graphics/compute if a dedicated transfer queue isn't available
    if (!transferQueue.empty())
      return transferQueue[0];
    if (!computeQueue.empty())
      return computeQueue[0];
    return graphicsQueue[0];

  case Queue::Present:
    // Presentation usually requires a specific VkSurfaceKHR check.
    // Since submit() shouldn't handle raw presentation logic directly:
    throw std::invalid_argument("Queue::Present cannot be used for command submission");

  default:
    throw std::invalid_argument("Unknown Queue type requested");
  }
}
void VulkanRHI::processPresentations(CommandBuffer *cmds, uint32_t count, const std::vector<VkSemaphore> &signalSemaphores)
{
  for (uint32_t i = 0; i < count; i++)
  {
    auto cmdBuf = commandBuffers[cmds[i]];
    for (auto &frameData : cmdBuf->renderPasses)
    {
      // Group swapchain images by their respective present queues
      std::unordered_map<VkQueue, std::vector<VkSwapchainKHR>> queueGroups;
      std::unordered_map<VkQueue, std::vector<uint32_t>> indexGroups;

      for (auto &attachment : frameData.attatchments)
      {
        if (attachment.swapChain != (SwapChain)-1)
        {
          auto sc = swapChains[(SwapChain)attachment.swapChain];
          queueGroups[attachment.presentQueue].push_back(sc->swapChain);
          indexGroups[attachment.presentQueue].push_back(attachment.swapChainImageIndex);
        }
      }

      for (auto &[presentQueue, vkSwaps] : queueGroups)
      {
        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
        presentInfo.pWaitSemaphores = signalSemaphores.data();
        presentInfo.swapchainCount = static_cast<uint32_t>(vkSwaps.size());
        presentInfo.pSwapchains = vkSwaps.data();
        presentInfo.pImageIndices = indexGroups[presentQueue].data();

        vkQueuePresentKHR(presentQueue, &presentInfo);
        // Note: Error handling (OUT_OF_DATE) should typically trigger a swapchain recreation flag
      }
    }
  }
}
GPUFuture VulkanRHI::submit(Queue queueType, CommandBuffer *cmds, uint32_t count, GPUFuture *wait, uint32_t waitCount)
{
  VkQueue queue = getQueueHandle(queueType); // Helper to handle the switch logic

  std::vector<VkCommandBuffer> vkCmds;
  std::vector<VkSemaphore> waitSemaphores;
  std::vector<VkPipelineStageFlags> waitStages;
  std::vector<VkSemaphore> signalSemaphores;

  // Resource tracking for the handler
  std::vector<VkFramebuffer> framebuffers;
  std::vector<VulkanTextureView> views;

  vkCmds.reserve(count);

  if (wait != nullptr)
  {
    for (uint32_t i = 0; i < waitCount; i++)
    {
      auto internalEvent = wait[i].getIf<rendering::AsyncEvent<VulkanAsyncHandler>>();
      if (internalEvent && internalEvent->isValid())
      {
        waitSemaphores.push_back(internalEvent->getFence()->semaphore);
        waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      }
    }
  }

  VkSemaphore mainSemaphore = getSemaphore();
  signalSemaphores.push_back(mainSemaphore);

  for (uint32_t i = 0; i < count; ++i)
  {
    auto cmdBuf = commandBuffers[cmds[i]];
    vkCmds.push_back(cmdBuf->commandBuffer);

    for (auto &frameData : cmdBuf->renderPasses)
    {
      framebuffers.push_back(frameData.frameBuffer);
      views.insert(views.end(), frameData.views.begin(), frameData.views.end());

      for (auto s : frameData.achireSemaphores)
      {
        if (s != VK_NULL_HANDLE)
        {
          waitSemaphores.push_back(s);
          waitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
      }
      for (auto s : frameData.presentSemaphores)
      {
        if (s != VK_NULL_HANDLE)
          signalSemaphores.push_back(s);
      }
    }
  }

  VkFence fence = getFence();

  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
  submitInfo.pWaitSemaphores = waitSemaphores.data();
  submitInfo.pWaitDstStageMask = waitStages.data(); // FIXED: No longer nullptr
  submitInfo.commandBufferCount = static_cast<uint32_t>(vkCmds.size());
  submitInfo.pCommandBuffers = vkCmds.data();
  submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
  submitInfo.pSignalSemaphores = signalSemaphores.data();

  if (vkQueueSubmit(queue, 1, &submitInfo, fence) != VK_SUCCESS)
  {
    throw std::runtime_error("vkQueueSubmit failed");
  }

  processPresentations(cmds, count, signalSemaphores);

  VulkanAsyncHandler handler(this, fence, mainSemaphore, framebuffers, views);
  GPUFuture resultFuture = eventLoop.submit(std::move(handler), cleanupSubmitCallback);

  eventLoop.tick();

  return resultFuture;
}

// waitIdle
void VulkanRHI::waitIdle()
{
  vkDeviceWaitIdle(device);
  eventLoop.tick();
}

void VulkanRHI::blockUntil(GPUFuture &future)
{
  auto internalEvent = future.getIf<rendering::AsyncEvent<VulkanAsyncHandler>>();
  eventLoop.blockUntil(internalEvent);
}

bool VulkanRHI::isCompleted(GPUFuture &future)
{
  eventLoop.tick();

  if (future.checkStatus() == FenceStatus::PENDING)
  {
    return false;
  }

  return true;
}

} // namespace vulkan
} // namespace backend
} // namespace rendering