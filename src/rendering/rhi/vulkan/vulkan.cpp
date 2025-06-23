#include <algorithm>
#include <assert.h>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

#include "vulkan.hpp"

#ifdef SDL3_AVAILABLE
#include "window/sdl3/SDL3Window.hpp"
#include <SDL3/SDL_vulkan.h>
#endif

using namespace rhi;
using namespace window;
using namespace vulkan;

#define VULKAN_DEVICE_API_CALL_CHECKS assert(initialized);
#define VULKAN_DEVICE_SETUP_CHECKS assert(!initialized);

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

void destroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks *pAllocator)
{
  auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
  if (func != nullptr)
  {
    func(instance, debugMessenger, pAllocator);
  }
}

void VulkanDevice::setupDebugMessenger()
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

struct DeviceResult
{
  VkPhysicalDevice device;
  std::uint64_t feature_flags;
  DeviceProperties properties;
};

VulkanQueueFamilyIndices findQueueFamilyIndices(VkPhysicalDevice device, std::vector<VkSurfaceKHR> &surfaces)
{
  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

  VulkanQueueFamilyIndices indices;

  indices.hasGraphicsFamily = false;
  indices.hasComputeFamily = false;
  indices.hasTransferFamily = false;

  for (uint32_t i = 0; i < queueFamilyCount; ++i)
  {
    if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
    {
      indices.graphicsFamily = i;
      indices.hasGraphicsFamily = true;
    }
    if (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
    {
      indices.transferFamily = i;
      indices.hasTransferFamily = true;
    }

    if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
    {
      indices.computeFamily = i;
      indices.hasComputeFamily = true;
    }

    for (uint32_t j = 0; j < surfaces.size(); j++)
    {
      VkBool32 supported = VK_FALSE;
      
      vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surfaces[j], &supported);
      
      if (supported)
      {
        indices.presentFamily = i;
      }
    }
  }

  return indices;
}

DeviceResult getPhysicalDevice(std::vector<VkPhysicalDevice> &devices, DeviceRequiredLimits requiredLimits, uint64_t requiredFeatures)
{
  std::vector<DeviceResult> suitableDevices;
  for (const auto &dev : devices)
  {
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures features;

    vkGetPhysicalDeviceProperties(dev, &props);
    vkGetPhysicalDeviceFeatures(dev, &features);

    bool isDedicated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    bool isIntegrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

    std::uint64_t featureFlags = 0;
    DeviceProperties devProps;

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(dev, &memProps);

    size_t totalMemory = 0;

    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i)
    {
      if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      {
        totalMemory += memProps.memoryHeaps[i].size;
      }
    }

    // Check for device properties
    if (totalMemory < requiredLimits.minimumMemory)
    {
      continue;
    }

    if (props.limits.maxComputeSharedMemorySize < requiredLimits.minimumComputeSharedMemory)
    {
      continue;
    }

    if (props.limits.maxComputeWorkGroupInvocations < requiredLimits.minimumComputeWorkGroupInvocations)
    {
      continue;
    }

    if (features.multiDrawIndirect)
    {
      featureFlags |= DeviceFeatures_MultiDrawIndirect;
    }

    if (features.drawIndirectFirstInstance)
    {
      featureFlags |= DeviceFeatures_DrawIndirectFirstInstance;
    }

    devProps.maxMemory = totalMemory;
    devProps.maxComputeSharedMemorySize = props.limits.maxComputeSharedMemorySize;
    devProps.maxComputeWorkGroupInvocations = props.limits.maxComputeWorkGroupInvocations;

    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
    {
      featureFlags |= DeviceFeatures_Dedicated;
    }

    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
    {
      featureFlags |= DeviceFeatures_Integrated;
    }

    featureFlags |= DeviceFeatures_Atomic32_AllOps;

    VkPhysicalDeviceShaderAtomicInt64Features atomic64Features = {};
    atomic64Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &atomic64Features;
    vkGetPhysicalDeviceFeatures2(dev, &features2);

    if (atomic64Features.shaderBufferInt64Atomics)
    {
      featureFlags |= DeviceFeatures_Atomic64_MinMax;
    }
    if (atomic64Features.shaderSharedInt64Atomics)
    {
      featureFlags |= DeviceFeatures_Atomic64_AllOps;
    }

    VkPhysicalDeviceDescriptorIndexingFeatures indexingFeatures = {};
    indexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    features2.pNext = &indexingFeatures;
    vkGetPhysicalDeviceFeatures2(dev, &features2);

    if (indexingFeatures.runtimeDescriptorArray && indexingFeatures.descriptorBindingPartiallyBound)
    {
      featureFlags |= DeviceFeatures_Bindless;
    }

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> properties(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, properties.data());
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

    if (hasTimestamp)
    {
      featureFlags |= DeviceFeatures_Timestamp;
    }
    if (hasCompute)
    {
      featureFlags |= DeviceFeatures_Compute;
    }
    if (hasGraphics)
    {
      featureFlags |= DeviceFeatures_Graphics;
    }

    VkPhysicalDeviceProperties2 props2 = {};
    VkPhysicalDeviceSubgroupProperties subgroupProps = {};

    subgroupProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;

    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &subgroupProps;

    vkGetPhysicalDeviceProperties2(dev, &props2);

    devProps.sugroupSize = subgroupProps.subgroupSize;

    if (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT)
    {
      featureFlags |= DeviceFeatures_Subgroup_Basic;
    }
    if (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT)
    {
      featureFlags |= DeviceFeatures_Subgroup_Vote;
    }
    if (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)
    {
      featureFlags |= DeviceFeatures_Subgroup_Arithmetic;
    }
    if (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT)
    {
      featureFlags |= DeviceFeatures_Subgroup_Ballot;
    }
    if (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT)
    {
      featureFlags |= DeviceFeatures_Subgroup_Shuffle;
    }
    if (subgroupProps.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT)
    {
      featureFlags |= DeviceFeatures_Subgroup_ShuffleRelative;
    }

    std::vector<VkExtensionProperties> extensions;
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
    extensions.resize(extCount);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, extensions.data());

    bool hasSwapchain = std::any_of(
        extensions.begin(),
        extensions.end(),
        [](const auto &e)
        {
          return strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        });

    if (hasSwapchain)
    {
      featureFlags |= DeviceFeatures_SwapChain;
    }

    if ((featureFlags & requiredFeatures) != requiredFeatures)
    {
      continue;
    }

    DeviceResult result;
    result.device = dev;
    result.feature_flags = featureFlags;
    result.properties = devProps;

    suitableDevices.push_back(result);
  }

  std::sort(
      suitableDevices.begin(),
      suitableDevices.end(),
      [](const auto &a, const auto &b)
      {
        uint32_t scoreA = 0;
        uint32_t scoreB = 0;

        scoreA += a.properties.maxComputeWorkGroupInvocations;
        scoreB += b.properties.maxComputeWorkGroupInvocations;

        return scoreA > scoreB;
      });

  if (suitableDevices.size() == 0)
  {
    throw std::runtime_error("Not sutiable device found");
  }

  return suitableDevices[0];
}

VulkanDevice::VulkanDevice(VulkanVersion version, DeviceRequiredLimits requiredLimits, std::vector<DeviceFeatures> requestedFeatures)
{
  VULKAN_DEVICE_SETUP_CHECKS

  this->version = version;
  this->requiredLimits = requiredLimits;
  this->requestedFeaturesFlags = 0;

  for (DeviceFeatures &f : requestedFeatures)
  {
    this->requestedFeaturesFlags |= f;
  }

  this->initialized = false;
}

void VulkanDevice::onWindowResized(const window::Window *window, VulkanDevice *self)
{

  for (auto &kv : self->windows)
  {
    Window *w = kv.second;
    SurfaceHandle handle = kv.first;

    if (w == window)
    {
      self->frameBuffersResized.push(handle);
      break;
    }
  }
}

void VulkanDevice::drawPreProcess(VkRenderPass renderPass)
{
  while (frameBuffersResized.size())
  {
    recreateSwapChain(surfaces[frameBuffersResized.top()], renderPass);
    frameBuffersResized.pop();
  }
}

SurfaceHandle VulkanDevice::addWindowForDrawing(window::Window *canvas)
{
  VULKAN_DEVICE_SETUP_CHECKS

  if ((requestedFeaturesFlags & DeviceFeatures::DeviceFeatures_Graphics) == 0)
  {
    throw std::runtime_error("To add a window for rendering, enable DeviceFeatures_Graphics when creating the device");
  }

  std::set<std::string> uniqueExtensions;

  for (auto &extension : instanceExtensions)
  {
    uniqueExtensions.insert(extension);
  }

#ifdef SDL3_AVAILABLE
  window::sdl3::SDL3Window *sdl3Window = reinterpret_cast<window::sdl3::SDL3Window *>(canvas);
  if (sdl3Window)
  {
    for (uint32_t j = 0; j < sdl3Window->extensionCount; j++)
    {

      if (uniqueExtensions.count(sdl3Window->extensions[j]) == 0)
      {
        instanceExtensions.push_back(sdl3Window->extensions[j]);
      }

      uniqueExtensions.insert(sdl3Window->extensions[j]);
    }
  }
#endif

  canvas->onWindowRezizedEvent.addListener((Window::WindowEvent::Callback)onWindowResized, this);

  // TODO: if window is removed we can't add a new one otherwise we will reuse index
  // right now we can only add windows during setup, so this is fine
  SurfaceHandle handle = static_cast<SurfaceHandle>(windows.size());

  windows[handle] = canvas;

  return handle;
}

void VulkanDevice::init()
{
  VULKAN_DEVICE_SETUP_CHECKS

  bufferAllocationsCount = 0;

  printf("Vulkan extensions:\n");
  for (uint32_t i = 0; i < instanceExtensions.size(); i++)
  {
    printf("%s\n", instanceExtensions[i]);
  }

  initializeInstance(version);
  setupDebugMessenger();
  initializePhysicalDevice();

  std::cout << "VulkanDevice created successfully!" << std::endl;

  for (auto &kv : windows)
  {
    Window *window = kv.second;
    addWindowSurface(window);
  }

  createLogicalDevice();
  initialized = true;
  std::cout << "VulkanDevice initialized successfully!" << std::endl;
}

void VulkanDevice::addWindowSurface(window::Window *windowObj)
{
  VULKAN_DEVICE_SETUP_CHECKS

  VkSurfaceKHR surface;
  bool surfaceRetrieved = false;

#ifdef SDL3_AVAILABLE
  window::sdl3::SDL3Window *sdl3Window = reinterpret_cast<window::sdl3::SDL3Window *>(windowObj);

  if (sdl3Window != nullptr)
  {
    if (!SDL_Vulkan_CreateSurface(sdl3Window->sdlWindow, instance, NULL, &surface))
    {
      throw std::runtime_error("Could not create surface");
    }
    SurfaceHandle handle = static_cast<SurfaceHandle>(surfaces.size());
    surfaces[handle] = surface;
    surfaceRetrieved = true;
  }

#endif

  if (surfaceRetrieved == false)
  {
    throw std::runtime_error("Failed to get surface");
  }
}

void VulkanDevice::createLogicalDevice()
{
  std::vector<VkSurfaceKHR> vkSurfaces;

  for (auto &p : surfaces)
  {
    vkSurfaces.push_back(p.second);
  }

  indices = findQueueFamilyIndices(physicalDevice, vkSurfaces);

  if ((featureFlags & DeviceFeatures_Graphics) && !indices.hasGraphicsFamily)
  {
    throw std::runtime_error("Missing required queue families");
  }

  if ((featureFlags & DeviceFeatures_Compute) && !indices.hasComputeFamily)
  {
    throw std::runtime_error("Missing required queue families");
  }

  float queuePriority = 1.0f;

  std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
  std::set<uint32_t> uniqueFamiles;
  std::unordered_map<uint32_t, uint32_t> familyToCount;

  if (indices.hasComputeFamily)
  {
    if (familyToCount.count(indices.computeFamily) == 0)
    {
      familyToCount[indices.computeFamily] = 0;
    }

    familyToCount[indices.computeFamily] = 1;
    uniqueFamiles.insert(indices.computeFamily);
  }

  if (indices.hasGraphicsFamily)
  {
    if (familyToCount.count(indices.graphicsFamily) == 0)
    {
      familyToCount[indices.graphicsFamily] = 0;
    }

    familyToCount[indices.graphicsFamily] = 1;
    uniqueFamiles.insert(indices.graphicsFamily);
  }

  if (indices.hasTransferFamily)
  {
    if (familyToCount.count(indices.transferFamily) == 0)
    {
      familyToCount[indices.transferFamily] = 0;
    }

    familyToCount[indices.transferFamily] += 1;
    uniqueFamiles.insert(indices.transferFamily);
  }

  if (indices.hasPresentFamily)
  {
    if (familyToCount.count(indices.presentFamily) == 0)
    {
      familyToCount[indices.presentFamily] = 0;
    }

    familyToCount[indices.presentFamily] += surfaces.size();
    uniqueFamiles.insert(indices.presentFamily);
  }

  for (uint32_t familyIndex : uniqueFamiles)
  {
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = familyIndex;
    queueCreateInfo.queueCount = familyToCount[familyIndex];
    queueCreateInfo.pQueuePriorities = &queuePriority;
    queueCreateInfos.push_back(queueCreateInfo);
  }

  VkPhysicalDeviceFeatures deviceFeatures{};

  deviceFeatures.multiDrawIndirect = featureFlags & DeviceFeatures_MultiDrawIndirect ? VK_TRUE : VK_FALSE;
  deviceFeatures.drawIndirectFirstInstance = featureFlags & DeviceFeatures_DrawIndirectFirstInstance ? VK_TRUE : VK_FALSE;

  VkDeviceCreateInfo createInfo{};

  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
  createInfo.pQueueCreateInfos = queueCreateInfos.data();
  createInfo.pEnabledFeatures = &deviceFeatures;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
  createInfo.ppEnabledExtensionNames = deviceExtensions.data();

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
    if (info.queueFamilyIndex == indices.presentFamily)
    {
      index = presentCount;
      presentCount++;
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
    if (info.queueFamilyIndex == indices.presentFamily)
    {
      presentQueues.push_back(queue);
    }
  }

  // uint32_t index = 0;
  // for (auto &kv : surfaces)
  // {
  //   VkSurfaceKHR surface = kv.second;
  //   VkQueue queue = VK_NULL_HANDLE;
  //   vkGetDeviceQueue(device, indices.presentFamily, index++, &queue);
  //   presentQueues.push_back(queue);
  // }
}

bool VulkanDevice::checkValidationLayerSupport()
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

void VulkanDevice::initializeInstance(VulkanVersion version)
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

void VulkanDevice::initializePhysicalDevice()
{
  uint32_t deviceCount = 0;

  vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

  if (deviceCount == 0)
  {
    throw std::runtime_error("Failed to find GPUs with Vulkan support");
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

  DeviceResult dev = getPhysicalDevice(devices, requiredLimits, requestedFeaturesFlags);

  physicalDevice = dev.device;
  featureFlags = dev.feature_flags;
  properties = dev.properties;

  if (physicalDevice == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Failed to find a suitable GPU");
  }
}

VulkanDevice::~VulkanDevice()
{
  if (enableValidationLayers)
  {
    destroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
  }

  for (auto &kv : windows)
  {
    Window *canvas = kv.second;
    canvas->onWindowRezizedEvent.removeListener((Window::WindowEvent::Callback)onWindowResized, this);
  }

  for (auto &kv : surfaces)
  {
    VkSurfaceKHR surface = kv.second;

    for (auto framebuffer : swapChainFramebuffers[surface])
    {
      vkDestroyFramebuffer(device, framebuffer, nullptr);
    }

    for (auto imageView : swapChainImageViews[surface])
    {
      vkDestroyImageView(device, imageView, nullptr);
    }

    vkDestroySwapchainKHR(device, swapChain[surface], nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
  }

  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
}

BufferHandle VulkanDevice::createBuffer(size_t size, BufferUsage usage, void *data)
{
  VULKAN_DEVICE_API_CALL_CHECKS

  VkBufferUsageFlags usageFlags = 0;
  VkMemoryPropertyFlags memoryProperties = 0;

  if (usage & BufferUsage_Uniform)
    usageFlags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (usage & BufferUsage_Storage)
    usageFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (usage & BufferUsage_Vertex)
    usageFlags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (usage & BufferUsage_Indirect)
    usageFlags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if (usage & BufferUsage_Timestamp)
    usageFlags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  // Flags for CPU mapping
  if (usage & BufferUsage_Push)
    memoryProperties |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  if (usage & BufferUsage_Pull)
    memoryProperties |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  if ((usage & BufferUsage_Push) == 0 && (usage & BufferUsage_Pull) == 0)
    memoryProperties |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  // Create buffer
  VkBufferCreateInfo bufferInfo = {};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usageFlags;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkBuffer buffer;
  vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);

  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

  VkMemoryAllocateInfo allocInfo = {};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, memoryProperties);

  VkDeviceMemory bufferMemory;
  vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory);
  vkBindBufferMemory(device, buffer, bufferMemory, 0);

  if (data && (memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
  {
    void *mapped;
    vkMapMemory(device, bufferMemory, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(device, bufferMemory);
  }

  VulkanBuffer vkbuf = {buffer, bufferMemory, size};

  BufferHandle handle = static_cast<BufferHandle>(bufferAllocationsCount++);

  buffers[handle] = vkbuf;

  return handle;
}

void VulkanDevice::destroyBuffer(BufferHandle handle)
{
  VULKAN_DEVICE_API_CALL_CHECKS

  assert(initialized);

  auto it = buffers.find(handle);

  if (it != buffers.end())
  {
    vkDestroyBuffer(device, it->second.buffer, nullptr);
    vkFreeMemory(device, it->second.memory, nullptr);
    buffers.erase(it);
  }
}

const void *VulkanDevice::mapBufferRead(BufferHandle handle)
{
  VULKAN_DEVICE_API_CALL_CHECKS

  auto &buf = buffers.at(handle);
  if (!buf.mapped)
  {
    vkMapMemory(device, buf.memory, 0, buf.size, 0, &buf.mapped);
  }
  return buf.mapped;
}

void *VulkanDevice::mapBufferWrite(BufferHandle handle)
{
  VULKAN_DEVICE_API_CALL_CHECKS

  auto &buf = buffers.at(handle);

  if (!buf.mapped)
  {
    vkMapMemory(device, buf.memory, 0, buf.size, 0, &buf.mapped);
  }

  return buf.mapped;
}

void VulkanDevice::unmap(BufferHandle handle)
{
  VULKAN_DEVICE_API_CALL_CHECKS

  auto &buf = buffers.at(handle);
  if (buf.mapped)
  {
    vkUnmapMemory(device, buf.memory);
    buf.mapped = nullptr;
  }
}

uint32_t VulkanDevice::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
  VULKAN_DEVICE_API_CALL_CHECKS

  VkPhysicalDeviceMemoryProperties memProperties;

  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

  for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
  {
    if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
    {
      return i;
    }
  }

  throw std::runtime_error("Failed to find suitable memory type");
}

SwapChainSupportDetails querySwapChainSupport(VkSurfaceKHR surface, VkPhysicalDevice device)
{

  SwapChainSupportDetails details;

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

VkSurfaceFormatKHR VulkanDevice::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats)
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

VkPresentModeKHR VulkanDevice::chooseSwapPresentMode(const std::vector<VkPresentModeKHR> &availablePresentModes)
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

VkExtent2D VulkanDevice::chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities, VkSurfaceKHR surface)
{
  if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
  {
    return capabilities.currentExtent;
  }
  else
  {
    SurfaceHandle index = static_cast<SurfaceHandle>(0xFFFFFFFF);

    for (auto &kv : surfaces)
    {
      if (kv.second == surface)
      {
        index = kv.first;
        break;
      }
    }

    uint32_t height = windows[index]->getHeightInPixels();
    uint32_t width = windows[index]->getWidthInPixels();

    VkExtent2D actualExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

    return actualExtent;
  }
}

void VulkanDevice::createSwapChain(VkSurfaceKHR surface)
{
  VULKAN_DEVICE_API_CALL_CHECKS

  SwapChainSupportDetails swapChainSupport = querySwapChainSupport(surface, physicalDevice);

  VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
  VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
  VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities, surface);

  uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
  if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount)
  {
    imageCount = swapChainSupport.capabilities.maxImageCount;
  }

  VkSwapchainCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  createInfo.surface = surface;

  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = extent;
  createInfo.imageArrayLayers = 1;
  createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

  std::vector<VkSurfaceKHR> surfaces = {surface};

  uint32_t queueFamilyIndices[2] = {indices.graphicsFamily, indices.presentFamily};

  if (indices.graphicsFamily != indices.presentFamily)
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

  VkSwapchainKHR swapChain = VK_NULL_HANDLE;

  if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create swap chain!");
  }

  vkGetSwapchainImagesKHR(device, swapChain, &imageCount, nullptr);

  if (swapChainImages.count(surface) == 0)
  {
    swapChainImages[surface] = std::vector<VkImage>();
  }

  swapChainImages[surface].resize(imageCount);

  vkGetSwapchainImagesKHR(device, swapChain, &imageCount, swapChainImages[surface].data());
  swapChainImageFormat[surface] = surfaceFormat.format;
  swapChainExtent[surface] = extent;
}

void VulkanDevice::createImageViews(VkSurfaceKHR surface)
{
  if (swapChainImageViews.count(surface) == 0)
  {
    swapChainImageViews[surface] = std::vector<VkImageView>();
  }

  swapChainImageViews[surface].resize(swapChainImages[surface].size());

  for (size_t i = 0; i < swapChainImages.size(); i++)
  {
    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = swapChainImages[surface][i];
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = swapChainImageFormat[surface];
    createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = 1;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &createInfo, nullptr, &swapChainImageViews[surface][i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create image views!");
    }
  }
}
void VulkanDevice::cleanupSwapChain(VkSurfaceKHR surface)
{
  for (auto framebuffer : swapChainFramebuffers[surface])
  {
    vkDestroyFramebuffer(device, framebuffer, nullptr);
  }

  for (auto imageView : swapChainImageViews[surface])
  {
    vkDestroyImageView(device, imageView, nullptr);
  }

  vkDestroySwapchainKHR(device, swapChain[surface], nullptr);
}

void VulkanDevice::createFramebuffers(VkSurfaceKHR surface, VkRenderPass renderPass)
{
  if (swapChainFramebuffers.count(surface) == 0)
  {
    swapChainFramebuffers[surface] = std::vector<VkFramebuffer>();
  }
  swapChainFramebuffers[surface].resize(swapChainImageViews.size());

  for (size_t i = 0; i < swapChainImageViews.size(); i++)
  {
    VkImageView attachments[] = {swapChainImageViews[surface][i]};

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = swapChainExtent[surface].width;
    framebufferInfo.height = swapChainExtent[surface].height;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapChainFramebuffers[surface][i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create framebuffer!");
    }
  }
}
void VulkanDevice::recreateSwapChain(VkSurfaceKHR surface, VkRenderPass renderPass)
{
  SurfaceHandle index = static_cast<SurfaceHandle>(0xFFFFFFFF);

  for (auto &kv : surfaces)
  {
    if (kv.second == surface)
    {
      index = kv.first;
      break;
    }
  }

  uint32_t height = windows[index]->getHeightInPixels();
  uint32_t width = windows[index]->getWidthInPixels();
  
  assert(height > 0 && width > 0);

  vkDeviceWaitIdle(device);

  cleanupSwapChain(surface);
  createSwapChain(surface);
  createImageViews(surface);
  createFramebuffers(surface, renderPass);
}