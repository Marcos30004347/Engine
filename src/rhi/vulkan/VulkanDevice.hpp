#pragma once

#include "datastructure/ConcurrentQueue.hpp"
#include "datastructure/ThreadLocalStorage.hpp"

#include "rhi/Device.hpp"

#include <map>
#include <stack>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_format_traits.hpp>

#include "rhi/BufferAllocator.hpp"
#include "rendering/gpu/EventLoop.hpp"

#ifdef VULKAN_DEVICE_LOG
#include "os/Logger.hpp"
#endif

namespace rhi
{
namespace vulkan
{

enum VulkanVersion
{
  Vulkan_1_2,
  Vulkan_1_3,
};

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif

struct VulkanQueueFamilyIndices
{
  bool hasGraphicsFamily = false;
  bool hasComputeFamily = false;
  bool hasTransferFamily = false;

  uint32_t graphicsFamily;
  uint32_t computeFamily;
  uint32_t transferFamily;
  uint32_t graphicsQueueCount;
  uint32_t computeQueueCount;
  uint32_t transferQueueCount;
  // uint32_t presentFamily;
};

struct SwapChainSupportDetails
{
  VkSurfaceCapabilitiesKHR capabilities;
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> presentModes;
};

// struct VulkanBuffer
// {
//   VkBuffer buffer;
//   VkDeviceMemory memory;
//   VkDeviceSize size;
//   void *mapped = nullptr;
// };

struct VulkanQueue
{
  VkQueue queue;
};

struct VulkanSurface : public SurfaceImp
{
public:
  VkSurfaceKHR surfaces;
  uint32_t presentFamily;
  VulkanQueue presentQueue;
  bool hasPresentFamily = false;
};

struct VulkanSwapChain : public SwapChainImp
{
  VkSwapchainKHR swapChain;
  VkFormat swapChainImageFormat;
  VkExtent2D swapChainExtent;
  SwapChainSupportDetails support;

  VulkanQueue presentQueue;

  std::vector<TextureView> swapChainImages;
  std::vector<VkSemaphore> achireSemaphores;
  std::vector<VkSemaphore> presentSemaphores;

  std::atomic<uint32_t> currentPrimitive;
  VulkanSwapChain &operator=(VulkanSwapChain &);
};

struct VulkanTextureViewRender
{
  std::unordered_map<VkRenderPass, VkFramebuffer> frameBuffers;
  uint32_t swapChainImageIndex = UINT32_MAX;
  VulkanSwapChain *swapChain = NULL;
};

struct VulkanTextureView : public TextureViewImp
{
  VkImageView view;
  std::atomic<VkFence> fence;
  std::atomic<VkSemaphore> achireSemaphore;
  std::atomic<VkSemaphore> presentSemaphore;
  VulkanTextureViewRender renderData;
};

struct VulkanPhysicalDevice
{
  VkPhysicalDevice device;
  DeviceFeatures feature_flags;
  DeviceProperties properties;
};

struct VulkanLogicalDevice
{
  VkDevice device;
};

struct VulkanGraphicsPipeline : public GraphicsPipelineImp
{
  VkPipeline pipeline;
  VkRenderPass renderPass;
  BindingsLayout layout;
  GraphicsPipelineInfo info;
};

struct VulkanBindingsLayout : public BindingsLayoutImp
{
  VkPipelineLayout pipelineLayout;
  std::vector<VkDescriptorSetLayout> descriptorSetLayouts;
  BindingsLayoutInfo info;
};

struct VulkanSpirVShaderData
{
  std::string src;
};

struct VulkanShader : public ShaderImp
{
  VkShaderModule shaderModule;
};

struct VulkanAttatchment
{
  VulkanSwapChain *swapChain;
  uint32_t swapChainImageIndex;
  VkQueue presentQueue;
};

struct VulkanCommandBufferRenderPass
{
  VkRenderPass renderPass;
  VkFramebuffer frameBuffer;
  std::vector<VkSemaphore> achireSemaphores;
  std::vector<VkSemaphore> presentSemaphores;
  std::vector<TextureView> views;
  std::vector<VulkanAttatchment> attatchments;
};

struct VulkanComputePipeline : public ComputePipelineImp
{
  VkPipeline pipeline;
  BindingsLayout layout;
};

struct VulkanImage : public TextureImp
{
  VkImage image;
  VkDeviceMemory memory;
  VkFormat format;
  uint32_t width;
  uint32_t height;
};

class VulkanDevice;

class VulkanAsyncHandler
{
public:
  using VulkanFutureCallback = void (*)(VulkanDevice *);

  VulkanDevice *device;

  VkFence fence;
  VkSemaphore semaphore;

  std::vector<VkFramebuffer> framebuffers;
  std::vector<TextureView> views;

  VulkanAsyncHandler(VulkanDevice *, VkFence, VkSemaphore, std::vector<VkFramebuffer> &, std::vector<TextureView> &);
  static rendering::FenceStatus getStatus(VulkanAsyncHandler &future);
};

class VulkanFuture : public detail::GPUFutureImp
{
public:
  rendering::AsyncEvent<VulkanAsyncHandler> handler;
  VulkanFuture(rendering::AsyncEvent<VulkanAsyncHandler> &&handler);
};

struct VulkanBindingGroups : public BindingGroupsImp
{
  std::vector<VkDescriptorPool> descriptorPools;
  std::vector<VkDescriptorSet> descriptorSets;
};

struct VulkanCommandBuffer : public CommandBufferImp
{
  VkCommandBuffer commandBuffer;
  VkCommandPool commandPool;
  VulkanGraphicsPipeline *boundGraphicsPipeline;
  VulkanComputePipeline *boundComputePipeline;
  VulkanBindingGroups *boundGroups;
  std::vector<VulkanCommandBufferRenderPass> renderPasses;
};
class VulkanBuffer : public BufferImp
{
public:
  std::atomic<BufferMap> mapped;
  VulkanDevice *device;
  VkBuffer buffer;
  BufferUsage usage;
  VkDeviceMemory deviceMemory;

  VulkanBuffer(VulkanDevice *device, VkDeviceMemory, VkBuffer, size_t, BufferUsage);
  ~VulkanBuffer();
};

// class VulkanBufferView : public BufferViewImp {
//   public:
//   Buffer parent;
//   VulkanBufferView(Buffer parent);
// };

struct VulkanSampler : public SamplerImp
{
  VkSampler sampler;
};

class VulkanDevice : public Device
{
  friend class VulkanAsyncHandler;

public:
  VulkanDevice(VulkanVersion, DeviceRequiredLimits, DeviceFeatures, std::vector<std::string> extensions);
  ~VulkanDevice();

  // SurfaceHandle addWindowForDrawing(window::Window *) override;
  Format getSwapChainFormat(SwapChain) override;

  Buffer createBuffer(const BufferInfo &, void *) override;

  void destroyBuffer(BufferImp *) override;
  BufferMapStatus mapBuffer(BufferView, BufferMap, void **) override;
  void unmapBuffer(BufferView) override;

  // BufferView createBufferView(Buffer& buffer, const uint32_t size) override;
  // void destroyBufferView(BufferViewImp *) override;

  // void destroyBuffer(BufferHandle) override;

  SwapChain createSwapChain(Surface surface, uint32_t width, uint32_t height) override;
  void destroySwapChain(SwapChainImp *swapChain) override;

  // TODO: view not being deallocated
  TextureView getCurrentSwapChainTextureView(SwapChain) override;

  void destroyShader(ShaderImp *handle) override;

  BindingsLayout createBindingsLayout(const BindingsLayoutInfo &) override;
  void destroyBindingsLayout(BindingsLayoutImp *) override;

  BindingGroups createBindingGroups(const BindingGroupsInfo &info) override;
  void destroyBindingGroups(BindingGroupsImp *bindingGroup) override;

  GraphicsPipeline createGraphicsPipeline(GraphicsPipelineInfo) override;
  void destroyGraphicsPipeline(GraphicsPipelineImp *) override;

  ComputePipeline createComputePipeline(const ComputePipelineInfo &info) override;
  void destroyComputePipeline(ComputePipelineImp *) override;

  CommandBuffer createCommandBuffer(const CommandBufferInfo &) override;
  void destroyCommandBuffer(CommandBufferImp *) override;

  Sampler createSampler(const SamplerInfo &info) override;
  void destroySampler(SamplerImp *handle) override;

  void beginCommandBuffer(CommandBuffer) override;
  void endCommandBuffer(CommandBuffer) override;

  void cmdCopyBuffer(CommandBuffer cmdBuffer, BufferView src, BufferView dst, uint32_t srcOffset, uint32_t dstOffset, uint32_t size) override;

  void cmdBeginRenderPass(CommandBuffer, const RenderPassInfo &) override;
  void cmdEndRenderPass(CommandBuffer) override;

  void cmdBindBindingGroups(CommandBuffer cmdBuffer, BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount) override;
  void cmdBindGraphicsPipeline(CommandBuffer, GraphicsPipeline) override;
  void cmdBindComputePipeline(CommandBuffer, ComputePipeline) override;

  void cmdBindVertexBuffer(CommandBuffer, uint32_t slot, BufferView) override;
  void cmdBindIndexBuffer(CommandBuffer, BufferView, Type type) override;

  void cmdDraw(CommandBuffer, uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) override;

  void cmdDrawIndexed(CommandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) override;

  void cmdDrawIndexedIndirect(CommandBuffer, BufferView indirectBuffer, size_t offset, uint32_t drawCount, uint32_t stride) override;
  void cmdDispatch(CommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;

  void cmdImageBarrier(
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
      uint32_t dst_queue_family) override;

  void cmdBufferBarrier(
      CommandBuffer cmd,
      Buffer b,
      PipelineStage src_stage,
      PipelineStage dst_stage,
      AccessPattern src_access,
      AccessPattern dst_access,
      uint32_t offset,
      uint32_t size,
      uint32_t src_queue_family,
      uint32_t dst_queue_family) override;

  void cmdMemoryBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access) override;
  void cmdPipelineBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access) override;

  // Submission
  GPUFuture submit(QueueHandle queue, CommandBuffer *commandBuffers, uint32_t count, GPUFuture *wait = NULL) override;

  // Sync
  void waitIdle() override;
  void tick() override;

  QueueHandle getQueue(QueueType) override;
  // void submitCommandBuffer(QueueHandle, CommandBuffer) override;

  Texture createTexture(const TextureInfo &info) override;
  void destroyTexture(TextureImp *handle) override;

  TextureView createTextureView(const TextureViewInfo &info) override;
  void destroyTextureView(TextureViewImp *handle) override;

  void wait(GPUFuture &future) override;

  // Vulkan specific
  Surface addSurface(VkSurfaceKHR surface, const SurfaceInfo &info);
  Shader createShader(const VulkanSpirVShaderData &data, const BindingsLayoutInfo &interface);

  inline VkInstance getInstance()
  {
    return instance;
  }
  inline VkDevice getDevice()
  {
    return logicalDevice.device;
  }

  inline void enqueueFreeSubmission()
  {
  }

private:
  rendering::EventLoop<VulkanAsyncHandler> eventLoop;

  // Device info
  bool initialized;

  VulkanVersion version;
  DeviceRequiredLimits requiredLimits;
  uint64_t requestedFeaturesFlags;

  std::vector<const char *> validationLayers = {
    "VK_LAYER_KHRONOS_validation",
  };

  std::vector<char *> instanceExtensions = {};

  std::vector<const char *> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
  };

  // Vulkan Core
  VkInstance instance;

  VulkanPhysicalDevice physicalDevice;
  VulkanLogicalDevice logicalDevice;

  std::vector<Surface> surfaces;

  // Debug
  VkDebugUtilsMessengerEXT debugMessenger;

  lib::ConcurrentQueue<VkFence> *fences;
  lib::ConcurrentQueue<VkSemaphore> *semaphores;

  VulkanQueueFamilyIndices indices;

  std::vector<QueueHandle> graphicsQueue;
  std::vector<QueueHandle> computeQueue;
  std::vector<QueueHandle> transferQueue;
  std::vector<VulkanQueue> queues;

  // lib::ThreadLocalStorage<GraphicsPipeline> *boundGraphicsPipeline;

  // Tracking
  // std::stack<SurfaceHandle> frameBuffersResized;
  void init() override;

  void setupDebugMessenger();
  void pickPhysicalDevice();
  void createLogicalDevice();
  void initializePhysicalDevice();
  void initializeInstance(VulkanVersion version);
  bool checkValidationLayerSupport();

  Format vkFormatToFormat(VkFormat vkFmt);
  VkFormat formatToVkFormat(Format fmt);
  VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats);
  VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR> &availablePresentModes);
  VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities, uint32_t width, uint32_t height);
  uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
  VulkanQueueFamilyIndices findQueueFamilyIndices();

  VkDescriptorSetLayout createDescriptorSetLayoutFromGroup(const BindingGroupLayout &group);
  void collectDescriptorSetLayouts(const BindingsLayoutInfo &info, std::vector<VkDescriptorSetLayout> &outLayouts);
  VkRenderPass createRenderPass(ColorAttatchment *attachments, uint32_t attatchmentsCount, DepthAttatchment depth);

  VkFence getFence();
  VkSemaphore getSemaphore();

  VkCommandPool createCommandPool(uint32_t queueFamilyIndex);
  void destroyCommandPool(VkCommandPool cp);
  std::vector<VkCommandBuffer> allocateCommandBuffers(VkCommandPool commandPool, uint32_t count, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);
  void freeCommandBuffers(VkCommandPool cp, std::vector<VkCommandBuffer> commandBuffers);

  static void cleanupSubmitCallback(VulkanAsyncHandler &);
};
} // namespace vulkan
} // namespace rhi