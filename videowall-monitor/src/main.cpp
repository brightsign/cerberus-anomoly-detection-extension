#include "wvm/config.hpp"
#include "wvm/logger.hpp"
#include "wvm/pipeline.hpp"
#include <csignal>
#include <atomic>
#include <thread>

static std::atomic<bool> g_stop{false};

static void on_sig(int) { g_stop = true; }

int main(int argc, char** argv) {
  std::string cfg_path = (argc > 1) ? argv[1] : "config/videowall.json";

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
