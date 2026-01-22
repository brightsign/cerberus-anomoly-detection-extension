#include "wvm/config.hpp"
#include "wvm/logger.hpp"
#include "wvm/pipeline.hpp"
#include <csignal>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <sys/stat.h>

static std::atomic<bool> g_stop{false};

static void on_sig(int) { g_stop = true; }

static bool file_exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static std::string find_config() {
  // Priority 1: Environment variable
  const char* env_cfg = std::getenv("BSEXT_CONFIG");
  if (env_cfg && file_exists(env_cfg)) {
    return env_cfg;
  }
  
  // Priority 2: SD card override (writable, survives package updates)
  const std::string sd_cfg = "/storage/sd/configs/config.json";
  if (file_exists(sd_cfg)) {
    return sd_cfg;
  }
  
  // Priority 3: Package default (read-only)
  return "config/videowall.json";
}

int main(int argc, char** argv) {
  // Priority 0: Command line argument (highest priority)
  std::string cfg_path = (argc > 1) ? argv[1] : find_config();

  wvm::AppConfig cfg;
  if (!wvm::load_config(cfg_path, cfg)) return 2;

  wvm::Logger::instance().init(wvm::parse_level(cfg.logging.level), cfg.logging.path);
  wvm::Logger::instance().log(wvm::LogLevel::INFO, "Starting videowall-monitor with %s", cfg_path.c_str());

  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);

  wvm::Pipeline pipe(cfg);
  if (!pipe.start()) return 3;

  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

  pipe.stop();
  wvm::Logger::instance().log(wvm::LogLevel::INFO, "Stopped.");
  return 0;
}
