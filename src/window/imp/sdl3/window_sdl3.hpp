#pragma once

#ifdef SDL3_AVAILABLE

#include "window/window.hpp"
#include "SDL3/SDL.h"

namespace window
{
    namespace imp
    {
        namespace window_sdl3
        {
            class WindowSDL3 : public Window
            {
            public:
                WindowSDL3(const char *title = "SDL3 Window", int width = 800, int height = 600);
                ~WindowSDL3();

                bool shouldClose() override;
                bool update() override;

            private:
                SDL_Window *sdlWindow = nullptr;
                SDL_Event event;
                bool isRunning = true;
            };
        }
    }
}

#endif