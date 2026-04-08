#include "File.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <vector>

#ifdef _WIN32
#include <windows.h> // For GetModuleFileNameW
#elif __APPLE__
#include <mach-o/dyld.h> // For _NSGetExecutablePath
#include <limits.h>      // For PATH_MAX (though dynamic buffer is safer)
#elif __linux__
#include <unistd.h>      // For readlink
#include <limits.h>      // For PATH_MAX
#endif

namespace os
{
namespace io
{
std::string readFile(const std::string &filePath)
{
  std::ifstream file(filePath);

  if (!file)
  {
    std::cerr << "Error opening file: " << filePath << std::endl;
    abort();
  }

  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  if (file.bad())
  {
    std::cerr << "Error reading file: " << filePath << std::endl;
    abort();
  }

  return content;
}

std::vector<char> readBinaryFile(const std::string &filePath)
{
  std::ifstream file(filePath, std::ios::binary);

  if (!file)
  {
    std::cerr << "Error opening file: " << filePath << std::endl;
    return {};
  }

  file.seekg(0, std::ios::end);
  std::streamsize fileSize = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<char> buffer(fileSize);

  if (!file.read(buffer.data(), fileSize))
  {
    std::cerr << "Error reading file: " << filePath << std::endl;
    return {};
  }

  return buffer;
}

std::string getExecutablePath()
{
#ifdef _WIN32
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    return std::filesystem::path(path).string(); // Use .string() for native encoding, .generic_string() for '/'
#elif __APPLE__
    uint32_t bufferSize = PATH_MAX; // Start with a reasonable size
    std::vector<char> pathBuffer(bufferSize);
    
    while (_NSGetExecutablePath(pathBuffer.data(), &bufferSize) == -1) {
        pathBuffer.resize(bufferSize); // Resize buffer to the required size
    }
    
    char resolved_path[PATH_MAX];
    if (realpath(pathBuffer.data(), resolved_path) == NULL) {
        return std::string(pathBuffer.data());
    }
    return std::string(resolved_path);
#elif __linux__
    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    if (count > 0) {
        char resolved_path[PATH_MAX];
        if (realpath(std::string(result, count).c_str(), resolved_path) != NULL) {
            return std::string(resolved_path);
        } else {
            return std::string(result, count);
        }
    }
    return ""; 
#else
    return "";
#endif
}

std::string getExecutableDirectory()
{
  std::string path = getExecutablePath();
  size_t pos = path.find_last_of("\\/");
  return path.substr(0, pos);
}

std::string readRelativeFile(const std::string &relativePath)
{
  std::string base = getExecutableDirectory();
  return readFile(base + "/" + relativePath);
}

} // namespace io
} // namespace os