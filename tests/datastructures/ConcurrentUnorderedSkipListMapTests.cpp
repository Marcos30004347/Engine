#include "datastructure/ConcurrentUnorderedSkipListMap.hpp"
#include "memory/SystemMemoryManager.hpp"
#include "os/Thread.hpp"
#include "os/print.hpp"
#include "time/TimeSpan.hpp"

#include <algorithm>
#include <assert.h>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <string>
#include <unordered_set>

using Map = lib::ConcurrentUnorderedSkipListMap<std::string, int>;

static inline std::string makeKey(int x) {
    return "key_" + std::to_string(x);
}

static inline long long nsNow() {
    return lib::time::TimeSpan::now().nanoseconds();
}

// ---------------------------------------------------------
// Basic tests
// ---------------------------------------------------------
void basicTests() {
    os::print("Running basic tests...\n");

    Map map;
printf("aaaa\n");

    assert(map.insert("a", 1));
printf("aaaa\n");

    assert(map.insert("b", 2));
    assert(map.insert("c", 3));
    assert(map.getSize() == 3);
printf("aaaa\n");

    // Duplicate insert
    assert(!map.insert("a", 10));
    assert(map.getSize() == 3);

    int value;
    assert(map.find("a", value) && value == 1);
    assert(map.find("b", value) && value == 2);
    assert(map.find("c", value) && value == 3);
    assert(!map.find("d", value));

    // Remove
    assert(map.remove("b"));
    assert(map.getSize() == 2);
    assert(!map.find("b", value));
    assert(!map.remove("b"));

    assert(map.find("a", value) && value == 1);
    assert(map.find("c", value) && value == 3);

    os::print("Basic tests passed!\n");
}

// ---------------------------------------------------------
// Iterator tests
// ---------------------------------------------------------
void iteratorTests() {
    os::print("Running iterator tests...\n");

    Map map;

    for (int i = 0; i < 10; i++)
        map.insert(makeKey(i), i * 10);

    int count = 0;
    for (auto e : map) {
        count++;
    }
    assert(count == 10);

    map.remove(makeKey(3));
    map.remove(makeKey(7));

    count = 0;
    for (auto e : map)
        count++;

    assert(count == 8);

    os::print("Iterator tests passed!\n");
}

// ---------------------------------------------------------
// Multithreaded insert + benchmark
// ---------------------------------------------------------
void multiThreadInsertBench() {
    os::print("Running multi-threaded insert benchmark...\n");

    Map map;

    size_t totalThreads = os::Thread::getHardwareConcurrency();
    os::Thread threads[totalThreads];
    std::atomic<bool> started(false);

    // For aggregated benchmark
    std::atomic<long long> globalTotalNs(0);
    std::atomic<long long> globalCount(0);

    for (size_t i = 0; i < totalThreads; i++) {
        threads[i] = os::Thread([&, i]() {
            while (!started.load()) {}

            long long threadTotal = 0;
            long long ops = 0;

            int base = i * 1000;
            for (int j = 0; j < 1000; j++) {
                long long t0 = nsNow();
                bool ok = map.insert(makeKey(base + j), (base + j) * 10);
                long long dt = nsNow() - t0;

                if (ok) {
                    threadTotal += dt;
                    ops++;
                }
            }

            globalTotalNs.fetch_add(threadTotal);
            globalCount.fetch_add(ops);

            os::print("Thread %u avg insert: %.2f ns\n",
                      os::Thread::getCurrentThreadId(),
                      ops ? (double)threadTotal / ops : 0.0);
        });
    }

    started.store(true);
    for (size_t i = 0; i < totalThreads; i++)
        threads[i].join();

    os::print("Global avg insert: %.2f ns\n",
              (double)globalTotalNs.load() / (double)globalCount.load());
}

// ---------------------------------------------------------
// Multithreaded remove + benchmark
// ---------------------------------------------------------
void multiThreadRemoveBench() {
    os::print("Running multi-threaded remove benchmark...\n");

    Map map;

    size_t totalThreads = os::Thread::getHardwareConcurrency();
    size_t perThread = 1000;

    // Prepopulate
    for (size_t i = 0; i < totalThreads * perThread; i++)
        map.insert(makeKey(i), i);

    os::Thread threads[totalThreads];
    std::atomic<bool> started(false);
    std::atomic<long long> globalTotalNs(0);
    std::atomic<long long> globalCount(0);

    for (size_t i = 0; i < totalThreads; i++) {
        threads[i] = os::Thread([&, i]() {
            while (!started.load()) {}

            long long threadTotal = 0;
            long long ops = 0;

            int base = i * perThread;
            for (size_t j = 0; j < perThread; j++) {
                long long t0 = nsNow();
                bool ok = map.remove(makeKey(base + j));
                long long dt = nsNow() - t0;

                if (ok) {
                    threadTotal += dt;
                    ops++;
                }
            }

            globalTotalNs.fetch_add(threadTotal);
            globalCount.fetch_add(ops);

            os::print("Thread %u avg remove: %.2f ns\n",
                      os::Thread::getCurrentThreadId(),
                      ops ? (double)threadTotal / ops : 0.0);
        });
    }

    started.store(true);
    for (size_t i = 0; i < totalThreads; i++)
        threads[i].join();

    os::print("Global avg remove: %.2f ns\n",
              (double)globalTotalNs.load() / (double)globalCount.load());
}

// ---------------------------------------------------------
// Multithreaded mixed ops benchmark
// ---------------------------------------------------------
void mixedOpsBench() {
    os::print("Running mixed operations benchmark...\n");

    Map map;

    size_t totalThreads = os::Thread::getHardwareConcurrency();
    os::Thread threads[totalThreads];

    std::atomic<bool> started(false);
    std::atomic<long long> insertNs(0), insertOps(0);
    std::atomic<long long> removeNs(0), removeOps(0);
    std::atomic<long long> findNs(0), findOps(0);

    for (size_t i = 0; i < totalThreads; i++) {
        threads[i] = os::Thread([&, i]() {
            while (!started.load()) {}

            unsigned int seed = time(nullptr) + i;

            for (int j = 0; j < 2000; j++) {
                int r = rand_r(&seed) % 3;
                std::string k = makeKey(rand_r(&seed) % 10000);

                if (r == 0) {
                    long long t0 = nsNow();
                    bool ok = map.insert(k, 123);
                    long long dt = nsNow() - t0;
                    insertNs.fetch_add(dt);
                    insertOps.fetch_add(1);
                } else if (r == 1) {
                    long long t0 = nsNow();
                    bool ok = map.remove(k);
                    long long dt = nsNow() - t0;
                    removeNs.fetch_add(dt);
                    removeOps.fetch_add(1);
                } else {
                    int value;
                    long long t0 = nsNow();
                    map.find(k, value);
                    long long dt = nsNow() - t0;
                    findNs.fetch_add(dt);
                    findOps.fetch_add(1);
                }
            }
        });
    }

    started.store(true);
    for (size_t i = 0; i < totalThreads; i++)
        threads[i].join();

    os::print("Avg insert: %.2f ns\n", (double)insertNs.load() / insertOps.load());
    os::print("Avg remove: %.2f ns\n", (double)removeNs.load() / removeOps.load());
    os::print("Avg find: %.2f ns\n", (double)findNs.load() / findOps.load());
}

// ---------------------------------------------------------
// Main
// ---------------------------------------------------------
int main() {
    lib::memory::SystemMemoryManager::init();
    srand(time(nullptr));

    basicTests();
    iteratorTests();
    multiThreadInsertBench();
    multiThreadRemoveBench();
    mixedOpsBench();

    os::print("\n=== All unordered map tests + benchmarks passed! ===\n");

    lib::memory::SystemMemoryManager::shutdown();
    return 0;
}
