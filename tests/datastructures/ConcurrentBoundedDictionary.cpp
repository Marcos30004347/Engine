#include "datastructure/ConcurrentBoundedDictionary.hpp"
#include "memory/SystemMemoryManager.hpp"
#include "os/Thread.hpp"
#include "os/print.hpp"
#include "time/TimeSpan.hpp"

#include <cassert>
#include <random>
#include <string>

void concurrentDictionaryMultiThreadTest()
{
  size_t totalThreads = os::Thread::getHardwareConcurrency();
  size_t updatesPerThread = 100;

  lib::ConcurrentBoundedDictionary<size_t, size_t> dict(totalThreads);

  bool started = false;

  os::Thread threads[totalThreads];

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&, i]()
        {
          while (!started)
          {
          }

          lib::time::TimeSpan then;
          double total_insert_ns = 0;
          double total_get_ns = 0;
          double total_update_ns = 0;

          then = lib::time::TimeSpan::now();
          bool inserted = dict.insert(i, 0);
          total_insert_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
       
          assert(inserted);
          
          for (size_t j = 1; j <= updatesPerThread; j++)
          {
            then = lib::time::TimeSpan::now();
            auto &val = dict.get(i);

            val += 1;

            total_get_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
            assert(val == j);
          }

          os::print("Thread %u: insert %f ns, get %f ns (avg %f)\n", os::Thread::getCurrentThreadId(), total_insert_ns, total_get_ns, total_get_ns / updatesPerThread);
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

  for (size_t i = 0; i < totalThreads; i++)
  {
    assert(dict.contains(i));
    // assert(dict.get(i) == updatesPerThread);
  }
}

int main()
{
  lib::time::TimeSpan then = lib::time::TimeSpan::now();
  lib::memory::SystemMemoryManager::init();

  lib::ConcurrentBoundedDictionary<std::string, int> dict(3);
  dict.insert("apple", 1);
  dict.insert("banana", 2);
  dict.insert("cherry", 3);

  assert(dict.contains("apple"));
  assert(dict.get("banana") == 2);
  assert(!dict.contains("durian"));

  concurrentDictionaryMultiThreadTest();

  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}
