#pragma once
#include <cstdint>

namespace window {

enum WindowBackend {
    WindowBackend_SDL3,
};

class Window {
    public:
    virtual bool shouldClose() = 0;
    virtual bool update() = 0;
};

Window* createWindow(WindowBackend, const char* title = "Window", int width = 800, int height = 600);

};