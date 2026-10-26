#pragma once

#ifndef SDL3_AVAILABLE
#define SDL3_AVAILABLE 1
#endif

#if SDL3_AVAILABLE

#include "SDL3/SDL.h"
#include "window/window.hpp"
#include <string>

namespace window
{
namespace sdl3
{

class SDL3Window : public Window
{
public:
  SDL3Window(WindowSurfaceType surface, const char *title = "SDL3 Window", int width = 800, int height = 600);
  ~SDL3Window();

  bool shouldClose() override;
  bool update() override;

  SDL_Window *sdlWindow = nullptr;
  SDL_Event event;

  bool isRunning = true;

  unsigned int extensionCount;
  const char *const *extensions;

  uint32_t getWidth() override;
  uint32_t getHeight() override;
  
  uint32_t width;
  uint32_t height;
  
  std::vector<std::string> getVulkanExtensions() override;
  VkSurfaceKHR getVulkanSurface(VkInstance instance) override;
};

} // namespace sdl3
} // namespace window

#endif