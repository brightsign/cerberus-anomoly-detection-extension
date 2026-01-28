#pragma once
#include <string>
#include <vector>
#include "wvm/types.hpp"

namespace wvm {

struct DeviceConfig {
  std::string camera_device = "/dev/video0";
  int width = 1920;
  int height = 1080;
  int fps = 30;
  PixelFormat pixel_format = PixelFormat::YUYV;
};

struct ModelConfig {
  std::string rknn_path;
  int input_w = 224;
  int input_h = 224;
  int embedding_dim = 1280;
};

struct ReferenceConfig {
  bool enabled = true;
  int ref_fps = 5;
  // Reference modes:
  //  - "file": load embeddings_path (offline reference)
  //  - "auto": build a reference online from the camera ROIs and detect simultaneously
  std::string mode = "file";

  std::string embeddings_path;
  std::string index_path;
  double search_window_seconds = 2.0;

  // Auto mode settings
  // How to pick the online reference each tick:
  //  - "tv_id": use ref_tv_id as the reference stream (best for demos)
  //  - "consensus": choose the medoid embedding across TVs each tick (best for 5-6 TVs)
  std::string auto_source = "tv_id";
  std::string ref_tv_id = "tv1";

  // Warmup before enabling reference matching anomalies (BLACK can trigger immediately)
  double auto_min_ref_seconds = 3.0;

  // Optional on-device recording of the generated reference timeline (for debugging / reuse)
  bool auto_persist = true;
  std::string auto_persist_path = "/storage/sd/references/auto_ref_embeddings.f32";

  // Cap in-memory reference length to bound RAM (ring buffer)
  int auto_max_seconds = 900; // 15 minutes @ ref_fps
};

struct RoiConfig {
  std::string mode = "rect";
  std::vector<RoiRect> tvs;
};

struct AnomalyConfig {
  bool enabled = true;
  int sample_fps = 5;
  
  // BLACK detection with hysteresis (separate enter/exit thresholds)
  float black_enter_mean = 25.0f;
  float black_exit_mean  = 40.0f;
  float black_enter_var  = 200.0f;
  float black_exit_var   = 500.0f;
  int persist_black_ms = 1500;
  int persist_black_recover_ms = 1500;
  
  int persist_freeze_ms = 2000;
  int persist_mismatch_ms = 2500;
  float lag_threshold_s = 1.5f;
  int stutter_window_ms = 3000;
  float similarity_min = 0.75f;
  
  // Basic anomaly detection (reference-less)
  float freeze_similarity = 0.9995f;
  // Optional: don't generate FREEZE events for these TV ids in basic mode.
  // Useful when one ROI is considered the "reference" screen (tv1) and you only
  // want freeze alerts for the other screens.
  std::vector<std::string> freeze_ignore_tvs;
  int camera_timeout_ms = 1500;
  bool peer_enabled = true;
  float peer_similarity_min = 0.85f;
  int persist_outlier_ms = 2500;
};

struct MqttConfig {
  bool enabled = true;
  std::string host = "127.0.0.1";
  int port = 1883;
  std::string topic = "videowall/events";
  std::string client_id = "xt5-wall-monitor";
};

struct LoggingConfig {
  std::string level = "INFO";
  std::string path = "/var/log/videowall-monitor.log";
};

struct AppConfig {
  DeviceConfig device;
  ModelConfig model;
  ReferenceConfig reference;
  RoiConfig roi;
  AnomalyConfig anomaly;
  MqttConfig mqtt;
  LoggingConfig logging;
};

bool load_config(const std::string& path, AppConfig& out);

} // namespace wvm
