#include "datastructure/ThreadLocalStorage.hpp"
#include "memory/SystemMemoryManager.hpp"
#include "time/TimeSpan.hpp"

#include "os/print.hpp"
#include <assert.h>

void multiThreadTests()
{
  lib::ThreadLocalStorage<size_t> *storage = new lib::ThreadLocalStorage<size_t>();

  bool started = false;

  size_t totalThreads = 128;//os::Thread::getHardwareConcurrency();
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

          then = lib::time::TimeSpan::now();
          storage->set(os::Thread::getCurrentThreadId());
          total_insert_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          size_t x;
          then = lib::time::TimeSpan::now();
          assert(storage->get(x));
          total_get_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          assert(x == os::Thread::getCurrentThreadId());

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

  #if !defined(USE_THREAD_LOCAL)
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
  #endif
  multiThreadTests();
  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}