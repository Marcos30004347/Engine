#pragma once

#include <string>
#include <vector>

namespace os
{
namespace io
{
std::string readFile(const std::string &filePath);
std::vector<char> readBinaryFile(const std::string &filePath);
std::string getExecutablePath();
std::string getExecutableDirectory();
std::string readRelativeFile(const std::string &relativePath);
} // namespace io
} // namespace os