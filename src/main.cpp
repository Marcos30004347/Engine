#include "window/window.hpp"
#include "jobsystem/jobsystem.hpp"
#include "rhi/rhi.hpp"

using namespace window;
using namespace jobsystem;


int main() {
    Window* appWindow = createWindow(WindowBackend::WindowBackend_SDL3, "Engine");
    
    JobSystem::init();

    rhi::DeviceRequiredLimits limits;
    
    limits.minimumComputeSharedMemory = 0;
    limits.minimumComputeWorkGroupInvocations = 0;
    limits.minimumMemory = 0;

    rhi::DeviceFeatures features = rhi::DeviceFeatures::DeviceFeatures_None;

    rhi::Device* device = rhi::Device::create(rhi::DeviceBackend_Vulkan1_2, limits, features);

    while (!appWindow->shouldClose())
    {
        appWindow->update();
    }

    JobSystem::shutdown();
    
    delete device;

    return 0;
}