#include "./VulkanRHI.hpp"
#include "datastructure/FlatMap.hpp"
#include "os/Logger.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vulkan/vulkan_format_traits.hpp>

// #define VULKAN_RHI_LOGS

namespace rendering
{
namespace backend
{
namespace vulkan
{

struct VulkanPhysicalDevice
{
  VkPhysicalDevice device;
  DeviceFeatures feature_flags;
  DeviceProperties properties;
};

template <typename Handle, typename T> static Handle makeHandle(T *ptr)
{
  const auto value = reinterpret_cast<uintptr_t>(ptr);
  if (value == 0)
    throw std::runtime_error("Attempted to create a handle from a null Vulkan object");
  return static_cast<Handle>(value);
}

template <typename T, typename Handle> static T *getHandlePtr(Handle handle, const char *typeName)
{
  const auto value = static_cast<uintptr_t>(handle);
  if (value == 0)
    throw std::runtime_error(std::string(typeName) + " not found");
  return reinterpret_cast<T *>(value);
}

static VulkanSwapChain *getSwapChainHandle(SwapChain handle)
{
  return getHandlePtr<VulkanSwapChain>(handle, "VulkanSwapChain");
}

static VulkanCommandBuffer *getCommandBufferHandle(CommandBuffer handle)
{
  return getHandlePtr<VulkanCommandBuffer>(handle, "VulkanCommandBuffer");
}

static VkBufferUsageFlags toVkBufferUsageFlags(BufferUsage usage)
{
  VkBufferUsageFlags flags = 0;

  // Pipeline usage
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

  // Timestamp buffers are written by GPU
  if (usage & BufferUsage_Timestamp)
    flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  // Explicit transfer usage (WebGPU-like)
  if (usage & BufferUsage_CopySrc)
    flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

  if (usage & BufferUsage_CopyDst)
    flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

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

static VkPipelineColorBlendAttachmentState toVkColorBlendAttachmentState(const ColorAttatchment &attachment)
{
  VkPipelineColorBlendAttachmentState state{};
  state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  switch (attachment.blendMode)
  {
  case ColorBlendMode::AlphaBlend:
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    break;
  case ColorBlendMode::Replace:
    state.blendEnable = VK_FALSE;
    state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    break;
  case ColorBlendMode::Max:
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    state.colorBlendOp = VK_BLEND_OP_MAX;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.alphaBlendOp = VK_BLEND_OP_MAX;
    break;
  case ColorBlendMode::Min:
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    state.colorBlendOp = VK_BLEND_OP_MIN;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.alphaBlendOp = VK_BLEND_OP_MIN;
    break;
  }

  return state;
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

static void resetRecordedRenderPassState(VkDevice device, VulkanCommandBuffer &commandBuffer)
{
  for (auto &renderPassData : commandBuffer.renderPasses)
  {
    for (auto &view : renderPassData.views)
    {
      if (view.view != VK_NULL_HANDLE)
      {
        vkDestroyImageView(device, view.view, nullptr);
        view.view = VK_NULL_HANDLE;
      }
    }

    if (renderPassData.frameBuffer != VK_NULL_HANDLE)
    {
      vkDestroyFramebuffer(device, renderPassData.frameBuffer, nullptr);
      renderPassData.frameBuffer = VK_NULL_HANDLE;
    }
  }

  commandBuffer.renderPasses.clear();
  commandBuffer.hasGraphicsPipeline = false;
  commandBuffer.hascomputePipeline = false;
  commandBuffer.boundGraphicsPipeline = GraphicsPipeline{.name = ""};
  commandBuffer.boundComputePipeline = ComputePipeline{.name = ""};
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

static VkCompareOp toVkCompareOp(ComparisonOp op)
{
  switch (op)
  {
  case ComparisonOp::ComparisonOp_GreaterOrEqual:
    return VK_COMPARE_OP_GREATER_OR_EQUAL;
  case ComparisonOp::ComparisonOp_LessOrEqual:
    return VK_COMPARE_OP_LESS_OR_EQUAL;
  case ComparisonOp::ComparisonOp_Always:
    return VK_COMPARE_OP_ALWAYS;
  case ComparisonOp::ComparisonOp_Equal:
    return VK_COMPARE_OP_EQUAL;
  default:
    return VK_COMPARE_OP_ALWAYS;
  }
}

static VkStencilOp toVkStencilOp(StencilOp op)
{
  switch (op)
  {
  case StencilOp::Keep:
    return VK_STENCIL_OP_KEEP;
  case StencilOp::Zero:
    return VK_STENCIL_OP_ZERO;
  case StencilOp::Replace:
    return VK_STENCIL_OP_REPLACE;
  default:
    return VK_STENCIL_OP_KEEP;
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

static bool shouldIgnore(const VkDebugUtilsMessengerCallbackDataEXT *data)
{
  if (!data || !data->pMessage)
    return false;

  if (data->pMessageIdName && strcmp(data->pMessageIdName, "VUID-StandaloneSpirv-None-10684") == 0)
    return true;

  return false;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *)
{
  if (shouldIgnore(data))
  {
    return VK_FALSE;
  }

  const char *sevStr = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)     ? "ERROR"
                       : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN"
                       : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)    ? "INFO"
                                                                                      : "VERBOSE";

  const char *typeStr = (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) ? "VALIDATION" : (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) ? "PERFORMANCE" : "GENERAL";

  char buffer[4096];
  snprintf(buffer, sizeof(buffer), "[Vulkan][%s][%s] %s", sevStr, typeStr, data->pMessage);

  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
  {
    os::Logger::error(buffer);
    exit(1);
  }
  else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
  {
    os::Logger::warning(buffer);
  }
  else
  {
    os::Logger::log(buffer);
  }

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
  VkDescriptorType type;

  switch (entry.type)
  {
  case BufferBindingType_UniformBuffer:
    type = entry.isDynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

    break;
  case BufferBindingType_StorageBuffer:
    type = entry.isDynamic ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    break;
  }

  binding.descriptorType = type;
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
    throw std::runtime_error("No Vulkan physical devices found.");

  std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
  vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());

  std::vector<VulkanPhysicalDevice> matchingDevices;

  for (VkPhysicalDevice device : physicalDevices)
  {
    /* --------------------------------------------------------------------- */
    /* Properties                                                            */
    /* --------------------------------------------------------------------- */

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device, &memProps);

    VkPhysicalDeviceSubgroupProperties subgroupProps{};
    subgroupProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;

    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &subgroupProps;
    vkGetPhysicalDeviceProperties2(device, &props2);

    /* --------------------------------------------------------------------- */
    /* Features (Vulkan 1.2 ONLY)                                             */
    /* --------------------------------------------------------------------- */

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;

    vkGetPhysicalDeviceFeatures2(device, &features2);

    const VkPhysicalDeviceFeatures &features = features2.features;

    /* --------------------------------------------------------------------- */
    /* Memory                                                                */
    /* --------------------------------------------------------------------- */

    size_t totalMemory = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i)
    {
      if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        totalMemory += memProps.memoryHeaps[i].size;
    }

    /* --------------------------------------------------------------------- */
    /* Feature flags                                                         */
    /* --------------------------------------------------------------------- */

    DeviceFeatures featureFlags = DeviceFeatures_None;

    featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Atomic32_AllOps);

    // Subgroups
    bool hasSubgroupCompute = (subgroupProps.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) && (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT);

    if (features12.drawIndirectCount)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_DrawIndirectCount);

    if (hasSubgroupCompute)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Subgroup_Basic);

    if (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Subgroup_Vote);

    if (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Subgroup_Arithmetic);

    if (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Subgroup_Ballot);

    if (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Subgroup_Shuffle);

    if (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Subgroup_ShuffleRelative);

    // Atomics (Vulkan 1.2)
    if (features12.shaderBufferInt64Atomics)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Atomic64_MinMax);

    if (features12.shaderSharedInt64Atomics)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Atomic64_AllOps);

    if (features.drawIndirectFirstInstance)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_DrawIndirectFirstInstance);

    if (features.multiDrawIndirect)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_MultiDrawIndirect);

    if (features.geometryShader)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_GeometryShader);

    if (features.fragmentStoresAndAtomics)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_FragmentStoresAndAtomics);

    // Device type
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Integrated);
    else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Dedicated);

    /* --------------------------------------------------------------------- */
    /* Queues                                                                */
    /* --------------------------------------------------------------------- */

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queues(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queues.data());

    bool hasCompute = false;
    bool hasGraphics = false;
    bool hasTimestamp = false;

    for (const auto &q : queues)
    {
      if (q.queueFlags & VK_QUEUE_COMPUTE_BIT)
        hasCompute = true;
      if (q.queueFlags & VK_QUEUE_GRAPHICS_BIT)
        hasGraphics = true;
      if (q.timestampValidBits > 0)
        hasTimestamp = true;
    }

    if (hasCompute)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Compute);
    if (hasGraphics)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Graphics);
    if (hasTimestamp)
      featureFlags = (DeviceFeatures)(featureFlags | DeviceFeatures_Timestamp);

    /* --------------------------------------------------------------------- */
    /* Limits                                                                */
    /* --------------------------------------------------------------------- */

    if (!features12.timelineSemaphore)
      continue;

    DeviceProperties dprops{};
    dprops.sugroupSize = subgroupProps.subgroupSize;
    dprops.maxMemory = totalMemory;
    dprops.maxComputeSharedMemorySize = props.limits.maxComputeSharedMemorySize;
    dprops.maxComputeWorkGroupInvocations = props.limits.maxComputeWorkGroupInvocations;
    dprops.uniformBufferAlignment = props.limits.minUniformBufferOffsetAlignment;

    if (dprops.maxMemory >= requiredLimits.minimumMemory && dprops.maxComputeSharedMemorySize >= requiredLimits.minimumComputeSharedMemory &&
        dprops.maxComputeWorkGroupInvocations >= requiredLimits.minimumComputeWorkGroupInvocations)
    {
      matchingDevices.push_back({
        .device = device,
        .feature_flags = featureFlags,
        .properties = dprops,
      });
    }
  }

  /* ----------------------------------------------------------------------- */
  /* Sort                                                                    */
  /* ----------------------------------------------------------------------- */

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

VkResult createDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDebugUtilsMessengerEXT *pDebugMessenger)
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
  if (!validationLayersEnabled)
    return;

  VkDebugUtilsMessengerCreateInfoEXT createInfo;
  populateDebugMessengerCreateInfo(createInfo);

  if (createDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to set up debug messenger!");
  }
}

VulkanRHI::VulkanRHI(
    VulkanVersion version,
    DeviceRequiredLimits requiredLimits,
    DeviceFeatures requestedFeatures,
    std::vector<std::string> extensions,
    bool enableValidationLayers)
    : RHI(), eventLoop(VulkanAsyncHandler::getStatus)
{
  this->version = version;
  this->requiredLimits = requiredLimits;
  this->requestedFeaturesFlags = requestedFeatures;
  this->validationLayersEnabled = enableValidationLayers;

  instanceExtensions.push_back(strdup(VK_KHR_SURFACE_EXTENSION_NAME));
  instanceExtensions.push_back(strdup(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME));
  if (validationLayersEnabled)
  {
    instanceExtensions.push_back(strdup(VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
  }

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
  if (device != VK_NULL_HANDLE)
  {
    waitIdle();
    destroyRetiredSwapChains();
  }
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
    os::Logger::logf("Vulkan Layer available: %s", layer.layerName);
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
  if (validationLayersEnabled && !checkValidationLayerSupport())
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

#if defined(__APPLE__)
  const char *VK_EXT_METAL_SURFACE_EXTENSION_NAME = "VK_EXT_metal_surface";
  const char *VK_MVK_MACOS_SURFACE_EXTENSION_NAME = "VK_MVK_macos_surface";

  instanceExtensions.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
  instanceExtensions.push_back(VK_MVK_MACOS_SURFACE_EXTENSION_NAME);
#endif

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
  createInfo.ppEnabledExtensionNames = instanceExtensions.data();

  VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};

  if (validationLayersEnabled)
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
    throw std::runtime_error("Missing required graphics queue family");

  if ((features & DeviceFeatures_Compute) && !indices.hasComputeFamily)
    throw std::runtime_error("Missing required compute queue family");

  /* ------------------------------------------------------------
   * Queue creation
   * ------------------------------------------------------------ */

  std::set<uint32_t> uniqueFamilies;

  if (indices.hasGraphicsFamily)
    uniqueFamilies.insert(indices.graphicsFamily);

  if (indices.hasComputeFamily)
    uniqueFamilies.insert(indices.computeFamily);

  if (indices.hasTransferFamily)
    uniqueFamilies.insert(indices.transferFamily);

  for (auto &surface : surfaces)
  {
    if (surface.hasPresentFamily)
      uniqueFamilies.insert(surface.presentFamily);
  }

  std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
  std::vector<float> queuePriorities(uniqueFamilies.size(), 1.0f);

  uint32_t priorityIndex = 0;
  for (uint32_t family : uniqueFamilies)
  {
    VkDeviceQueueCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    info.queueFamilyIndex = family;
    info.queueCount = 1;
    info.pQueuePriorities = &queuePriorities[priorityIndex++];

    queueCreateInfos.push_back(info);
  }

  /* ------------------------------------------------------------
   * Core (1.0) features
   * ------------------------------------------------------------ */

  VkPhysicalDeviceFeatures coreFeatures{};
  coreFeatures.samplerAnisotropy = VK_TRUE;
  coreFeatures.multiDrawIndirect = (features & DeviceFeatures_MultiDrawIndirect) ? VK_TRUE : VK_FALSE;
  coreFeatures.drawIndirectFirstInstance = (features & DeviceFeatures_DrawIndirectFirstInstance) ? VK_TRUE : VK_FALSE;
  coreFeatures.fragmentStoresAndAtomics = (features & DeviceFeatures_FragmentStoresAndAtomics) ? VK_TRUE : VK_FALSE;

  /* ------------------------------------------------------------
   * Vulkan 1.2 features (NO extension feature structs!)
   * ------------------------------------------------------------ */

  VkPhysicalDeviceVulkan12Features features12{};
  features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

  features12.timelineSemaphore = VK_TRUE;

  features12.drawIndirectCount = (features & DeviceFeatures_DrawIndirectCount) ? VK_TRUE : VK_FALSE;

  /* ------------------------------------------------------------
   * Feature chain
   * ------------------------------------------------------------ */

  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.features = coreFeatures;
  features2.pNext = &features12;

  /* ------------------------------------------------------------
   * Device extensions
   * ------------------------------------------------------------ */

  uint32_t extensionCount = 0;
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);

  std::vector<VkExtensionProperties> availableExtensions(extensionCount);
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, availableExtensions.data());

  deviceExtensions.clear();

  bool hasPortabilitySubset = false;
  const char *VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME = "VK_KHR_portability_subset";
  for (const auto &ext : availableExtensions)
  {
    if (strcmp(ext.extensionName, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME) == 0)
    {
      hasPortabilitySubset = true;
    }
  }

  if (hasPortabilitySubset)
    deviceExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);

  deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

  /* ------------------------------------------------------------
   * Device creation
   * ------------------------------------------------------------ */

  VkDeviceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
  createInfo.pQueueCreateInfos = queueCreateInfos.data();
  createInfo.pNext = &features2;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
  createInfo.ppEnabledExtensionNames = deviceExtensions.data();

  VkDevice device = VK_NULL_HANDLE;
  if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS)
    throw std::runtime_error("Failed to create logical device");

  /* ------------------------------------------------------------
   * Queue retrieval
   * ------------------------------------------------------------ */

  uint32_t graphicsIndex = 0;
  uint32_t computeIndex = 0;
  uint32_t transferIndex = 0;
  uint32_t presentIndex = 0;

  for (const auto &info : queueCreateInfos)
  {
    VkQueue queue = VK_NULL_HANDLE;

    uint32_t index = 0;

    if (info.queueFamilyIndex == indices.graphicsFamily)
      index = graphicsIndex++;

    if (info.queueFamilyIndex == indices.computeFamily)
      index = computeIndex++;

    if (info.queueFamilyIndex == indices.transferFamily)
      index = transferIndex++;

    for (auto &surface : surfaces)
    {
      if (info.queueFamilyIndex == surface.presentFamily)
      {
        index = presentIndex++;
        break;
      }
    }

    vkGetDeviceQueue(device, info.queueFamilyIndex, index, &queue);

    if (info.queueFamilyIndex == indices.graphicsFamily)
      graphicsQueue.push_back(queue);

    if (info.queueFamilyIndex == indices.computeFamily)
      computeQueue.push_back(queue);

    if (info.queueFamilyIndex == indices.transferFamily)
      transferQueue.push_back(queue);

    for (auto &surface : surfaces)
    {
      if (info.queueFamilyIndex == surface.presentFamily)
        surface.presentQueue = queue;
    }
  }

  this->device = device;
}

BufferId VulkanRHI::allocateBuffer(const BufferInfo &info)
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
  vkBuf->sharingMode = bufferInfo.sharingMode;

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

  // if (vkBuf->memoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
  // {
  //   vkMapMemory(device, vkBuf->memory, 0, info.size, 0, &vkBuf->mapped);
  // }

  return makeHandle<BufferId>(vkBuf);
}

void VulkanRHI::releaseBuffer(BufferId id)
{
  if (id == BufferId::Invalid)
    return;
  auto *buf = getHandlePtr<VulkanBuffer>(id, "VulkanBuffer");

  if (buf->mapped)
  {
    vkUnmapMemory(device, buf->memory);
    buf->mapped = nullptr;
  }

  if (buf->buffer != VK_NULL_HANDLE)
  {
    vkDestroyBuffer(device, buf->buffer, nullptr);
    buf->buffer = VK_NULL_HANDLE;
  }

  if (buf->memory != VK_NULL_HANDLE)
  {
    vkFreeMemory(device, buf->memory, nullptr);
    buf->memory = VK_NULL_HANDLE;
  }

  delete buf;
}

TextureId VulkanRHI::allocateTexture(const TextureInfo &info)
{
  VulkanTexture *tex = new VulkanTexture();
  tex->id = makeHandle<TextureId>(tex);
  tex->info = info;
  tex->format = toVkFormat(info.format);
  tex->extent = {info.width, info.height, std::max(1u, info.depth)};
  tex->mipLevels = std::max(1u, info.mipLevels);
  tex->usageFlags = toVkImageUsageFlags(info.usage);
  tex->memoryFlags = toVkMemoryPropertyFlags(info.memoryProperties, false);
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = info.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  imageInfo.extent = tex->extent;
  imageInfo.mipLevels = tex->mipLevels;
  imageInfo.arrayLayers = std::max(1u, info.arrayLayers);
  imageInfo.format = tex->format;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = tex->usageFlags;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  tex->sharingMode = imageInfo.sharingMode;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.flags = 0;

  if (vkCreateImage(device, &imageInfo, nullptr, &tex->image) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create Vulkan image!");
  }

  os::Logger::warningf("[VulkanRHI] allocating texture %s as image %p, width=%u height=%u, mips=%u", info.name.c_str(), tex->image, info.width, info.height, info.mipLevels);

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
  return tex->id;
}

void VulkanRHI::releaseTexture(TextureId id)
{
  if (id == TextureId::Invalid)
    return;
  auto *vkTex = getHandlePtr<VulkanTexture>(id, "VulkanTexture");

  if (vkTex->image != VK_NULL_HANDLE)
  {
    vkDestroyImage(device, vkTex->image, nullptr);
    vkTex->image = VK_NULL_HANDLE;
  }

  if (vkTex->memory != VK_NULL_HANDLE)
  {
    vkFreeMemory(device, vkTex->memory, nullptr);
    vkTex->memory = VK_NULL_HANDLE;
  }

  delete vkTex;
}

VulkanTextureView VulkanRHI::createTextureView(const TextureView &view)
{
  VulkanTextureView vkView{};
  const VulkanTexture &tex = getVulkanTexture(view.resourceId);

  vkView.image = tex.image;
  vkView.format = tex.format;
  vkView.viewType = tex.extent.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : view.layerCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;

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

SamplerId VulkanRHI::allocateSampler(const SamplerInfo &info)
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

  return makeHandle<SamplerId>(vkSampler);
}

void VulkanRHI::releaseSampler(SamplerId id)
{
  if (id == SamplerId::Invalid)
    return;
  auto *sampler = getHandlePtr<VulkanSampler>(id, "VulkanSampler");
  if (sampler->sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(device, sampler->sampler, nullptr);
    sampler->sampler = VK_NULL_HANDLE;
  }

  delete sampler;
}

BindingsLayoutId VulkanRHI::allocateBindingsLayout(const BindingsLayoutInfo &info)
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

  return makeHandle<BindingsLayoutId>(vkLayout);
}

void VulkanRHI::releaseBindingsLayout(BindingsLayoutId id)
{
  if (id == BindingsLayoutId::Invalid)
    return;
  auto *layout = getHandlePtr<VulkanBindingsLayout>(id, "VulkanBindingsLayout");
  for (VkDescriptorSetLayout setLayout : layout->setLayouts)
  {
    if (setLayout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
  }

  if (layout->pipelineLayout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout(device, layout->pipelineLayout, nullptr);
  }
  delete layout;
}

BindingGroupsId VulkanRHI::allocateBindings(const BindingGroupsInfo &groups, const VulkanBindingsLayout &layout)
{
  auto *resultGroups = new VulkanBindingGroups();
  resultGroups->info = groups;

  resultGroups->groups.reserve(groups.groups.size());

  for (size_t groupIndex = 0; groupIndex < groups.groups.size(); ++groupIndex)
  {
    const GroupInfo &groupInfo = groups.groups[groupIndex];
    const auto &groupLayout = layout.groups[groupIndex];

    VulkanBindingGroup vkGroup{};
    vkGroup.info = groupInfo;

    /* -----------------------------------------------------------
     * Descriptor pool sizing
     * ----------------------------------------------------------- */

    lib::FlatMap<VkDescriptorType, uint32_t> descriptorCounts;

    for (const auto &b : groupLayout.buffers)
    {
      VkDescriptorType type = b.type == BufferBindingType_UniformBuffer ? (b.isDynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                                                                        : (b.isDynamic ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

      descriptorCounts.upsert(
          type,
          [](uint32_t &v)
          {
            ++v;
          },
          []
          {
            return uint32_t{1};
          });
    }

    if (!groupInfo.samplers.empty())
    {
      const uint32_t n = static_cast<uint32_t>(groupInfo.samplers.size());
      descriptorCounts.upsert(
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          [n](uint32_t &v)
          {
            v += n;
          },
          [n]
          {
            return n;
          });
    }

    if (!groupInfo.textures.empty())
    {
      const uint32_t n = static_cast<uint32_t>(groupInfo.textures.size());
      descriptorCounts.upsert(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          [n](uint32_t &v)
          {
            v += n;
          },
          [n]
          {
            return n;
          });
    }

    if (!groupInfo.storageTextures.empty())
    {
      const uint32_t n = static_cast<uint32_t>(groupInfo.storageTextures.size());
      descriptorCounts.upsert(
          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          [n](uint32_t &v)
          {
            v += n;
          },
          [n]
          {
            return n;
          });
    }

    std::vector<VkDescriptorPoolSize> poolSizes;
    poolSizes.reserve(descriptorCounts.size());

    descriptorCounts.forEach(
        [&](VkDescriptorType type, uint32_t count)
        {
          poolSizes.push_back({type, count});
        });

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &vkGroup.descriptorPool) != VK_SUCCESS)
    {
      throw std::runtime_error("Failed to create descriptor pool");
    }

    /* -----------------------------------------------------------
     * Allocate descriptor set
     * ----------------------------------------------------------- */

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = vkGroup.descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout.setLayouts[groupIndex];

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet) != VK_SUCCESS)
    {
      throw std::runtime_error("Failed to allocate descriptor set");
    }

    vkGroup.descriptorSets.push_back(descriptorSet);

    /* -----------------------------------------------------------
     * Descriptor write storage (must outlive vkUpdateDescriptorSets)
     * ----------------------------------------------------------- */

    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(groupInfo.buffers.size() + groupInfo.samplers.size() + groupInfo.textures.size() + groupInfo.storageTextures.size());

    std::vector<VkDescriptorBufferInfo> bufferInfos;
    bufferInfos.reserve(groupInfo.buffers.size());

    std::vector<VkDescriptorImageInfo> imageInfos;
    imageInfos.reserve(groupInfo.samplers.size() + groupInfo.textures.size() + groupInfo.storageTextures.size());

    /* -----------------------------------------------------------
     * Buffers
     * ----------------------------------------------------------- */

    assert(groupLayout.buffers.size() == groupInfo.buffers.size());

    for (size_t i = 0; i < groupInfo.buffers.size(); ++i)
    {
      const BindingBuffer &binding = groupInfo.buffers[i];
      const auto &layoutBinding = groupLayout.buffers[i];

      VulkanBuffer buf = getVulkanBuffer(binding.bufferView.resourceId);

      bufferInfos.push_back({
        .buffer = buf.buffer,
        .offset = binding.bufferView.offset,
        .range = binding.bufferView.size,
      });

      VkDescriptorType type = layoutBinding.type == BufferBindingType_UniformBuffer ? (layoutBinding.isDynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                                                                                    : (layoutBinding.isDynamic ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptorSet;
      write.dstBinding = binding.binding;
      write.descriptorCount = 1;
      write.descriptorType = type;
      write.pBufferInfo = &bufferInfos.back();

      writes.push_back(write);
    }

    /* -----------------------------------------------------------
     * Samplers
     * ----------------------------------------------------------- */

    for (const auto &binding : groupInfo.samplers)
    {
      const VulkanSampler &sampler = getVulkanSampler(binding.sampler.id);

      VulkanTextureView view = createTextureView(binding.view);

      vkGroup.textureViews.push_back(view);

      imageInfos.push_back({
        .sampler = sampler.sampler,
        .imageView = view.view,
        .imageLayout = toVkImageLayout(binding.view.layout),
      });

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptorSet;
      write.dstBinding = binding.binding;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.pImageInfo = &imageInfos.back();

      writes.push_back(write);
    }

    /* -----------------------------------------------------------
     * Sampled textures
     * ----------------------------------------------------------- */

    for (const auto &binding : groupInfo.textures)
    {
      VulkanTextureView view = createTextureView(binding.textureView);

      vkGroup.textureViews.push_back(view);

      imageInfos.push_back({
        .sampler = VK_NULL_HANDLE,
        .imageView = view.view,
        .imageLayout = toVkImageLayout(binding.textureView.layout),
      });

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptorSet;
      write.dstBinding = binding.binding;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &imageInfos.back();

      writes.push_back(write);
    }

    /* -----------------------------------------------------------
     * Storage textures
     * ----------------------------------------------------------- */

    for (const auto &binding : groupInfo.storageTextures)
    {
      VulkanTextureView view = createTextureView(binding.textureView);

      vkGroup.textureViews.push_back(view);

      imageInfos.push_back({
        .sampler = VK_NULL_HANDLE,
        .imageView = view.view,
        .imageLayout = toVkImageLayout(binding.textureView.layout),
      });

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptorSet;
      write.dstBinding = binding.binding;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      write.pImageInfo = &imageInfos.back();

      writes.push_back(write);
    }

    /* -----------------------------------------------------------
     * Update descriptors (safe)
     * ----------------------------------------------------------- */

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    resultGroups->groups.push_back(vkGroup);
  }

  return makeHandle<BindingGroupsId>(resultGroups);
}

void VulkanRHI::releaseBindingGroup(BindingGroupsId id)
{
  if (id == BindingGroupsId::Invalid)
    return;
  auto *groups = getHandlePtr<VulkanBindingGroups>(id, "VulkanBindingGroups");
  for (auto &group : groups->groups)
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

  delete groups;
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

  os::Logger::logf("Swap chain extent = %u %u, width=%u, height=%u", extent.width, extent.height, width, height);

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

  auto *swapChainImp = new VulkanSwapChain{
    .swapChain = VK_NULL_HANDLE,
    .width = extent.width,
    .height = extent.height,
  };
  const SwapChain handle = makeHandle<SwapChain>(swapChainImp);

  if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &(swapChainImp->swapChain)) != VK_SUCCESS)
  {
    delete swapChainImp;
    throw std::runtime_error("failed to create swap chain!");
  }

  vkGetSwapchainImagesKHR(device, swapChainImp->swapChain, &imageCount, nullptr);
  std::vector<VkImage> images;
  std::vector<VkImageView> imagesViews;

  if (swapChainImp->swapChainImageViews.size() == 0)
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
    os::Logger::warningf("[VulkanRHI] creating swap chain image %u as image %p", i, images[i]);

    if (vkCreateImageView(device, &createInfo, nullptr, &imagesViews[i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create image views!");
    }
  }

  for (int i = 0; i < images.size(); i++)
  {
    VulkanTextureView *view = new VulkanTextureView();
    view->image = images[i];
    view->format = surfaceFormat.format;

    VulkanTexture *texture = new VulkanTexture();

    texture->image = images[i];
    texture->format = surfaceFormat.format;
    texture->info.depth = 1;
    texture->info.memoryProperties = BufferUsage::BufferUsage_None;
    texture->info.mipLevels = 1;
    texture->id = makeHandle<TextureId>(texture);
    texture->info.name = "_SwapChainImage[" + std::to_string(static_cast<uint64_t>(static_cast<uintptr_t>(handle))) + "," + std::to_string(i) + "].texture";
    texture->info.height = extent.height;
    texture->info.width = extent.width;
    texture->sharingMode = createInfo.imageSharingMode;
    texture->swapChainOwner = swapChainImp;
    texture->swapChainImageIndex = static_cast<uint32_t>(i);

    // view->fence = VK_NULL_HANDLE;
    // view->achireSemaphore = VK_NULL_HANDLE;
    // view->presentSemaphore = VK_NULL_HANDLE;

    view->view = imagesViews[i];
    // view->renderData.swapChain = swapChainImp;
    // view->renderData.swapChainImageIndex = i;
    // TextureViewInfo info;
    // info.name = "SwapChainImage";
    // info.flags = ImageAspectFlags::Color;
    swapChainImp->swapChainImages.push_back(texture);
    swapChainImp->swapChainImageViews.push_back(view);
  }

  swapChainImp->swapChainImageFormat = surfaceFormat.format;
  swapChainImp->swapChainExtent = extent;
  swapChainImp->support = swapChainSupport;
  swapChainImp->presentQueue = surfaceImp.presentQueue;
  swapChainImp->acquireSemaphores.resize(images.size(), VK_NULL_HANDLE);
  swapChainImp->acquireSemaphorePending.resize(images.size(), false);
  swapChainImp->presentSemaphores.resize(images.size(), VK_NULL_HANDLE);
  swapChainImp->overlayCommandPools.resize(images.size(), VK_NULL_HANDLE);
  swapChainImp->overlayCommandBuffers.resize(images.size(), VK_NULL_HANDLE);
  swapChainImp->overlayFences.resize(images.size(), VK_NULL_HANDLE);

  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkCommandBufferAllocateInfo cmdAllocInfo{};
  cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAllocInfo.commandBufferCount = 1u;

  for (size_t i = 0; i < images.size(); ++i)
  {
    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &swapChainImp->acquireSemaphores[i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create swapchain acquire semaphore");
    }

    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &swapChainImp->presentSemaphores[i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create swapchain present semaphore");
    }

    VkCommandPool overlayCommandPool = allocateCommandPool(indices.graphicsFamily).commandPool;
    swapChainImp->overlayCommandPools[i] = overlayCommandPool;
    cmdAllocInfo.commandPool = overlayCommandPool;
    if (vkAllocateCommandBuffers(device, &cmdAllocInfo, &swapChainImp->overlayCommandBuffers[i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to allocate swapchain overlay command buffer");
    }

    swapChainImp->overlayFences[i] = createFence(device, true);
  }

  return handle;
}

void VulkanRHI::destroySwapChainImmediate(VulkanSwapChain *swapChainImp)
{
  for (auto &imageView : swapChainImp->swapChainImageViews)
  {
    vkDestroyImageView(device, imageView->view, nullptr);
    delete imageView;
  }

  for (auto *texture : swapChainImp->swapChainImages)
  {
    delete texture;
  }

  for (VkSemaphore semaphore : swapChainImp->acquireSemaphores)
  {
    if (semaphore != VK_NULL_HANDLE)
    {
      vkDestroySemaphore(device, semaphore, nullptr);
    }
  }

  for (VkSemaphore semaphore : swapChainImp->presentSemaphores)
  {
    if (semaphore != VK_NULL_HANDLE)
    {
      vkDestroySemaphore(device, semaphore, nullptr);
    }
  }

  for (VkFence fence : swapChainImp->overlayFences)
  {
    if (fence != VK_NULL_HANDLE)
    {
      vkDestroyFence(device, fence, nullptr);
    }
  }

  for (size_t i = 0; i < swapChainImp->overlayCommandPools.size(); ++i)
  {
    if (swapChainImp->overlayCommandBuffers[i] != VK_NULL_HANDLE && swapChainImp->overlayCommandPools[i] != VK_NULL_HANDLE)
    {
      vkFreeCommandBuffers(device, swapChainImp->overlayCommandPools[i], 1u, &swapChainImp->overlayCommandBuffers[i]);
    }
    if (swapChainImp->overlayCommandPools[i] != VK_NULL_HANDLE)
    {
      vkDestroyCommandPool(device, swapChainImp->overlayCommandPools[i], nullptr);
    }
  }

  if (swapChainImp->swapChain != VK_NULL_HANDLE)
  {
    vkDestroySwapchainKHR(device, swapChainImp->swapChain, nullptr);
  }

  delete swapChainImp;
}

void VulkanRHI::destroyRetiredSwapChains()
{
  for (VulkanSwapChain *swapChain : retiredSwapChains)
  {
    if (swapChain != nullptr)
    {
      destroySwapChainImmediate(swapChain);
    }
  }

  retiredSwapChains.clear();
}

void VulkanRHI::tryReleaseDeferredResources()
{
  eventLoop.tick();

  for (VkFence fence : deferredFenceDeletes)
  {
    vkDestroyFence(device, fence, nullptr);
  }
  deferredFenceDeletes.clear();

  for (VkSemaphore semaphore : deferredIdleSemaphoreDeletes)
  {
    vkDestroySemaphore(device, semaphore, nullptr);
  }
  deferredIdleSemaphoreDeletes.clear();

  if (!eventLoop.empty())
  {
    return;
  }

  if (retiredSwapChains.empty())
  {
    return;
  }

  std::vector<VkQueue> waitedQueues;
  waitedQueues.reserve(retiredSwapChains.size() * 2u);

  auto waitQueueOnce = [&](VkQueue queue)
  {
    if (queue == VK_NULL_HANDLE)
    {
      return;
    }
    if (std::find(waitedQueues.begin(), waitedQueues.end(), queue) != waitedQueues.end())
    {
      return;
    }
    vkQueueWaitIdle(queue);
    waitedQueues.push_back(queue);
  };

  waitQueueOnce(getQueueHandle(Graphics));
  for (VulkanSwapChain *swapChain : retiredSwapChains)
  {
    if (swapChain != nullptr)
    {
      waitQueueOnce(swapChain->presentQueue);
    }
  }

  destroyRetiredSwapChains();
}

void VulkanRHI::destroySwapChain(SwapChain swapChain)
{
  auto *swapChainImp = getSwapChainHandle(swapChain);
  retiredSwapChains.push_back(swapChainImp);
  tryReleaseDeferredResources();
}

Format VulkanRHI::getSwapChainFormat(SwapChain handle)
{
  return vkFormatToFormat(getSwapChainHandle(handle)->swapChainImageFormat);
}

VkFormat VulkanRHI::getSwapChainVkFormat(SwapChain handle)
{
  return getSwapChainHandle(handle)->swapChainImageFormat;
}

const uint32_t VulkanRHI::getSwapChainImagesCount(SwapChain swapChainHandle)
{
  auto *swapChain = getSwapChainHandle(swapChainHandle);
  return swapChain->swapChainImageViews.size();
}

const TextureView VulkanRHI::getSwapChainTextureView(SwapChain swapChainHandle, uint32_t imageIndex)
{
  auto *swapChain = getSwapChainHandle(swapChainHandle);
  if (imageIndex >= swapChain->swapChainImages.size())
  {
    throw std::out_of_range("Swapchain image index out of range");
  }

  TextureView view;
  view.access = AccessPattern::NONE;
  view.layout = ResourceLayout::PRESENT_SRC;
  view.baseArrayLayer = 0;
  view.baseMipLevel = 0;
  view.layerCount = 1;
  view.levelCount = 1;
  view.flags = ImageAspectFlags::Color;
  view.swapChain = swapChainHandle;
  view.index = imageIndex;
  view.texture.name = "_SwapChainImage[" + std::to_string(static_cast<uint64_t>(static_cast<uintptr_t>(swapChainHandle))) + "," + std::to_string(imageIndex) + "].texture";
  view.resourceId = swapChain->swapChainImages[imageIndex]->id;
  return view;
}

const TextureView VulkanRHI::getCurrentSwapChainTextureView(SwapChain swapChainHandle)
{
  auto *swapChain = getSwapChainHandle(swapChainHandle);

  uint32_t index = UINT32_MAX;
  const uint32_t semaphoreIndex = swapChain->nextAcquireSemaphoreIndex % std::max<uint32_t>(1u, static_cast<uint32_t>(swapChain->acquireSemaphores.size()));
  VkSemaphore acquireSemaphore = swapChain->acquireSemaphores[semaphoreIndex];

  if (vkAcquireNextImageKHR(device, swapChain->swapChain, UINT64_MAX, acquireSemaphore, VK_NULL_HANDLE, &index) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to achire next image, you probably did not submit the commands");
  }
  swapChain->currentAcquiredImageIndex = index;
  swapChain->currentAcquiredSemaphoreIndex = semaphoreIndex;
  swapChain->acquireSemaphorePending[semaphoreIndex] = true;
  swapChain->nextAcquireSemaphoreIndex = (semaphoreIndex + 1u) % std::max<uint32_t>(1u, static_cast<uint32_t>(swapChain->acquireSemaphores.size()));
  return getSwapChainTextureView(swapChainHandle, index);
}

const uint32_t VulkanRHI::getSwapChainImagesWidth(SwapChain swapChainHandle)
{
  return getSwapChainHandle(swapChainHandle)->width;
}

const uint32_t VulkanRHI::getSwapChainImagesHeight(SwapChain swapChainHandle)
{
  return getSwapChainHandle(swapChainHandle)->height;
}

const VulkanTexture &VulkanRHI::getVulkanTexture(TextureId id)
{
  return *getHandlePtr<VulkanTexture>(id, "VulkanTexture");
}

const VulkanSampler &VulkanRHI::getVulkanSampler(SamplerId id)
{
  return *getHandlePtr<VulkanSampler>(id, "VulkanSampler");
}

const VulkanBuffer &VulkanRHI::getVulkanBuffer(BufferId id)
{
  return *getHandlePtr<VulkanBuffer>(id, "VulkanBuffer");
}

const VulkanBindingsLayout &VulkanRHI::getVulkanBindingsLayout(BindingsLayoutId id)
{
  return *getHandlePtr<VulkanBindingsLayout>(id, "VulkanBindingsLayout");
}

const VulkanBindingGroups &VulkanRHI::getVulkanBindingGroups(BindingGroupsId id)
{
  return *getHandlePtr<VulkanBindingGroups>(id, "VulkanBindingGroups");
}

const VulkanGraphicsPipeline &VulkanRHI::getVulkanGraphicsPipeline(GraphicsPipelineId id)
{
  return *getHandlePtr<VulkanGraphicsPipeline>(id, "VulkanGraphicsPipeline");
}

const VulkanComputePipeline &VulkanRHI::getVulkanComputePipeline(ComputePipelineId id)
{
  return *getHandlePtr<VulkanComputePipeline>(id, "VulkanComputePipeline");
}

void VulkanRHI::bufferRead(BufferId bufferId, const uint64_t offset, const uint64_t size, std::function<void(const void *)> callback)
{
  std::lock_guard<std::mutex> lock(hostAccessMutex_);
  const VulkanBuffer &heap = getVulkanBuffer(bufferId);
  void *ptr;
  vkMapMemory(device, heap.memory, offset, size, 0, &ptr);
  callback(ptr);
  vkUnmapMemory(device, heap.memory);
}

void VulkanRHI::bufferWrite(BufferId bufferId, const uint64_t offset, const uint64_t size, void *data)
{
  std::lock_guard<std::mutex> lock(hostAccessMutex_);
  void *ptr;
  const VulkanBuffer &heap = getVulkanBuffer(bufferId);
  auto result = vkMapMemory(device, heap.memory, offset, size, 0, &ptr);
  if (result != VK_SUCCESS)
  {
    os::Logger::errorf("Failed to map buffer memory: VkResult = %d", result);
    return;
  }

  memcpy(ptr, data, size);
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

VkRenderPass VulkanRHI::createRenderPass(const ColorAttatchment *attachments, uint32_t attatchmentsCount, const DepthAttatchment &depth)
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
    colorAttachment.initialLayout = attachments[i].loadOp == LoadOp::LoadOp_Clear ? VK_IMAGE_LAYOUT_UNDEFINED : toVkImageLayout(attachments[i].initialLayout); // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = toVkImageLayout(attachments[i].finalLayout);                                                                                 // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    attachmentsDescriptions.push_back(colorAttachment);

    VkAttachmentReference colorRef{};
    colorRef.attachment = static_cast<uint32_t>(i);
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachmentRefs.push_back(colorRef);
  }

  // Depth attachment (optional)
  VkAttachmentReference depthAttachmentRef{};
  if (depth.enabled)
  {
    VkAttachmentDescription depthAttachment{};

    switch (depth.format)
    {
    case Format_Stencil8:
      depthAttachment.format = VK_FORMAT_S8_UINT;
      break;
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
    if (depth.format == Format_Stencil8)
    {
      depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      depthAttachment.stencilLoadOp = loadOpToVkLoadOp(depth.loadOp);
      depthAttachment.stencilStoreOp = storeOpToVkStoreOp(depth.storeOp);
    }
    else
    {
      depthAttachment.loadOp = loadOpToVkLoadOp(depth.loadOp);
      depthAttachment.storeOp = storeOpToVkStoreOp(depth.storeOp);
      depthAttachment.stencilLoadOp = loadOpToVkLoadOp(depth.loadOp);
      depthAttachment.stencilStoreOp = storeOpToVkStoreOp(depth.storeOp);
    }
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
  bool hasDepth = depth.enabled; //.format != Format_None;
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

GraphicsPipelineId VulkanRHI::allocateGraphicsPipeline(const GraphicsPipelineInfo &info)
{
#ifdef VULKAN_DEVICE_LOG
  os::Logger::logf("VulkanDevice creating (GraphicsPipeline)%s", info.name.c_str());
#endif

  std::vector<VkDynamicState> dynamicStates = {
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
  };

  VkPipelineDynamicStateCreateInfo dynamicState = {};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = dynamicStates.size();
  dynamicState.pDynamicStates = dynamicStates.data();
  dynamicState.pNext = nullptr;

  VkPipelineViewportStateCreateInfo viewportState = {};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.pNext = nullptr;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;
  viewportState.pViewports = nullptr;
  viewportState.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.pNext = nullptr;
  rasterizer.flags = 0;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.depthBiasEnable = VK_FALSE;

  switch (info.vertexStage.cullType)
  {
  case CullMode::Back:
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    break;

  case CullMode::Front:
    rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT;
    break;

  case CullMode::None:
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    break;
  }

  switch (info.vertexStage.winding)
  {
  case WindingOrder::CCW:
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    break;

  case WindingOrder::CW:
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    break;
  }
  // switch (info.vertexStage.cullType)
  // {
  // case PrimitiveCullType_CCW:
  //   rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  //   break;
  // case PrimitiveCullType_CW:
  // default:
  //   rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
  //   break;
  // }

  rasterizer.depthBiasEnable = VK_FALSE;
  rasterizer.depthBiasSlopeFactor = 1.0f;
  rasterizer.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.pNext = nullptr;
  multisampling.flags = 0;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  multisampling.sampleShadingEnable = VK_FALSE;

  const uint32_t colorAttachmentCount = static_cast<uint32_t>(info.fragmentStage.colorAttatchments.size());

  std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
  colorBlendAttachments.reserve(colorAttachmentCount);
  for (const auto &attachment : info.fragmentStage.colorAttatchments)
  {
    colorBlendAttachments.push_back(toVkColorBlendAttachmentState(attachment));
  }

  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.pNext = nullptr;
  colorBlending.flags = 0;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.attachmentCount = colorAttachmentCount;      // was hardcoded to 1
  colorBlending.pAttachments = colorBlendAttachments.data(); // was &colorBlendAttachment

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;

  std::vector<VkVertexInputAttributeDescription> attributes;
  lib::FlatMap<uint32_t, uint32_t> bindingStrideMap;

  for (int i = 0; i < info.vertexStage.vertexLayoutElements.size(); i++)
  {
    VkVertexInputAttributeDescription desc = {};
    desc.format = toVkFormat(typeToFormat(info.vertexStage.vertexLayoutElements[i].type));
    desc.binding = info.vertexStage.vertexLayoutElements[i].binding;
    desc.location = info.vertexStage.vertexLayoutElements[i].location;
    desc.offset = info.vertexStage.vertexLayoutElements[i].offset;
    attributes.push_back(desc);

    const uint32_t attributeEndOffset = desc.offset + GetVkFormatSize(desc.format);
    bindingStrideMap.upsert(
        desc.binding,
        [attributeEndOffset](uint32_t &v)
        {
          v = std::max(v, attributeEndOffset);
        },
        [attributeEndOffset]
        {
          return attributeEndOffset;
        });
  }

  std::vector<VkVertexInputBindingDescription> bindings;
  bindingStrideMap.forEach(
      [&](uint32_t bindingId, uint32_t stride)
      {
        VkVertexInputBindingDescription bindDesc = {};
        bindDesc.binding = bindingId;
        bindDesc.stride = stride;
        bindDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindings.push_back(bindDesc);
      });

  std::sort(
      bindings.begin(),
      bindings.end(),
      [](const VkVertexInputBindingDescription &a, const VkVertexInputBindingDescription &b)
      {
        return a.binding < b.binding;
      });

  VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
  vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
  vertexInputInfo.pVertexBindingDescriptions = bindings.data();
  vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
  vertexInputInfo.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.pNext = nullptr;
  inputAssembly.flags = 0;
  inputAssembly.primitiveRestartEnable = VK_FALSE;

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
  VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
  inputAssembly.primitiveRestartEnable = VK_FALSE;
  uint32_t stageCount = 0;
  if (info.vertexStage.shaderEntry.length() > 0)
  {
    vertex = getVulkanShader(info.vertexStage.vertexShader.id).shaderModule;
    if (vertex == VK_NULL_HANDLE)
      throw std::runtime_error("Invalid vertex shader!");
    stageCount += 1;
  }

  if (info.fragmentStage.shaderEntry.length() > 0)
  {
    fragment = getVulkanShader(info.fragmentStage.fragmentShader.id).shaderModule;
    if (fragment == VK_NULL_HANDLE)
      throw std::runtime_error("Invalid fragment shader!");
    stageCount += 1;
  }

  VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
  vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vertShaderStageInfo.pNext = nullptr;
  vertShaderStageInfo.flags = 0;
  vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vertShaderStageInfo.module = vertex;
  vertShaderStageInfo.pName = info.vertexStage.shaderEntry.c_str();

  VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
  fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fragShaderStageInfo.pNext = nullptr;
  fragShaderStageInfo.flags = 0;
  fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  fragShaderStageInfo.module = fragment;
  fragShaderStageInfo.pName = info.fragmentStage.shaderEntry.c_str();

  VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_FALSE;
  depthStencil.depthWriteEnable = VK_FALSE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
  depthStencil.depthBoundsTestEnable = VK_FALSE;
  depthStencil.stencilTestEnable = VK_FALSE;

  if (info.fragmentStage.depthAttatchment.enabled)
  {
    const DepthAttatchment &depthInfo = info.fragmentStage.depthAttatchment;
    depthStencil.depthTestEnable = depthInfo.depthTestEnabled ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = depthInfo.depthWriteEnabled ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = toVkCompareOp(depthInfo.comparison);

    const auto makeStencilState = [](const DepthAttatchment::StencilState &state) -> VkStencilOpState
    {
      VkStencilOpState vkState{};
      vkState.failOp = toVkStencilOp(state.failOp);
      vkState.passOp = toVkStencilOp(state.passOp);
      vkState.depthFailOp = toVkStencilOp(state.depthFailOp);
      vkState.compareOp = toVkCompareOp(state.comparison);
      vkState.compareMask = state.compareMask;
      vkState.writeMask = state.writeMask;
      vkState.reference = state.reference;
      return vkState;
    };

    depthStencil.stencilTestEnable = (depthInfo.stencilFront.enabled || depthInfo.stencilBack.enabled) ? VK_TRUE : VK_FALSE;
    depthStencil.front = makeStencilState(depthInfo.stencilFront);
    depthStencil.back = makeStencilState(depthInfo.stencilBack.enabled ? depthInfo.stencilBack : depthInfo.stencilFront);
  }

  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.stageCount = stageCount;
  pipelineInfo.pStages = shaderStages;
  pipelineInfo.pVertexInputState = &vertexInputInfo;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDynamicState = &dynamicState;

  const VulkanBindingsLayout &layout = getVulkanBindingsLayout(info.layout.id);
  pipelineInfo.layout = layout.pipelineLayout;

  VkRenderPass renderPass = createRenderPass(info.fragmentStage.colorAttatchments.data(), info.fragmentStage.colorAttatchments.size(), info.fragmentStage.depthAttatchment);

  pipelineInfo.renderPass = renderPass;

  VkPipeline pipeline = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline) != VK_SUCCESS)
    throw std::runtime_error("Failed to create graphics pipeline!");

  VulkanGraphicsPipeline *result = new VulkanGraphicsPipeline();
  result->pipeline = pipeline;
  result->renderPass = renderPass;
  result->info = info;
  result->layout = info.layout;

  return makeHandle<GraphicsPipelineId>(result);
}

void VulkanRHI::releaseGraphicsPipeline(GraphicsPipelineId id)
{
  if (id == GraphicsPipelineId::Invalid)
    return;
  auto *handle = getHandlePtr<VulkanGraphicsPipeline>(id, "VulkanGraphicsPipeline");
  vkDestroyPipeline(device, handle->pipeline, NULL);
  vkDestroyRenderPass(device, handle->renderPass, NULL);
  delete handle;
}

ComputePipelineId VulkanRHI::allocateComputePipeline(const ComputePipelineInfo &info)
{
  VkPipelineShaderStageCreateInfo computeShaderStageInfo{};
  computeShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  computeShaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;

  const VulkanShader &shader = getVulkanShader(info.shader.id);
  if (shader.shaderModule == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Invalid compute shader!");
  }
  computeShaderStageInfo.module = shader.shaderModule;
  computeShaderStageInfo.pName = info.entry;

  const VulkanBindingsLayout &layout = getVulkanBindingsLayout(info.layout.id);
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

  return makeHandle<ComputePipelineId>(result);
}

void VulkanRHI::releaseComputePipeline(ComputePipelineId id)
{
  if (id == ComputePipelineId::Invalid)
    return;
  auto *vkPipeline = getHandlePtr<VulkanComputePipeline>(id, "VulkanComputePipeline");
  if (vkPipeline->pipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(device, vkPipeline->pipeline, nullptr);
  }
  delete vkPipeline;
}

VulkanCommandPool VulkanRHI::allocateCommandPool(uint32_t queueFamilyIndex)
{
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.queueFamilyIndex = queueFamilyIndex;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // Optional

  VkCommandPool commandPool = VK_NULL_HANDLE;
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

std::vector<CommandBuffer> VulkanRHI::allocateCommandBuffers(Queue queue, uint32_t count)
{
  VkCommandBufferAllocateInfo allocInfo{};
  VulkanCommandPool commandPool;

  // TODO: reuse command pools
  switch (queue)
  {
  case Queue::Graphics:
    if (!graphicsCommandPool.dequeue(commandPool))
    {
      commandPool = allocateCommandPool(indices.graphicsFamily);
    }
    break;
  case Queue::Compute:
    if (!computeCommandPool.dequeue(commandPool))
    {
      commandPool = allocateCommandPool(indices.computeFamily);
    }
    break;
  case Queue::Transfer:
    if (!transferCommandPool.dequeue(commandPool))
    {
      commandPool = allocateCommandPool(indices.transferFamily);
    }
    break;
  default:
    throw new std::runtime_error("Unsuported queue");
    break;
  }

  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = commandPool.commandPool;
  allocInfo.level = VkCommandBufferLevel::VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = count;

  std::vector<VkCommandBuffer> cmbs(count);

  if (vkAllocateCommandBuffers(device, &allocInfo, cmbs.data()) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to allocate command buffers");
  }

  std::vector<CommandBuffer> buffers;

  for (uint32_t i = 0; i < count; i++)
  {
    auto *commandBufferData = new VulkanCommandBuffer{
      .submited = false,
      .fence = VK_NULL_HANDLE,
      .queue = queue,
      .commandBuffer = cmbs[i],
      .commandPool = commandPool,
      .hasGraphicsPipeline = false,
      .hascomputePipeline = false,
      .boundGraphicsPipeline = GraphicsPipeline{.name = ""},
      .boundComputePipeline = ComputePipeline{.name = ""},
      .renderPasses = std::vector<VulkanCommandBufferRenderPass>(),
    };

    buffers.push_back(makeHandle<CommandBuffer>(commandBufferData));
  }

  return buffers;
}

void VulkanRHI::releaseCommandBuffer(std::vector<CommandBuffer> &buffers)
{
  for (auto &buff : buffers)
  {
    auto *commandbuffer = getCommandBufferHandle(buff);

    if (commandbuffer->submited)
    {
      vkWaitForFences(device, 1, &commandbuffer->fence, VK_TRUE, UINT64_MAX);
    }

    resetRecordedRenderPassState(device, *commandbuffer);

    vkFreeCommandBuffers(device, commandbuffer->commandPool.commandPool, 1, &(commandbuffer->commandBuffer));

    switch (commandbuffer->queue)
    {
    case Queue::Graphics:
      graphicsCommandPool.enqueue(commandbuffer->commandPool);
      break;
    case Queue::Compute:
      computeCommandPool.enqueue(commandbuffer->commandPool);
      break;
    case Queue::Transfer:
      transferCommandPool.enqueue(commandbuffer->commandPool);
      break;
    default:
      throw std::runtime_error("Invalid Command Buffer");
      break;
    }

    // releaseCommandPool(commandbuffer->commandPool);

    delete commandbuffer;
    buff = CommandBuffer::Invalid;
  }
}

void VulkanRHI::beginCommandBuffer(CommandBuffer handle, bool oneTimeSubmit)
{
  auto *cmd = getCommandBufferHandle(handle);

  resetRecordedRenderPassState(device, *cmd);

  const VkResult resetResult = vkResetCommandBuffer(cmd->commandBuffer, 0u);
  if (resetResult != VK_SUCCESS)
    throw std::runtime_error("vkResetCommandBuffer failed");

  VkCommandBufferBeginInfo beginInfo{
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
  };

  beginInfo.flags = oneTimeSubmit ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT : 0u;
  beginInfo.pInheritanceInfo = nullptr;

  VkResult r = vkBeginCommandBuffer(cmd->commandBuffer, &beginInfo);
  if (r != VK_SUCCESS)
    throw std::runtime_error("vkBeginCommandBuffer failed");
}

void VulkanRHI::endCommandBuffer(CommandBuffer handle)
{
  auto *cmd = getCommandBufferHandle(handle);

  VkResult r = vkEndCommandBuffer(cmd->commandBuffer);
  if (r != VK_SUCCESS)
    throw std::runtime_error("vkEndCommandBuffer failed");
}

void VulkanRHI::cmdCopyBuffer(CommandBuffer cmdBuffer, const BufferView &src, const BufferView &dst)
{
  auto *cmd = getCommandBufferHandle(cmdBuffer);
  VkCommandBuffer vkCmd = cmd->commandBuffer;

  VkBufferCopy copyRegion{};

  copyRegion.srcOffset = src.offset;
  copyRegion.dstOffset = dst.offset;
  copyRegion.size = src.size;
  auto srcBuffer = getVulkanBuffer(src.resourceId);
  auto dstBuffer = getVulkanBuffer(dst.resourceId);

  vkCmdCopyBuffer(vkCmd, srcBuffer.buffer, dstBuffer.buffer, 1, &copyRegion);
}

void VulkanRHI::cmdCopyImage(CommandBuffer cmdBuffer, const TextureView &src, const TextureView &dst)
{
  auto *cmd = getCommandBufferHandle(cmdBuffer);
  VulkanTextureView srcView = createTextureView(src);
  VulkanTextureView dstView = createTextureView(dst);
  const VulkanTexture &srcTexture = getVulkanTexture(src.resourceId);
  const VulkanTexture &dstTexture = getVulkanTexture(dst.resourceId);

  VkImageCopy copyRegion{};
  copyRegion.srcSubresource.aspectMask = srcView.range.aspectMask;
  copyRegion.srcSubresource.mipLevel = src.baseMipLevel;
  copyRegion.srcSubresource.baseArrayLayer = src.baseArrayLayer;
  copyRegion.srcSubresource.layerCount = src.layerCount;
  copyRegion.dstSubresource.aspectMask = dstView.range.aspectMask;
  copyRegion.dstSubresource.mipLevel = dst.baseMipLevel;
  copyRegion.dstSubresource.baseArrayLayer = dst.baseArrayLayer;
  copyRegion.dstSubresource.layerCount = dst.layerCount;
  copyRegion.extent.width = std::min(srcTexture.extent.width, dstTexture.extent.width);
  copyRegion.extent.height = std::min(srcTexture.extent.height, dstTexture.extent.height);
  copyRegion.extent.depth = std::min(std::max(srcTexture.extent.depth, 1u), std::max(dstTexture.extent.depth, 1u));

  vkCmdCopyImage(
      cmd->commandBuffer,
      srcView.image,
      toVkImageLayout(src.layout),
      dstView.image,
      toVkImageLayout(dst.layout),
      1,
      &copyRegion);

  destroyTextureView(srcView);
  destroyTextureView(dstView);
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
  const char *prefix = "_SwapChainImage[";
  const char *suffix = "].texture";

  const size_t prefixLen = std::strlen(prefix);
  const size_t suffixLen = std::strlen(suffix);

  // Check prefix
  if (input.compare(0, prefixLen, prefix) != 0)
    return false;

  // Check suffix
  if (input.size() < prefixLen + suffixLen || input.compare(input.size() - suffixLen, suffixLen, suffix) != 0)
    return false;

  // Find closing bracket
  const size_t numbersStart = prefixLen;
  const size_t numbersEnd = input.find(']', numbersStart);
  if (numbersEnd == std::string::npos)
    return false;

  // Extract "handle,index"
  const std::string numbers = input.substr(numbersStart, numbersEnd - numbersStart);

  const size_t commaPos = numbers.find(',');
  if (commaPos == std::string::npos)
    return false;

  const std::string handleStr = numbers.substr(0, commaPos);
  const std::string indexStr = numbers.substr(commaPos + 1);

  // Parse handle
  char *endPtr = nullptr;
  unsigned long long handle = std::strtoull(handleStr.c_str(), &endPtr, 10);

  if (endPtr == handleStr.c_str() || *endPtr != '\0')
    return false;

  // Parse index
  endPtr = nullptr;
  unsigned long index = std::strtoul(indexStr.c_str(), &endPtr, 10);

  if (endPtr == indexStr.c_str() || *endPtr != '\0')
    return false;

  outInfo.handle = static_cast<uint64_t>(handle);
  outInfo.index = static_cast<uint32_t>(index);
  return true;
}

// void VulkanRHI::cmdCopyBuffer(CommandBuffer cmdHandle, Buffer srcHandle, Buffer dstHandle, uint32_t srcOffset, uint32_t dstOffset, uint32_t size)
// {
//   auto cmd = commandBuffers[cmdHandle]; // *commandBuffers.get(cmdHandle);
//   auto src = getVulkanBuffer(srcHandle.name);
//   auto dst = getVulkanBuffer(dstHandle.name);

//   VkBufferCopy region{};
//   region.srcOffset = srcOffset;
//   region.dstOffset = dstOffset;
//   region.size = size;

//   vkCmdCopyBuffer(cmd->commandBuffer, src.buffer, dst.buffer, 1, &region);
// }

void VulkanRHI::cmdBindGraphicsPipeline(CommandBuffer handle, GraphicsPipeline pipelineHandle)
{
  auto *commandBuffer = getCommandBufferHandle(handle);
  VkCommandBuffer cmd = commandBuffer->commandBuffer;
  auto pipeline = getVulkanGraphicsPipeline(pipelineHandle.id); // reinterpret_cast<VulkanGraphicsPipeline *>(pipelineHandle.get())->pipeline;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);

  // if (commandBuffer->hascomputePipeline || commandBuffer->hasGraphicsPipeline)
  // {
  //   throw std::runtime_error("pipeline already binded to command buffer");
  // }

  commandBuffer->hasGraphicsPipeline = true;
  commandBuffer->hascomputePipeline = false;
  commandBuffer->boundGraphicsPipeline = pipelineHandle; // reinterpret_cast<VulkanGraphicsPipeline *>(pipelineHandle.get());
  commandBuffer->boundComputePipeline = ComputePipeline{.name = ""};
}

void VulkanRHI::cmdBindComputePipeline(CommandBuffer handle, ComputePipeline pipelineHandle)
{
  auto *commandBuffer = getCommandBufferHandle(handle);
  VkCommandBuffer cmd = commandBuffer->commandBuffer;
  // VkPipeline pipeline = reinterpret_cast<VulkanComputePipeline *>(pipelineHandle.get())->pipeline;

  auto pipeline = getVulkanComputePipeline(pipelineHandle.id); // reinterpret_cast<VulkanGraphicsPipeline *>(pipelineHandle.get())->pipeline;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);

  // if (commandBuffer->hascomputePipeline || commandBuffer->hasGraphicsPipeline)
  // {
  //   throw std::runtime_error("pipeline already binded to command buffer");
  // }

  commandBuffer->hasGraphicsPipeline = false;
  commandBuffer->hascomputePipeline = true;
  commandBuffer->boundGraphicsPipeline = GraphicsPipeline{.name = ""};
  commandBuffer->boundComputePipeline = pipelineHandle; // reinterpret_cast<VulkanComputePipeline *>(pipelineHandle);
}

void VulkanRHI::cmdBeginRenderPass(CommandBuffer cmdHandle, const RenderPassInfo &rpInfo)
{
  // Map your CommandBufferHandle to VkCommandBuffer

  auto *commandBuffer = getCommandBufferHandle(cmdHandle);

  // std::vector<VkImageMemoryBarrier> preBarriers;
  // for (int i = 0; i < rpInfo.colorAttachments.size(); i++)
  // {
  //   SwapChainInfo scinfo;
  //   if (parseSwapChainString(rpInfo.colorAttachments[i].view.texture.name, scinfo))
  //   {
  //     auto scTex = getVulkanTexture(rpInfo.colorAttachments[i].view.texture.name);

  //     VkImageMemoryBarrier barrier{};
  //     barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  //     barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  //     barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  //     barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  //     barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  //     barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  //     barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  //     barrier.image = scTex.image;
  //     barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  //     preBarriers.push_back(barrier);
  //   }
  // }

  // if (!preBarriers.empty())
  // {
  //   vkCmdPipelineBarrier(
  //       commandBuffer->commandBuffer,
  //       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // srcStage: after present read
  //       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // dstStage: before color write
  //       VK_DEPENDENCY_BY_REGION_BIT,
  //       0,
  //       nullptr,
  //       0,
  //       nullptr,
  //       static_cast<uint32_t>(preBarriers.size()),
  //       preBarriers.data());
  // }

  if (!commandBuffer->hasGraphicsPipeline)
  {
    throw std::runtime_error("no pipeline was bound");
  }

  auto pipelineData = getVulkanGraphicsPipeline(commandBuffer->boundGraphicsPipeline.id);

  if (pipelineData.renderPass == VK_NULL_HANDLE)
  {
    throw std::runtime_error("no render pass");
  }

  if (pipelineData.pipeline == VK_NULL_HANDLE)
  {
    throw std::runtime_error("no pipeline");
  }

  VkCommandBuffer cmdBuffer = commandBuffer->commandBuffer;

  VkFramebufferCreateInfo framebufferInfo{};
  framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebufferInfo.renderPass = pipelineData.renderPass;

  std::vector<VulkanTextureView> views;
  std::vector<VkImageView> attachments;
  // std::vector<VkSemaphore> achireSemaphores;
  std::vector<VkSemaphore> presentSemaphores;

  if (rpInfo.colorAttachments.size() != pipelineData.info.fragmentStage.colorAttatchments.size())
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
      auto *swapChain = getSwapChainHandle(static_cast<SwapChain>(scinfo.handle));
      // os::Logger::warningf(
      //     "[VulkanRHI] %s achireSemaphoreIndex of swapChin %u, vkImage=%p, index = %u, %p",
      //     rpInfo.colorAttachments[i].view.texture.name.c_str(),
      //     scinfo.handle,
      //     swapChain->swapChainImages[i]->image,
      //     scinfo.index,
      //     swapChain->achireSemaphores[scinfo.index]);
      // achireSemaphores.push_back(swapChain->achireSemaphores[scinfo.index]);
      // presentSemaphores.push_back(swapChain->presentSemaphores[scinfo.index]);
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

  if (rpInfo.depthStencilAttachment.enabled)
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
  commandBufferFrameData.renderPass = pipelineData.renderPass;
  commandBufferFrameData.views = std::move(views);

  // for (int i = 0; i < rpInfo.colorAttachments.size(); i++)
  // {
  //   // auto view = createTextureView(rpInfo.colorAttachments[i].view);
  //   SwapChainInfo nameinfo;

  //   if (parseSwapChainString(rpInfo.colorAttachments[i].view.texture.name, nameinfo))
  //   {
  //     VulkanAttatchment info = {};

  //     info.presentQueue = swapChains[(SwapChain)nameinfo.handle]->presentQueue; // view->renderData.swapChain->presentQueue.queue;
  //     info.swapChain = (SwapChain)nameinfo.handle;
  //     info.swapChainImageIndex = nameinfo.index;

  //     commandBufferFrameData.attatchments.push_back(info);
  //   }
  //   // else
  //   // {
  //   //   VulkanAttatchment info = {};

  //   //   info.presentQueue = VK_NULL_HANDLE;
  //   //   info.swapChain = (SwapChain)(-1);
  //   //   info.swapChainImageIndex = -1;

  //   //   commandBufferFrameData.attatchments.push_back(info);
  //   // }
  // }

  commandBuffer->renderPasses.push_back(std::move(commandBufferFrameData));

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

  if (rpInfo.depthStencilAttachment.enabled)
  {
    VkClearValue clearDepth{};
    clearDepth.depthStencil.depth = rpInfo.depthStencilAttachment.clearDepth;
    clearDepth.depthStencil.stencil = rpInfo.depthStencilAttachment.clearStencil;
    clearValues.push_back(clearDepth);
  }

  // Begin info
  VkRenderPassBeginInfo rpBeginInfo{};
  rpBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpBeginInfo.renderPass = pipelineData.renderPass;
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
  auto *cmdBuffer = getCommandBufferHandle(cmdHandle);
  vkCmdEndRenderPass(cmdBuffer->commandBuffer);
}

void VulkanRHI::cmdBindBindingGroups(CommandBuffer cmdBuffer, BindingGroupsId groupsId, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount)
{
  auto *commandBuffer = getCommandBufferHandle(cmdBuffer);
  VkPipelineLayout layout = VK_NULL_HANDLE;

  if (commandBuffer->hascomputePipeline)
  {
    auto pip = getVulkanComputePipeline(commandBuffer->boundComputePipeline.id);
    layout = getVulkanBindingsLayout(pip.layout.id).pipelineLayout;
  }
  else if (commandBuffer->hasGraphicsPipeline)
  {
    auto pip = getVulkanGraphicsPipeline(commandBuffer->boundGraphicsPipeline.id);
    layout = getVulkanBindingsLayout(pip.layout.id).pipelineLayout;
  }
  else
  {
    throw std::runtime_error("No bound pipeline");
  }

  auto vkGroups = getVulkanBindingGroups(groupsId);

  VkPipelineBindPoint point = VK_PIPELINE_BIND_POINT_MAX_ENUM;

  if (commandBuffer->hasGraphicsPipeline)
  {
    point = VK_PIPELINE_BIND_POINT_GRAPHICS;
  }
  if (commandBuffer->hascomputePipeline)
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

void VulkanRHI::cmdBindVertexBuffer(CommandBuffer handle, uint32_t slot, const BufferView &buffer)
{
  auto *cmd = getCommandBufferHandle(handle);
  auto heap = getVulkanBuffer(buffer.resourceId); // reinterpret_cast<VulkanBuffer *>(bufferHandle.buffer.get());
  VkBuffer vkBuf = heap.buffer;
  VkDeviceSize vkOffset = static_cast<VkDeviceSize>(buffer.offset);

  vkCmdBindVertexBuffers(cmd->commandBuffer, slot, 1, &vkBuf, &vkOffset);
}

void VulkanRHI::cmdBindIndexBuffer(CommandBuffer handle, const BufferView &buffer, Type type)
{
  auto *cmd = getCommandBufferHandle(handle);
  auto heap = getVulkanBuffer(buffer.resourceId); // reinterpret_cast<VulkanBuffer *>(bufferHandle.buffer.get());

  VkBuffer vkBuf = heap.buffer;

  VkDeviceSize vkOffset = static_cast<VkDeviceSize>(buffer.offset);

  VkIndexType vkIndexType = VK_INDEX_TYPE_UINT32;
  if (type == Type_Uint16)
    vkIndexType = VK_INDEX_TYPE_UINT16;
  else if (type == Type_Uint32)
    vkIndexType = VK_INDEX_TYPE_UINT32;
  vkCmdBindIndexBuffer(cmd->commandBuffer, vkBuf, vkOffset, vkIndexType);
}

void VulkanRHI::cmdDraw(CommandBuffer handle, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
  auto *cmd = getCommandBufferHandle(handle);
  vkCmdDraw(cmd->commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanRHI::cmdDrawIndirect(CommandBuffer handle, const BufferView &indirectBuffer, uint32_t drawCount, uint32_t stride)
{
  auto *cmd = getCommandBufferHandle(handle);
  auto heap = getVulkanBuffer(indirectBuffer.resourceId);
  VkBuffer vkBuf = heap.buffer;
  VkDeviceSize vkOffset = static_cast<VkDeviceSize>(indirectBuffer.offset);
  vkCmdDrawIndirect(cmd->commandBuffer, vkBuf, vkOffset, drawCount, stride);
}

void VulkanRHI::cmdDrawIndirectCount(CommandBuffer handle, const BufferView &indirectBuffer, const BufferView &countBuffer, uint32_t maxDrawCount, uint32_t stride)
{
  auto *cmd = getCommandBufferHandle(handle);
  auto heap = getVulkanBuffer(indirectBuffer.resourceId);
  VkBuffer vkBuf = heap.buffer;
  auto count = getVulkanBuffer(countBuffer.resourceId);
  VkBuffer vkCount = count.buffer;

  VkDeviceSize vkOffset = static_cast<VkDeviceSize>(indirectBuffer.offset);
  VkDeviceSize vkCountOffset = static_cast<VkDeviceSize>(countBuffer.offset);

  vkCmdDrawIndirectCount(cmd->commandBuffer, vkBuf, vkOffset, vkCount, vkCountOffset, maxDrawCount, stride);
}

void VulkanRHI::cmdDrawIndexed(CommandBuffer handle, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
  auto *cmd = getCommandBufferHandle(handle);
  vkCmdDrawIndexed(cmd->commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VulkanRHI::cmdDrawIndexedIndirect(CommandBuffer handle, const BufferView &indirectBuffer, uint32_t drawCount, uint32_t stride)
{
  auto *cmd = getCommandBufferHandle(handle);
  auto heap = getVulkanBuffer(indirectBuffer.resourceId);
  VkBuffer vkBuf = heap.buffer;

  VkDeviceSize vkOffset = static_cast<VkDeviceSize>(indirectBuffer.offset);

  vkCmdDrawIndexedIndirect(cmd->commandBuffer, vkBuf, vkOffset, drawCount, stride);
}

void VulkanRHI::cmdDispatchIndirect(CommandBuffer commandBuffer, const BufferView &indirectBuffer)
{
  auto *cmd = getCommandBufferHandle(commandBuffer);

  if (!cmd->hascomputePipeline)
  {
    throw std::runtime_error("Attempted to dispatch with no compute pipeline bound!");
  }

  auto indirectBufferVk = getVulkanBuffer(indirectBuffer.resourceId);
  VkBuffer vkBuf = indirectBufferVk.buffer;

  vkCmdDispatchIndirect(cmd->commandBuffer, vkBuf, indirectBuffer.offset);
}

void VulkanRHI::cmdDispatch(CommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
  auto *cmd = getCommandBufferHandle(commandBuffer);

  if (!cmd->hascomputePipeline)
  {
    throw std::runtime_error("Attempted to dispatch with no compute pipeline bound!");
  }

  vkCmdDispatch(cmd->commandBuffer, groupCountX, groupCountY, groupCountZ);
}

static VkAccessFlags toVulkanAccess(AccessPattern access)
{
  VkAccessFlags flags = 0;
  uint64_t bits = static_cast<uint64_t>(access);

  if (bits & static_cast<uint64_t>(AccessPattern::VERTEX_ATTRIBUTE_READ))
    flags |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::INDEX_READ))
    flags |= VK_ACCESS_INDEX_READ_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::UNIFORM_READ))
    flags |= VK_ACCESS_UNIFORM_READ_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::SHADER_READ))
    flags |= VK_ACCESS_SHADER_READ_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::SHADER_WRITE))
    flags |= VK_ACCESS_SHADER_WRITE_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::COLOR_ATTACHMENT_READ))
    flags |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::COLOR_ATTACHMENT_WRITE))
    flags |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ))
    flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE))
    flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::TRANSFER_READ))
    flags |= VK_ACCESS_TRANSFER_READ_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::TRANSFER_WRITE))
    flags |= VK_ACCESS_TRANSFER_WRITE_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::INDIRECT_COMMAND_READ))
    flags |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::MEMORY_READ))
    flags |= VK_ACCESS_MEMORY_READ_BIT;

  if (bits & static_cast<uint64_t>(AccessPattern::MEMORY_WRITE))
    flags |= VK_ACCESS_MEMORY_WRITE_BIT;

  return flags;
}

static VkPipelineStageFlagBits toVulkanStage(PipelineStage stage)
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
    VkImageAspectFlags aspect_mask,
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

static void sanitizeQueueFamilyOwnership(VkSharingMode sharingMode, uint32_t &srcQueueFamily, uint32_t &dstQueueFamily)
{
  if (sharingMode == VK_SHARING_MODE_CONCURRENT)
  {
    srcQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    dstQueueFamily = VK_QUEUE_FAMILY_IGNORED;
  }
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

VkFence VulkanRHI::getFence()
{
  VkFence data;

  // if (fences.dequeue(data))
  // {
  //   vkResetFences(device, 1, &data);
  // }
  // else
  // {
  data = createFence(device, false);
  // }

  return data;
}

VkSemaphore VulkanRHI::getSemaphore()
{
  VkSemaphore semaphore = VK_NULL_HANDLE;

  VkSemaphoreTypeCreateInfo typeInfo{};
  typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  typeInfo.pNext = nullptr;
  typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  typeInfo.initialValue = 0;

  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphoreInfo.pNext = &typeInfo;
  semaphoreInfo.flags = 0;

  if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create timeline semaphore");
  }

  return semaphore;
}

void VulkanRHI::cleanupSubmitCallback(VulkanRHI::VulkanAsyncHandler &future)
{
  if (future.releaseCommandBuffersOnCompletion)
  {
    future.device->releaseCommandBuffer(future.commandBuffers);
  }
  else
  {
    for (const CommandBuffer commandBufferHandle : future.commandBuffers)
    {
      auto *commandBuffer = getCommandBufferHandle(commandBufferHandle);
      if (commandBuffer == nullptr)
      {
        continue;
      }

      commandBuffer->submited = false;
      commandBuffer->fence = VK_NULL_HANDLE;
    }
  }
  future.device->deferredFenceDeletes.push_back(future.fence);
  future.device->deferredIdleSemaphoreDeletes.push_back(future.semaphore);
}

void VulkanRHI::flushDeferredDeletions()
{
  tryReleaseDeferredResources();
}

VulkanRHI::VulkanAsyncHandler::VulkanAsyncHandler(VulkanRHI *device, std::vector<CommandBuffer> cb, VkFence f, VkSemaphore s, bool releaseCommandBuffersOnCompletion)
{
  this->device = device;
  this->fence = f;
  this->semaphore = s;
  this->commandBuffers = cb;
  this->releaseCommandBuffersOnCompletion = releaseCommandBuffersOnCompletion;
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

void VulkanRHI::present(SwapChain &swapChain, TextureId textureId, ResourceLayout currentLayout)
{
  presentWithOverlay(swapChain, textureId, currentLayout, {});
}

void VulkanRHI::presentWithOverlay(
    SwapChain &swapChain,
    TextureId textureId,
    ResourceLayout currentLayout,
    const std::function<void(VkCommandBuffer, VkImageView, VkExtent2D)> &overlayCallback)
{
  auto *vkSwapChain = getSwapChainHandle(swapChain);
  uint32_t imageIndex = 0;
  bool found = false;

  for (uint32_t i = 0; i < vkSwapChain->swapChainImages.size(); ++i)
  {
    if (vkSwapChain->swapChainImages[i]->id == textureId)
    {
      imageIndex = i;
      found = true;
      break;
    }
  }

  if (!found)
  {
    throw std::invalid_argument("Texture does not belong to this swapchain");
  }

  auto vkTexture = vkSwapChain->swapChainImages[imageIndex];
  VkCommandBuffer cmd = vkSwapChain->overlayCommandBuffers[imageIndex];
  VkFence overlayFence = vkSwapChain->overlayFences[imageIndex];

  vkWaitForFences(device, 1u, &overlayFence, VK_TRUE, UINT64_MAX);
  vkResetFences(device, 1u, &overlayFence);
  vkResetCommandPool(device, vkSwapChain->overlayCommandPools[imageIndex], 0u);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  vkBeginCommandBuffer(cmd, &beginInfo);

  if (overlayCallback)
  {
    if (currentLayout != ResourceLayout::COLOR_ATTACHMENT)
    {
      VkImageMemoryBarrier toColorAttachment{};
      toColorAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      toColorAttachment.oldLayout = toVulkanLayout(currentLayout);
      toColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      toColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toColorAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toColorAttachment.image = vkTexture->image;
      toColorAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      toColorAttachment.subresourceRange.baseMipLevel = 0;
      toColorAttachment.subresourceRange.levelCount = 1;
      toColorAttachment.subresourceRange.baseArrayLayer = 0;
      toColorAttachment.subresourceRange.layerCount = 1;
      toColorAttachment.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
      toColorAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

      vkCmdPipelineBarrier(
          cmd,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          0,
          0,
          nullptr,
          0,
          nullptr,
          1,
          &toColorAttachment);
    }

    overlayCallback(cmd, vkSwapChain->swapChainImageViews[imageIndex]->view, vkSwapChain->swapChainExtent);
    currentLayout = ResourceLayout::COLOR_ATTACHMENT;
  }

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = toVulkanLayout(currentLayout);
  barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = vkTexture->image;

  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  barrier.srcAccessMask = (currentLayout == ResourceLayout::COLOR_ATTACHMENT)
                              ? (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
                              : (VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT);
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

  vkCmdPipelineBarrier(
      cmd,
      currentLayout == ResourceLayout::COLOR_ATTACHMENT ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
      0,
      0,
      nullptr,
      0,
      nullptr,
      1,
      &barrier);

  vkEndCommandBuffer(cmd);

  VkSemaphore presentReady = vkSwapChain->presentSemaphores[imageIndex];

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmd;
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = &presentReady;
  VkQueue queue = getQueueHandle(Graphics);

  vkQueueSubmit(queue, 1, &submitInfo, overlayFence);

  VkPresentInfoKHR presentInfo{};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = &presentReady;
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = &vkSwapChain->swapChain;
  presentInfo.pImageIndices = &imageIndex;

  vkQueuePresentKHR(vkSwapChain->presentQueue, &presentInfo);
}

// void VulkanRHI::processPresentations(CommandBuffer *cmds, uint32_t count, const std::vector<VkSemaphore> &signalSemaphores)
// {
//   for (uint32_t i = 0; i < count; i++)
//   {
//     auto cmdBuf = commandBuffers[cmds[i]];
//     for (auto &frameData : cmdBuf->renderPasses)
//     {
//       // Group swapchain images by their respective present queues
//       std::unordered_map<VkQueue, std::vector<VkSwapchainKHR>> queueGroups;
//       std::unordered_map<VkQueue, std::vector<uint32_t>> indexGroups;

//       for (auto &attachment : frameData.attatchments)
//       {
//         if (attachment.swapChain != (SwapChain)-1)
//         {
//           auto sc = swapChains[(SwapChain)attachment.swapChain];
//           queueGroups[attachment.presentQueue].push_back(sc->swapChain);
//           indexGroups[attachment.presentQueue].push_back(attachment.swapChainImageIndex);
//         }
//       }

//       for (auto &[presentQueue, vkSwaps] : queueGroups)
//       {
//         VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
//         presentInfo.waitSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
//         presentInfo.pWaitSemaphores = signalSemaphores.data();
//         presentInfo.swapchainCount = static_cast<uint32_t>(vkSwaps.size());
//         presentInfo.pSwapchains = vkSwaps.data();
//         presentInfo.pImageIndices = indexGroups[presentQueue].data();

//         vkQueuePresentKHR(presentQueue, &presentInfo);
//         // Note: Error handling (OUT_OF_DATE) should typically trigger a swapchain recreation flag
//       }
//     }
//   }
// }

GPUFuture VulkanRHI::submit(Queue queueType, CommandBuffer *cmds, uint32_t count, GPUFuture *wait, uint32_t waitCount, bool releaseCommandBuffersOnCompletion)
{
  VkQueue queue = getQueueHandle(queueType);
  std::vector<VkCommandBuffer> vkCmds;
  vkCmds.reserve(count);
  std::vector<VkSemaphore> timelineWaitSemaphores;
  timelineWaitSemaphores.reserve(waitCount);
  std::vector<uint64_t> timelineWaitValues;
  timelineWaitValues.reserve(waitCount);
  std::vector<VkSemaphore> timelineSignalSemaphores;
  timelineSignalSemaphores.reserve(1u);
  std::vector<uint64_t> timelineSignalValues;
  timelineSignalValues.reserve(1u);
  std::vector<VkSemaphore> binaryWaitSemaphores;
  binaryWaitSemaphores.reserve(count);
  std::vector<VkPipelineStageFlags> binaryWaitStages;
  binaryWaitStages.reserve(count);
  std::vector<VkSemaphore> binarySignalSemaphores;
  binarySignalSemaphores.reserve(1u);
  // std::vector<VkSemaphore> presentSignalSemaphores;

  if (wait != nullptr)
  {
    for (uint32_t i = 0; i < waitCount; i++)
    {
      auto internalEvent = wait[i].getIf<rendering::AsyncEvent<VulkanAsyncHandler>>();
      if (internalEvent && internalEvent->isValid() && internalEvent->checkStatus() == FenceStatus::PENDING)
      {
        timelineWaitSemaphores.push_back(internalEvent->getFence()->semaphore);
        timelineWaitValues.push_back(1); // single-shot timeline
      }
    }
  }

  VkSemaphore timelineSemaphore = getSemaphore(); // TIMELINE
  VkFence fence = getFence();
  timelineSignalSemaphores.push_back(timelineSemaphore);
  timelineSignalValues.push_back(1); // first signal

  auto appendUniqueWaitSemaphore = [&](VkSemaphore semaphore, VkPipelineStageFlags stageMask)
  {
    if (semaphore == VK_NULL_HANDLE)
    {
      return;
    }
    if (std::find(binaryWaitSemaphores.begin(), binaryWaitSemaphores.end(), semaphore) != binaryWaitSemaphores.end())
    {
      return;
    }
    binaryWaitSemaphores.push_back(semaphore);
    binaryWaitStages.push_back(stageMask);
  };

  for (uint32_t i = 0; i < count; ++i)
  {
    auto *cmdBuf = getCommandBufferHandle(cmds[i]);
    vkCmds.push_back(cmdBuf->commandBuffer);
    cmdBuf->fence = fence;
    cmdBuf->submited = true;
    for (auto &frameData : cmdBuf->renderPasses)
    {
      for (const auto &view : frameData.views)
      {
        if (view.original.resourceId == TextureId::Invalid)
        {
          continue;
        }

        const VulkanTexture &vkTexture = getVulkanTexture(view.original.resourceId);
        auto *swapChain = vkTexture.swapChainOwner;
        if (swapChain == nullptr)
        {
          continue;
        }

        const uint32_t acquireSemaphoreIndex = swapChain->currentAcquiredSemaphoreIndex;
        if (acquireSemaphoreIndex < swapChain->acquireSemaphores.size() && acquireSemaphoreIndex < swapChain->acquireSemaphorePending.size() &&
            swapChain->acquireSemaphorePending[acquireSemaphoreIndex] && vkTexture.swapChainImageIndex == swapChain->currentAcquiredImageIndex)
        {
          appendUniqueWaitSemaphore(swapChain->acquireSemaphores[acquireSemaphoreIndex], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
          swapChain->acquireSemaphorePending[acquireSemaphoreIndex] = false;
        }
      }
    }
  }

  // Build unified wait arrays — values must be parallel to semaphores
  // Binary semaphore slots get value 0 (ignored by the driver)
  std::vector<VkSemaphore> waitSemaphores;
  std::vector<VkPipelineStageFlags> waitStages;
  std::vector<uint64_t> waitValues;
  waitSemaphores.reserve(timelineWaitSemaphores.size() + binaryWaitSemaphores.size());
  waitStages.reserve(timelineWaitSemaphores.size() + binaryWaitSemaphores.size());
  waitValues.reserve(timelineWaitSemaphores.size() + binaryWaitSemaphores.size());

  for (size_t i = 0; i < timelineWaitSemaphores.size(); ++i)
  {
    waitSemaphores.push_back(timelineWaitSemaphores[i]);
    waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    waitValues.push_back(timelineWaitValues[i]);
  }
  for (size_t i = 0; i < binaryWaitSemaphores.size(); ++i)
  {
    waitSemaphores.push_back(binaryWaitSemaphores[i]);
    waitStages.push_back(binaryWaitStages[i]);
    waitValues.push_back(0); // ignored for binary semaphores, must still be present
  }

  // Build unified signal arrays — values must be parallel to semaphores
  // Binary semaphore slots get value 0 (ignored by the driver)
  std::vector<VkSemaphore> signalSemaphores;
  std::vector<uint64_t> signalValues;
  signalSemaphores.reserve(timelineSignalSemaphores.size() + binarySignalSemaphores.size());
  signalValues.reserve(timelineSignalSemaphores.size() + binarySignalSemaphores.size());

  for (size_t i = 0; i < timelineSignalSemaphores.size(); ++i)
  {
    signalSemaphores.push_back(timelineSignalSemaphores[i]);
    signalValues.push_back(timelineSignalValues[i]);
  }
  for (auto s : binarySignalSemaphores)
  {
    signalSemaphores.push_back(s);
    signalValues.push_back(0); // ignored for binary semaphores, must still be present
  }

  VkTimelineSemaphoreSubmitInfo timelineInfo{
    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
    .waitSemaphoreValueCount = uint32_t(waitValues.size()), // must equal waitSemaphoreCount
    .pWaitSemaphoreValues = waitValues.data(),
    .signalSemaphoreValueCount = uint32_t(signalValues.size()), // must equal signalSemaphoreCount
    .pSignalSemaphoreValues = signalValues.data(),
  };

  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.pNext = &timelineInfo;
  submitInfo.waitSemaphoreCount = uint32_t(waitSemaphores.size());
  submitInfo.pWaitSemaphores = waitSemaphores.data();
  submitInfo.pWaitDstStageMask = waitStages.data();
  submitInfo.commandBufferCount = uint32_t(vkCmds.size());
  submitInfo.pCommandBuffers = vkCmds.data();
  submitInfo.signalSemaphoreCount = uint32_t(signalSemaphores.size());
  submitInfo.pSignalSemaphores = signalSemaphores.data();

  if (vkQueueSubmit(queue, 1, &submitInfo, fence) != VK_SUCCESS)
    throw std::runtime_error("vkQueueSubmit failed");

  // processPresentations(cmds, count, binarySignalSemaphores);

  std::vector<CommandBuffer> cbs;

  for (uint32_t i = 0; i < count; i++)
  {
    cbs.push_back(cmds[i]);
  }

  VulkanAsyncHandler handler(this, cbs, fence, timelineSemaphore, releaseCommandBuffersOnCompletion);
  GPUFuture resultFuture = eventLoop.submit(std::move(handler), cleanupSubmitCallback);
  tryReleaseDeferredResources();
  return resultFuture;
}

void VulkanRHI::waitIdle()
{
  vkDeviceWaitIdle(device);
  eventLoop.tick();
  tryReleaseDeferredResources();
}

void VulkanRHI::blockUntil(GPUFuture &future)
{
  auto internalEvent = future.getIf<rendering::AsyncEvent<VulkanAsyncHandler>>();
  if (internalEvent == nullptr || !internalEvent->isValid())
  {
    return;
  }

  const VulkanAsyncHandler *handler = internalEvent->getFence();
  if (handler == nullptr || handler->fence == VK_NULL_HANDLE)
  {
    return;
  }

  const VkFence fence = handler->fence;
  const VkResult result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
  if (result != VK_SUCCESS)
  {
    throw std::runtime_error("vkWaitForFences failed while waiting for a submitted frame");
  }

  tryReleaseDeferredResources();
}

bool VulkanRHI::isCompleted(GPUFuture &future)
{
  tryReleaseDeferredResources();

  if (future.checkStatus() == FenceStatus::PENDING)
  {
    return false;
  }

  return true;
}

BufferId VulkanRHI::createBuffer(const BufferInfo &info)
{
  return allocateBuffer(info);
}

TextureId VulkanRHI::createTexture(const TextureInfo &info)
{
  return allocateTexture(info);
}

SamplerId VulkanRHI::createSampler(const SamplerInfo &info)
{
  return allocateSampler(info);
}

BindingsLayoutId VulkanRHI::createBindingsLayout(const BindingsLayoutInfo &info)
{
  return allocateBindingsLayout(info);
}

BindingGroupsId VulkanRHI::createBindingGroups(const BindingGroupsInfo &info)
{
  const VulkanBindingsLayout &vkLayout = getVulkanBindingsLayout(info.layout.id);
  return allocateBindings(info, vkLayout);
}

GraphicsPipelineId VulkanRHI::createGraphicsPipeline(const GraphicsPipelineInfo &info)
{
  return allocateGraphicsPipeline(info);
}

ComputePipelineId VulkanRHI::createComputePipeline(const ComputePipelineInfo &info)
{
  return allocateComputePipeline(info);
}

void VulkanRHI::deleteBuffer(BufferId resourceId)
{
  releaseBuffer(resourceId);
}

void VulkanRHI::deleteTexture(TextureId resourceId)
{
  releaseTexture(resourceId);
}

void VulkanRHI::deleteSampler(SamplerId resourceId)
{
  releaseSampler(resourceId);
}

void VulkanRHI::deleteBindingsLayout(BindingsLayoutId resourceId)
{
  releaseBindingsLayout(resourceId);
}

void VulkanRHI::deleteBindingGroups(BindingGroupsId resourceId)
{
  releaseBindingGroup(resourceId);
}

void VulkanRHI::deleteGraphicsPipeline(GraphicsPipelineId resourceId)
{
  releaseGraphicsPipeline(resourceId);
}

void VulkanRHI::deleteComputePipeline(ComputePipelineId resourceId)
{
  releaseComputePipeline(resourceId);
}

ShaderId VulkanRHI::createShader(const ShaderInfo info)
{
  if (info.type != SpirV)
  {
    os::Logger::errorf("Invalid type of shader %s, VulkanRHI only accepts spirv shader types", info.name.c_str());
    exit(1);
  }

  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = info.src.size();
  createInfo.pCode = reinterpret_cast<const uint32_t *>(info.src.data());

  VkShaderModule shaderModule = VK_NULL_HANDLE;

  if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create shader module!");
  }

  VulkanShader *s = new VulkanShader();
  s->shaderModule = shaderModule;
  s->info = info;

  return makeHandle<ShaderId>(s);
}

const VulkanShader &VulkanRHI::getVulkanShader(ShaderId id)
{
  return *getHandlePtr<VulkanShader>(id, "VulkanShader");
}

void VulkanRHI::deleteShader(ShaderId handle)
{
  if (handle == ShaderId::Invalid)
    return;
  auto *s = getHandlePtr<VulkanShader>(handle, "VulkanShader");
  vkDestroyShaderModule(device, s->shaderModule, nullptr);
  delete s;
}

void VulkanRHI::reserveTimerCapacity(uint32_t maxTimers)
{
  reservedTimerCapacity = std::max(reservedTimerCapacity, std::max(1u, maxTimers));

  if (timerArena.queryPool != VK_NULL_HANDLE && timerArena.activeTimers == 0u && timerArena.maxTimers < reservedTimerCapacity)
  {
    destroyTimerArena();
  }
}

void VulkanRHI::ensureTimerArena()
{
  if (timerArena.queryPool != VK_NULL_HANDLE)
  {
    return;
  }

  timerArena.maxTimers = std::max(1u, reservedTimerCapacity);
  timerArena.totalQueries = timerArena.maxTimers * 2u;

  VkQueryPoolCreateInfo qpci{};
  qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qpci.queryCount = timerArena.totalQueries;

  if (vkCreateQueryPool(device, &qpci, nullptr, &timerArena.queryPool) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create shared timestamp query pool");
  }

  timerArena.valuesBuffer = allocateBuffer(
      BufferInfo{
        .name = "__TimestampArenaValues.buffer",
        .size = static_cast<uint64_t>(timerArena.totalQueries) * sizeof(uint64_t),
        .usage = BufferUsage_Timestamp | BufferUsage_Pull | BufferUsage_CopyDst,
      });
  timerArena.freeRanges.clear();
  timerArena.freeRanges.push_back(VulkanTimerRange{
      .firstQuery = 0u,
      .queryCount = timerArena.totalQueries,
  });
}

void VulkanRHI::destroyTimerArena()
{
  if (timerArena.queryPool != VK_NULL_HANDLE)
  {
    vkDestroyQueryPool(device, timerArena.queryPool, nullptr);
    timerArena.queryPool = VK_NULL_HANDLE;
  }

  if (timerArena.valuesBuffer != BufferId::Invalid)
  {
    releaseBuffer(timerArena.valuesBuffer);
    timerArena.valuesBuffer = BufferId::Invalid;
  }

  timerArena.maxTimers = 0u;
  timerArena.totalQueries = 0u;
  timerArena.activeTimers = 0u;
  timerArena.freeRanges.clear();
}

VulkanTimerRange VulkanRHI::allocateTimerRange(uint32_t queryCount)
{
  ensureTimerArena();

  for (size_t rangeIndex = 0u; rangeIndex < timerArena.freeRanges.size(); ++rangeIndex)
  {
    auto &range = timerArena.freeRanges[rangeIndex];
    if (range.queryCount < queryCount)
    {
      continue;
    }

    const VulkanTimerRange allocatedRange{
      .firstQuery = range.firstQuery,
      .queryCount = queryCount,
    };

    range.firstQuery += queryCount;
    range.queryCount -= queryCount;
    if (range.queryCount == 0u)
    {
      timerArena.freeRanges.erase(timerArena.freeRanges.begin() + static_cast<std::vector<VulkanTimerRange>::difference_type>(rangeIndex));
    }

    return allocatedRange;
  }

  throw std::runtime_error("Shared timestamp arena is exhausted");
}

void VulkanRHI::releaseTimerRange(const VulkanTimerRange &range)
{
  if (range.queryCount == 0u || timerArena.queryPool == VK_NULL_HANDLE)
  {
    return;
  }

  timerArena.freeRanges.push_back(range);
  std::sort(
      timerArena.freeRanges.begin(),
      timerArena.freeRanges.end(),
      [](const VulkanTimerRange &a, const VulkanTimerRange &b)
      {
        return a.firstQuery < b.firstQuery;
      });

  std::vector<VulkanTimerRange> mergedRanges;
  mergedRanges.reserve(timerArena.freeRanges.size());
  for (const auto &freeRange : timerArena.freeRanges)
  {
    if (!mergedRanges.empty())
    {
      auto &lastRange = mergedRanges.back();
      if (lastRange.firstQuery + lastRange.queryCount >= freeRange.firstQuery)
      {
        const uint32_t mergedEnd = std::max(lastRange.firstQuery + lastRange.queryCount, freeRange.firstQuery + freeRange.queryCount);
        lastRange.queryCount = mergedEnd - lastRange.firstQuery;
        continue;
      }
    }

    mergedRanges.push_back(freeRange);
  }

  timerArena.freeRanges = std::move(mergedRanges);
}

const Timer VulkanRHI::createTimer(const TimerInfo info)
{
  if ((this->features & DeviceFeatures_Timestamp) == 0)
  {
    throw std::runtime_error("Timestamp not available");
  }

  if (vkTimers.contains(info.name))
  {
    throw std::runtime_error("A Timer is already created with the same name");
  }
  ensureTimerArena();

  if (timerArena.activeTimers >= timerArena.maxTimers)
  {
    throw std::runtime_error(
        "Timer capacity exhausted while creating '" + info.name +
        "'. Increase RenderGraph::Settings.maxTimers (current capacity: " + std::to_string(timerArena.maxTimers) + ")");
  }

  const uint32_t sampleCount = std::max(1u, info.sampleCount);
  const uint32_t queryCount = sampleCount * 2u;
  if (queryCount > timerArena.totalQueries)
  {
    throw std::runtime_error(
        "Timer '" + info.name + "' requires " + std::to_string(queryCount) +
        " timestamp queries, which exceeds the shared arena capacity");
  }

  VulkanTimerRange allocatedRange{};
  try
  {
    allocatedRange = allocateTimerRange(queryCount);
  }
  catch (const std::runtime_error &)
  {
    throw std::runtime_error(
        "Timer query capacity exhausted while creating '" + info.name +
        "'. Increase RenderGraph::Settings.maxTimers (current capacity: " + std::to_string(timerArena.maxTimers) + ")");
  }

  auto timer = new VulkanTimer();
  timer->info = info;
  timer->info.sampleCount = sampleCount;
  timer->firstQuery = allocatedRange.firstQuery;
  timer->queryCount = allocatedRange.queryCount;
  vkTimers.insert(info.name, timer);
  ++timerArena.activeTimers;

  return Timer{
    .name = info.name,
    .sampleCount = sampleCount,
  };
}

void VulkanRHI::deleteTimer(const Timer &timer)
{
  auto it = vkTimers.find(timer.name);
  if (it == vkTimers.end())
    return;

  VulkanTimer *vkTimer = it.value();
  releaseTimerRange(
      VulkanTimerRange{
        .firstQuery = vkTimer->firstQuery,
        .queryCount = vkTimer->queryCount,
      });
  if (timerArena.activeTimers > 0u)
  {
    --timerArena.activeTimers;
  }

  delete vkTimer;
  vkTimers.remove(timer.name);

  if (timerArena.activeTimers == 0u && timerArena.maxTimers < reservedTimerCapacity)
  {
    destroyTimerArena();
  }
}

void VulkanRHI::cmdStartTimer(CommandBuffer handle, const Timer &timer, uint32_t sampleIndex, PipelineStage stage)
{
  auto *cmd = getCommandBufferHandle(handle);
  auto vkTimer = vkTimers.find(timer.name);
  if (vkTimer == vkTimers.end())
  {
    throw std::runtime_error("Timer '" + timer.name + "' was not found");
  }
  if (sampleIndex >= vkTimer.value()->info.sampleCount)
  {
    throw std::runtime_error("Timer '" + timer.name + "' sample index is out of range");
  }

  const uint32_t queryIndex = vkTimer.value()->firstQuery + sampleIndex * 2u;
  vkCmdWriteTimestamp(cmd->commandBuffer, toVulkanStage(stage), timerArena.queryPool, queryIndex);
}

void VulkanRHI::cmdStopTimer(CommandBuffer handle, const Timer &timer, uint32_t sampleIndex, PipelineStage stage)
{
  auto *cmd = getCommandBufferHandle(handle);
  auto vkTimer = vkTimers.find(timer.name);
  if (vkTimer == vkTimers.end())
  {
    throw std::runtime_error("Timer '" + timer.name + "' was not found");
  }
  if (sampleIndex >= vkTimer.value()->info.sampleCount)
  {
    throw std::runtime_error("Timer '" + timer.name + "' sample index is out of range");
  }

  const uint32_t queryIndex = vkTimer.value()->firstQuery + sampleIndex * 2u;
  vkCmdWriteTimestamp(cmd->commandBuffer, toVulkanStage(stage), timerArena.queryPool, queryIndex + 1u);
}

void VulkanRHI::cmdResetTimer(CommandBuffer handle, const Timer &timer)
{
  auto *cmd = getCommandBufferHandle(handle);
  auto vkTimer = vkTimers.find(timer.name);
  if (vkTimer == vkTimers.end())
  {
    throw std::runtime_error("Timer '" + timer.name + "' was not found");
  }

  vkCmdResetQueryPool(cmd->commandBuffer, timerArena.queryPool, vkTimer.value()->firstQuery, vkTimer.value()->queryCount);
}

void VulkanRHI::cmdResolveTimersToBuffer(CommandBuffer handle, const Timer *timers, uint32_t count, BufferId destinationBuffer, uint64_t destinationOffset)
{
  if (timers == nullptr || count == 0u || destinationBuffer == BufferId::Invalid || timerArena.queryPool == VK_NULL_HANDLE)
  {
    return;
  }

  auto *cmd = getCommandBufferHandle(handle);
  const VulkanBuffer &destination = getVulkanBuffer(destinationBuffer);

  uint64_t writeOffset = destinationOffset;
  for (uint32_t timerIndex = 0u; timerIndex < count; ++timerIndex)
  {
    auto vkTimer = vkTimers.find(timers[timerIndex].name);
    if (vkTimer == vkTimers.end())
    {
      throw std::runtime_error("Timer '" + timers[timerIndex].name + "' was not found");
    }

    const VulkanTimer &timer = *vkTimer.value();
    vkCmdCopyQueryPoolResults(
        cmd->commandBuffer,
        timerArena.queryPool,
        timer.firstQuery,
        timer.queryCount,
        destination.buffer,
        writeOffset,
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    writeOffset += static_cast<uint64_t>(timer.queryCount) * sizeof(uint64_t);
  }
}

double VulkanRHI::readTimer(const Timer &timer)
{
  double value = 0.0;
  readTimers(&timer, 1u, &value);
  return value;
}

double VulkanRHI::getTimestampPeriodNs() const
{
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physicalDevice, &props);
  return static_cast<double>(props.limits.timestampPeriod);
}

void VulkanRHI::readTimers(const Timer *timers, uint32_t count, double *outValues)
{
  std::lock_guard<std::mutex> lock(hostAccessMutex_);
  if (outValues == nullptr || count == 0u)
  {
    return;
  }

  if (timerArena.queryPool == VK_NULL_HANDLE)
  {
    for (uint32_t timerIndex = 0u; timerIndex < count; ++timerIndex)
    {
      outValues[timerIndex] = 0.0;
    }
    return;
  }

  struct ResolvedTimer
  {
    const VulkanTimer *timer = nullptr;
    TimerUnit unit = TimerUnit::Miliseconds;
  };

  struct QueryReadRange
  {
    uint32_t firstQuery = 0u;
    uint32_t queryCount = 0u;
  };

  std::vector<ResolvedTimer> resolvedTimers(count);
  for (uint32_t timerIndex = 0u; timerIndex < count; ++timerIndex)
  {
    auto vkTimer = vkTimers.find(timers[timerIndex].name);
    if (vkTimer == vkTimers.end())
    {
      throw std::runtime_error("Timer '" + timers[timerIndex].name + "' was not found");
    }

    resolvedTimers[timerIndex] = ResolvedTimer{
      .timer = vkTimer.value(),
      .unit = vkTimer.value()->info.unit,
    };
  }

  const VulkanBuffer &timerValuesBuffer = getVulkanBuffer(timerArena.valuesBuffer);
  void *mappedValues = nullptr;
  vkMapMemory(device, timerValuesBuffer.memory, 0, timerValuesBuffer.size, 0, &mappedValues);

  std::vector<QueryReadRange> readRanges;
  readRanges.reserve(count);
  for (const ResolvedTimer &resolvedTimer : resolvedTimers)
  {
    readRanges.push_back(
        QueryReadRange{
          .firstQuery = resolvedTimer.timer->firstQuery,
          .queryCount = resolvedTimer.timer->queryCount,
        });
  }

  std::sort(
      readRanges.begin(),
      readRanges.end(),
      [](const QueryReadRange &a, const QueryReadRange &b)
      {
        return a.firstQuery < b.firstQuery;
      });

  std::vector<QueryReadRange> mergedReadRanges;
  mergedReadRanges.reserve(readRanges.size());
  for (const QueryReadRange &range : readRanges)
  {
    if (!mergedReadRanges.empty())
    {
      QueryReadRange &lastRange = mergedReadRanges.back();
      const uint32_t lastEnd = lastRange.firstQuery + lastRange.queryCount;
      if (lastEnd == range.firstQuery)
      {
        lastRange.queryCount += range.queryCount;
        continue;
      }
    }

    mergedReadRanges.push_back(range);
  }

  uint8_t *mappedBytes = reinterpret_cast<uint8_t *>(mappedValues);
  for (const QueryReadRange &range : mergedReadRanges)
  {
    vkGetQueryPoolResults(
        device,
        timerArena.queryPool,
        range.firstQuery,
        range.queryCount,
        static_cast<size_t>(range.queryCount) * sizeof(uint64_t),
        mappedBytes + static_cast<size_t>(range.firstQuery) * sizeof(uint64_t),
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  }

  const double timestampPeriodNs = getTimestampPeriodNs();

  const uint64_t *timestamps = reinterpret_cast<const uint64_t *>(mappedValues);
  for (uint32_t timerIndex = 0u; timerIndex < count; ++timerIndex)
  {
    const VulkanTimer &timer = *resolvedTimers[timerIndex].timer;
    const uint32_t sampleCount = std::max(1u, timer.info.sampleCount);

    double timeNs = 0.0;
    for (uint32_t sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
      const uint32_t queryIndex = timer.firstQuery + sampleIndex * 2u;
      const uint64_t start = timestamps[queryIndex + 0u];
      const uint64_t end = timestamps[queryIndex + 1u];
      if (end >= start)
      {
        timeNs += static_cast<double>(end - start) * timestampPeriodNs;
      }
    }

    switch (resolvedTimers[timerIndex].unit)
    {
    case TimerUnit::Nanoseconds:
      outValues[timerIndex] = timeNs;
      break;

    case TimerUnit::Miliseconds:
      outValues[timerIndex] = timeNs * 1e-6;
      break;

    case TimerUnit::Seconds:
      outValues[timerIndex] = timeNs * 1e-9;
      break;
    }
  }

  vkUnmapMemory(device, timerValuesBuffer.memory);
}

#ifdef VULKAN_RHI_LOGS

// Helper function to convert VkAccessFlags to string
static std::string vkAccessFlagsToString(VkAccessFlags flags)
{
  if (flags == 0)
    return "0";

  std::vector<std::string> accessFlags;

  if (flags & VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT)
    accessFlags.push_back("VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT");
  if (flags & VK_ACCESS_INDEX_READ_BIT)
    accessFlags.push_back("VK_ACCESS_INDEX_READ_BIT");
  if (flags & VK_ACCESS_UNIFORM_READ_BIT)
    accessFlags.push_back("VK_ACCESS_UNIFORM_READ_BIT");
  if (flags & VK_ACCESS_SHADER_READ_BIT)
    accessFlags.push_back("VK_ACCESS_SHADER_READ_BIT");
  if (flags & VK_ACCESS_SHADER_WRITE_BIT)
    accessFlags.push_back("VK_ACCESS_SHADER_WRITE_BIT");
  if (flags & VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
    accessFlags.push_back("VK_ACCESS_COLOR_ATTACHMENT_READ_BIT");
  if (flags & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
    accessFlags.push_back("VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT");
  if (flags & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT)
    accessFlags.push_back("VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT");
  if (flags & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
    accessFlags.push_back("VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT");
  if (flags & VK_ACCESS_TRANSFER_READ_BIT)
    accessFlags.push_back("VK_ACCESS_TRANSFER_READ_BIT");
  if (flags & VK_ACCESS_TRANSFER_WRITE_BIT)
    accessFlags.push_back("VK_ACCESS_TRANSFER_WRITE_BIT");
  if (flags & VK_ACCESS_INDIRECT_COMMAND_READ_BIT)
    accessFlags.push_back("VK_ACCESS_INDIRECT_COMMAND_READ_BIT");
  if (flags & VK_ACCESS_MEMORY_READ_BIT)
    accessFlags.push_back("VK_ACCESS_MEMORY_READ_BIT");
  if (flags & VK_ACCESS_MEMORY_WRITE_BIT)
    accessFlags.push_back("VK_ACCESS_MEMORY_WRITE_BIT");

  if (accessFlags.empty())
    return "0";

  std::string result;
  for (size_t i = 0; i < accessFlags.size(); ++i)
  {
    result += accessFlags[i];
    if (i < accessFlags.size() - 1)
      result += " | ";
  }
  return result;
}

// Helper function to convert VkPipelineStageFlags to string
static std::string vkPipelineStageFlagsToString(VkPipelineStageFlags flags)
{
  if (flags == 0)
    return "0";

  std::vector<std::string> stageFlags;

  if (flags & VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT");
  if (flags & VK_PIPELINE_STAGE_VERTEX_INPUT_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_VERTEX_INPUT_BIT");
  if (flags & VK_PIPELINE_STAGE_VERTEX_SHADER_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_VERTEX_SHADER_BIT");
  if (flags & VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT");
  if (flags & VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT");
  if (flags & VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT");
  if (flags & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT");
  if (flags & VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT");
  if (flags & VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT");
  if (flags & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT");
  if (flags & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT");
  if (flags & VK_PIPELINE_STAGE_TRANSFER_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_TRANSFER_BIT");
  if (flags & VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT");
  if (flags & VK_PIPELINE_STAGE_HOST_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_HOST_BIT");
  if (flags & VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT");
  if (flags & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
    stageFlags.push_back("VK_PIPELINE_STAGE_ALL_COMMANDS_BIT");

  if (stageFlags.empty())
    return "0";

  std::string result;
  for (size_t i = 0; i < stageFlags.size(); ++i)
  {
    result += stageFlags[i];
    if (i < stageFlags.size() - 1)
      result += " | ";
  }
  return result;
}

// Helper function to convert VkImageLayout to string
static const char *vkImageLayoutToString(VkImageLayout layout)
{
  switch (layout)
  {
  case VK_IMAGE_LAYOUT_UNDEFINED:
    return "VK_IMAGE_LAYOUT_UNDEFINED";
  case VK_IMAGE_LAYOUT_GENERAL:
    return "VK_IMAGE_LAYOUT_GENERAL";
  case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    return "VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL";
  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    return "VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    return "VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL";
  case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    return "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL";
  case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    return "VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL";
  case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    return "VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL";
  case VK_IMAGE_LAYOUT_PREINITIALIZED:
    return "VK_IMAGE_LAYOUT_PREINITIALIZED";
  case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
    return "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR";
  default:
    return "VK_IMAGE_LAYOUT_UNKNOWN";
  }
}

// Helper function to convert VkImageAspectFlags to string
static std::string vkImageAspectFlagsToString(VkImageAspectFlags flags)
{
  if (flags == 0)
    return "0";

  std::vector<std::string> aspectFlags;

  if (flags & VK_IMAGE_ASPECT_COLOR_BIT)
    aspectFlags.push_back("VK_IMAGE_ASPECT_COLOR_BIT");
  if (flags & VK_IMAGE_ASPECT_DEPTH_BIT)
    aspectFlags.push_back("VK_IMAGE_ASPECT_DEPTH_BIT");
  if (flags & VK_IMAGE_ASPECT_STENCIL_BIT)
    aspectFlags.push_back("VK_IMAGE_ASPECT_STENCIL_BIT");
  if (flags & VK_IMAGE_ASPECT_METADATA_BIT)
    aspectFlags.push_back("VK_IMAGE_ASPECT_METADATA_BIT");

  if (aspectFlags.empty())
    return "0";

  std::string result;
  for (size_t i = 0; i < aspectFlags.size(); ++i)
  {
    result += aspectFlags[i];
    if (i < aspectFlags.size() - 1)
      result += " | ";
  }
  return result;
}

#endif // VULKAN_RHI_LOGS

// Modified barrier functions with logging

void VulkanRHI::cmdBufferBarrier(
    CommandBuffer cmd,
    BufferId b,
    PipelineStage src_stage,
    PipelineStage dst_stage,
    AccessPattern src_access,
    AccessPattern dst_access,
    uint32_t offset,
    uint32_t size,
    Queue src_queue_family,
    Queue dst_queue_family)
{
  auto *commandBuffer = getCommandBufferHandle(cmd);
  auto buffer = getVulkanBuffer(b);
  uint32_t queueFamilySrc = VK_QUEUE_FAMILY_IGNORED;
  uint32_t queueFamilyDst = VK_QUEUE_FAMILY_IGNORED;

  switch (src_queue_family)
  {
  case Queue::Compute:
    queueFamilySrc = indices.computeFamily;
    break;
  case Queue::Graphics:
    queueFamilySrc = indices.graphicsFamily;
    break;
  case Queue::Transfer:
    queueFamilySrc = indices.transferFamily;
    break;
  default:
    queueFamilySrc = VK_QUEUE_FAMILY_IGNORED;
    break;
  }

  switch (dst_queue_family)
  {
  case Queue::Compute:
    queueFamilyDst = indices.computeFamily;
    break;
  case Queue::Graphics:
    queueFamilyDst = indices.graphicsFamily;
    break;
  case Queue::Transfer:
    queueFamilyDst = indices.transferFamily;
    break;
  default:
    queueFamilyDst = VK_QUEUE_FAMILY_IGNORED;
    break;
  }

  sanitizeQueueFamilyOwnership(buffer.sharingMode, queueFamilySrc, queueFamilyDst);

  VkBufferMemoryBarrier barrier = createBufferBarrier(buffer.buffer, src_stage, dst_stage, src_access, dst_access, offset, size, queueFamilySrc, queueFamilyDst);

#ifdef VULKAN_RHI_LOGS
  VkPipelineStageFlags vkSrcStage = toVulkanStage(src_stage);
  VkPipelineStageFlags vkDstStage = toVulkanStage(dst_stage);
  VkAccessFlags vkSrcAccess = toVulkanAccess(src_access);
  VkAccessFlags vkDstAccess = toVulkanAccess(dst_access);

  os::Logger::logf(
      "[VulkanRHI][Barrier][Buffer] vkCmdPipelineBarrier\n"
      "  srcStage: %s\n"
      "  dstStage: %s\n"
      "  srcAccess: %s\n"
      "  dstAccess: %s\n"
      "  offset: %u, size: %s\n"
      "  srcQueueFamily: %u, dstQueueFamily: %u",
      vkPipelineStageFlagsToString(vkSrcStage).c_str(),
      vkPipelineStageFlagsToString(vkDstStage).c_str(),
      vkAccessFlagsToString(vkSrcAccess).c_str(),
      vkAccessFlagsToString(vkDstAccess).c_str(),
      offset,
      size == VK_WHOLE_SIZE ? "VK_WHOLE_SIZE" : std::to_string(size).c_str(),
      queueFamilySrc,
      queueFamilyDst);
#endif

  vkCmdPipelineBarrier(commandBuffer->commandBuffer, toVulkanStage(src_stage), toVulkanStage(dst_stage), VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 1, &barrier, 0, nullptr);
}

void VulkanRHI::cmdImageBarrier(
    CommandBuffer cmd,
    TextureId image,
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
    Queue src_queue_family,
    Queue dst_queue_family)
{
  auto *commandBuffer = getCommandBufferHandle(cmd);
  auto vkImage = getVulkanTexture(image);

  uint32_t queueFamilySrc = VK_QUEUE_FAMILY_IGNORED;
  uint32_t queueFamilyDst = VK_QUEUE_FAMILY_IGNORED;

  switch (src_queue_family)
  {
  case Queue::Compute:
    queueFamilySrc = indices.computeFamily;
    break;
  case Queue::Graphics:
    queueFamilySrc = indices.graphicsFamily;
    break;
  case Queue::Transfer:
    queueFamilySrc = indices.transferFamily;
    break;
  default:
    queueFamilySrc = VK_QUEUE_FAMILY_IGNORED;
    break;
  }

  switch (dst_queue_family)
  {
  case Queue::Compute:
    queueFamilyDst = indices.computeFamily;
    break;
  case Queue::Graphics:
    queueFamilyDst = indices.graphicsFamily;
    break;
  case Queue::Transfer:
    queueFamilyDst = indices.transferFamily;
    break;
  default:
    queueFamilyDst = VK_QUEUE_FAMILY_IGNORED;
    break;
  }

  sanitizeQueueFamilyOwnership(vkImage.sharingMode, queueFamilySrc, queueFamilyDst);

  VkImageAspectFlags vkAspectMask = imageAspectFlagsToVkImageAspectFlags(aspect_mask);

  VkImageMemoryBarrier barrier =
      createImageBarrier(vkImage.image, src_stage, dst_stage, src_access, dst_access, old_layout, new_layout, vkAspectMask, base_mip_level, level_count, base_array_layer, layer_count, queueFamilySrc, queueFamilyDst);

#ifdef VULKAN_RHI_LOGS
  VkPipelineStageFlags vkSrcStage = toVulkanStage(src_stage);
  VkPipelineStageFlags vkDstStage = toVulkanStage(dst_stage);
  VkAccessFlags vkSrcAccess = toVulkanAccess(src_access);
  VkAccessFlags vkDstAccess = toVulkanAccess(dst_access);
  VkImageLayout vkOldLayout = toVulkanLayout(old_layout);
  VkImageLayout vkNewLayout = toVulkanLayout(new_layout);

  os::Logger::logf(
      "[VulkanRHI][Barrier][Image] vkCmdPipelineBarrier\n"
      "  srcStage: %s\n"
      "  dstStage: %s\n"
      "  srcAccess: %s\n"
      "  dstAccess: %s\n"
      "  oldLayout: %s\n"
      "  newLayout: %s\n"
      "  aspectMask: %s\n"
      "  baseMipLevel: %u, levelCount: %s\n"
      "  baseArrayLayer: %u, layerCount: %s\n"
      "  srcQueueFamily: %u, dstQueueFamily: %u",
      vkPipelineStageFlagsToString(vkSrcStage).c_str(),
      vkPipelineStageFlagsToString(vkDstStage).c_str(),
      vkAccessFlagsToString(vkSrcAccess).c_str(),
      vkAccessFlagsToString(vkDstAccess).c_str(),
      vkImageLayoutToString(vkOldLayout),
      vkImageLayoutToString(vkNewLayout),
      vkImageAspectFlagsToString(vkAspectMask).c_str(),
      base_mip_level,
      level_count == VK_REMAINING_MIP_LEVELS ? "VK_REMAINING_MIP_LEVELS" : std::to_string(level_count).c_str(),
      base_array_layer,
      layer_count == VK_REMAINING_ARRAY_LAYERS ? "VK_REMAINING_ARRAY_LAYERS" : std::to_string(layer_count).c_str(),
      queueFamilySrc,
      queueFamilyDst);
#endif

  vkCmdPipelineBarrier(commandBuffer->commandBuffer, toVulkanStage(src_stage), toVulkanStage(dst_stage), VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanRHI::cmdMemoryBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access)
{
  auto *commandBuffer = getCommandBufferHandle(cmd);
  VkMemoryBarrier barrier = createMemoryBarrier(src_access, dst_access);

#ifdef VULKAN_RHI_LOGS
  VkPipelineStageFlags vkSrcStage = toVulkanStage(src_stage);
  VkPipelineStageFlags vkDstStage = toVulkanStage(dst_stage);
  VkAccessFlags vkSrcAccess = toVulkanAccess(src_access);
  VkAccessFlags vkDstAccess = toVulkanAccess(dst_access);

  os::Logger::logf(
      "[VulkanRHI][Barrier][Memory] vkCmdPipelineBarrier\n"
      "  srcStage: %s\n"
      "  dstStage: %s\n"
      "  srcAccess: %s\n"
      "  dstAccess: %s",
      vkPipelineStageFlagsToString(vkSrcStage).c_str(),
      vkPipelineStageFlagsToString(vkDstStage).c_str(),
      vkAccessFlagsToString(vkSrcAccess).c_str(),
      vkAccessFlagsToString(vkDstAccess).c_str());
#endif

  vkCmdPipelineBarrier(commandBuffer->commandBuffer, toVulkanStage(src_stage), toVulkanStage(dst_stage), VK_DEPENDENCY_BY_REGION_BIT, 1, &barrier, 0, nullptr, 0, nullptr);
}

void VulkanRHI::cmdPipelineBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access)
{
  auto *commandBuffer = getCommandBufferHandle(cmd);

  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = toVulkanAccess(src_access);
  barrier.dstAccessMask = toVulkanAccess(dst_access);

#ifdef VULKAN_RHI_LOGS
  VkPipelineStageFlags vkSrcStage = toVulkanStage(src_stage);
  VkPipelineStageFlags vkDstStage = toVulkanStage(dst_stage);
  VkAccessFlags vkSrcAccess = barrier.srcAccessMask;
  VkAccessFlags vkDstAccess = barrier.dstAccessMask;

  os::Logger::logf(
      "[VulkanRHI][Barrier][Pipeline] vkCmdPipelineBarrier\n"
      "  srcStage: %s\n"
      "  dstStage: %s\n"
      "  srcAccess: %s\n"
      "  dstAccess: %s",
      vkPipelineStageFlagsToString(vkSrcStage).c_str(),
      vkPipelineStageFlagsToString(vkDstStage).c_str(),
      vkAccessFlagsToString(vkSrcAccess).c_str(),
      vkAccessFlagsToString(vkDstAccess).c_str());
#endif

  vkCmdPipelineBarrier(commandBuffer->commandBuffer, toVulkanStage(src_stage), toVulkanStage(dst_stage), VK_DEPENDENCY_BY_REGION_BIT, 1, &barrier, 0, nullptr, 0, nullptr);
}

} // namespace vulkan
} // namespace backend
} // namespace rendering
