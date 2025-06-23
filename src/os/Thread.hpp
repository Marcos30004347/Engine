#pragma once

#include <functional>
#include <future>
#include <thread>
#include <utility>

namespace os
{

/**
 * @brief A simple abstraction for a C++ thread.
 *
 * This class wraps std::thread to provide a more convenient interface
 * for creating, managing, and interacting with threads.
 * It also includes a static method to get the ID of the current executing thread.
 */
class Thread
{
public:
  /**
   * @brief Default constructor. Creates a thread object that does not
   * represent an active thread of execution.
   */
  Thread() = default;

  /**
   * @brief Constructs a Thread object and starts a new thread of execution.
   * @tparam F The type of the function or callable object.
   * @tparam Args The types of the arguments to pass to the function.
   * @param f The function or callable object to execute in the new thread.
   * @param args The arguments to pass to the function.
   *
   * @details This constructor takes a callable (function, lambda, functor)
   * and its arguments, then constructs an `std::thread` object
   * to execute the given callable in a new thread.
   * The arguments are perfectly forwarded to the underlying `std::thread` constructor.
   */
  template <typename F, typename... Args> explicit Thread(F &&f, Args &&...args) : m_thread(std::forward<F>(f), std::forward<Args>(args)...)
  {
  }

  // Delete copy constructor and copy assignment operator to prevent
  // accidental copying of thread objects, as std::thread is non-copyable.
  Thread(const Thread &) = delete;
  Thread &operator=(const Thread &) = delete;

  /**
   * @brief Move constructor. Allows moving ownership of the underlying std::thread.
   * @param other The Thread object to move from.
   */
  Thread(Thread &&other) noexcept;

  /**
   * @brief Move assignment operator. Allows moving ownership of the underlying std::thread.
   * @param other The Thread object to move from.
   * @return A reference to the moved-to Thread object.
   */
  Thread &operator=(Thread &&other) noexcept
  {
    if (this != &other)
    {
      if (m_thread.joinable())
      {
        // It's crucial to handle existing joinable thread if any
        // The common practice is to join or detach the old thread
        // before assigning a new one. For simplicity, we'll join here.
        // In a real-world scenario, you might want to throw an exception
        // or detach based on desired behavior.
        m_thread.join();
      }
      m_thread = std::move(other.m_thread);
    }
    return *this;
  }

  /**
   * @brief Destructor. If the thread is joinable, it will be joined
   * to prevent `std::terminate` being called. This ensures resources
   * are cleaned up.
   * Consider calling join() or detach() explicitly before the Thread object
   * goes out of scope if you need different behavior.
   */
  ~Thread();

  /**
   * @brief Waits for the thread to finish its execution.
   * This function blocks the calling thread until the thread
   * represented by this object has finished its execution.
   * It can only be called once for a given thread.
   * @details After `join()` returns, `isRunning()` will return `false`.
   */
  void join();

  /**
   * @brief Detaches the thread from its associated `std::thread` object.
   * The thread continues its execution independently.
   * Once detached, the `Thread` object no longer represents the
   * thread of execution, and `join()` cannot be called on it.
   * @details After `detach()` returns, `isRunning()` will return `false`.
   */
  void detach();

  /**
   * @brief Checks if the thread object represents an active thread of execution.
   * @return `true` if the thread is joinable (i.e., actively running or joinable),
   * `false` otherwise (e.g., default constructed, moved-from, joined, or detached).
   */
  bool isRunning() const;
  /**
   * @brief Gets the underlying std::thread::id of this thread object.
   * @return The std::thread::id associated with this Thread object.
   */
  std::thread::id getId() const;

  /**
   * @brief Static method to get the ID of the current executing thread.
   * @return A `size_t` value representing the unique ID of the current thread.
   *
   * @details This function uses `std::this_thread::get_id()` to obtain the
   * unique identifier for the calling thread, and then hashes it
   * to return a `size_t` value. While `std::thread::id` is the
   * canonical unique ID, `size_t` is often more convenient for
   * logging or simple comparisons.
   */
  static size_t getCurrentThreadId();

  /**
   * @brief Static method to get the number of concurrent threads supported by the system.
   * @return The number of hardware thread contexts available, or 0 if not detectable.
   */
  static unsigned int getHardwareConcurrency();

private:
  std::thread m_thread; ///< The underlying standard C++ thread object.
};

} // namespace os
