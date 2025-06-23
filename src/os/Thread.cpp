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
