#include "string.hpp"

std::string formatString(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);

  size_t size = std::vsnprintf(nullptr, 0, fmt, args);
  va_end(args);

  char *data = new char[size];

  va_start(args, fmt);
  std::vsnprintf(data, size + 1, fmt, args);
  va_end(args);

  std::string result(data);
  delete[] data;
  return result;
}