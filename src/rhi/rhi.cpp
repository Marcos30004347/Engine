#include "rhi.hpp"
#include "imp/vulkan/vulkan.hpp"

using namespace rhi;

Device* Device::create(DeviceBackend backend, DeviceRequiredLimits limits, DeviceFeatures features) {
    switch (backend)
    {
    case DeviceBackend_Vulkan1_2:
        return new imp::vulkan::DeviceVulkan(limits, features);
        break;
    default:
        break;
    }
}
