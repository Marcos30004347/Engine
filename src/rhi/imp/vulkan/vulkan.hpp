#include "rhi/rhi.hpp"

#include <vulkan/vulkan.h>

namespace rhi
{
    namespace imp
    {
        namespace vulkan
        {
#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif

            struct QueueFamilyIndices {
                bool hasGraphicsFamily = false;
                uint32_t graphicsFamily;

                // bool hasPresentFamily = false;
                // uint32_t presentFamily;

                bool hasComputeFamily = false;
                uint32_t computeFamily;
            };

            const std::vector<const char*> validationLayers = {
                "VK_LAYER_KHRONOS_validation"
            };

            const std::vector<const char*> instanceExtensions = {
                VK_KHR_SURFACE_EXTENSION_NAME,
                VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
            };

            const std::vector<const char*> deviceExtensions = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME,     
            };

            struct SwapChainSupportDetails {
                VkSurfaceCapabilitiesKHR capabilities;
                std::vector<VkSurfaceFormatKHR> formats;
                std::vector<VkPresentModeKHR> presentModes;
            };

            class DeviceVulkan : public Device
            {
            public:
                std::uint64_t featureFlags;
                DeviceProperties properties;
                
                QueueFamilyIndices indices;

                VkInstance instance;

                VkPhysicalDevice physicalDevice;
                VkDevice device;

                VkSurfaceKHR surface;

                VkSwapchainKHR swapChain;
                std::vector<VkImage> swapChainImages;
                VkFormat swapChainImageFormat;
                VkExtent2D swapChainExtent;
                std::vector<VkImageView> swapChainImageViews;
                std::vector<VkFramebuffer> swapChainFramebuffers;

                VkQueue graphicsQueue;
                VkQueue presentQueue;
                VkQueue computeQueue;

                DeviceVulkan(DeviceRequiredLimits, DeviceFeatures);
                void pickPhysicalDevice(DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures);
                void createLogicalDevice(DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures);
                void initializePhysicalDevice(DeviceRequiredLimits, DeviceFeatures);
                void initializeInstance(DeviceRequiredLimits requiredLimits, DeviceFeatures requestedFeatures);
            };
        }
    }

}