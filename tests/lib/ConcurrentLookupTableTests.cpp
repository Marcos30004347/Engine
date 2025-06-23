#include "lib/datastructure/ConcurrentLookupTable.hpp"
#include "lib/memory/SystemMemoryManager.hpp"
#include "lib/time/TimeSpan.hpp"

#include "os/print.hpp"
#include <assert.h>

int main()
{
  lib::time::TimeSpan then = lib::time::TimeSpan::now();
  lib::memory::SystemMemoryManager::init();

  lib::ConcurrentLookupTable<int> *lookupTable = new lib::ConcurrentLookupTable<int>();

  then = lib::time::TimeSpan::now();
  lookupTable->insert(0, 0);
  os::threadSafePrintf("Inserting 0 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  lookupTable->insert(1, 1);
  os::threadSafePrintf("Inserting 1 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  lookupTable->insert(2, 2);
  os::threadSafePrintf("Inserting 2 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  int x;

  then = lib::time::TimeSpan::now();
  lookupTable->get(2, x);
  os::threadSafePrintf("Getting value %i from key 2 = in %fns\n", x, (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  lookupTable->get(0, x);
  os::threadSafePrintf("Getting value %i from key 0 = in %fns\n", x, (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  lookupTable->get(1, x);
  os::threadSafePrintf("Getting value %i from key 1 = in %fns\n", x, (lib::time::TimeSpan::now() - then).nanoseconds());

  delete lookupTable;

  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}