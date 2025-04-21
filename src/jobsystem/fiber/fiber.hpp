#pragma once

#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include "fcontext/fcontext.h"

namespace jobsystem
{
    namespace fiber
    {
        struct Fiber
        {
            fcontext_t ctx = nullptr;
            fcontext_stack_t stack{};
            std::function<void()> fn;
            std::atomic<bool> finished{false};
            fcontext_t scheduler_ctx = nullptr;

            Fiber(std::function<void()> fn);
            ~Fiber();

            void reset(std::function<void()> new_fn);
            void run();
            void resume();
            static void yield(); // static so it can be called inside fiber
            static Fiber* current();
        private:
            static fcontext_transfer_t yield_entry(fcontext_transfer_t t);
        };
    }
}