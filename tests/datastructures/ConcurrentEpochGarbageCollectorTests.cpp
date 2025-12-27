#include "datastructure/ConcurrentEpochGarbageCollector.hpp"
#include "os/Thread.hpp"
#include "os/print.hpp"
#include "time/TimeSpan.hpp"

int main()
{
  lib::ConcurrentEpochGarbageCollector<char> gc;

  constexpr size_t NUM_INSERTS = 2000;
  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];

  std::atomic<bool> started(false);

  for (size_t t = 0; t < totalThreads; t++)
  {
    threads[t] = os::Thread(
        [&, t]()
        {
          while (!started)
          {
          } // sync

          lib::time::TimeSpan then;
          double total_ns = 0;

          for (size_t i = 0; i < NUM_INSERTS; i++)
          {
            then = lib::time::TimeSpan::now();
            auto scope = gc.openEpochGuard();
            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

            for (int k = 0; k < 4; k++)
            {
             char *buff = gc.allocate(scope);// (char *)malloc(sizeof(char));
             scope.retire(buff);
            }
          }

          os::print("Thread %u average allocation = %f ns\n", t, total_ns / NUM_INSERTS);
        });
  }

  started = true;

  for (size_t t = 0; t < totalThreads; t++)
    threads[t].join();

  return 0;
}