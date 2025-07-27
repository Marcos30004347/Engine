#include "os/Thread.hpp"
#include "lib/datastructure/FlatMap.hpp"
#include "lib/memory/SystemMemoryManager.hpp"
#include "lib/time/TimeSpan.hpp"

#include "os/print.hpp"
#include <assert.h>


int main()
{
  lib::time::TimeSpan then = lib::time::TimeSpan::now();
  lib::memory::SystemMemoryManager::init();


  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}