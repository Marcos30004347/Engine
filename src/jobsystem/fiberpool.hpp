#pragma once

#include "fiber.hpp"

namespace jobsystem
{
namespace fiber
{
class FiberPool
{
public:
  static Fiber *acquire(Fiber::Handler, void* userData);
  static void release(Fiber *fiber);
private:
  static std::queue<Fiber *> pool;
  static std::mutex pool_mutex;
};
} // namespace fiber
} // namespace jobsystem