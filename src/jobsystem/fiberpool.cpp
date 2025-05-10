#include "fiberpool.hpp"
#include "jobsystem.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

lib::parallel::Queue<Fiber *> FiberPool::pool[64];
std::atomic<uint32_t> FiberPool::poolSize[64];

uint32_t nextPowerOfTwo(uint32_t n)
{
  if (n == 0)
    return 1;
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

uint32_t log2(uint32_t x)
{
  return 31u - __builtin_clz(x);
}

void FiberPool::init()
{
  for (uint32_t i = 0; i < 64; i++)
  {
    poolSize[i] = 0;
  }
}

void FiberPool::shutdown()
{
}

Fiber *FiberPool::acquire(Fiber::Handler func, void *data, size_t stack_size)
{
  Fiber *fiber;
  size_t size = nextPowerOfTwo(stack_size);
  size = size < 8192 ? 8192 : size;
  size_t sizeclass = log2(size) - log2(8192);

  if (pool[sizeclass].dequeue(fiber))
  {
    fiber->reset(func, data);
    return fiber;
  }

  return new Fiber(func, data, stack_size);
}

void FiberPool::release(Fiber *fiber)
{
  size_t size = fiber->getStackSize();

  uint32_t max = 1024;

  if (size >= 1024 * 32)
    max = 1024;
  if (size >= 1024 * 64)
    max = 512;
  if (size >= 1024 * 1024)
    max = 256;
  if (size >= 1024 * 1024 * 2)
    max = 128;
  if (size >= 1024 * 1024 * 3)
    max = 0;

  size_t sizeclass = log2(size) - log2(8192);

  if (poolSize[sizeclass].fetch_add(1) <= max)
  {
    pool[sizeclass].enqueue(fiber);
  }
  else
  {
    poolSize[sizeclass].fetch_sub(1);
    delete fiber;
  }
}