#pragma once
#include "datastructure/ConcurrentQueue.hpp"
#include "time/TimeSpan.hpp"
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace os
{

class Logger
{
public:
  enum class Level
  {
    Info,
    Warning,
    Error
  };

  static void start(uint32_t maxQueueSize = 1024);
  static void shutdown();
  static void setOutputFile(const std::string &path, bool append = false);
  static void setConsoleEnabled(bool enabled);
  static void setIdleSleep(lib::time::TimeSpan span);

  static void log(std::string_view msg);
  static void warning(std::string_view msg);
  static void error(std::string_view msg);

  // printf-style formatted logging
  static void logf(const char *fmt, ...);
  static void warningf(const char *fmt, ...);
  static void errorf(const char *fmt, ...);
  static void setMaxQueueSize(size_t maxSize);

private:
  struct LogItem
  {
    Level level;
    std::string text;
    std::chrono::system_clock::time_point ts;
    std::thread::id tid;
  };
  static std::atomic<size_t> s_maxQueueSize;
  static std::atomic<size_t> s_resumeQueueSize;
  static void ensureStarted();
  static void enqueue(Level lvl, std::string_view msg);
  static void enqueuef(Level lvl, const char *fmt, va_list args);
  static void waitForQueueSpace();

  static const char *levelToString(Level l);
  static std::string formatTimestamp(const std::chrono::system_clock::time_point &tp);
  static void writeItem(const LogItem &it);
  static void workerLoop();

  static std::atomic<bool> s_started;
  static std::atomic<bool> s_running;
  static std::atomic<bool> s_consoleEnabled;
  static std::atomic<int> s_idleSleep;
  static std::thread s_worker;
  static lib::ConcurrentQueue<LogItem> s_queue;

  static std::mutex s_fileMutex;
  static std::ofstream s_file;
};

} // namespace os
