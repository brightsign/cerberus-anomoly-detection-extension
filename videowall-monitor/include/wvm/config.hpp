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
  std::string embeddings_path;
  std::string index_path;
  double search_window_seconds = 2.0;
};

struct RoiConfig {
  std::string mode = "rect";
  std::vector<RoiRect> tvs;
};

struct AnomalyConfig {
  bool enabled = true;
  int sample_fps = 5;
  float black_luma_mean = 12.0f;
  float black_luma_var  = 8.0f;
  int persist_black_ms = 1500;
  int persist_freeze_ms = 2000;
  int persist_mismatch_ms = 2500;
  float lag_threshold_s = 1.5f;
  int stutter_window_ms = 3000;
  float similarity_min = 0.75f;
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
