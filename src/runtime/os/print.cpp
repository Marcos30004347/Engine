#include "print.hpp"
#include <mutex>
#include <system_error>

namespace os
{
std::mutex print_mutex;

void print(const char *format, ...)
{
  try
  {
    std::lock_guard<std::mutex> lock(print_mutex);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
  }
  catch (const std::system_error &error)
  {
    std::fprintf(stderr, "[print][Mutex] mutex=%p error=%s\n", static_cast<void *>(&print_mutex), error.what());
    std::fflush(stderr);
    throw;
  }
}

} // namespace os
