#include "window_sdl3.hpp"

#ifdef SDL3_AVAILABLE

#include <iostream>
using namespace window;
using namespace imp::window_sdl3;

WindowSDL3::WindowSDL3(const char *title, int width, int height)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        std::cerr << "Failed to initialize SDL3: " << SDL_GetError() << std::endl;
        isRunning = false;
        return;
    }

    sdlWindow = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE);
    if (!sdlWindow)
    {
        std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
        isRunning = false;
    }
}

WindowSDL3::~WindowSDL3()
{
    if (sdlWindow)
    {
        SDL_DestroyWindow(sdlWindow);
    }

    SDL_Quit();
}

bool WindowSDL3::shouldClose()
{
    return !isRunning;
}

bool WindowSDL3::update()
{
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_QUIT)
        {
            isRunning = false;
        }
    }

    return isRunning;
}

#endif