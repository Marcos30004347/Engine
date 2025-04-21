#include <stdexcept>
#include <iostream>
#include <cstring>
#include <set>

#include "vulkan.hpp"

using namespace rhi;
using namespace imp;
using namespace vulkan;

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void *pUserData)
{
    std::cerr << "Validation layer: " << pCallbackData->pMessage << std::endl;
    return VK_FALSE;
}

void checkVk(VkResult result, const char *msg)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(msg);
    }
}

struct DeviceResult
{
    VkPhysicalDevice device;
    std::uint64_t feature_flags;
    DeviceProperties properties;
};

QueueFamilyIndices findQueueFamilyIndices(VkPhysicalDevice device, DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures)
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
    
    QueueFamilyIndices indices;

    indices.hasGraphicsFamily = false;
    // indices.hasPresentFamily = false;
    indices.hasComputeFamily = false;
    
    for (uint32_t i = 0; i < queueFamilyCount; ++i)
    {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            VkBool32 presentSupport = false;

            // vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

            // if (presentSupport)
            // {
            //     indices.presentFamily = i;
            //     indices.hasPresentFamily = true;
            // }

            indices.graphicsFamily = i;
            indices.hasGraphicsFamily = true;
        }

        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
        {
            indices.computeFamily = i;
            indices.hasComputeFamily = true;
        }
    }

    return indices;
}

DeviceResult getPhysicalDevice(std::vector<VkPhysicalDevice> &devices, DeviceRequiredLimits requiredLimits, DeviceFeatures requiredFeatures)
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
        bool hasTimestamp = std::any_of(properties.begin(), properties.end(), [](const auto &q)
                                        { return q.timestampValidBits > 0; });
        bool hasCompute = std::any_of(properties.begin(), properties.end(), [](const auto &q)
                                      { return q.queueFlags & VK_QUEUE_COMPUTE_BIT; });
        bool hasGraphics = std::any_of(properties.begin(), properties.end(), [](const auto &q)
                                       { return q.queueFlags & VK_QUEUE_GRAPHICS_BIT; });

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

        // VkBool32 supported = VK_FALSE;
        // vkGetPhysicalDeviceSurfaceSupportKHR(dev, queueFamilyIndex, surface, &supported);

        // bool hasSurface = supported == VK_TRUE;

        // if (hasSurface)
        // {
        //     featureFlags |= DeviceFeatures_Surface;
        // }

        std::vector<VkExtensionProperties> extensions;
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        extensions.resize(extCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, extensions.data());

        bool hasSwapchain = std::any_of(extensions.begin(), extensions.end(), [](const auto &e)
                                        { return strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; });

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

    std::sort(suitableDevices.begin(), suitableDevices.end(), [](const auto &a, const auto &b)
              {
        uint32_t scoreA = 0;
        uint32_t scoreB = 0;

        scoreA += a.properties.maxComputeWorkGroupInvocations;
        scoreB += b.properties.maxComputeWorkGroupInvocations;
        
        return scoreA > scoreB; });

    if (suitableDevices.size() == 0)
    {
        throw std::runtime_error("Not sutiable device found");
    }

    return suitableDevices[0];
}

DeviceVulkan::DeviceVulkan(DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures)
{
    initializeInstance(requiredLimits, requestedFeatures);

    surface = VK_NULL_HANDLE;

    initializePhysicalDevice(requiredLimits, requestedFeatures);
    createLogicalDevice(requiredLimits, requestedFeatures);

    std::cout << "VulkanApp initialized successfully!" << std::endl;
}

void DeviceVulkan::createLogicalDevice(DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures)
{
    indices = findQueueFamilyIndices(physicalDevice, requiredLimits, requestedFeatures);

    if ((requestedFeatures & DeviceFeatures_Graphics) && !indices.hasGraphicsFamily)
    {
        throw std::runtime_error("Missing required queue families");
    }

    if ((requestedFeatures & DeviceFeatures_Compute) && !indices.hasComputeFamily)
    {
        throw std::runtime_error("Missing required queue families");
    }

    // if ((requestedFeatures & DeviceFeatures_SwapChain) && !indices.hasPresentFamily)
    // {
    //     throw std::runtime_error("Missing required queue families");
    // }

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies;

    if (indices.hasGraphicsFamily)
    {
        uniqueQueueFamilies.insert(indices.graphicsFamily);
    }
    
    if (indices.hasComputeFamily)
    {
        uniqueQueueFamilies.insert(indices.computeFamily);
    }

    // if (indices.hasPresentFamily)
    // {
    //     uniqueQueueFamilies.insert(indices.presentFamily);
    // }

    float queuePriority = 1.0f;

    for (uint32_t queueFamily : uniqueQueueFamilies)
    {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};

    deviceFeatures.multiDrawIndirect = requestedFeatures & DeviceFeatures_MultiDrawIndirect ? VK_TRUE : VK_FALSE;
    deviceFeatures.drawIndirectFirstInstance = requestedFeatures & DeviceFeatures_DrawIndirectFirstInstance ? VK_TRUE : VK_FALSE;

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

    if (featureFlags & DeviceFeatures_Graphics)
    {
        vkGetDeviceQueue(device, indices.graphicsFamily, 0, &graphicsQueue);
    }

    // if (featureFlags & DeviceFeatures_SwapChain)
    // {
    //     vkGetDeviceQueue(device, indices.presentFamily, 0, &presentQueue);
    // }

    if (featureFlags & DeviceFeatures_Compute)
    {
        vkGetDeviceQueue(device, indices.computeFamily, 0, &computeQueue);
    }
}

bool checkValidationLayerSupport() {
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
    
    for (const auto& layer : availableLayers) {
        printf("Vulkan Layer available: %s\n", layer.layerName);
    }

    for (const char* layerName : validationLayers) {
        bool layerFound = false;

        for (const auto& layerProperties : availableLayers) {

            if (strcmp(layerName, layerProperties.layerName) == 0) {
                layerFound = true;
                break;
            }
        }

        if (!layerFound) {
            return false;
        }
    }

    return true;
}

void DeviceVulkan::initializeInstance(DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures)
{
    if (enableValidationLayers && !checkValidationLayerSupport()) {
        throw std::runtime_error("validation layers requested, but not available!");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "RHI Vulkan App";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    createInfo.ppEnabledExtensionNames = instanceExtensions.data();

    checkVk(vkCreateInstance(&createInfo, nullptr, &instance), "Failed to create Vulkan instance");
}

void DeviceVulkan::initializePhysicalDevice(DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures)
{
    uint32_t deviceCount = 0;

    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (deviceCount == 0)
    {
        throw std::runtime_error("Failed to find GPUs with Vulkan support");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    DeviceResult dev = getPhysicalDevice(devices, requiredLimits, requestedFeatures);

    physicalDevice = dev.device;
    featureFlags = dev.feature_flags;
    properties = dev.properties;

    if (physicalDevice == VK_NULL_HANDLE)
    {
        throw std::runtime_error("Failed to find a suitable GPU");
    }
}

