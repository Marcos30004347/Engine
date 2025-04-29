#include "print.hpp"
#include <mutex>

std::mutex print_mutex; 


void threadSafePrintf(const char *format, ...) {
    std::lock_guard<std::mutex> lock(print_mutex);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}