#include <algorithm>
#include <assert.h>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

#include "VulkanDevice.hpp"
#include "os/print.hpp"
#include "time/TimeSpan.hpp"
#include <unordered_set>

using namespace rhi;
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

VulkanQueueFamilyIndices VulkanDevice::findQueueFamilyIndices()
{
  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice.device, &queueFamilyCount, nullptr);

  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice.device, &queueFamilyCount, queueFamilies.data());

  VulkanQueueFamilyIndices indices{};

  indices.hasComputeFamily = false;
  indices.hasGraphicsFamily = false;
  indices.hasTransferFamily = false;

  std::unordered_set<uint32_t> usedIndices;

  for (uint32_t i = 0; i < queueFamilyCount; ++i)
  {
    const auto &props = queueFamilies[i];

    os::print("Family %u: flags = %x, count = %u\n", i, props.queueFlags, props.queueCount);

    for (auto surface : surfaces)
    {
      VulkanSurface *surfaceImp = reinterpret_cast<VulkanSurface *>(surface.imp);
      VkBool32 supported = VK_FALSE;

      vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice.device, i, surfaceImp->surfaces, &supported);

      if (supported && !surfaceImp->hasPresentFamily)
      {
        surfaceImp->hasPresentFamily = true;
        surfaceImp->presentFamily = i;
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
    VulkanSurface *surfaceImp = reinterpret_cast<VulkanSurface *>(surface.imp);
    if (surfaceImp->hasPresentFamily && !usedIndices.count(surfaceImp->presentFamily))
    {
      usedIndices.insert(surfaceImp->presentFamily);
    }
    else if (!surfaceImp->hasPresentFamily && indices.hasGraphicsFamily)
    {
      surfaceImp->presentFamily = indices.graphicsFamily;
      surfaceImp->hasPresentFamily = true;
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

Surface VulkanDevice::addSurface(VkSurfaceKHR vkSurface)
{
  VulkanSurface *surfaceImp = new VulkanSurface();
  surfaceImp->surfaces = vkSurface;
  Surface surface = buildSurface(surfaceImp);
  surfaces.push_back(surface);
  return surface;
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

VulkanDevice::VulkanDevice(VulkanVersion version, DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures, std::vector<std::string> extensions)
    : eventLoop(VulkanAsyncHandler::getStatus)
{
  VULKAN_DEVICE_SETUP_CHECKS

  this->version = version;
  this->requiredLimits = requiredLimits;
  this->requestedFeaturesFlags = requestedFeatures;
  this->initialized = false;

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
    os::print("[Vulkan Extension]: %s\n", extension);
  }

  initializeInstance(version);
  setupDebugMessenger();
}

void VulkanDevice::init()
{
  VULKAN_DEVICE_SETUP_CHECKS

  for (uint32_t i = 0; i < instanceExtensions.size(); i++)
  {
    os::print("[Vulkan Extension]: %s\n", instanceExtensions[i]);
  }

  initializePhysicalDevice();
  createLogicalDevice();

  fences = new lib::ConcurrentQueue<VkFence>();

  // for (int i = 0; i < 256; i++)
  // {
  //   fences->enqueue(createFence(logicalDevice.device, false));
  // }
}

void VulkanDevice::createLogicalDevice()
{
  // std::vector<VkSurfaceKHR> vkSurfaces;
  // for (auto &p : surfaces)
  // {
  //   vkSurfaces.push_back(p.second.surfaces);
  // }
  indices = findQueueFamilyIndices();

  if ((featureFlags & DeviceFeatures_Graphics) && !indices.hasGraphicsFamily)
  {
    throw std::runtime_error("Missing required queue families");
  }

  if ((featureFlags & DeviceFeatures_Compute) && !indices.hasComputeFamily)
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
    os::print("Compute family = %u\n", indices.computeFamily);
  }

  if (indices.hasGraphicsFamily)
  {
    familyToCount[indices.graphicsFamily] = indices.graphicsQueueCount;
    uniqueFamiles.insert(indices.graphicsFamily);
    os::print("Graphics family = %u\n", indices.graphicsFamily);
  }

  if (indices.hasTransferFamily)
  {
    familyToCount[indices.transferFamily] = indices.transferQueueCount;
    uniqueFamiles.insert(indices.transferFamily);
    os::print("Transfer family = %u\n", indices.transferFamily);
  }

  for (auto surface : surfaces)
  {
    VulkanSurface *surfaceImp = reinterpret_cast<VulkanSurface *>(surface.imp);

    if (surfaceImp->hasPresentFamily)
    {
      familyToCount[surfaceImp->presentFamily] += 1;
      uniqueFamiles.insert(surfaceImp->presentFamily);
      os::print("Preset family = %u\n", surfaceImp->presentFamily);
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

    printf("Family %u queue count = %u\n", familyIndex, familyToCount[familyIndex]);
  }

  VkPhysicalDeviceFeatures deviceFeatures{};

  deviceFeatures.multiDrawIndirect = featureFlags & DeviceFeatures_MultiDrawIndirect ? VK_TRUE : VK_FALSE;
  deviceFeatures.drawIndirectFirstInstance = featureFlags & DeviceFeatures_DrawIndirectFirstInstance ? VK_TRUE : VK_FALSE;

  VkDeviceCreateInfo createInfo = {};

  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
  createInfo.pQueueCreateInfos = queueCreateInfos.data();
  createInfo.pEnabledFeatures = &deviceFeatures;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
  createInfo.ppEnabledExtensionNames = deviceExtensions.data();

  VkDevice device = VK_NULL_HANDLE;

  if (vkCreateDevice(physicalDevice.device, &createInfo, nullptr, &device) != VK_SUCCESS)
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
      VulkanSurface *surfaceImp = reinterpret_cast<VulkanSurface *>(surface.imp);

      if (info.queueFamilyIndex == surfaceImp->presentFamily)
      {
        index = presentCount;
        presentCount++;
        break;
      }
    }

    vkGetDeviceQueue(device, info.queueFamilyIndex, index, &queue);

    if (info.queueFamilyIndex == indices.computeFamily)
    {
      computeQueue.push_back((QueueHandle)queues.size());
      queues.push_back((VulkanQueue){
        .queue = queue,
      });
    }

    if (info.queueFamilyIndex == indices.graphicsFamily)
    {
      graphicsQueue.push_back((QueueHandle)queues.size());
      queues.push_back((VulkanQueue){
        .queue = queue,
      });
    }

    if (info.queueFamilyIndex == indices.transferFamily)
    {
      transferQueue.push_back((QueueHandle)queues.size());
      queues.push_back((VulkanQueue){
        .queue = queue,
      });
    }

    for (auto &surface : surfaces)
    {
      VulkanSurface *surfaceImp = reinterpret_cast<VulkanSurface *>(surface.imp);

      if (info.queueFamilyIndex == surfaceImp->presentFamily)
      {
        surfaceImp->presentQueue = (VulkanQueue){
          .queue = queue,
        };

        break;
      }
    }
  }

  logicalDevice = (VulkanLogicalDevice){
    .device = device,
  };
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

QueueHandle VulkanDevice::getQueue(QueueType type)
{
  switch (type)
  {
  case Queue_Compute:
    return computeQueue[0];
  case Queue_Graphics:
    return graphicsQueue[0];
  case Queue_Transfer:
    return transferQueue[0];
  default:
    abort();
  }
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

    os::print("Device: %s\n", props.deviceName);
    os::print(
        "  Vendor ID: 0x%04x, Device ID: 0x%04x, API Version: %u.%u.%u\n",
        props.vendorID,
        props.deviceID,
        VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion),
        VK_VERSION_PATCH(props.apiVersion));

    os::print("  Features:\n");
    if (featureFlags & DeviceFeatures_Atomic32_AllOps)
      os::print("    - Atomic32_AllOps\n");
    if (featureFlags & DeviceFeatures_Atomic64_MinMax)
      os::print("    - Atomic64_MinMax\n");
    if (featureFlags & DeviceFeatures_Atomic64_AllOps)
      os::print("    - Atomic64_AllOps\n");
    if (featureFlags & DeviceFeatures_DrawIndirectFirstInstance)
      os::print("    - DrawIndirectFirstInstance\n");
    if (featureFlags & DeviceFeatures_MultiDrawIndirect)
      os::print("    - MultiDrawIndirect\n");
    if (featureFlags & DeviceFeatures_GeometryShader)
      os::print("    - GeometryShader\n");
    if (featureFlags & DeviceFeatures_Compute)
      os::print("    - Compute\n");
    if (featureFlags & DeviceFeatures_Graphics)
      os::print("    - Graphics\n");
    if (featureFlags & DeviceFeatures_Timestamp)
      os::print("    - Timestamp\n");
    if (featureFlags & DeviceFeatures_Dedicated)
      os::print("    - Dedicated GPU\n");
    if (featureFlags & DeviceFeatures_Integrated)
      os::print("    - Integrated GPU\n");

    os::print("  Limits:\n");
    os::print("    - Subgroup Size: %zu\n", dprops.sugroupSize);
    os::print("    - Max Memory: %.2f GB\n", static_cast<double>(dprops.maxMemory) / (1024 * 1024 * 1024));
    os::print("    - Max Shared Memory: %.2f KB\n", static_cast<double>(dprops.maxComputeSharedMemorySize) / 1024);
    os::print("    - Max Workgroup Invocations: %zu\n", dprops.maxComputeWorkGroupInvocations);
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

  physicalDevice = physicalDevices[0];
}

VulkanDevice::~VulkanDevice()
{
  if (enableValidationLayers)
  {
    destroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
  }

  for (auto &s : instanceExtensions)
  {
    free(s);
  }

  for (auto &surface : surfaces)
  {
    VulkanSurface *surfaceImp = reinterpret_cast<VulkanSurface *>(surface.imp);
    vkDestroySurfaceKHR(instance, surfaceImp->surfaces, nullptr);
    delete surfaceImp;
  }

  VkFence fence = VK_NULL_HANDLE;
  while (fences->tryDequeue(fence))
  {
    vkDestroyFence(getDevice(), fence, NULL);
  }

  delete fences;

  vkDestroyDevice(logicalDevice.device, nullptr);
  vkDestroyInstance(instance, nullptr);
}

VkMemoryPropertyFlags bufferUsageToVkMemoryPropertyFlags(BufferUsage usage)
{

  VkMemoryPropertyFlags memoryProperties = 0;

  // Flags for CPU mapping
  if (usage & BufferUsage_Push)
    memoryProperties |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  if (usage & BufferUsage_Pull)
    memoryProperties |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  if ((usage & BufferUsage_Push) == 0 && (usage & BufferUsage_Pull) == 0)
    memoryProperties |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  return memoryProperties;
}
VkBufferUsageFlags bufferUsageToVkBufferUsageFlags(BufferUsage usage)
{
  VkBufferUsageFlags usageFlags = 0;

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
  if (usage & BufferUsage_Index)
    usageFlags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  return usageFlags;
}

VulkanHeap::VulkanHeap(VulkanDevice *device, VkDeviceMemory deviceMemory, VkBuffer buffer, size_t size, BufferUsage usage)
    : buffer(buffer), deviceMemory(deviceMemory), device(device), usage(usage), mapped(BufferMap::BufferMap_None), GPUHeap(size)
{
}
VulkanHeap::~VulkanHeap()
{
  vkDestroyBuffer(device->getDevice(), buffer, NULL);
  vkFreeMemory(device->getDevice(), deviceMemory, NULL);
}

GPUHeap *VulkanDevice::allocateHeap(size_t size, BufferUsage usage, void *data)
{
  VULKAN_DEVICE_API_CALL_CHECKS

  // Create buffer
  VkBufferCreateInfo bufferInfo = {};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = bufferUsageToVkBufferUsageFlags(usage);
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkBuffer buffer;
  vkCreateBuffer(logicalDevice.device, &bufferInfo, nullptr, &buffer);

  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(logicalDevice.device, buffer, &memRequirements);

  VkMemoryAllocateInfo allocInfo = {};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, bufferUsageToVkMemoryPropertyFlags(usage));

  VkDeviceMemory bufferMemory;
  vkAllocateMemory(logicalDevice.device, &allocInfo, nullptr, &bufferMemory);
  vkBindBufferMemory(logicalDevice.device, buffer, bufferMemory, 0);

  if (data && (bufferUsageToVkMemoryPropertyFlags(usage) & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
  {
    void *mapped;
    vkMapMemory(logicalDevice.device, bufferMemory, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(logicalDevice.device, bufferMemory);
  }

  return new VulkanHeap(this, bufferMemory, buffer, size, usage);
}

void VulkanDevice::freeHeap(GPUHeap *handle)
{
  VULKAN_DEVICE_API_CALL_CHECKS
  delete handle;
}

BufferMapStatus VulkanDevice::mapBuffer(GPUBuffer handle, BufferMap map, void **ptr)
{
  VULKAN_DEVICE_API_CALL_CHECKS

  if ((uint32_t)map & (uint32_t)BufferMap::BufferMap_Read && (uint32_t)map & (uint32_t)BufferMap::BufferMap_Write)
  {
    throw std::runtime_error("Buffer map needs to be either read or write, not both!");
  }

  VulkanHeap *heap = reinterpret_cast<VulkanHeap *>(handle.heap);
  BufferMap expected = BufferMap::BufferMap_None;

  if (!heap->mapped.compare_exchange_strong(expected, map))
  {
    return BufferMapStatus::BufferMapStatus_Failed;
  }

  vkMapMemory(logicalDevice.device, heap->deviceMemory, handle.offset, handle.size, 0, ptr);
  return BufferMapStatus::BufferMapStatus_Success;
}

void VulkanDevice::unmapBuffer(GPUBuffer handle)
{
  VULKAN_DEVICE_API_CALL_CHECKS
  VulkanHeap *heap = reinterpret_cast<VulkanHeap *>(handle.heap);

  if (heap->mapped.load() != BufferMap::BufferMap_None)
  {
    vkUnmapMemory(logicalDevice.device, heap->deviceMemory);
    heap->mapped.store(BufferMap::BufferMap_None);
  }
}

uint32_t VulkanDevice::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
  VULKAN_DEVICE_API_CALL_CHECKS

  VkPhysicalDeviceMemoryProperties memProperties;
  vkGetPhysicalDeviceMemoryProperties(physicalDevice.device, &memProperties);

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

template <typename T> const T &clamp(const T &value, const T &low, const T &high)
{
  if (value < low)
    return low;
  if (value > high)
    return high;
  return value;
}

VkExtent2D VulkanDevice::chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities, uint32_t width, uint32_t height)
{
  if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
  {
    return capabilities.currentExtent;
  }
  else
  {
    // SurfaceHandle index = static_cast<SurfaceHandle>(0xFFFFFFFF);

    // for (auto& &kv : surfaces)
    // {
    //   if (kv.second == surface)
    //   {
    //     index = kv.first;
    //     break;
    //   }
    // }

    // uint32_t height =  windows[index]->getHeightInPixels();
    // uint32_t width = windows[index]->getWidthInPixels();

    VkExtent2D actualExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    actualExtent.width = clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    actualExtent.height = clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

    return actualExtent;
  }
}

SwapChain VulkanDevice::createSwapChain(Surface surfaceHandle, uint32_t width, uint32_t height)
{
  VULKAN_DEVICE_API_CALL_CHECKS
  VulkanSurface *surfaceImp = reinterpret_cast<VulkanSurface *>(surfaceHandle.imp);

  SwapChainSupportDetails swapChainSupport = querySwapChainSupport(surfaceImp->surfaces, physicalDevice.device);
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
  createInfo.surface = surfaceImp->surfaces;

  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = extent;
  createInfo.imageArrayLayers = 1;
  createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

  std::vector<VkSurfaceKHR> surfaces = {surfaceImp->surfaces};

  uint32_t queueFamilyIndices[2] = {indices.graphicsFamily, surfaceImp->presentFamily};

  if (indices.graphicsFamily != surfaceImp->presentFamily)
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

  // VkSwapchainKHR swapChain = VK_NULL_HANDLE;
  VulkanSwapChain *swapChainImp = new VulkanSwapChain();

  if (vkCreateSwapchainKHR(logicalDevice.device, &createInfo, nullptr, &swapChainImp->swapChain) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create swap chain!");
  }

  vkGetSwapchainImagesKHR(logicalDevice.device, swapChainImp->swapChain, &imageCount, nullptr);
  std::vector<VkImage> images;
  std::vector<VkImageView> imagesViews;

  if (swapChainImp->swapChainImages.size() == 0)
  {
    imagesViews = std::vector<VkImageView>(imageCount);
    images = std::vector<VkImage>(imageCount);
  }

  vkGetSwapchainImagesKHR(logicalDevice.device, swapChainImp->swapChain, &imageCount, images.data());

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

    if (vkCreateImageView(logicalDevice.device, &createInfo, nullptr, &imagesViews[i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create image views!");
    }
  }

  for (int i = 0; i < images.size(); i++)
  {
    VulkanTextureView *view = new VulkanTextureView();

    view->fence = VK_NULL_HANDLE;
    view->achireSemaphore = VK_NULL_HANDLE;
    view->presentSemaphore = VK_NULL_HANDLE;
    view->view = imagesViews[i];
    view->renderData.swapChain = swapChainImp;
    view->renderData.swapChainImageIndex = i;

    swapChainImp->swapChainImages.push_back(buildTextureView(view));
  }

  swapChainImp->swapChainImageFormat = surfaceFormat.format;
  swapChainImp->swapChainExtent = extent;
  swapChainImp->support = swapChainSupport;
  swapChainImp->presentQueue = surfaceImp->presentQueue;
  swapChainImp->achireSemaphores.resize(images.size());
  swapChainImp->presentSemaphores.resize(images.size());

  for (int i = 0; i < images.size(); i++)
  {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(logicalDevice.device, &semaphoreInfo, nullptr, &swapChainImp->achireSemaphores[i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create semaphore for image views!");
    }
  }

  for (int i = 0; i < images.size(); i++)
  {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(logicalDevice.device, &semaphoreInfo, nullptr, &swapChainImp->presentSemaphores[i]) != VK_SUCCESS)
    {
      throw std::runtime_error("failed to create semaphore for image views!");
    }
  }

  return buildSwapChain(swapChainImp);
}

VulkanSwapChain &VulkanSwapChain::operator=(VulkanSwapChain &other)
{
  this->swapChain = other.swapChain;
  this->swapChainImageFormat = other.swapChainImageFormat;
  this->swapChainExtent = other.swapChainExtent;
  this->support = other.support;
  this->presentQueue = other.presentQueue;
  this->swapChainImages = std::move(other.swapChainImages);
  this->presentSemaphores = std::move(other.presentSemaphores);
  this->achireSemaphores = std::move(other.achireSemaphores);
  this->currentPrimitive = 0;

  other.swapChain = VK_NULL_HANDLE;
  other.swapChainImageFormat = VK_FORMAT_UNDEFINED;
  other.swapChainExtent = {0, 0};
  other.presentQueue.queue = VK_NULL_HANDLE;
  return *this;
}

void VulkanDevice::destroySwapChain(SwapChain swapChain)
{
  VulkanSwapChain *swapChainImp = reinterpret_cast<VulkanSwapChain *>(swapChain.imp);

  vkDeviceWaitIdle(logicalDevice.device);

  for (auto &imageView : swapChainImp->swapChainImages)
  {
    VulkanTextureView *view = reinterpret_cast<VulkanTextureView *>(imageView.imp);
    vkDestroyImageView(logicalDevice.device, view->view, nullptr);
    delete view;
  }

  swapChainImp->swapChainImages.clear();

  for (auto &semaphore : swapChainImp->achireSemaphores)
  {
    vkDestroySemaphore(getDevice(), semaphore, NULL);
  }

  for (auto &semaphore : swapChainImp->presentSemaphores)
  {
    vkDestroySemaphore(getDevice(), semaphore, NULL);
  }

  if (swapChainImp->swapChain != VK_NULL_HANDLE)
  {
    vkDestroySwapchainKHR(logicalDevice.device, swapChainImp->swapChain, nullptr);
  }

  delete swapChainImp;
}

Shader VulkanDevice::createShader(const VulkanSpirVShaderData &data)
{
  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = data.src.size();
  createInfo.pCode = reinterpret_cast<const uint32_t *>(data.src.data());

  VkShaderModule shaderModule = VK_NULL_HANDLE;

  if (vkCreateShaderModule(logicalDevice.device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create shader module!");
  }

  VulkanShader *s = new VulkanShader();
  s->shaderModule = shaderModule;
  return buildShader(s);
}

void VulkanDevice::destroyShader(Shader handle)
{
  VulkanShader *s = reinterpret_cast<VulkanShader *>(handle.imp);
  vkDestroyShaderModule(logicalDevice.device, s->shaderModule, nullptr);
  delete s;
}

VkShaderStageFlags toVkShaderStageFlags(BindingVisibility visibility)
{
  VkShaderStageFlags flags = 0;
  if (static_cast<uint32_t>(visibility) & static_cast<uint32_t>(BindingVisibility::BindingVisibility_Vertex))
    flags |= VK_SHADER_STAGE_VERTEX_BIT;
  if (static_cast<uint32_t>(visibility) & static_cast<uint32_t>(BindingVisibility::BindingVisibility_Fragment))
    flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
  if (static_cast<uint32_t>(visibility) & static_cast<uint32_t>(BindingVisibility::BindingVisibility_Compute))
    flags |= VK_SHADER_STAGE_COMPUTE_BIT;
  return flags;
}

VkDescriptorSetLayout VulkanDevice::createDescriptorSetLayoutFromGroup(const BindingGroupLayout &group)
{
  std::vector<VkDescriptorSetLayoutBinding> bindings;

  // ---- Buffers ----
  for (uint32_t i = 0; i < group.buffersCount; ++i)
  {
    const auto &entry = group.buffers[i];

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = entry.binding;
    binding.descriptorCount = 1;
    binding.stageFlags = toVkShaderStageFlags(entry.visibility);
    binding.pImmutableSamplers = nullptr;

    switch (entry.usage)
    {
    case BufferUsage::BufferUsage_Uniform:
      binding.descriptorType = entry.isDynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      break;
    case BufferUsage::BufferUsage_Storage:
      binding.descriptorType = entry.isDynamic ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      break;
    default:
      throw std::runtime_error("Buffer type not supported for binding");
    }
    bindings.push_back(binding);
  }

  for (uint32_t i = 0; i < group.samplersCount; ++i)
  {
    const auto &entry = group.samplers[i];

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = entry.binding;
    binding.descriptorCount = 1;
    binding.stageFlags = toVkShaderStageFlags(entry.visibility);
    binding.pImmutableSamplers = nullptr;

    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings.push_back(binding);
  }

  for (uint32_t i = 0; i < group.texturesCount; ++i)
  {
    const auto &entry = group.textures[i];

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = entry.binding;
    binding.descriptorCount = 1;
    binding.stageFlags = toVkShaderStageFlags(entry.visibility);
    binding.pImmutableSamplers = nullptr;

    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings.push_back(binding);
  }

  for (uint32_t i = 0; i < group.storageTexturesCount; ++i)
  {
    const auto &entry = group.storageTextures[i];

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = entry.binding;
    binding.descriptorCount = 1;
    binding.stageFlags = toVkShaderStageFlags(entry.visibility);
    binding.pImmutableSamplers = nullptr;

    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings.push_back(binding);
  }

  if (bindings.empty())
    return VK_NULL_HANDLE;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();

  VkDescriptorSetLayout descriptorSetLayout;
  if (vkCreateDescriptorSetLayout(logicalDevice.device, &layoutInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create descriptor set layout!");
  }

  return descriptorSetLayout;
}

void VulkanDevice::collectDescriptorSetLayouts(const BindingsLayoutInfo &info, std::vector<VkDescriptorSetLayout> &outLayouts)
{
  for (size_t i = 0; i < info.groupsCount; ++i)
  {
    VkDescriptorSetLayout layout = createDescriptorSetLayoutFromGroup(info.groups[i]);
    if (layout != VK_NULL_HANDLE)
    {
      outLayouts.push_back(layout);
    }
  }
}

BindingsLayout VulkanDevice::createBindingsLayout(const BindingsLayoutInfo &info)
{
  std::vector<VkDescriptorSetLayout> descriptorSetLayouts;
  collectDescriptorSetLayouts(info, descriptorSetLayouts);

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(descriptorSetLayouts.size());
  pipelineLayoutInfo.pSetLayouts = descriptorSetLayouts.data();
  pipelineLayoutInfo.pushConstantRangeCount = 0;
  pipelineLayoutInfo.pPushConstantRanges = nullptr;

  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  if (vkCreatePipelineLayout(getDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create pipeline layout!");
  }

  VulkanBindingsLayout *l = new VulkanBindingsLayout();
  l->pipelineLayout = pipelineLayout;
  l->descriptorSetLayouts = std::move(descriptorSetLayouts);
  l->info = info;
  return buildBindingsLayout(l);
}

void VulkanDevice::destroyBindingsLayout(BindingsLayout layout)
{
  VulkanBindingsLayout *l = reinterpret_cast<VulkanBindingsLayout *>(layout.imp);
  vkDestroyPipelineLayout(getDevice(), l->pipelineLayout, NULL);
  delete l;
}

Format VulkanDevice::getSwapChainFormat(SwapChain handle)
{
  VulkanSwapChain *sc = reinterpret_cast<VulkanSwapChain *>(handle.imp);
  return vkFormatToFormat(sc->swapChainImageFormat);
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

VkRenderPass VulkanDevice::createRenderPass(ColorAttatchment *attachments, uint32_t attatchmentsCount, DepthAttatchment depth)
{
  std::vector<VkAttachmentDescription> attachmentsDescriptions;
  std::vector<VkAttachmentReference> colorAttachmentRefs;

  // Color attachments
  for (size_t i = 0; i < attatchmentsCount; ++i)
  {
    VkAttachmentDescription colorAttachment{};

    colorAttachment.format = formatToVkFormat(attachments[i].format);
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

  if (vkCreateRenderPass(logicalDevice.device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create render pass!");
  }

  return renderPass;
}

// void VulkanDevice::destroyRenderPass(RenderPassHandle handle)
// {
//   // vkDestroyFramebuffer(logicalDevice.device, renderPasses[handle].frameBuffer, NULL);
//   vkDestroyRenderPass(logicalDevice.device, renderPasses[handle].renderPass, NULL);
//   renderPasses.erase(handle);
// }

size_t GetVkFormatSize(VkFormat format)
{
  return vk::blockSize((vk::Format)format);
}

ComputePipeline VulkanDevice::createComputePipeline(const ComputePipelineInfo &info)
{
  VkPipelineShaderStageCreateInfo computeShaderStageInfo{};
  computeShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  computeShaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;

  VulkanShader *shader = reinterpret_cast<VulkanShader *>(info.shader.imp);
  if (shader->shaderModule == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Invalid compute shader!");
  }
  computeShaderStageInfo.module = shader->shaderModule;
  computeShaderStageInfo.pName = info.entry;

  VulkanBindingsLayout *layout = reinterpret_cast<VulkanBindingsLayout *>(info.layout.imp);
  if (layout->pipelineLayout == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Invalid pipeline layout in ComputePipelineInfo!");
  }

  VkComputePipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipelineInfo.stage = computeShaderStageInfo;
  pipelineInfo.layout = layout->pipelineLayout;
  pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
  pipelineInfo.basePipelineIndex = -1;

  VkPipeline pipeline = VK_NULL_HANDLE;
  if (vkCreateComputePipelines(logicalDevice.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create compute pipeline!");
  }

  VulkanComputePipeline *vkPipeline = new VulkanComputePipeline();
  vkPipeline->pipeline = pipeline;
  vkPipeline->layout = info.layout;

  return buildComputePipeline(vkPipeline);
}

void VulkanDevice::destroyComputePipeline(ComputePipeline pipeline)
{
  VulkanComputePipeline *vulkanPipeline = reinterpret_cast<VulkanComputePipeline *>(pipeline.imp);

  if (vulkanPipeline)
  {
    if (vulkanPipeline->pipeline != VK_NULL_HANDLE)
    {
      vkDestroyPipeline(logicalDevice.device, vulkanPipeline->pipeline, nullptr);
    }

    delete vulkanPipeline;
    pipeline.imp = nullptr;
  }
}

GraphicsPipeline VulkanDevice::createGraphicsPipeline(GraphicsPipelineInfo info)
{
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
  std::map<uint32_t, uint32_t> bindingStrideMap;

  for (int i = 0; i < info.vertexStage.vertexLayoutElementsCount; i++)
  {
    VkVertexInputAttributeDescription desc = {};

    desc.format = formatToVkFormat(typeToFormat(info.vertexStage.vertexLayoutElements[i].type));
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

  VulkanShader *vertex = reinterpret_cast<VulkanShader *>(info.vertexStage.vertexShader.imp);
  VulkanShader *fragment = reinterpret_cast<VulkanShader *>(info.fragmentStage.fragmentShader.imp);

  if (vertex->shaderModule == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Invalid vertex shader!");
  }
  if (fragment->shaderModule == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Invalid fragment shader!");
  }

  VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
  vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vertShaderStageInfo.module = vertex->shaderModule;
  vertShaderStageInfo.pName = info.vertexStage.shaderEntry;

  VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
  fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  fragShaderStageInfo.module = fragment->shaderModule;
  fragShaderStageInfo.pName = info.fragmentStage.shaderEntry;

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
  VulkanBindingsLayout *layout = reinterpret_cast<VulkanBindingsLayout *>(info.layout.imp);
  pipelineInfo.layout = layout->pipelineLayout;

  VkRenderPass renderPass = createRenderPass(info.fragmentStage.colorAttatchments, info.fragmentStage.colorAttatchmentsCount, info.fragmentStage.depthAttatchment);

  pipelineInfo.renderPass = renderPass;
  VkPipeline pipeline = VK_NULL_HANDLE;

  if (vkCreateGraphicsPipelines(logicalDevice.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create graphics pipeline!");
  }

  VulkanGraphicsPipeline *vkPipeline = new VulkanGraphicsPipeline();

  vkPipeline->pipeline = pipeline;
  vkPipeline->renderPass = renderPass;
  vkPipeline->info = info;

  return buildGraphicsPipeline(vkPipeline);
}

TextureView VulkanDevice::getCurrentSwapChainTextureView(SwapChain swapChainHandle)
{
  VulkanSwapChain *swapChain = reinterpret_cast<VulkanSwapChain *>(swapChainHandle.imp);

  uint32_t index = UINT32_MAX;
  uint32_t current = swapChain->currentPrimitive.fetch_add(1) % swapChain->swapChainImages.size();

  if (vkAcquireNextImageKHR(logicalDevice.device, swapChain->swapChain, UINT64_MAX, swapChain->achireSemaphores[current], VK_NULL_HANDLE, &index) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to achire next image, you probably did not submit the commands");
  }

  TextureView handle = swapChain->swapChainImages[index];

  VkSemaphore expectedAchire = VK_NULL_HANDLE;
  VkSemaphore expectedPresent = VK_NULL_HANDLE;

  VulkanTextureView *viewImp = reinterpret_cast<VulkanTextureView *>(handle.imp);

  while (!viewImp->achireSemaphore.compare_exchange_strong(expectedAchire, swapChain->achireSemaphores[current]))
  {
  }

  while (!viewImp->presentSemaphore.compare_exchange_strong(expectedPresent, swapChain->presentSemaphores[current]))
  {
  }

  return handle;
}

void VulkanDevice::destroyGraphicsPipeline(GraphicsPipeline handle)
{
  VulkanGraphicsPipeline *vkPipeline = reinterpret_cast<VulkanGraphicsPipeline *>(handle.imp);

  vkDestroyPipeline(getDevice(), vkPipeline->pipeline, NULL);
  vkDestroyRenderPass(getDevice(), vkPipeline->renderPass, NULL);

  delete vkPipeline;
}

VkCommandPool VulkanDevice::createCommandPool(uint32_t queueFamilyIndex)
{
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.queueFamilyIndex = queueFamilyIndex;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // Optional

  VkCommandPool commandPool;
  if (vkCreateCommandPool(logicalDevice.device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create command pool");
  }
  return commandPool;
}

void VulkanDevice::destroyCommandPool(VkCommandPool cp)
{
  vkDestroyCommandPool(logicalDevice.device, cp, NULL);
}

std::vector<VkCommandBuffer> VulkanDevice::allocateCommandBuffers(VkCommandPool commandPool, uint32_t count, VkCommandBufferLevel level)
{
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = commandPool;
  allocInfo.level = level;
  allocInfo.commandBufferCount = count;

  std::vector<VkCommandBuffer> commandBuffers(count);
  if (vkAllocateCommandBuffers(logicalDevice.device, &allocInfo, commandBuffers.data()) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to allocate command buffers");
  }
  return commandBuffers;
}

void VulkanDevice::freeCommandBuffers(VkCommandPool cp, std::vector<VkCommandBuffer> commandBuffers)
{
  vkFreeCommandBuffers(logicalDevice.device, cp, commandBuffers.size(), commandBuffers.data());
}

CommandBuffer VulkanDevice::createCommandBuffer()
{
  VkCommandPool commandPool = createCommandPool(indices.graphicsFamily);

  auto vkCmdBuffers = allocateCommandBuffers(commandPool, 1, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
  if (vkCmdBuffers.empty())
  {
    destroyCommandPool(commandPool);
    throw std::runtime_error("failed to allocate command buffer");
  }

  VkCommandBuffer cmd = vkCmdBuffers[0];
  VulkanCommandBuffer *commandBufferData = new VulkanCommandBuffer();
  commandBufferData->commandBuffer = cmd;
  commandBufferData->commandPool = commandPool;
  return buildCommandBuffer(commandBufferData);
}

void VulkanDevice::destroyCommandBuffer(CommandBuffer handle)
{
  VulkanCommandBuffer *data = reinterpret_cast<VulkanCommandBuffer *>(handle.imp);

  VkCommandBuffer cmd = data->commandBuffer;
  VkCommandPool pool = data->commandPool;

  freeCommandBuffers(pool, std::vector<VkCommandBuffer>{cmd});
  destroyCommandPool(pool);

  delete data;
}

void VulkanDevice::beginCommandBuffer(CommandBuffer handle)
{

  VkCommandBuffer cmd = reinterpret_cast<VulkanCommandBuffer *>(handle.imp)->commandBuffer;

  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  beginInfo.pInheritanceInfo = nullptr;

  VkResult r = vkBeginCommandBuffer(cmd, &beginInfo);
  if (r != VK_SUCCESS)
    throw std::runtime_error("vkBeginCommandBuffer failed");
}

void VulkanDevice::endCommandBuffer(CommandBuffer handle)
{
  VkCommandBuffer cmd = reinterpret_cast<VulkanCommandBuffer *>(handle.imp)->commandBuffer;
  VkResult r = vkEndCommandBuffer(cmd);
  if (r != VK_SUCCESS)
    throw std::runtime_error("vkEndCommandBuffer failed");
}

void VulkanDevice::cmdBindGraphicsPipeline(CommandBuffer handle, GraphicsPipeline pipelineHandle)
{
  VulkanCommandBuffer *commandBuffer = reinterpret_cast<VulkanCommandBuffer *>(handle.imp);
  VkCommandBuffer cmd = commandBuffer->commandBuffer;
  VkPipeline pipeline = reinterpret_cast<VulkanGraphicsPipeline *>(pipelineHandle.imp)->pipeline;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

  if (commandBuffer->boundComputePipeline || commandBuffer->boundGraphicsPipeline)
  {
    throw std::runtime_error("pipeline already binded to command buffer");
  }

  commandBuffer->boundGraphicsPipeline = reinterpret_cast<VulkanGraphicsPipeline *>(pipelineHandle.imp);
}

void VulkanDevice::cmdBindComputePipeline(CommandBuffer handle, ComputePipeline pipelineHandle)
{
  VulkanCommandBuffer *commandBuffer = reinterpret_cast<VulkanCommandBuffer *>(handle.imp);
  VkCommandBuffer cmd = commandBuffer->commandBuffer;
  VkPipeline pipeline = reinterpret_cast<VulkanComputePipeline *>(pipelineHandle.imp)->pipeline;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

  if (commandBuffer->boundComputePipeline || commandBuffer->boundGraphicsPipeline)
  {
    throw std::runtime_error("pipeline already binded to command buffer");
  }

  commandBuffer->boundComputePipeline = reinterpret_cast<VulkanComputePipeline *>(pipelineHandle.imp);
}

void VulkanDevice::cmdBindBindingGroups(CommandBuffer cmdBuffer, BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount)
{

  VulkanCommandBuffer *commandBuffer = reinterpret_cast<VulkanCommandBuffer *>(cmdBuffer.imp);
  VkPipelineLayout layout = VK_NULL_HANDLE;

  if (commandBuffer->boundComputePipeline)
  {
    layout = reinterpret_cast<VulkanBindingsLayout *>(commandBuffer->boundComputePipeline->layout.imp)->pipelineLayout;
  }
  else if (commandBuffer->boundGraphicsPipeline)
  {
    layout = reinterpret_cast<VulkanBindingsLayout *>(commandBuffer->boundGraphicsPipeline->layout.imp)->pipelineLayout;
  }
  else
  {
    throw std::runtime_error("No bound pipeline");
  }
  VulkanBindingGroups *vkGroups = reinterpret_cast<VulkanBindingGroups *>(groups.imp);

  if (vkGroups->descriptorSets.empty())
    return;

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

  vkCmdBindDescriptorSets(
      commandBuffer->commandBuffer, point, layout, 0, static_cast<uint32_t>(vkGroups->descriptorSets.size()), vkGroups->descriptorSets.data(), dynamicOffsetsCount, dynamicOffsets);
}

void VulkanDevice::cmdBindVertexBuffer(CommandBuffer handle, uint32_t slot, GPUBuffer bufferHandle)
{
  VkCommandBuffer cmd = reinterpret_cast<VulkanCommandBuffer *>(handle.imp)->commandBuffer;
  VkBuffer vkBuf = reinterpret_cast<VulkanHeap *>(bufferHandle.heap)->buffer;
  VkDeviceSize vkOffset = static_cast<VkDeviceSize>(bufferHandle.offset);

  vkCmdBindVertexBuffers(cmd, slot, 1, &vkBuf, &vkOffset);
}

void VulkanDevice::cmdBindIndexBuffer(CommandBuffer handle, GPUBuffer bufferHandle, Type type)
{
  VkCommandBuffer cmd = reinterpret_cast<VulkanCommandBuffer *>(handle.imp)->commandBuffer;
  VkBuffer vkBuf = reinterpret_cast<VulkanHeap *>(bufferHandle.heap)->buffer;
  VkDeviceSize vkOffset = static_cast<VkDeviceSize>(bufferHandle.offset);

  VkIndexType vkIndexType = VK_INDEX_TYPE_UINT32;
  if (type == Type_Uint16)
    vkIndexType = VK_INDEX_TYPE_UINT16;
  else if (type == Type_Uint32)
    vkIndexType = VK_INDEX_TYPE_UINT32;
  vkCmdBindIndexBuffer(cmd, vkBuf, vkOffset, vkIndexType);
}

void VulkanDevice::cmdDraw(CommandBuffer handle, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
  VkCommandBuffer cmd = reinterpret_cast<VulkanCommandBuffer *>(handle.imp)->commandBuffer;
  vkCmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanDevice::cmdDrawIndexed(CommandBuffer handle, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
  VkCommandBuffer cmd = reinterpret_cast<VulkanCommandBuffer *>(handle.imp)->commandBuffer;
  vkCmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VulkanDevice::cmdDrawIndexedIndirect(CommandBuffer handle, GPUBuffer indirectBuffer, size_t offset, uint32_t drawCount, uint32_t stride)
{
  VkCommandBuffer cmd = reinterpret_cast<VulkanCommandBuffer *>(handle.imp)->commandBuffer;
  VkBuffer vkBuf = reinterpret_cast<VulkanHeap *>(indirectBuffer.heap)->buffer;
  VkDeviceSize vkOffset = static_cast<VkDeviceSize>(offset);

  vkCmdDrawIndexedIndirect(cmd, vkBuf, vkOffset, drawCount, stride);
}

VkFence VulkanDevice::getFence()
{
  VkFence data;

  if (fences->tryDequeue(data))
  {
    vkResetFences(logicalDevice.device, 1, &data);
  }
  else
  {
    data = createFence(logicalDevice.device, false);
  }

  return data;
}

VulkanAsyncHandler::VulkanAsyncHandler(VulkanDevice *device, VkFence fence, std::vector<VkFramebuffer> &fb, std::vector<TextureView> &views)
{
  this->device = device;
  this->fence = fence;
  this->views = std::move(views);
  this->framebuffers = std::move(fb);
}

FenceStatus VulkanAsyncHandler::getStatus(VulkanAsyncHandler &future)
{
  switch (vkGetFenceStatus(future.device->getDevice(), future.fence))
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

void VulkanDevice::cleanupSubmitCallback(VulkanAsyncHandler &future)
{
  if (future.device->fences->getApproximateSize() > 1024)
  {
    vkDestroyFence(future.device->getDevice(), future.fence, NULL);
  }
  else
  {
    future.device->fences->enqueue(future.fence);
    future.fence = VK_NULL_HANDLE;
  }

  for (auto fb : future.framebuffers)
  {
    vkDestroyFramebuffer(future.device->getDevice(), fb, NULL);
  }

  for (auto &view : future.views)
  {
    VulkanTextureView *viewImp = reinterpret_cast<VulkanTextureView *>(view.imp);
    viewImp->fence = VK_NULL_HANDLE;
    viewImp->presentSemaphore = VK_NULL_HANDLE;
    viewImp->achireSemaphore = VK_NULL_HANDLE;
  }

  future.framebuffers.clear();
  future.views.clear();
}

VulkanFuture::VulkanFuture(AsyncEvent<VulkanAsyncHandler> &&handler) : handler(std::forward<AsyncEvent<VulkanAsyncHandler>>(handler))
{
}

void VulkanDevice::tick()
{
  eventLoop.tick();
}

GPUFuture VulkanDevice::submit(QueueHandle queueHandle, CommandBuffer *commandBuffers, uint32_t count)
{
  if ((uint32_t)queueHandle >= queues.size())
  {
    throw std::runtime_error("invalid queue handle");
  }

  VkQueue queue = queues[(uint32_t)queueHandle].queue;

  std::vector<VkCommandBuffer> vkCmds;

  vkCmds.reserve(count);

  for (uint32_t i = 0; i < count; ++i)
  {
    VulkanCommandBuffer *commandBuffer = reinterpret_cast<VulkanCommandBuffer *>(commandBuffers[i].imp);
    vkCmds.push_back(commandBuffer->commandBuffer);
  }

  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};

  std::vector<VkSemaphore> achireSemaphores;
  std::vector<VkSemaphore> presentSemaphores;

  for (int i = 0; i < count; i++)
  {
    VulkanCommandBuffer *commandBuffer = reinterpret_cast<VulkanCommandBuffer *>(commandBuffers[i].imp);
    for (auto frameData : commandBuffer->renderPasses)
    {
      for (auto s : frameData.achireSemaphores)
      {
        if (s != VK_NULL_HANDLE)
        {
          achireSemaphores.push_back(s);
        }
      }
      for (auto s : frameData.presentSemaphores)
      {
        if (s != VK_NULL_HANDLE)
        {
          presentSemaphores.push_back(s);
        }
      }
    }
  }

  submitInfo.waitSemaphoreCount = achireSemaphores.size();
  submitInfo.pWaitSemaphores = achireSemaphores.data();
  submitInfo.pWaitDstStageMask = nullptr;
  submitInfo.commandBufferCount = static_cast<uint32_t>(vkCmds.size());
  submitInfo.pCommandBuffers = vkCmds.data();
  submitInfo.signalSemaphoreCount = presentSemaphores.size();
  submitInfo.pSignalSemaphores = presentSemaphores.data();

  VkFence fence = getFence();

  if (vkQueueSubmit(queue, 1, &submitInfo, fence) != VK_SUCCESS)
  {
    throw std::runtime_error("vkQueueSubmit failed");
  }

  std::vector<VkFramebuffer> framebuffers;
  std::vector<TextureView> views;

  for (int i = 0; i < count; i++)
  {
    VulkanCommandBuffer *commandBuffer = reinterpret_cast<VulkanCommandBuffer *>(commandBuffers[i].imp);

    for (auto frameData : commandBuffer->renderPasses)
    {
      std::unordered_map<VkQueue, std::vector<VkSwapchainKHR>> vkSwapChains;
      std::unordered_map<VkQueue, std::vector<uint32_t>> swapChainsImageIndices;

      framebuffers.push_back(frameData.frameBuffer);

      for (auto view : frameData.views)
      {
        views.push_back(view);
      }

      for (auto info : frameData.attatchments)
      {
        if (info.swapChain != NULL)
        {
          vkSwapChains[info.presentQueue].push_back(info.swapChain->swapChain);
          swapChainsImageIndices[info.presentQueue].push_back(info.swapChainImageIndex);
        }
      }

      for (auto &[queue, swapChains] : vkSwapChains)
      {
        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};

        presentInfo.waitSemaphoreCount = presentSemaphores.size();
        presentInfo.pWaitSemaphores = presentSemaphores.data();

        presentInfo.swapchainCount = swapChains.size();
        presentInfo.pSwapchains = swapChains.data();
        presentInfo.pImageIndices = swapChainsImageIndices[queue].data();
        presentInfo.pResults = nullptr;

        VkResult result = vkQueuePresentKHR(queue, &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
        }
        if (result == VK_SUBOPTIMAL_KHR)
        {
        }
      }
    }
  }

  for (int i = 0; i < count; i++)
  {
    destroyCommandBuffer(commandBuffers[i]);
  }

  VulkanAsyncHandler handler = VulkanAsyncHandler(this, fence, framebuffers, views);
  VulkanFuture *future = new VulkanFuture(eventLoop.submit(handler, cleanupSubmitCallback));

  return GPUFuture(future);
}

// waitIdle
void VulkanDevice::waitIdle()
{
  vkDeviceWaitIdle(logicalDevice.device);
  eventLoop.tick();
}

void VulkanDevice::wait(GPUFuture &future)
{
  if (future.get() == NULL)
  {
    return;
  }

  VulkanFuture *vkFuture = reinterpret_cast<VulkanFuture *>(future.get());
  vkFuture->handler.wait(eventLoop);
}

void VulkanDevice::cmdBeginRenderPass(CommandBuffer cmdHandle, const rhi::RenderPassInfo &rpInfo)
{
  // Map your CommandBufferHandle to VkCommandBuffer
  GraphicsPipeline bindedPipeline;

  VulkanCommandBuffer *commandBuffer = reinterpret_cast<VulkanCommandBuffer *>(cmdHandle.imp);

  if (!commandBuffer->boundGraphicsPipeline)
  {
    throw std::runtime_error("no pipeline was bound");
  }

  VulkanGraphicsPipeline *pipelineData = reinterpret_cast<VulkanGraphicsPipeline *>(bindedPipeline.imp);

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

  std::vector<TextureView> views;
  std::vector<VkImageView> attachments;
  std::vector<VkSemaphore> achireSemaphores;
  std::vector<VkSemaphore> presentSemaphores;

  if (rpInfo.colorAttachmentsCount != pipelineData->info.fragmentStage.colorAttatchmentsCount)
  {
    throw std::runtime_error("render pass color attatchments count does not match pipeline");
  }

  if (rpInfo.depthStencilAttachment != NULL && pipelineData->info.fragmentStage.depthAttatchment.storeOp == StoreOp::StoreOp_DontCare)
  {
    throw std::runtime_error("render pass depth attatchment not configured given pipeline");
  }

  for (int i = 0; i < rpInfo.colorAttachmentsCount; i++)
  {
    if (rpInfo.colorAttachments[i].view.imp == NULL)
    {
      throw std::runtime_error("color attatchment view not setup");
    }

    VulkanTextureView *texture = reinterpret_cast<VulkanTextureView *>(rpInfo.colorAttachments[i].view.imp);

    views.push_back(rpInfo.colorAttachments[i].view);
    attachments.push_back(texture->view);

    if (texture->achireSemaphore != VK_NULL_HANDLE)
    {
      achireSemaphores.push_back(texture->achireSemaphore);
    }

    if (texture->presentSemaphore != VK_NULL_HANDLE)
    {
      presentSemaphores.push_back(texture->presentSemaphore);
    }
  }

  if (rpInfo.depthStencilAttachment != nullptr)
  {
    if (rpInfo.depthStencilAttachment->view.imp == NULL)
    {
      throw std::runtime_error("depth attatchment view not setup");
    }

    VulkanTextureView *depthTexture = reinterpret_cast<VulkanTextureView *>(rpInfo.depthStencilAttachment->view.imp);
    attachments.push_back(depthTexture->view);
    views.push_back(rpInfo.depthStencilAttachment->view);
  }

  framebufferInfo.attachmentCount = attachments.size();
  framebufferInfo.pAttachments = attachments.data();
  framebufferInfo.width = rpInfo.viewport.width;
  framebufferInfo.height = rpInfo.viewport.height;
  framebufferInfo.layers = 1;

  VkFramebuffer frameBuffer;

  if (vkCreateFramebuffer(logicalDevice.device, &framebufferInfo, nullptr, &frameBuffer) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create framebuffer!");
  }

  VulkanCommandBufferRenderPass commandBufferFrameData;
  commandBufferFrameData.frameBuffer = frameBuffer;
  commandBufferFrameData.renderPass = pipelineData->renderPass;
  commandBufferFrameData.achireSemaphores = std::move(achireSemaphores);
  commandBufferFrameData.presentSemaphores = std::move(presentSemaphores);
  commandBufferFrameData.views = std::move(views);

  for (int i = 0; i < rpInfo.colorAttachmentsCount; i++)
  {
    VulkanTextureView *view = reinterpret_cast<VulkanTextureView *>(rpInfo.colorAttachments[i].view.imp);
    if (view->renderData.swapChain != NULL)
    {
      VulkanAttatchment info = {
        .swapChain = NULL,
        .swapChainImageIndex = UINT32_MAX,
      };

      info.presentQueue = view->renderData.swapChain->presentQueue.queue;
      info.swapChain = view->renderData.swapChain;
      info.swapChainImageIndex = view->renderData.swapChainImageIndex;
      commandBufferFrameData.attatchments.push_back(info);
    }
  }

  commandBuffer->renderPasses.push_back(commandBufferFrameData);

  // Build clear values array (color + optional depth)
  std::vector<VkClearValue> clearValues;

  for (int i = 0; i < rpInfo.colorAttachmentsCount; i++)
  {
    VkClearValue clearColor{};

    clearColor.color.float32[0] = rpInfo.colorAttachments[i].clearValue.R;
    clearColor.color.float32[1] = rpInfo.colorAttachments[i].clearValue.G;
    clearColor.color.float32[2] = rpInfo.colorAttachments[i].clearValue.B;
    clearColor.color.float32[3] = rpInfo.colorAttachments[i].clearValue.A;

    clearValues.push_back(clearColor);
  }

  if (rpInfo.depthStencilAttachment != NULL)
  {
    VkClearValue clearDepth{};
    clearDepth.depthStencil.depth = rpInfo.depthStencilAttachment->clearDepth;
    clearDepth.depthStencil.stencil = rpInfo.depthStencilAttachment->clearStencil;
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

void VulkanDevice::cmdEndRenderPass(CommandBuffer cmdHandle)
{
  VkCommandBuffer cmdBuffer = reinterpret_cast<VulkanCommandBuffer *>(cmdHandle.imp)->commandBuffer;
  vkCmdEndRenderPass(cmdBuffer);
}

VkFormat VulkanDevice::formatToVkFormat(Format fmt)
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

Format VulkanDevice::vkFormatToFormat(VkFormat vkFmt)
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

Texture VulkanDevice::createImage(const ImageCreateInfo &info)
{
  VulkanImage *imageData = new VulkanImage();

  imageData->width = info.width;
  imageData->height = info.height;
  imageData->format = formatToVkFormat(info.format);

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = info.width;
  imageInfo.extent.height = info.height;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = formatToVkFormat(info.format);
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;

  VkImageUsageFlags vkUsage = 0;

  if (info.usage & ImageUsage_Sampled)
    vkUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (info.usage & ImageUsage_Storage)
    vkUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (info.usage & ImageUsage_ColorAttachment)
    vkUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (info.usage & ImageUsage_DepthStencilAttachment)
    vkUsage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (info.usage & ImageUsage_TransferSrc)
    vkUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (info.usage & ImageUsage_TransferDst)
    vkUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = vkUsage;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateImage(logicalDevice.device, &imageInfo, nullptr, &imageData->image) != VK_SUCCESS)
    throw std::runtime_error("Failed to create Vulkan image.");

  VkMemoryRequirements memRequirements;
  vkGetImageMemoryRequirements(logicalDevice.device, imageData->image, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, bufferUsageToVkMemoryPropertyFlags(info.memoryProperties));

  if (vkAllocateMemory(logicalDevice.device, &allocInfo, nullptr, &imageData->memory) != VK_SUCCESS)
    throw std::runtime_error("Failed to allocate image memory.");

  vkBindImageMemory(logicalDevice.device, imageData->image, imageData->memory, 0);

  return buildTexture(imageData);
}

void VulkanDevice::destroyImage(Texture handle)
{
  VulkanImage *image = reinterpret_cast<VulkanImage *>(handle.imp);
  vkDestroyImage(logicalDevice.device, image->image, nullptr);
  vkFreeMemory(logicalDevice.device, image->memory, nullptr);
  delete image;
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

TextureView VulkanDevice::createImageView(Texture imageHandle, ImageAspectFlags aspectFlags)
{
  VulkanImage *image = reinterpret_cast<VulkanImage *>(imageHandle.imp);
  VkImageViewCreateInfo viewInfo{};

  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = image->image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = image->format;

  viewInfo.subresourceRange.aspectMask = imageAspectFlagsToVkImageAspectFlags(aspectFlags);
  viewInfo.subresourceRange.baseMipLevel = 0;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.baseArrayLayer = 0;
  viewInfo.subresourceRange.layerCount = 1;

  VkImageView view;

  if (vkCreateImageView(logicalDevice.device, &viewInfo, nullptr, &view) != VK_SUCCESS)
  {
    throw std::runtime_error("Failed to create image view.");
  }

  VulkanTextureView *viewImp = new VulkanTextureView();

  viewImp->fence = VK_NULL_HANDLE;
  viewImp->achireSemaphore = VK_NULL_HANDLE;
  viewImp->presentSemaphore = VK_NULL_HANDLE;
  viewImp->view = view;
  viewImp->renderData.swapChain = NULL;
  viewImp->renderData.swapChainImageIndex = UINT32_MAX;

  return buildTextureView(viewImp);
}

void VulkanDevice::destroyImageView(TextureView handle)
{
  VulkanTextureView *view = reinterpret_cast<VulkanTextureView *>(handle.imp);
  vkDestroyImageView(logicalDevice.device, view->view, nullptr);
  delete view;
}

BindingGroups VulkanDevice::createBindingGroups(const BindingsLayout layout, const BindingGroupsInfo &info)
{
  VulkanBindingsLayout *vkLayout = static_cast<VulkanBindingsLayout *>(layout.imp);

  if (info.groupsCount != vkLayout->info.groupsCount)
    throw std::runtime_error("BindingGroups count does not match layout groups count");

  VulkanBindingGroups *groupImp = new VulkanBindingGroups();
  groupImp->descriptorPools.resize(info.groupsCount);
  groupImp->descriptorSets.resize(info.groupsCount);

  for (uint32_t groupIndex = 0; groupIndex < info.groupsCount; ++groupIndex)
  {
    const BindingGroupLayout &groupLayout = vkLayout->info.groups[groupIndex];
    const BindingGroupInfo &groupInfo = info.groups[groupIndex];

    std::vector<VkDescriptorPoolSize> poolSizes;

    if (groupLayout.buffersCount > 0)
    {
      uint32_t dynamicUniformBuffersCount = 0;
      uint32_t dynamicStorageBuffersCount = 0;
      uint32_t uniformBuffersCount = 0;
      uint32_t storageBuffersCount = 0;
      for (int i = 0; i < info.groups[groupIndex].buffersCount; i++)
      {
        if (vkLayout->info.groups[groupIndex].buffers[i].usage & BufferUsage::BufferUsage_Uniform)
        {
          if (vkLayout->info.groups[groupIndex].buffers[i].isDynamic)
          {
            dynamicUniformBuffersCount += 1;
          }
          else
          {
            uniformBuffersCount += 1;
          }
        }
        else if (vkLayout->info.groups[groupIndex].buffers[i].usage & BufferUsage::BufferUsage_Storage)
        {
          if (vkLayout->info.groups[groupIndex].buffers[i].isDynamic)
          {
            dynamicStorageBuffersCount += 1;
          }
          else
          {
            storageBuffersCount += 1;
          }
        }
      }

      if (uniformBuffersCount > 0)
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniformBuffersCount});
      if (storageBuffersCount > 0)
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storageBuffersCount});
      if (dynamicUniformBuffersCount > 0)
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, dynamicUniformBuffersCount});
      if (dynamicStorageBuffersCount > 0)
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, dynamicStorageBuffersCount});
    }

    if (groupLayout.samplersCount > 0)
    {
      poolSizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER, groupLayout.samplersCount});
    }
    if (groupLayout.texturesCount > 0)
    {
      poolSizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, groupLayout.texturesCount});
    }
    if (groupLayout.storageTexturesCount > 0)
    {
      poolSizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, groupLayout.storageTexturesCount});
    }

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    if (vkCreateDescriptorPool(getDevice(), &poolInfo, nullptr, &groupImp->descriptorPools[groupIndex]) != VK_SUCCESS)
    {
      throw std::runtime_error("Failed to create descriptor pool for group");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = groupImp->descriptorPools[groupIndex];
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &vkLayout->descriptorSetLayouts[groupIndex];

    if (vkAllocateDescriptorSets(getDevice(), &allocInfo, &groupImp->descriptorSets[groupIndex]) != VK_SUCCESS)
    {
      throw std::runtime_error("Failed to allocate descriptor set");
    }

    std::vector<VkWriteDescriptorSet> descriptorWrites;
    std::vector<VkDescriptorBufferInfo> bufferInfos(groupLayout.buffersCount);
    std::vector<VkDescriptorImageInfo> samplerInfos(groupLayout.samplersCount);
    std::vector<VkDescriptorImageInfo> textureInfos(groupLayout.texturesCount);
    std::vector<VkDescriptorImageInfo> storageTextureInfos(groupLayout.storageTexturesCount);

    // Buffers
    for (uint32_t i = 0; i < groupLayout.buffersCount; ++i)
    {
      VulkanHeap *heap = reinterpret_cast<VulkanHeap *>(groupInfo.buffers[i].buffer.heap);
      bufferInfos[i].buffer = heap->buffer;
      bufferInfos[i].offset = groupInfo.buffers[i].buffer.offset;
      bufferInfos[i].range = VkDeviceSize(groupInfo.buffers[i].buffer.size);

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = groupImp->descriptorSets[groupIndex];
      write.dstBinding = groupInfo.buffers[i].binding;
      write.dstArrayElement = 0;
      write.descriptorCount = 1;
      write.pBufferInfo = &bufferInfos[i];

      if (vkLayout->info.groups[groupIndex].buffers[i].usage & BufferUsage::BufferUsage_Uniform)
      {
        if (vkLayout->info.groups[groupIndex].buffers[i].isDynamic)
        {
          write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        }
        else
        {
          write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        }
      }
      else if (vkLayout->info.groups[groupIndex].buffers[i].usage & BufferUsage::BufferUsage_Storage)
      {
        if (vkLayout->info.groups[groupIndex].buffers[i].isDynamic)
        {
          write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        }
        else
        {
          write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
      }

      descriptorWrites.push_back(write);
    }

    // Samplers
    for (uint32_t i = 0; i < groupLayout.samplersCount; ++i)
    {
      VulkanSampler *res = reinterpret_cast<VulkanSampler *>(groupInfo.samplers[i].sampler.imp);
      samplerInfos[i].sampler = res->sampler;

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = groupImp->descriptorSets[groupIndex];
      write.dstBinding = groupLayout.samplers[i].binding;
      write.dstArrayElement = 0;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      write.descriptorCount = 1;
      write.pImageInfo = &samplerInfos[i];
      descriptorWrites.push_back(write);
    }

    // Textures
    for (uint32_t i = 0; i < groupLayout.texturesCount; ++i)
    {
      VulkanTextureView *viewData = reinterpret_cast<VulkanTextureView *>(groupInfo.textures[i].textureView.imp);
      textureInfos[i].imageView = viewData->view;
      textureInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = groupImp->descriptorSets[groupIndex];
      write.dstBinding = groupInfo.textures[i].binding;
      write.dstArrayElement = 0;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.descriptorCount = 1;
      write.pImageInfo = &textureInfos[i];
      descriptorWrites.push_back(write);
    }

    // Storage Textures
    for (uint32_t i = 0; i < groupLayout.storageTexturesCount; ++i)
    {
      VulkanTextureView *viewData = reinterpret_cast<VulkanTextureView *>(groupInfo.storageTextures[i].textureView.imp);
      storageTextureInfos[i].imageView = viewData->view;
      storageTextureInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = groupImp->descriptorSets[groupIndex];
      write.dstBinding = groupInfo.storageTextures[i].binding;
      write.dstArrayElement = 0;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      write.descriptorCount = 1;
      write.pImageInfo = &storageTextureInfos[i];
      descriptorWrites.push_back(write);
    }

    vkUpdateDescriptorSets(getDevice(), static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
  }

  return buildBindingGroups(groupImp);
}

void VulkanDevice::destroyBindingGroups(BindingGroups groups)
{
  if (!groups.imp)
    return;

  VulkanBindingGroups *groupImp = static_cast<VulkanBindingGroups *>(groups.imp);

  for (VkDescriptorPool pool : groupImp->descriptorPools)
  {
    if (pool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool(getDevice(), pool, nullptr);
    }
  }

  groupImp->descriptorPools.clear();
  groupImp->descriptorSets.clear();

  delete groupImp;
}

Sampler VulkanDevice::createSampler(const SamplerCreateInfo &info)
{
  VulkanSampler *imp = new VulkanSampler();

  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

  auto toVkFilter = [](Filter f) -> VkFilter
  {
    switch (f)
    {
    case Filter::Nearest:
      return VK_FILTER_NEAREST;
    case Filter::Linear:
      return VK_FILTER_LINEAR;
    default:
      return VK_FILTER_LINEAR;
    }
  };

  auto toVkAddressMode = [](SamplerAddressMode mode) -> VkSamplerAddressMode
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
  };

  samplerInfo.minFilter = toVkFilter(info.minFilter);
  samplerInfo.magFilter = toVkFilter(info.magFilter);
  samplerInfo.addressModeU = toVkAddressMode(info.addressModeU);
  samplerInfo.addressModeV = toVkAddressMode(info.addressModeV);
  samplerInfo.addressModeW = toVkAddressMode(info.addressModeW);
  samplerInfo.anisotropyEnable = info.anisotropyEnable ? VK_TRUE : VK_FALSE;
  samplerInfo.maxAnisotropy = info.maxAnisotropy;
  samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  samplerInfo.unnormalizedCoordinates = VK_FALSE;
  samplerInfo.compareEnable = VK_FALSE;
  samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerInfo.mipLodBias = 0.0f;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = info.maxLod;

  if (vkCreateSampler(getDevice(), &samplerInfo, nullptr, &imp->sampler) != VK_SUCCESS)
  {
    delete imp;
    throw std::runtime_error("Failed to create Vulkan sampler");
  }

  return buildSampler(imp);
}

void VulkanDevice::destroySampler(Sampler handle)
{
  if (!handle.imp)
    return;

  VulkanSampler *imp = static_cast<VulkanSampler *>(handle.imp);

  if (imp->sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(getDevice(), imp->sampler, nullptr);
    imp->sampler = VK_NULL_HANDLE;
  }

  delete imp;
}

void VulkanDevice::cmdCopyBuffer(CommandBuffer cmdBuffer, GPUBuffer src, GPUBuffer dst, uint32_t srcOffset, uint32_t dstOffset, uint32_t size)
{
  VulkanCommandBuffer *commandBuffer = reinterpret_cast<VulkanCommandBuffer *>(cmdBuffer.imp);
  VkCommandBuffer vkCmd = commandBuffer->commandBuffer;

  VkBufferCopy copyRegion{};
  copyRegion.srcOffset = src.offset + srcOffset;
  copyRegion.dstOffset = dst.offset + dstOffset;
  copyRegion.size = size;

  VulkanHeap *srcHeap = reinterpret_cast<VulkanHeap *>(src.heap);
  VulkanHeap *dstHeap = reinterpret_cast<VulkanHeap *>(dst.heap);

  vkCmdCopyBuffer(vkCmd, srcHeap->buffer, dstHeap->buffer, 1, &copyRegion);
}

void VulkanDevice::cmdDispatch(CommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
  VulkanCommandBuffer *vkCmdBuf = static_cast<VulkanCommandBuffer *>(commandBuffer.imp);

  if (vkCmdBuf->boundComputePipeline == VK_NULL_HANDLE)
  {
    throw std::runtime_error("Attempted to dispatch with no compute pipeline bound!");
  }

  vkCmdDispatch(vkCmdBuf->commandBuffer, groupCountX, groupCountY, groupCountZ);

  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

  vkCmdPipelineBarrier(
      static_cast<rhi::vulkan::VulkanCommandBuffer *>(commandBuffer.imp)->commandBuffer,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT,
      0,
      1,
      &barrier,
      0,
      nullptr,
      0,
      nullptr);
}