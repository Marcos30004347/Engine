#include "window_sdl3.hpp"

#ifdef SDL3_AVAILABLE

#include <SDL3/SDL_vulkan.h>
#include <iostream>
#include <vector>

using namespace window;
using namespace sdl3;
using namespace core;

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

SDL3Window::SDL3Window(WindowSurfaceType surface, const char *title, int width, int height)
{
  if (SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    std::cerr << "Failed to initialize SDL3: " << SDL_GetError() << std::endl;
    isRunning = false;
    return;
  }

  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;

  switch (surface)
  {
  case WindowSurfaceType::WindowSurface_Vulkan:
    flags |= SDL_WINDOW_VULKAN;
    break;
  }

  sdlWindow = SDL_CreateWindow(title, width, height, flags);

  if (!sdlWindow)
  {
    std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
    isRunning = false;
  }

  extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);

  SDL_Surface *s = SDL_GetWindowSurface(sdlWindow);
  format = GetColorFormatFromSurface(s);

  int iw, ih, ipw, iph;

  SDL_GetWindowSize(sdlWindow, &iw, &ih);
  SDL_GetWindowSizeInPixels(sdlWindow, &ipw, &iph);

  this->width = iw;
  this->height = ih;
  this->heightInPixels = ipw;
  this->heightInPixels = iph;
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
      SDL_GetWindowSizeInPixels(sdlWindow, &ipw, &iph);

      width = iw;
      height = ih;
      heightInPixels = ipw;
      heightInPixels = iph;

      onWindowRezizedEvent.invoke(this);
    }
  }

  return isRunning;
}

#endif