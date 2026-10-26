#pragma once

// Portable "run this thread at highest performance" helper.
// Best-effort, never throws, safe to call multiple times.

#if defined(_WIN32)
#define HQOS_WINDOWS
#include <windows.h>

#elif defined(__APPLE__)
#define HQOS_MACOS
#include <pthread.h>

#elif defined(__linux__)
#define HQOS_LINUX
#include <errno.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#else
#define HQOS_UNKNOWN
#endif
namespace os
{
namespace hqos
{

inline void setHighQos() noexcept
{
#if defined(HQOS_MACOS)

  // Highest user-facing QoS, maps to performance cores
  // Safe even without entitlements
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

#elif defined(HQOS_LINUX)

  // Try real-time scheduling first (requires CAP_SYS_NICE)
  sched_param param{};
  param.sched_priority = sched_get_priority_max(SCHED_FIFO);

  if (sched_setscheduler(0, SCHED_FIFO, &param) != 0)
  {
    // Fallback: best-effort priority boost
    setpriority(PRIO_PROCESS, 0, -20);
  }

#elif defined(HQOS_WINDOWS)

  // Raise process priority
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

  // Raise thread priority
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

#else
  // Unknown platform → no-op
  (void)0;
#endif
}


// The counterpart for background work: a worker thread that generates or
// meshes terrain must never take a core from the render thread. On macOS a
// thread inherits the QoS class of the thread that created it, so pools made
// from the (user-interactive) render thread ran at its own priority and
// starved it for whole frames once generation became continuous (SDL event
// polling at 380 ms). Default is the class to want: below the render thread,
// level with the GPU driver's threads. Measured alternatives: utility throttled
// generation to a crawl (9 chunks in 200 frames against 105-216), and
// user-initiated starved the driver instead (GPU back-pressure p90 123 ms).
inline void setWorkerQos() noexcept
{
#if defined(HQOS_MACOS)
  pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0);
#elif defined(HQOS_WINDOWS)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
  (void)0;
#endif
}
} // namespace hqos
} // namespace os
