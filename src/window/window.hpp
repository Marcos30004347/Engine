#pragma once
#include "core/color.hpp"
#include "core/event.hpp"

#include <cstdint>

#include <vulkan/vulkan.h>

namespace window
{

// enum WindowBackend
// {
//   WindowBackend_SDL3,
// };

enum WindowSurfaceType
{
  WindowSurface_Vulkan,
};

class Window
{
public:
  // using WindowEvent = core::FixedEvent<const Window *, void *>;
  virtual ~Window() = default;

  virtual bool shouldClose() = 0;
  virtual bool update() = 0;

  // inline uint32_t getWidthInPixels()
  // {
  //   return widthInPixels;
  // }
  // inline uint32_t getHeightInPixels()
  // {
  //   return heightInPixels;
  // }
  // inline core::ColorFormat getColorFormat()
  // {
  //   return format;
  // }

  // WindowEvent onWindowRezizedEvent;

  virtual std::vector<std::string> getVulkanExtensions() = 0;
  virtual VkSurfaceKHR getVulkanSurface(VkInstance) = 0;
  virtual uint32_t getWidth() = 0;
  virtual uint32_t getHeight() = 0;

protected:
  // uint32_t width;
  // uint32_t height;

  // uint32_t widthInPixels;
  // uint32_t heightInPixels;

  // core::ColorFormat format = core::ColorFormat::ColorFormat_UNDEFINED;
};

// Window *createWindow(WindowBackend, WindowSurfaceType, const char *title = "Window", int width = 800, int height = 600);

}; // namespace window