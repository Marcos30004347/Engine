#include "rhi/rhi.hpp"

#include <map>
#include <stack>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace rhi
{
namespace vulkan
{

enum VulkanVersion
{
  Vulkan_1_2,
};

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif

struct QueueFamilyIndices
{
  bool hasGraphicsFamily = false;
  uint32_t graphicsFamily;
  bool hasComputeFamily = false;
  uint32_t computeFamily;
  bool hasTransferFamily = false;
  uint32_t transferFamily;
  std::unordered_map<VkSurfaceKHR, uint32_t> surface2PresentQueueFamily;
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

class VulkanDevice : public Device
{
public:
  VulkanDevice(VulkanVersion, DeviceRequiredLimits, std::vector<DeviceFeatures>);
  ~VulkanDevice();

  SurfaceHandle addWindowForDrawing(window::Window *) override;

  void init() override;

  BufferHandle createBuffer(size_t size, BufferUsage, void *) override;
  const void *mapBufferRead(BufferHandle) override;
  void *mapBufferWrite(BufferHandle) override;
  void destroyBuffer(BufferHandle) override;
  void unmap(BufferHandle) override;

private:
  // Device info
  bool initialized;

  VulkanVersion version;
  DeviceRequiredLimits requiredLimits;
  uint64_t requestedFeaturesFlags;
  DeviceProperties properties;

  std::vector<const char *> validationLayers = {
    "VK_LAYER_KHRONOS_validation",
  };

  std::vector<const char *> instanceExtensions = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
  };

  std::vector<const char *> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
  };

  // Vulkan Core
  VkInstance instance;
  VkDevice device;
  VkPhysicalDevice physicalDevice;

  // Debug
  VkDebugUtilsMessengerEXT debugMessenger;

  // Presentation
  std::unordered_map<SurfaceHandle, window::Window *> windows;
  std::unordered_map<SurfaceHandle, VkSurfaceKHR> surfaces;

  std::unordered_map<VkSurfaceKHR, VkSwapchainKHR> swapChain;
  std::unordered_map<VkSurfaceKHR, std::vector<VkImage>> swapChainImages;
  std::unordered_map<VkSurfaceKHR, VkFormat> swapChainImageFormat;
  std::unordered_map<VkSurfaceKHR, VkExtent2D> swapChainExtent;
  std::unordered_map<VkSurfaceKHR, std::vector<VkImageView>> swapChainImageViews;
  std::unordered_map<VkSurfaceKHR, std::vector<VkFramebuffer>> swapChainFramebuffers;
  std::unordered_map<VkSurfaceKHR, VkQueue> presentQueues;

  // Queues
  QueueFamilyIndices indices;
  std::vector<VkQueue> graphicsQueue;
  std::vector<VkQueue> computeQueue;
  std::vector<VkQueue> transferQueue;

  // Buffers
  std::unordered_map<BufferHandle, VulkanBuffer> buffers;
  uint32_t bufferAllocationsCount;

  // Tracking
  std::stack<SurfaceHandle> frameBuffersResized;

  void setupDebugMessenger();
  void pickPhysicalDevice();
  void createLogicalDevice();
  void initializePhysicalDevice();
  void initializeInstance(VulkanVersion version);
  bool checkValidationLayerSupport();
  void createSwapChain(VkSurfaceKHR surface);
  void addWindowSurface(window::Window *windowObj);
  VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats);
  VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR> &availablePresentModes);
  VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities, VkSurfaceKHR surface);
  uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
  void createImageViews(VkSurfaceKHR surface);
  void cleanupSwapChain(VkSurfaceKHR surface);
  void recreateSwapChain(VkSurfaceKHR surface, VkRenderPass renderPass);
  void createFramebuffers(VkSurfaceKHR surface, VkRenderPass renderPass);
  void drawPreProcess(VkRenderPass renderPass);

  // callbacks:
  static void onWindowResized(const window::Window *window, VulkanDevice *);
};
} // namespace vulkan
} // namespace rhi