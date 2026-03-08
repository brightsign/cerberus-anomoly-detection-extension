#include "wvm/config.hpp"
#include "wvm/logger.hpp"
#include "wvm/pipeline.hpp"
#include <csignal>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

static std::atomic<bool> g_stop{false};

static void on_sig(int) { g_stop = true; }

// SIGSEGV/SIGABRT handler: write crash marker then exit(1) so the
// bsext_init wrapper STOPS restarting and lets us SSH in.
static void on_crash(int sig) {
  // Use only async-signal-safe functions here (write, not fprintf).
  const char* msg = "[MAIN] FATAL SIGNAL received - writing crash marker and exiting\n";
  write(STDERR_FILENO, msg, __builtin_strlen(msg));

  // Write a crash marker file so we can see what happened after reboot.
  int fd = open("/tmp/anomaly_crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "CRASH: signal %d\n", sig);
    write(fd, buf, n);
    close(fd);
  }
  // Exit with 1 — wrapper breaks on exit(1) (not 42/139/134), stops restart loop.
  _exit(1);
}

static bool file_exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

// Returns true if any non-loopback interface has an IPv4 address.
// Outputs the interface name and IP via out parameters.
static bool any_interface_has_ip(std::string& out_iface, std::string& out_ip) {
  struct ifaddrs* ifap = nullptr;
  if (getifaddrs(&ifap) != 0) return false;

  bool found = false;
  for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    // Skip loopback
    if (std::string(ifa->ifa_name) == "lo") continue;

    char ip[INET_ADDRSTRLEN] = {};
    auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));

    // Skip 0.0.0.0
    if (std::string(ip) == "0.0.0.0") continue;

    out_iface = ifa->ifa_name;
    out_ip    = ip;
    found = true;
    break;
  }
  freeifaddrs(ifap);
  return found;
}

// Block until a non-loopback interface has an IP (needed before RTSP connect).
// Returns true if network became ready, false if timed out.
static bool wait_for_network(int retries = 30, int delay_sec = 2) {
  fprintf(stderr, "[NET] Waiting for network connectivity...\n");
  for (int i = 0; i < retries; i++) {
    std::string iface, ip;
    if (any_interface_has_ip(iface, ip)) {
      fprintf(stderr, "[NET] ✅ Network ready: interface=%s ip=%s\n", iface.c_str(), ip.c_str());
      return true;
    }
    fprintf(stderr, "[NET] Waiting for network (attempt %d/%d)...\n", i + 1, retries);
    std::this_thread::sleep_for(std::chrono::seconds(delay_sec));
  }
  fprintf(stderr, "[NET] ❌ ERROR: No network interface got an IP after %d attempts (%ds)\n",
          retries, retries * delay_sec);
  return false;
}

static bool is_rtsp_url(const std::string& s) {
  return s.find("rtsp://") == 0 || s.find("RTSP://") == 0;
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

  // Early logging to stderr before logger is initialized
  fprintf(stderr, "[MAIN] ========================================\n");
  fprintf(stderr, "[MAIN] Starting anomaly_detection\n");
  fprintf(stderr, "[MAIN] Build: %s %s\n", __DATE__, __TIME__);
  fprintf(stderr, "[MAIN] ========================================\n");
  fprintf(stderr, "[MAIN] Environment variables:\n");
  fprintf(stderr, "[MAIN]   LD_LIBRARY_PATH=%s\n", getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "(not set)");
  fprintf(stderr, "[MAIN]   GST_PLUGIN_PATH=%s\n", getenv("GST_PLUGIN_PATH") ? getenv("GST_PLUGIN_PATH") : "(not set)");
  fprintf(stderr, "[MAIN]   GST_PLUGIN_SYSTEM_PATH=%s\n", getenv("GST_PLUGIN_SYSTEM_PATH") ? getenv("GST_PLUGIN_SYSTEM_PATH") : "(not set)");
  fprintf(stderr, "[MAIN]   GST_PLUGIN_SCANNER=%s\n", getenv("GST_PLUGIN_SCANNER") ? getenv("GST_PLUGIN_SCANNER") : "(not set)");
  fprintf(stderr, "[MAIN]   GST_REGISTRY=%s\n", getenv("GST_REGISTRY") ? getenv("GST_REGISTRY") : "(not set)");
  fprintf(stderr, "[MAIN] ========================================\n");
  fprintf(stderr, "[MAIN] Config path: %s\n", cfg_path.c_str());
  fprintf(stderr, "[MAIN] Config exists: %s\n", file_exists(cfg_path) ? "YES" : "NO");

  wvm::AppConfig cfg;
  fprintf(stderr, "[MAIN] Loading config...\n");
  if (!wvm::load_config(cfg_path, cfg)) {
    fprintf(stderr, "[MAIN] ERROR: Failed to load config from %s\n", cfg_path.c_str());
    return 2;
  }
  fprintf(stderr, "[MAIN] Config loaded successfully\n");
  fprintf(stderr, "[MAIN] camera_device: '%s'\n", cfg.device.camera_device.c_str());
  fprintf(stderr, "[MAIN] mqtt.host: '%s', mqtt.enabled: %d\n", cfg.mqtt.host.c_str(), (int)cfg.mqtt.enabled);

  wvm::Logger::instance().init(wvm::parse_level(cfg.logging.level), cfg.logging.path);
  wvm::Logger::instance().log(wvm::LogLevel::INFO, "Starting videowall-monitor with %s", cfg_path.c_str());
  wvm::Logger::instance().log(wvm::LogLevel::INFO, "Camera device: %s", cfg.device.camera_device.c_str());
  wvm::Logger::instance().log(wvm::LogLevel::INFO, "Model path: %s", cfg.model.rknn_path.c_str());
  wvm::Logger::instance().log(wvm::LogLevel::INFO, "Anomaly enabled: %d", (int)cfg.anomaly.enabled);
  wvm::Logger::instance().log(wvm::LogLevel::INFO, "Reference enabled: %d", (int)cfg.reference.enabled);
  wvm::Logger::instance().log(wvm::LogLevel::INFO, "Health enabled: %d", (int)cfg.health.enabled);

  std::signal(SIGINT,  on_sig);
  std::signal(SIGTERM, on_sig);
  // Catch crashes so we can write a marker and stop the restart loop.
  std::signal(SIGSEGV, on_crash);
  std::signal(SIGABRT, on_crash);
  std::signal(SIGBUS,  on_crash);

  fprintf(stderr, "[MAIN] Creating pipeline...\n");
  wvm::Pipeline pipe(cfg);

  // Wait for network before starting RTSP capture
  if (is_rtsp_url(cfg.device.camera_device)) {
    fprintf(stderr, "[MAIN] RTSP source detected - waiting for network before starting pipeline...\n");
    if (!wait_for_network(30, 2)) {
      fprintf(stderr, "[MAIN] WARNING: Proceeding without confirmed network (will try anyway)\n");
    }
  }

  fprintf(stderr, "[MAIN] Starting pipeline...\n");
  if (!pipe.start()) {
    fprintf(stderr, "[MAIN] ERROR: Pipeline start failed! Exiting with code 1 (stops restart loop).\n");
    wvm::Logger::instance().log(wvm::LogLevel::ERROR, "Pipeline start failed");
    // Write marker so it's visible after we exit
    FILE* f = fopen("/tmp/anomaly_crash.txt", "w");
    if (f) { fprintf(f, "Pipeline start failed - check /tmp/anomaly_detection.log\n"); fclose(f); }
    return 1;
  }
  fprintf(stderr, "[MAIN] Pipeline started successfully\n");

  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

  pipe.stop();
  wvm::Logger::instance().log(wvm::LogLevel::INFO, "Stopped.");
  return 0;
}
