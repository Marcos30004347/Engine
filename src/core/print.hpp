#pragma once

#include <iostream>
#include <stdio.h>
#include <mutex>
#include <sstream>
#include <string>
#include <stdarg.h>

extern std::mutex print_mutex;

void threadSafePrintf(const char *format, ...);