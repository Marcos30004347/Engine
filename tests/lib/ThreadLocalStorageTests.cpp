#include "lib/datastructure/ThreadLocalStorage.hpp"
#include "lib/memory/SystemMemoryManager.hpp"
#include "lib/time/TimeSpan.hpp"

#include "os/print.hpp"
#include <assert.h>

void multiThreadTests()
{
  lib::ThreadLocalStorage<int> *storage = new lib::ThreadLocalStorage<int>();

  bool started = false;

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&]()
        {
          while (!started)
          {
          }

          lib::time::TimeSpan then = lib::time::TimeSpan::now();
          double total_insert_ns = 0;
          double total_get_ns = 0;

          storage->set(0);

          int x;

          assert(storage->get(x));

          for (size_t j = 0; j < 1000; j++)
          {
            then = lib::time::TimeSpan::now();
            storage->set(j);
            total_insert_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

            then = lib::time::TimeSpan::now();
            assert(storage->get(x));
            total_get_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
            assert(x == j);
          }

          os::print("Thread %u average insertion time is %fns\n", os::Thread::getCurrentThreadId(), total_insert_ns / 1000);
          os::print("Thread %u average get time is %fns\n", os::Thread::getCurrentThreadId(), total_get_ns / 1000);
        });
  }

  started = true;

  for (size_t i = 0; i < totalThreads; i++)
  {
    if (threads[i].isRunning())
    {
      threads[i].join();
    }
  }
}
int main()
{
  lib::time::TimeSpan then = lib::time::TimeSpan::now();
  lib::memory::SystemMemoryManager::init();

  lib::detail::ConcurrentLookupTable<int> *lookupTable = new lib::detail::ConcurrentLookupTable<int>();

  then = lib::time::TimeSpan::now();
  lookupTable->insert(0, 0);
  os::print("Inserting 0 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  lookupTable->insert(1, 1);
  os::print("Inserting 1 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  lookupTable->insert(2, 2);
  os::print("Inserting 2 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  int x;

  then = lib::time::TimeSpan::now();
  lookupTable->get(2, x);
  os::print("Getting value %i from key 2 = in %fns\n", x, (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  lookupTable->get(0, x);
  os::print("Getting value %i from key 0 = in %fns\n", x, (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  lookupTable->get(1, x);
  os::print("Getting value %i from key 1 = in %fns\n", x, (lib::time::TimeSpan::now() - then).nanoseconds());

  delete lookupTable;
  multiThreadTests();
  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}