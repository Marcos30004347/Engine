#include "rendering/gpu/RenderGraph.hpp"
#include "datastructure/ConcurrentHashMap.hpp"
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

struct VulkanTextureView
{
  VkImageView view = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageSubresourceRange range{};
  TextureView original;
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

  std::atomic<uint32_t> currentPrimitive;
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

class VulkanRHI : public RHI
{
private:
  VulkanVersion version;
  VkInstance instance;

  VkDevice device;
  VkPhysicalDevice physicalDevice;
  VkDebugUtilsMessengerEXT debugMessenger;

  VulkanQueueFamilyIndices indices;

  std::vector<VulkanSurface> surfaces;

  std::vector<VkQueue> graphicsQueue;
  std::vector<VkQueue> computeQueue;
  std::vector<VkQueue> transferQueue;

  std::vector<VulkanSwapChain *> swapChains;

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

  VkRenderPass createRenderPass(const ColorAttatchment *attachments, uint32_t attatchmentsCount, DepthAttatchment depth);
  VulkanQueueFamilyIndices findQueueFamilyIndices();

  lib::ConcurrentHashMap<std::string, VulkanBuffer*> vkBuffers;
  lib::ConcurrentHashMap<std::string, VulkanTexture*> vkTextures;
  lib::ConcurrentHashMap<std::string, VulkanSampler*> vkSamplers;
  lib::ConcurrentHashMap<std::string, VulkanShader*> vkShaders;
  lib::ConcurrentHashMap<std::string, VulkanBindingsLayout*> vkBindingsLayout;
  lib::ConcurrentHashMap<std::string, VulkanBindingGroups*> vkBindingsGroups;
  lib::ConcurrentHashMap<std::string, VulkanGraphicsPipeline*> vkGraphicsPipeline;
  lib::ConcurrentHashMap<std::string, VulkanComputePipeline*> vkComputePipeline;

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

  // const BindingGroups getBindingGroups(const std::string &name) const;
  // const BindingsLayout getBindingsLayout(const std::string &name) const;
  // const GraphicsPipeline getGraphicsPipeline(const std::string &name) const;
  // const ComputePipeline getComputePipeline(const std::string &name) const;
  // const Texture getTexture(const std::string &name) const;
  // const Buffer getBuffer(const std::string &name) const;
  // const Sampler getSampler(const std::string &name) const;
  // const Buffer getScratchBuffer(const std::string &name) const;

  const VulkanShader &getVulkanShader(const std::string &obj);
  const VulkanTexture &getVulkanTexture(const std::string &obj);
  const VulkanSampler &getVulkanSampler(const std::string &obj);
  const VulkanBuffer &getVulkanBuffer(const std::string &obj);
  const VulkanBindingsLayout &getVulkanBindingsLayout(const std::string &obj);
  const VulkanBindingGroups &getVulkanBindingGroups(const std::string &obj);
  const VulkanGraphicsPipeline &getVulkanGraphicsPipeline(const std::string &obj);
  const VulkanComputePipeline &getVulkanComputePipeline(const std::string &obj);

public:
  VulkanRHI(VulkanVersion version, DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures, std::vector<std::string> extensions);
  ~VulkanRHI();

  void bufferMapRead(const Buffer &buffer, const uint64_t offset, const uint64_t size, void **ptr) override;
  void bufferUnmap(const Buffer &buffer) override;
  void bufferWrite(const Buffer &buffer, const uint64_t offset, const uint64_t size, void *data) override;

  const SwapChain createSwapChain(uint32_t surfaceIndex, uint32_t width, uint32_t height) override;
  void destroySwapChain(SwapChain) override;
  const TextureView getCurrentSwapChainTextureView(SwapChain swapChainHandle) override;
};

} // namespace vulkan
} // namespace backend
} // namespace rendering