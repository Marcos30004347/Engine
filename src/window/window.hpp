#pragma once
#include "core/color.hpp"
#include "core/event.hpp"

#include <cstdint>

namespace window {

enum WindowBackend {
  WindowBackend_SDL3,
};

enum WindowSurfaceType {
  WindowSurface_Vulkan,
};

class Window {
public:
  using WindowEvent = core::FixedEvent<const Window *, void *>;

  virtual bool shouldClose() = 0;
  virtual bool update() = 0;

  inline uint32_t getWidthInPixels() { return widthInPixels; }
  inline uint32_t getHeightInPixels() { return heightInPixels; }
  inline core::ColorFormat getColorFormat() { return format; }

  WindowEvent onWindowRezizedEvent;

protected:
  uint32_t width;
  uint32_t height;

  uint32_t widthInPixels;
  uint32_t heightInPixels;

  core::ColorFormat format = core::ColorFormat::ColorFormat_UNDEFINED;
};

Window *createWindow(WindowBackend, WindowSurfaceType, const char *title = "Window", int width = 800, int height = 600);

}; // namespace window