#include "Logger.hpp"
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace os
{
std::atomic<bool> Logger::s_started{false};
std::atomic<bool> Logger::s_running{false};
std::atomic<bool> Logger::s_consoleEnabled{true};
std::atomic<int> Logger::s_idleSleep{10};
std::thread Logger::s_worker;

lib::ConcurrentQueue<Logger::LogItem> Logger::s_queue;

std::mutex Logger::s_fileMutex;
std::ofstream Logger::s_file;

void Logger::start()
{
  bool expected = false;
  if (s_started.compare_exchange_strong(expected, true))
  {
    s_running.store(true);
    s_worker = std::thread(&Logger::workerLoop);
    std::atexit(
        []
        {
          Logger::shutdown();
        });
  }
}

void Logger::shutdown()
{
  if (!s_started.load())
    return;
  bool expected = true;
  if (s_running.compare_exchange_strong(expected, false))
  {
    if (s_worker.joinable())
      s_worker.join();
    // Flush remaining messages
    LogItem item;
    while (s_queue.tryDequeue(item))
    {
      writeItem(item);
    }
    std::lock_guard<std::mutex> lk(s_fileMutex);
    if (s_file.is_open())
      s_file.close();
  }
}

void Logger::setOutputFile(const std::string &path, bool append)
{
  std::lock_guard<std::mutex> lk(s_fileMutex);
  if (s_file.is_open())
    s_file.close();
  std::ios::openmode mode = std::ios::out;
  if (append)
    mode |= std::ios::app;
  s_file.open(path, mode);
}

void Logger::setConsoleEnabled(bool enabled)
{
  s_consoleEnabled.store(enabled);
}

void Logger::setIdleSleep(lib::time::TimeSpan span)
{
  s_idleSleep.store(span.milliseconds());
}

void Logger::log(std::string_view msg)
{
  enqueue(Level::Info, msg);
}
void Logger::warning(std::string_view msg)
{
  enqueue(Level::Warning, msg);
}
void Logger::error(std::string_view msg)
{
  enqueue(Level::Error, msg);
}

// Private helpers
void Logger::ensureStarted()
{
  if (!s_started.load())
    start();
}

void Logger::enqueue(Level lvl, std::string_view msg)
{
  ensureStarted();
  LogItem item;
  item.level = lvl;
  item.text.assign(msg.begin(), msg.end());
  item.ts = std::chrono::system_clock::now();
  item.tid = std::this_thread::get_id();
  s_queue.enqueue(std::move(item));
}

const char *Logger::levelToString(Level l)
{
  switch (l)
  {
  case Level::Info:
    return "INFO";
  case Level::Warning:
    return "WARN";
  case Level::Error:
    return "ERROR";
  }
  return "?";
}

std::string Logger::formatTimestamp(const std::chrono::system_clock::time_point &tp)
{
  using namespace std::chrono;
  auto t = system_clock::to_time_t(tp);
  auto tp_ms = time_point_cast<milliseconds>(tp);
  auto ms = static_cast<int>(tp_ms.time_since_epoch().count() % 1000);

  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif

  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms;
  return oss.str();
}

void Logger::writeItem(const LogItem &it)
{
  std::ostringstream line;
  // line << '[' << formatTimestamp(it.ts) << "] ";

  std::string levelStr;
  const char *reset = "\033[0m";
  const char *color = "";

  switch (it.level)
  {
  case Level::Info:
    color = "\033[32m";
    break;
  case Level::Warning:
    color = "\033[33m";
    break;
  case Level::Error:
    color = "\033[31m";
    break;
  }

  levelStr = std::string(color) + levelToString(it.level) + reset;

  line << levelStr << " " << it.text << '\n';
  //line << levelStr << " [tid " << it.tid << "] " << it.text << '\n';

  const std::string s = line.str();

  if (s_consoleEnabled.load())
  {
    if (it.level == Level::Error)
    {
      std::fwrite(s.data(), 1, s.size(), stderr);
      std::fflush(stderr);
    }
    else
    {
      std::fwrite(s.data(), 1, s.size(), stdout);
      std::fflush(stdout);
    }
  }

  std::lock_guard<std::mutex> lk(s_fileMutex);
  if (s_file.is_open())
  {
    std::ostringstream fline;
    fline << levelToString(it.level) << " " << it.text << '\n';
    // fline << '[' << formatTimestamp(it.ts) << "] " << levelToString(it.level) << " [tid " << it.tid << "] " << it.text << '\n';

    s_file << fline.str();
    s_file.flush();
  }
}

void Logger::workerLoop()
{
  constexpr size_t kBatch = 64;
  LogItem items[kBatch];

  while (s_running.load())
  {
    size_t popped = 0;
    for (; popped < kBatch; ++popped)
    {
      if (!s_queue.tryDequeue(items[popped]))
        break;
    }

    if (popped == 0)
    {
      auto ms = s_idleSleep.load();
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      continue;
    }

    for (size_t i = 0; i < popped; ++i)
    {
      writeItem(items[i]);
    }
  }
}

void Logger::enqueuef(Level lvl, const char *fmt, va_list args)
{
  ensureStarted();

  va_list args_copy;
  va_copy(args_copy, args);

  char buffer[512];
  int needed = vsnprintf(buffer, sizeof(buffer), fmt, args);

  std::string msg;
  if (needed < 0)
  {
    msg = "Logger format error";
  }
  else if (needed < (int)sizeof(buffer))
  {
    msg.assign(buffer, needed);
  }
  else
  {
    std::vector<char> dynbuf(needed + 1);
    vsnprintf(dynbuf.data(), dynbuf.size(), fmt, args_copy);
    msg.assign(dynbuf.data(), needed);
  }

  va_end(args_copy);

  enqueue(lvl, msg);
}

void Logger::logf(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  enqueuef(Level::Info, fmt, args);
  va_end(args);
}

void Logger::warningf(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  enqueuef(Level::Warning, fmt, args);
  va_end(args);
}

void Logger::errorf(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  enqueuef(Level::Error, fmt, args);
  va_end(args);
}

} // namespace os
