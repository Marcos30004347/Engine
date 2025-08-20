#pragma once

#include "datastructure/ConcurrentQueue.hpp"
#include "datastructure/ThreadLocalStorage.hpp"

#include "rhi/Device.hpp"

#include <map>
#include <stack>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_format_traits.hpp>

#include "rhi/EventLoop.hpp"
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

struct VulkanBuffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkDeviceSize size;
  void *mapped = nullptr;
};

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

struct VulkanCommandBuffer : public CommandBufferImp
{
  VkCommandBuffer commandBuffer;
  VkCommandPool commandPool;
  VulkanGraphicsPipeline *boundGraphicsPipeline;
  VulkanComputePipeline *boundComputePipeline;
  std::vector<VulkanCommandBufferRenderPass> renderPasses;
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
  std::vector<VkFramebuffer> framebuffers;
  std::vector<TextureView> views;

  VulkanAsyncHandler(VulkanDevice *, VkFence, std::vector<VkFramebuffer> &, std::vector<TextureView> &);
  static FenceStatus getStatus(VulkanAsyncHandler &future);
};

class VulkanFuture : public detail::GPUFutureImp
{
public:
  AsyncEvent<VulkanAsyncHandler> handler;
  VulkanFuture(AsyncEvent<VulkanAsyncHandler> &&handler);
};

struct VulkanBindingGroups : public BindingGroupsImp
{
  std::vector<VkDescriptorPool> descriptorPools;
  std::vector<VkDescriptorSet> descriptorSets;
};

class VulkanHeap : public GPUHeap
{
public:
  std::atomic<BufferMap> mapped;
  VulkanDevice *device;
  VkBuffer buffer;
  BufferUsage usage;
  VkDeviceMemory deviceMemory;
  VulkanHeap(VulkanDevice *device, VkDeviceMemory, VkBuffer, size_t, BufferUsage);
  ~VulkanHeap();
};

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

  GPUHeap *allocateHeap(size_t size, BufferUsage, void *) override;
  void freeHeap(GPUHeap *) override;
  BufferMapStatus mapBuffer(GPUBuffer, BufferMap, void **) override;
  void unmapBuffer(GPUBuffer) override;

  // void destroyBuffer(BufferHandle) override;

  SwapChain createSwapChain(Surface surface, uint32_t width, uint32_t height) override;
  void destroySwapChain(SwapChain swapChain) override;

  // TODO: view not being deallocated
  TextureView getCurrentSwapChainTextureView(SwapChain) override;

  void destroyShader(Shader handle) override;

  BindingsLayout createBindingsLayout(const BindingsLayoutInfo &) override;
  void destroyBindingsLayout(BindingsLayout) override;

  BindingGroups createBindingGroups(const BindingsLayout layout, const BindingGroupsInfo &info) override;
  void destroyBindingGroups(BindingGroups bindingGroup) override;

  GraphicsPipeline createGraphicsPipeline(GraphicsPipelineInfo) override;
  void destroyGraphicsPipeline(GraphicsPipeline) override;

  ComputePipeline createComputePipeline(const ComputePipelineInfo &info) override;
  void destroyComputePipeline(ComputePipeline) override;

  CommandBuffer createCommandBuffer() override;
  void destroyCommandBuffer(CommandBuffer) override;

  Sampler createSampler(const SamplerCreateInfo &info) override;
  void destroySampler(Sampler handle) override;

  void beginCommandBuffer(CommandBuffer) override;
  void endCommandBuffer(CommandBuffer) override;

  void cmdCopyBuffer(CommandBuffer cmdBuffer, GPUBuffer src, GPUBuffer dst, uint32_t srcOffset, uint32_t dstOffset, uint32_t size) override;

  void cmdBeginRenderPass(CommandBuffer, const RenderPassInfo &) override;
  void cmdEndRenderPass(CommandBuffer) override;

  void cmdBindBindingGroups(CommandBuffer cmdBuffer, BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount) override;
  void cmdBindGraphicsPipeline(CommandBuffer, GraphicsPipeline) override;
  void cmdBindComputePipeline(CommandBuffer, ComputePipeline) override;

  void cmdBindVertexBuffer(CommandBuffer, uint32_t slot, GPUBuffer) override;
  void cmdBindIndexBuffer(CommandBuffer, GPUBuffer, Type type) override;

  void cmdDraw(CommandBuffer, uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) override;

  void cmdDrawIndexed(CommandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) override;

  void cmdDrawIndexedIndirect(CommandBuffer, GPUBuffer indirectBuffer, size_t offset, uint32_t drawCount, uint32_t stride) override;
  void cmdDispatch(CommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;

  // Submission
  GPUFuture submit(QueueHandle queue, CommandBuffer *commandBuffers, uint32_t count) override;
 
  // Sync
  void waitIdle() override;
  void tick() override;

  QueueHandle getQueue(QueueType) override;
  // void submitCommandBuffer(QueueHandle, CommandBuffer) override;

  Texture createImage(const ImageCreateInfo &info) override;
  void destroyImage(Texture handle) override;

  TextureView createImageView(Texture imageHandle, ImageAspectFlags aspectFlags) override;
  void destroyImageView(TextureView handle) override;

  void wait(GPUFuture &future) override;

  // Vulkan specific
  Surface addSurface(VkSurfaceKHR surface);
  Shader createShader(const VulkanSpirVShaderData &data);

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
  EventLoop<VulkanAsyncHandler> eventLoop;

  // Device info
  bool initialized;

  VulkanVersion version;
  DeviceRequiredLimits requiredLimits;
  uint64_t requestedFeaturesFlags;
  DeviceProperties properties;

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

  VkCommandPool createCommandPool(uint32_t queueFamilyIndex);
  void destroyCommandPool(VkCommandPool cp);
  std::vector<VkCommandBuffer> allocateCommandBuffers(VkCommandPool commandPool, uint32_t count, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);
  void freeCommandBuffers(VkCommandPool cp, std::vector<VkCommandBuffer> commandBuffers);

  static void cleanupSubmitCallback(VulkanAsyncHandler &);
};
} // namespace vulkan
} // namespace rhi