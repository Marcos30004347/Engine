#pragma once

#include <iostream>
#include <mutex>
#include <sstream>
#include <stdarg.h>
#include <stdio.h>
#include <string>

namespace os
{
void print(const char *format, ...);
}