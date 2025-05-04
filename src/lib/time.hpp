#pragma once
#include <chrono>
namespace lib
{
class TimeSpan
{
public:
  explicit TimeSpan(std::chrono::nanoseconds ns) : duration_ns(ns)
  {
  }

  TimeSpan() : duration_ns(0)
  {
  }

  double seconds() const
  {
    return std::chrono::duration<double>(duration_ns).count();
  }

  double milliseconds() const
  {
    return std::chrono::duration<double, std::milli>(duration_ns).count();
  }

  double microseconds() const
  {
    return std::chrono::duration<double, std::micro>(duration_ns).count();
  }

  double nanoseconds() const
  {
    return static_cast<double>(duration_ns.count());
  }

  double minutes() const
  {
    return std::chrono::duration<double, std::ratio<60>>(duration_ns).count();
  }

  double hours() const
  {
    return std::chrono::duration<double, std::ratio<3600>>(duration_ns).count();
  }

  double days() const
  {
    return std::chrono::duration<double, std::ratio<86400>>(duration_ns).count();
  }

  static TimeSpan now()
  {
    auto now = std::chrono::steady_clock::now();
    return TimeSpan(std::chrono::time_point_cast<std::chrono::nanoseconds>(now).time_since_epoch());
  }

  static TimeSpan fromSeconds(double seconds)
  {
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(seconds));
    return TimeSpan(duration);
  }

  static TimeSpan fromMilliseconds(double milliseconds)
  {
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::milli>(milliseconds));
    return TimeSpan(duration);
  }

  static TimeSpan fromMicroseconds(double microseconds)
  {
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::micro>(microseconds));
    return TimeSpan(duration);
  }

  static TimeSpan fromNanoseconds(double nanoseconds)
  {
    auto duration = std::chrono::nanoseconds(static_cast<long long>(nanoseconds));
    return TimeSpan(duration);
  }

  static TimeSpan fromMinutes(double minutes)
  {
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::ratio<60>>(minutes));
    return TimeSpan(duration);
  }

  static TimeSpan fromHours(double hours)
  {
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::ratio<3600>>(hours));
    return TimeSpan(duration);
  }
  static TimeSpan fromDays(double days)
  {
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::ratio<86400>>(days));
    return TimeSpan(duration);
  }
  TimeSpan operator+(const TimeSpan &other) const
  {
    return TimeSpan(duration_ns + other.duration_ns);
  }

  TimeSpan operator-(const TimeSpan &other) const
  {
    return TimeSpan(duration_ns - other.duration_ns);
  }
  bool operator>(const TimeSpan &other) const
  {
    return duration_ns > other.duration_ns;
  }

  bool operator>=(const TimeSpan &other) const
  {
    return duration_ns >= other.duration_ns;
  }

  bool operator<(const TimeSpan &other) const
  {
    return duration_ns < other.duration_ns;
  }

  bool operator<=(const TimeSpan &other) const
  {
    return duration_ns <= other.duration_ns;
  }

private:
  std::chrono::nanoseconds duration_ns;
};

class Timer
{
public:
  void start()
  {
    start_time = std::chrono::high_resolution_clock::now();
  }

  TimeSpan end()
  {
    auto end_time = std::chrono::high_resolution_clock::now();
    return TimeSpan(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time));
  }

private:
  std::chrono::high_resolution_clock::time_point start_time;
};
} // namespace lib