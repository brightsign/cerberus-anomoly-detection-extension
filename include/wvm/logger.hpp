#pragma once
#include <string>
#include <mutex>
#include <cstdio>

namespace wvm {

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
public:
  static Logger& instance();

  void init(LogLevel level, const std::string& path);
  void log(LogLevel lvl, const char* fmt, ...);

  LogLevel level() const { return level_; }

private:
  Logger() = default;
  std::mutex m_;
  LogLevel level_ = LogLevel::INFO;
  std::FILE* fp_ = nullptr;
};

LogLevel parse_level(const std::string& s);

} // namespace wvm
