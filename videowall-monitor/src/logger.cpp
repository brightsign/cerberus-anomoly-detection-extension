#include "wvm/logger.hpp"
#include <cstdarg>
#include <ctime>

namespace wvm {

Logger& Logger::instance() {
  static Logger inst;
  return inst;
}

void Logger::init(LogLevel level, const std::string& path) {
  std::lock_guard<std::mutex> lk(m_);
  level_ = level;
  if (fp_) std::fclose(fp_);
  fp_ = std::fopen(path.c_str(), "a");
  if (!fp_) fp_ = stderr;
}

void Logger::log(LogLevel lvl, const char* fmt, ...) {
  if ((int)lvl < (int)level_) return;

  std::lock_guard<std::mutex> lk(m_);
  if (!fp_) fp_ = stderr;

  std::time_t t = std::time(nullptr);
  char tb[32];
  std::strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", std::localtime(&t));

  const char* ls = "INFO";
  if (lvl == LogLevel::DEBUG) ls = "DEBUG";
  else if (lvl == LogLevel::WARN) ls = "WARN";
  else if (lvl == LogLevel::ERROR) ls = "ERROR";

  std::fprintf(fp_, "[%s] %s: ", tb, ls);

  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(fp_, fmt, ap);
  va_end(ap);

  std::fprintf(fp_, "\n");
  std::fflush(fp_);
}

LogLevel parse_level(const std::string& s) {
  if (s == "DEBUG") return LogLevel::DEBUG;
  if (s == "WARN") return LogLevel::WARN;
  if (s == "ERROR") return LogLevel::ERROR;
  return LogLevel::INFO;
}

} // namespace wvm
