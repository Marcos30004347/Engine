#include "datastructure/ConcurrentHashMap.hpp"
#include "rendering/gpu/EventLoop.hpp"
#include "rendering/gpu/RenderGraph.hpp"
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
  VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

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

  std::vector<VulkanTextureView *> swapChainImages;
  std::vector<VkSemaphore> achireSemaphores;
  std::vector<VkSemaphore> presentSemaphores;

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
  std::vector<VkSemaphore> achireSemaphores;
  std::vector<VkSemaphore> presentSemaphores;
  std::vector<VulkanTextureView> views;
  std::vector<VulkanAttatchment> attatchments;
};

struct VulkanCommandBuffer
{
  VkCommandBuffer commandBuffer;
  VkCommandPool commandPool;
  VulkanGraphicsPipeline *boundGraphicsPipeline;
  VulkanComputePipeline *boundComputePipeline;
  // VulkanBindingGroups *boundGroups;
  std::vector<VulkanCommandBufferRenderPass> renderPasses;
};
struct VulkanCommandPool
{
  VkCommandPool commandPool;
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

    std::vector<VkFramebuffer> framebuffers;
    std::vector<VulkanTextureView> views;

    VulkanAsyncHandler(VulkanRHI *, VkFence, VkSemaphore, std::vector<VkFramebuffer> &, std::vector<VulkanTextureView> &);
    static rendering::FenceStatus getStatus(VulkanAsyncHandler &future);
  };

  // class VulkanFuture : public GPUFutureImp
  // {
  // public:
  //   rendering::AsyncEvent<VulkanAsyncHandler> handler;
  //   VulkanFuture(rendering::AsyncEvent<VulkanAsyncHandler> &&handler);
  // };

  VulkanVersion version;
  VkInstance instance;

  VkDevice device;
  VkPhysicalDevice physicalDevice;
  VkDebugUtilsMessengerEXT debugMessenger;

  VulkanQueueFamilyIndices indices;

  std::atomic<uint64_t> commandBuffersAllocated;

  std::vector<VulkanSurface> surfaces;

  std::vector<VkQueue> graphicsQueue;
  std::vector<VkQueue> computeQueue;
  std::vector<VkQueue> transferQueue;

  EventLoop<VulkanRHI::VulkanAsyncHandler> eventLoop;

  lib::ConcurrentHashMap<SwapChain, VulkanSwapChain> swapChains;
  lib::ConcurrentHashMap<CommandBuffer, VulkanCommandBuffer> commandBuffers;

  std::vector<const char *> validationLayers = {
    "VK_LAYER_KHRONOS_validation",
  };
  std::vector<char *> instanceExtensions = {};
  std::vector<const char *> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
  };

  uint64_t requestedFeaturesFlags;

  void init(std::vector<VkSurfaceKHR> &surfaces);

  void setupDebugMessenger();
  void createLogicalDevice();
  void initializePhysicalDevice();
  void initializeInstance(VulkanVersion version);
  bool checkValidationLayerSupport();

  lib::ConcurrentShardedQueue<VkFence> fences;
  lib::ConcurrentShardedQueue<VkSemaphore> semaphores;

  VkFence getFence();
  VkSemaphore getSemaphore();

  VkRenderPass createRenderPass(const ColorAttatchment *attachments, uint32_t attatchmentsCount, DepthAttatchment depth);
  VulkanQueueFamilyIndices findQueueFamilyIndices();

  lib::ConcurrentHashMap<std::string, VulkanBuffer *> vkBuffers;
  lib::ConcurrentHashMap<std::string, VulkanTexture *> vkTextures;
  lib::ConcurrentHashMap<std::string, VulkanSampler *> vkSamplers;
  lib::ConcurrentHashMap<std::string, VulkanShader *> vkShaders;
  lib::ConcurrentHashMap<std::string, VulkanBindingsLayout *> vkBindingsLayout;
  lib::ConcurrentHashMap<std::string, VulkanBindingGroups *> vkBindingsGroups;
  lib::ConcurrentHashMap<std::string, VulkanGraphicsPipeline *> vkGraphicsPipeline;
  lib::ConcurrentHashMap<std::string, VulkanComputePipeline *> vkComputePipeline;

  VkQueue getQueueHandle(Queue queueType);
  void processPresentations(CommandBuffer *cmds, uint32_t count, const std::vector<VkSemaphore> &signalSemaphores);

protected:
  VulkanBuffer &allocateBuffer(const BufferInfo &info);
  void releaseBuffer(VulkanBuffer &buf);

  VulkanTexture &allocateTexture(const TextureInfo &info);
  void releaseTexture(VulkanTexture &tex);

  VulkanSampler &allocateSampler(const SamplerInfo &info);
  void releaseSampler(VulkanSampler &sampler);

  VulkanBindingsLayout &allocateBindingsLayout(const BindingsLayoutInfo &info);
  void releaseBindingsLayout(VulkanBindingsLayout &layout);

  VulkanBindingGroups &allocateBindings(const BindingGroupsInfo &groups, const VulkanBindingsLayout &layout);
  void releaseBindingGroup(VulkanBindingGroups &group);

  VulkanGraphicsPipeline &allocateGraphicsPipeline(const GraphicsPipelineInfo &info);
  void releaseGraphicsPipeline(VulkanGraphicsPipeline &pipeline);

  VulkanComputePipeline &allocateComputePipeline(const ComputePipelineInfo &info);
  void releaseComputePipeline(VulkanComputePipeline &pipeline);

  VulkanTextureView createTextureView(const TextureView &view);
  void destroyTextureView(VulkanTextureView view);

  VulkanCommandPool allocateCommandPool(uint32_t queueFamilyIndex);
  void releaseCommandPool(VulkanCommandPool &pool);

  std::vector<CommandBuffer> allocateCommandBuffers(VulkanCommandPool &commandPool, uint32_t count, VkCommandBufferLevel level);
  void releaseCommandBuffer(std::vector<CommandBuffer> &buffers);

  const VulkanShader &getVulkanShader(const std::string &obj);
  const VulkanTexture &getVulkanTexture(const std::string &obj);
  const VulkanSampler &getVulkanSampler(const std::string &obj);
  const VulkanBuffer &getVulkanBuffer(const std::string &obj);
  const VulkanBindingsLayout &getVulkanBindingsLayout(const std::string &obj);
  const VulkanBindingGroups &getVulkanBindingGroups(const std::string &obj);
  const VulkanGraphicsPipeline &getVulkanGraphicsPipeline(const std::string &obj);
  const VulkanComputePipeline &getVulkanComputePipeline(const std::string &obj);

  void beginCommandBuffer(CommandBuffer) override;
  void endCommandBuffer(CommandBuffer) override;
  void cmdCopyBuffer(CommandBuffer cmdBuffer, Buffer src, Buffer dst, uint32_t srcOffset, uint32_t dstOffset, uint32_t size) override;
  void cmdBeginRenderPass(CommandBuffer, const RenderPassInfo &) override;
  void cmdEndRenderPass(CommandBuffer) override;
  void cmdBindBindingGroups(CommandBuffer cmdBuffer, BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount) override;
  void cmdBindGraphicsPipeline(CommandBuffer, GraphicsPipeline) override;
  void cmdBindComputePipeline(CommandBuffer, ComputePipeline) override;
  void cmdBindVertexBuffer(CommandBuffer, uint32_t slot, Buffer, uint64_t offset) override;
  void cmdBindIndexBuffer(CommandBuffer, Buffer, Type type, uint64_t offset) override;
  void cmdDraw(CommandBuffer, uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) override;
  void cmdDrawIndexed(CommandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) override;
  void cmdDrawIndexedIndirect(CommandBuffer, Buffer indirectBuffer, size_t offset, uint32_t drawCount, uint32_t stride) override;

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

  GPUFuture submit(Queue queue, CommandBuffer *commandBuffers, uint32_t count, GPUFuture *wait, uint32_t waitCount) override;

  void blockUntil(GPUFuture &) override;
  void waitIdle() override;
  bool isCompleted(GPUFuture &future) override;
  static void cleanupSubmitCallback(VulkanRHI::VulkanAsyncHandler &future);

public:
  VulkanRHI(VulkanVersion version, DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures, std::vector<std::string> extensions);
  ~VulkanRHI();

  void bufferMapRead(const Buffer &buffer, const uint64_t offset, const uint64_t size, void **ptr) override;
  void bufferUnmap(const Buffer &buffer) override;
  void bufferWrite(const Buffer &buffer, const uint64_t offset, const uint64_t size, void *data) override;

  const SwapChain createSwapChain(uint32_t surfaceIndex, uint32_t width, uint32_t height) override;
  const uint32_t getSwapChainImagesCount(SwapChain swapChainHandle);
  void destroySwapChain(SwapChain) override;
  const TextureView getCurrentSwapChainTextureView(SwapChain swapChainHandle, uint32_t imageIndex) override;
};

} // namespace vulkan
} // namespace backend
} // namespace rendering