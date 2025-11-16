#include "rendering/gpu/EventLoop.hpp"
#include <cassert>
#include <iostream>
struct Fence
{
  int id;
};
using namespace rendering;

FenceStatus getFenceStatus(Fence &fence)
{
  os::print("finishing %u\n", fence.id);
  return FenceStatus::FINISHED;
}

int main()
{
  EventLoop<Fence> *eventLoop = new EventLoop<Fence>(getFenceStatus);

  eventLoop->submit({.id = 0});
  eventLoop->submit({.id = 1});
  eventLoop->submit({.id = 2});

  eventLoop->tick();

  eventLoop->submit({.id = 3});
  eventLoop->submit({.id = 4});
 
  eventLoop->tick();
}