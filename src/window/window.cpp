#include "window.hpp"
#include <cstdlib>

#include "./imp/sdl3/window_sdl3.hpp"

namespace window {

Window* createWindow(WindowBackend backend, const char* title, int width, int height) {
    switch (backend)
    {
    #if SDL3_AVAILABLE
    case WindowBackend_SDL3:
        return new imp::window_sdl3::WindowSDL3(title, width, height);
    break;
    #endif

    default:
        // TODO: repport error
        abort();
        break;
    }

}

}
