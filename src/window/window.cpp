#include "window.hpp"
#include <cstdlib>

#include "./sdl3/window_sdl3.hpp"

namespace window {

Window *createWindow(WindowBackend backend, WindowSurfaceType surface, const char *title, int width, int height) {
  switch (backend) {
#ifdef SDL3_AVAILABLE
  case WindowBackend_SDL3:
    return (Window *)(new sdl3::SDL3Window(surface, title, width, height));
    break;
#endif

  default:
    break;
  }

  return nullptr;
}

} // namespace window
