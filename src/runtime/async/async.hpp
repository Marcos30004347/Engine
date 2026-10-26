#pragma once

#include "AsyncManager.hpp"

namespace async
{

static void init(void (*entry)(), SystemSettings settings)
{
  return detail::AsyncManager::init(entry, settings);
}

template <typename F, typename... Args> auto enqueue(F &&f, Args &&...args)
{
  return detail::AsyncManager::enqueue(std::forward<F>(f), std::forward<Args>(args)...);
}

template <typename T> inline T &wait(Promise<T> &promise)
{
  detail::AsyncManager::sleepAndWakeOnPromiseResolve(promise.job);
  return *(promise.data);
}

template <typename T> inline T &wait(Promise<T> &&promise)
{
  detail::AsyncManager::sleepAndWakeOnPromiseResolve(promise.job);
  return *(promise.data);
}

inline void wait(Promise<void> &promise)
{
  detail::AsyncManager::sleepAndWakeOnPromiseResolve(promise.job);
}

inline void wait(Promise<void> &&promise)
{
  detail::AsyncManager::sleepAndWakeOnPromiseResolve(promise.job);
}
inline static void shutdown()
{
  detail::AsyncManager::shutdown();
}

inline static void yield()
{
  detail::AsyncManager::yield();
}
inline static void stop()
{
  detail::AsyncManager::stop();
}
// inline static void delay(lib::time::TimeSpan ts)
// {
//   detail::AsyncManager::delay(ts);
// }

} // namespace async