#include "Thread.hpp"

namespace os
{

Thread::Thread(Thread &&other) noexcept : m_thread(std::move(other.m_thread))
{
}

Thread::~Thread()
{
  if (m_thread.joinable())
  {
    m_thread.join();
  }
}

void Thread::join()
{
  if (m_thread.joinable())
  {
    m_thread.join();
  }
}

void Thread::detach()
{
  if (m_thread.joinable())
  {
    m_thread.detach();
  }
}

bool Thread::isRunning() const
{
  return m_thread.joinable();
}

void Thread::setAffinity(size_t core)
{
#if defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core, &cpuset);

  int result = pthread_setaffinity_np(m_thread.native_handle(), sizeof(cpu_set_t), &cpuset);
  if (result != 0)
  {
    // You can log an error here if needed
  }

#elif defined(_WIN32)
  DWORD_PTR mask = 1ull << core;
  HANDLE handle = static_cast<HANDLE>(m_thread.native_handle());

  DWORD_PTR result = SetThreadAffinityMask(handle, mask);
  if (result == 0)
  {
    // You can log an error here if needed
  }

#else
  // Affinity not supported on this platform
  (void)core;
#endif
}

std::thread::id Thread::getId() const
{
  return m_thread.get_id();
}

size_t Thread::getCurrentThreadId()
{
  std::hash<std::thread::id> hasher;
  return hasher(std::this_thread::get_id());
}

unsigned int Thread::getHardwareConcurrency()
{
  return std::thread::hardware_concurrency();
}

} // namespace os
