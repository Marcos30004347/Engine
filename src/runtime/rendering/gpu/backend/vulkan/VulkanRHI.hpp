#include "datastructure/ConcurrentHashMap.hpp"
#include "rendering/gpu/EventLoop.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include <functional>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>

namespace rendering
{
namespace backend
{
namespace vulkan
{

enum VulkanVersion
{
  Vulkan_1_2,
  Vulkan_1_3,
};

struct VulkanSwapChain;

struct VulkanBuffer
{
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  VkBufferUsageFlags usageFlags = 0;
  VkMemoryPropertyFlags memoryFlags = 0;
  VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  void *mapped = nullptr;
  BufferInfo info;
};

struct VulkanTexture
{
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkExtent3D extent{};
  uint32_t mipLevels = 1;
  VkImageUsageFlags usageFlags = 0;
  VkMemoryPropertyFlags memoryFlags = 0;
  VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VulkanSwapChain *swapChainOwner = nullptr;
  uint32_t swapChainImageIndex = UINT32_MAX;

  TextureId id = TextureId::Invalid;
  TextureInfo info;
};
struct VulkanTextureViewRender
{
  std::unordered_map<VkRenderPass, VkFramebuffer> frameBuffers;
  uint32_t swapChainImageIndex = UINT32_MAX;
  VulkanSwapChain *swapChain = NULL;
};
struct VulkanTextureView
{
  VkImageView view = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageSubresourceRange range{};
  TextureView original;
  VulkanTextureViewRender renderData;
};

struct VulkanSampler
{
  VkSampler sampler = VK_NULL_HANDLE;
  SamplerInfo info;
};

struct VulkanBindingsLayout
{
  std::string name;
  VkPipelineLayout pipelineLayout;
  std::vector<VkDescriptorSetLayout> setLayouts;
  std::vector<BindingGroupLayout> groups;
};

struct VulkanBindingGroup
{
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> descriptorSets;
  std::vector<VulkanTextureView> textureViews;
  GroupInfo info;
};

struct VulkanBindingGroups
{
  BindingGroupsInfo info;
  std::vector<VulkanBindingGroup> groups;
};

struct VulkanSurface
{
public:
  VkSurfaceKHR surface;
  uint32_t presentFamily;
  VkQueue presentQueue;
  bool hasPresentFamily = false;
};

struct VulkanShader
{
  VkShaderModule shaderModule;
  ShaderInfo info;
};

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
};

struct VulkanSwapChainSupportDetails
{
  VkSurfaceCapabilitiesKHR capabilities;
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> presentModes;
};

struct VulkanSwapChain
{
  VkSwapchainKHR swapChain;
  VkFormat swapChainImageFormat;
  VkExtent2D swapChainExtent;
  VulkanSwapChainSupportDetails support;

  uint32_t width;
  uint32_t height;

  VkQueue presentQueue;
  std::vector<VulkanTexture *> swapChainImages;
  std::vector<VulkanTextureView *> swapChainImageViews;
  std::vector<VkSemaphore> acquireSemaphores;
  std::vector<bool> acquireSemaphorePending;
  uint32_t nextAcquireSemaphoreIndex = 0u;
  uint32_t currentAcquiredSemaphoreIndex = 0u;
  uint32_t currentAcquiredImageIndex = UINT32_MAX;
  std::vector<VkSemaphore> presentSemaphores;
  std::vector<VkCommandPool> overlayCommandPools;
  std::vector<VkCommandBuffer> overlayCommandBuffers;
  std::vector<VkFence> overlayFences;

  // std::atomic<uint32_t> currentPrimitive;
  VulkanSwapChain &operator=(VulkanSwapChain &);
};

struct VulkanGraphicsPipeline
{
  VkPipeline pipeline;
  VkRenderPass renderPass;
  BindingsLayout layout;
  GraphicsPipelineInfo info;
};

struct VulkanComputePipeline
{
  VkPipeline pipeline;
  BindingsLayout layout;
  ComputePipelineInfo info;
};

struct VulkanAttatchment
{
  SwapChain swapChain;
  uint32_t swapChainImageIndex;
  VkQueue presentQueue;
};

struct VulkanCommandBufferRenderPass
{
  VkRenderPass renderPass;
  VkFramebuffer frameBuffer;
  std::vector<VulkanTextureView> views;
};

struct VulkanCommandPool
{
  VkCommandPool commandPool;
};
struct VulkanCommandBuffer
{
  bool submited;

  VkFence fence;
  Queue queue;
  VkCommandBuffer commandBuffer;
  VulkanCommandPool commandPool;
  bool hasGraphicsPipeline = false;
  bool hascomputePipeline = false;
  GraphicsPipeline boundGraphicsPipeline;
  ComputePipeline boundComputePipeline;
  std::vector<VulkanCommandBufferRenderPass> renderPasses;
};

struct VulkanTimer
{
  TimerInfo info;
  uint32_t firstQuery = 0u;
  uint32_t queryCount = 0u;
};

struct VulkanTimerRange
{
  uint32_t firstQuery = 0u;
  uint32_t queryCount = 0u;
};

struct VulkanTimerArena
{
  VkQueryPool queryPool = VK_NULL_HANDLE;
  BufferId valuesBuffer = BufferId::Invalid;
  uint32_t maxTimers = 0u;
  uint32_t totalQueries = 0u;
  uint32_t activeTimers = 0u;
  std::vector<VulkanTimerRange> freeRanges;
};

class VulkanRHI : public RHI
{
private:
  class VulkanAsyncHandler
  {
  public:
    using VulkanFutureCallback = void (*)(VulkanRHI *);

    VulkanRHI *device;

    VkFence fence;
    VkSemaphore semaphore;
    std::vector<CommandBuffer> commandBuffers;
    bool releaseCommandBuffersOnCompletion = true;
    // bool valid();
    VulkanAsyncHandler(VulkanRHI *, std::vector<CommandBuffer>, VkFence, VkSemaphore, bool releaseCommandBuffersOnCompletion);
    static rendering::FenceStatus getStatus(VulkanAsyncHandler &future);
  };

  // class VulkanFuture : public GPUFutureImp
  // {
  // public:
  //   rendering::AsyncEvent<VulkanAsyncHandler> handler;
  //   VulkanFuture(rendering::AsyncEvent<VulkanAsyncHandler> &&handler);
  // };

  VulkanVersion version;
  VkInstance instance = VK_NULL_HANDLE;

  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
  bool validationLayersEnabled = false;

  VulkanQueueFamilyIndices indices;

  std::vector<VulkanSurface> surfaces;

  std::vector<VkQueue> graphicsQueue;
  std::vector<VkQueue> computeQueue;
  std::vector<VkQueue> transferQueue;

  EventLoop<VulkanRHI::VulkanAsyncHandler> eventLoop;

  std::vector<const char *> validationLayers = {
    "VK_LAYER_KHRONOS_validation",
  };
  std::vector<const char *> instanceExtensions = {};
  std::vector<const char *> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
  };

  uint64_t requestedFeaturesFlags;

  void setupDebugMessenger();
  void createLogicalDevice();
  void initializePhysicalDevice();
  void initializeInstance(VulkanVersion version);
  bool checkValidationLayerSupport();

  // lib::ConcurrentShardedQueue<VkFence> fences;
  // lib::ConcurrentShardedQueue<VkSemaphore> semaphores;

  std::vector<VkFence> deferredFenceDeletes;
  std::vector<VkSemaphore> deferredIdleSemaphoreDeletes;
  std::vector<VulkanSwapChain *> retiredSwapChains;

  VkFence getFence();
  VkSemaphore getSemaphore();
  void destroySwapChainImmediate(VulkanSwapChain *swapChain);
  void destroyRetiredSwapChains();
  void tryReleaseDeferredResources();

  VkRenderPass createRenderPass(const ColorAttatchment *attachments, uint32_t attatchmentsCount, const DepthAttatchment &depth);
  VulkanQueueFamilyIndices findQueueFamilyIndices();

  // Timers keep using a name-based map since they are not on the render hot-path.
  lib::ConcurrentHashMap<std::string, VulkanTimer *> vkTimers;
  VulkanTimerArena timerArena;
  mutable std::mutex hostAccessMutex_;
  uint32_t reservedTimerCapacity = 1u;

  lib::ConcurrentShardedQueue<VulkanCommandPool> graphicsCommandPool;
  lib::ConcurrentShardedQueue<VulkanCommandPool> transferCommandPool;
  lib::ConcurrentShardedQueue<VulkanCommandPool> computeCommandPool;

  VkQueue getQueueHandle(Queue queueType);
  void ensureTimerArena();
  void destroyTimerArena();
  VulkanTimerRange allocateTimerRange(uint32_t queryCount);
  void releaseTimerRange(const VulkanTimerRange &range);
  // void processPresentations(CommandBuffer *cmds, uint32_t count, const std::vector<VkSemaphore> &signalSemaphores);

protected:
  BufferId allocateBuffer(const BufferInfo &info);
  void releaseBuffer(BufferId id);

  TextureId allocateTexture(const TextureInfo &info);
  void releaseTexture(TextureId id);

  SamplerId allocateSampler(const SamplerInfo &info);
  void releaseSampler(SamplerId id);

  BindingsLayoutId allocateBindingsLayout(const BindingsLayoutInfo &info);
  void releaseBindingsLayout(BindingsLayoutId id);

  BindingGroupsId allocateBindings(const BindingGroupsInfo &groups, const VulkanBindingsLayout &layout);
  void releaseBindingGroup(BindingGroupsId id);

  GraphicsPipelineId allocateGraphicsPipeline(const GraphicsPipelineInfo &info);
  void releaseGraphicsPipeline(GraphicsPipelineId id);

  ComputePipelineId allocateComputePipeline(const ComputePipelineInfo &info);
  void releaseComputePipeline(ComputePipelineId id);

  VulkanTextureView createTextureView(const TextureView &view);
  void destroyTextureView(VulkanTextureView view);

  VulkanCommandPool allocateCommandPool(uint32_t queueFamilyIndex);
  void releaseCommandPool(VulkanCommandPool &pool);

  std::vector<CommandBuffer> allocateCommandBuffers(Queue queue, uint32_t count) override;
  void releaseCommandBuffer(std::vector<CommandBuffer> &buffers) override;

  const VulkanShader &getVulkanShader(ShaderId id);
  const VulkanTexture &getVulkanTexture(TextureId id);
  const VulkanSampler &getVulkanSampler(SamplerId id);
  const VulkanBuffer &getVulkanBuffer(BufferId id);
  const VulkanBindingsLayout &getVulkanBindingsLayout(BindingsLayoutId id);
  const VulkanBindingGroups &getVulkanBindingGroups(BindingGroupsId id);
  const VulkanGraphicsPipeline &getVulkanGraphicsPipeline(GraphicsPipelineId id);
  const VulkanComputePipeline &getVulkanComputePipeline(ComputePipelineId id);

  void beginCommandBuffer(CommandBuffer, bool oneTimeSubmit = true) override;
  void endCommandBuffer(CommandBuffer) override;
  void cmdCopyBuffer(CommandBuffer cmdBuffer, const BufferView &src, const BufferView &dst) override;
  void cmdCopyImage(CommandBuffer cmdBuffer, const TextureView &src, const TextureView &dst) override;
  void cmdBeginRenderPass(CommandBuffer, const RenderPassInfo &) override;
  void cmdEndRenderPass(CommandBuffer) override;
  void cmdBindBindingGroups(CommandBuffer cmdBuffer, BindingGroupsId groupsId, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount) override;
  void cmdBindGraphicsPipeline(CommandBuffer, GraphicsPipeline) override;
  void cmdBindComputePipeline(CommandBuffer, ComputePipeline) override;
  void cmdBindVertexBuffer(CommandBuffer, uint32_t slot, const BufferView &) override;
  void cmdBindIndexBuffer(CommandBuffer, const BufferView &, Type type) override;
  void cmdDraw(CommandBuffer, uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) override;
  void cmdDrawIndexed(CommandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) override;
  void cmdDrawIndexedIndirect(CommandBuffer, const BufferView &indirectBuffer, uint32_t drawCount, uint32_t stride) override;
  void cmdDrawIndirect(CommandBuffer, const BufferView &indirectBuffer, uint32_t drawCount, uint32_t stride) override;
  void cmdDrawIndirectCount(CommandBuffer handle, const BufferView &indirectBuffer, const BufferView &countBuffer, uint32_t maxDrawCount, uint32_t stride) override;
  void cmdResetTimer(CommandBuffer, const Timer &) override;
  void cmdStartTimer(CommandBuffer, const Timer &, uint32_t sampleIndex, PipelineStage stage) override;
  void cmdStopTimer(CommandBuffer, const Timer &, uint32_t sampleIndex, PipelineStage stage) override;
  void cmdResolveTimersToBuffer(CommandBuffer, const Timer *timers, uint32_t count, BufferId destinationBuffer, uint64_t destinationOffset) override;
  // void cmdCopyBuffer(
  //     CommandBuffer cmdHandle,
  //     Buffer srcHandle,
  //     Buffer dstHandle,
  //     uint32_t srcOffset,
  //     uint32_t dstOffset,
  //     uint32_t size) override;
  void cmdDispatchIndirect(CommandBuffer commandBuffer, const BufferView &indirectBuffer) override;
  void cmdDispatch(CommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
  void cmdImageBarrier(
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
      Queue dst_queue_family) override;
  void cmdBufferBarrier(
      CommandBuffer cmd,
      BufferId b,
      PipelineStage src_stage,
      PipelineStage dst_stage,
      AccessPattern src_access,
      AccessPattern dst_access,
      uint32_t offset,
      uint32_t size,
      Queue src_queue_family,
      Queue dst_queue_family) override;

  void cmdMemoryBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access) override;
  void cmdPipelineBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access) override;

  GPUFuture submit(Queue queue, CommandBuffer *commandBuffers, uint32_t count, GPUFuture *wait, uint32_t waitCount, bool releaseCommandBuffersOnCompletion = true) override;

  static void cleanupSubmitCallback(VulkanRHI::VulkanAsyncHandler &future);

public:
  VulkanRHI(VulkanVersion version, DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures, std::vector<std::string> extensions, bool enableValidationLayers = false);
  ~VulkanRHI();

  void present(SwapChain &swapChain, TextureId textureId, ResourceLayout currentLayout) override;
  void presentWithOverlay(
      SwapChain &swapChain,
      TextureId textureId,
      ResourceLayout currentLayout,
      const std::function<void(VkCommandBuffer, VkImageView, VkExtent2D)> &overlayCallback);
  void waitIdle() override;
  void blockUntil(GPUFuture &) override;
  bool isCompleted(GPUFuture &future) override;
  void flushDeferredDeletions() override;
  void reserveTimerCapacity(uint32_t maxTimers) override;

  inline VkInstance getInstance()
  {
    return instance;
  }

  inline VkPhysicalDevice getPhysicalDevice() const
  {
    return physicalDevice;
  }

  inline VkDevice getDevice() const
  {
    return device;
  }

  inline uint32_t getGraphicsQueueFamilyIndex() const
  {
    return indices.graphicsFamily;
  }

  inline VkQueue getGraphicsQueue() const
  {
    return graphicsQueue.empty() ? VK_NULL_HANDLE : graphicsQueue[0];
  }

  VkFormat getSwapChainVkFormat(SwapChain handle);

  void init(std::vector<VkSurfaceKHR> &surfaces);

  void bufferRead(BufferId bufferId, const uint64_t offset, const uint64_t size, std::function<void(const void *)>) override;
  void bufferWrite(BufferId bufferId, const uint64_t offset, const uint64_t size, void *data) override;

  BufferId createBuffer(const BufferInfo &info) override;
  TextureId createTexture(const TextureInfo &info) override;
  SamplerId createSampler(const SamplerInfo &info) override;
  BindingsLayoutId createBindingsLayout(const BindingsLayoutInfo &info) override;
  BindingGroupsId createBindingGroups(const BindingGroupsInfo &info) override;
  GraphicsPipelineId createGraphicsPipeline(const GraphicsPipelineInfo &info) override;
  ComputePipelineId createComputePipeline(const ComputePipelineInfo &info) override;

  void deleteBuffer(BufferId resourceId) override;
  void deleteTexture(TextureId resourceId) override;
  void deleteSampler(SamplerId resourceId) override;
  void deleteBindingsLayout(BindingsLayoutId resourceId) override;
  void deleteBindingGroups(BindingGroupsId resourceId) override;
  void deleteGraphicsPipeline(GraphicsPipelineId resourceId) override;
  void deleteComputePipeline(ComputePipelineId resourceId) override;

  const SwapChain createSwapChain(uint32_t surfaceIndex, uint32_t width, uint32_t height) override;
  const uint32_t getSwapChainImagesCount(SwapChain swapChainHandle) override;
  void destroySwapChain(SwapChain) override;
  Format getSwapChainFormat(SwapChain handle) override;
  const TextureView getSwapChainTextureView(SwapChain swapChainHandle, uint32_t imageIndex) override;
  const TextureView getCurrentSwapChainTextureView(SwapChain swapChainHandle) override;
  const uint32_t getSwapChainImagesWidth(SwapChain swapChainHandle) override;
  const uint32_t getSwapChainImagesHeight(SwapChain swapChainHandle) override;

  ShaderId createShader(const ShaderInfo data) override;
  void deleteShader(ShaderId handle) override;

  const Timer createTimer(const TimerInfo) override;
  void deleteTimer(const Timer &timer) override;
  double readTimer(const Timer &timer) override;
  double getTimestampPeriodNs() const override;
  void readTimers(const Timer *timers, uint32_t count, double *outValues) override;
};

} // namespace vulkan
} // namespace backend
} // namespace rendering
