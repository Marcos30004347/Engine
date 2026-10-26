#include "SDL3Window.hpp"

#if SDL3_AVAILABLE

#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace window;
using namespace sdl3;
using namespace core;

std::vector<std::string> SDL3Window::getVulkanExtensions()
{
  std::vector<std::string> vkExtensions;

  for (int i = 0; i < extensionCount; i++)
  {
    vkExtensions.push_back(extensions[i]);
  }

  return vkExtensions;
}

VkSurfaceKHR SDL3Window::getVulkanSurface(VkInstance instance)
{
  VkSurfaceKHR surface = VK_NULL_HANDLE;

  if (!SDL_Vulkan_CreateSurface(sdlWindow, instance, NULL, &surface))
  {
    throw std::runtime_error(SDL_GetError());
  }

  return surface;
}

ColorFormat GetColorFormatFromSurface(SDL_Surface *surface)
{
  if (!surface || !surface->format)
  {
    throw std::runtime_error("Invalid surface");
  }

  SDL_PixelFormat format = surface->format;

  switch (format)
  {
  case SDL_PIXELFORMAT_RGBA8888:
    return ColorFormat_RGBA8;
  case SDL_PIXELFORMAT_BGRA8888:
    return ColorFormat_BGRA8;
  case SDL_PIXELFORMAT_ARGB8888:
    return ColorFormat_ARGB8;
  case SDL_PIXELFORMAT_ABGR8888:
    return ColorFormat_ABGR8;
  case SDL_PIXELFORMAT_XRGB8888:
    return ColorFormat_RGB8;
  case SDL_PIXELFORMAT_XBGR8888:
    return ColorFormat_BGR8;

  default:
    throw std::runtime_error("Missing color");
  }
}

SDL3Window::SDL3Window(
    WindowSurfaceType surface, const char *title, int width, int height,
    const bool requestHighPixelDensity)
{
  this->width = width;
  this->height = height;
  highPixelDensity = requestHighPixelDensity;

  if (SDL_Init(SDL_INIT_VIDEO) == false)
  {
    std::cerr << "Failed to initialize SDL3: " << SDL_GetError() << std::endl;
    isRunning = false;
    return;
  }

  // SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN;
  int windowWidth = width;
  int windowHeight = height;
  if (highPixelDensity)
  {
    flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    // Start conservatively so the macOS window manager does not clamp a
    // drawable-pixel request as though it were expressed in points. Once the
    // window exists SDL can report its actual backing pixel density, and the
    // exact logical size is installed below.
    windowWidth = std::max(1, width / 2);
    windowHeight = std::max(1, height / 2);
  }
  switch (surface)
  {
  case WindowSurfaceType::WindowSurface_Vulkan:
    flags |= SDL_WINDOW_VULKAN;
    break;
  }

  assert(flags & SDL_WINDOW_VULKAN);

  sdlWindow = SDL_CreateWindow(title, windowWidth, windowHeight, flags);

  if (!sdlWindow)
  {
    std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
    isRunning = false;
  }
  else if (highPixelDensity)
  {
    const float pixelDensity = SDL_GetWindowPixelDensity(sdlWindow);
    const float safeDensity = pixelDensity > 0.0f ? pixelDensity : 1.0f;
    const int targetWindowWidth = std::max(
        1, static_cast<int>(std::lround(
               static_cast<float>(width) / safeDensity)));
    const int targetWindowHeight = std::max(
        1, static_cast<int>(std::lround(
               static_cast<float>(height) / safeDensity)));
    SDL_SetWindowSize(sdlWindow, targetWindowWidth, targetWindowHeight);
    SDL_SyncWindow(sdlWindow);

    int pixelWidth = 0;
    int pixelHeight = 0;
    if (SDL_GetWindowSizeInPixels(sdlWindow, &pixelWidth, &pixelHeight))
    {
      this->width = static_cast<uint32_t>(std::max(1, pixelWidth));
      this->height = static_cast<uint32_t>(std::max(1, pixelHeight));
    }
  }

  extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);

  // SDL_Surface *s = SDL_GetWindowSurface(sdlWindow);
  // format = GetColorFormatFromSurface(s);

  // int iw, ih, ipw, iph;

  // SDL_GetWindowSize(sdlWindow, &iw, &ih);
  // SDL_GetWindowSizeInPixels(sdlWindow, &ipw, &iph);

  // this->width = iw;
  // this->height = ih;
  // this->heightInPixels = ipw;
  // this->heightInPixels = iph;
}

SDL3Window::~SDL3Window()
{
  if (sdlWindow)
  {
    SDL_DestroyWindow(sdlWindow);
  }

  SDL_Quit();
}

bool SDL3Window::shouldClose()
{
  return !isRunning;
}

bool SDL3Window::update()
{
  while (SDL_PollEvent(&event))
  {
    if (event.type == SDL_EVENT_QUIT)
    {
      isRunning = false;
    }

    if (event.type == SDL_EVENT_WINDOW_RESIZED)
    {
      int iw, ih, ipw, iph;

      SDL_GetWindowSize(sdlWindow, &iw, &ih);
      if (highPixelDensity &&
          SDL_GetWindowSizeInPixels(sdlWindow, &ipw, &iph))
      {
        width = static_cast<uint32_t>(ipw);
        height = static_cast<uint32_t>(iph);
      }
      else
      {
        width = static_cast<uint32_t>(iw);
        height = static_cast<uint32_t>(ih);
      }
      // heightInPixels = ipw;
      // heightInPixels = iph;

      // onWindowRezizedEvent.invoke(this);
    }
  }

  return isRunning;
}
uint32_t SDL3Window::getWidth()
{
  return width;
}
uint32_t SDL3Window::getHeight()
{
  return height;
}
#endif
