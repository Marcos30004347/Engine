#include "jobsystem/jobsystem.hpp"
#include "rhi/rhi.hpp"
#include "window/window.hpp"

using namespace window;
using namespace jobsystem;

int main()
{
  Window *appWindow = createWindow(WindowBackend::WindowBackend_SDL3, WindowSurfaceType::WindowSurface_Vulkan, "Engine");

  JobSystem::init();

  rhi::DeviceRequiredLimits limits;

  limits.minimumComputeSharedMemory = 0;
  limits.minimumComputeWorkGroupInvocations = 0;
  limits.minimumMemory = 1024 * 1024;

  std::vector<rhi::DeviceFeatures> features = {
    rhi::DeviceFeatures::DeviceFeatures_Graphics,
    rhi::DeviceFeatures::DeviceFeatures_Compute,
  };

  rhi::Device *device = rhi::Device::create(rhi::DeviceBackend_Vulkan_1_2, limits, features);

  rhi::SurfaceHandle surface = device->addWindowForDrawing(appWindow);

  device->init();

  while (!appWindow->shouldClose())
  {
    appWindow->update();
  }

  JobSystem::shutdown();

  delete device;

  return 0;
}