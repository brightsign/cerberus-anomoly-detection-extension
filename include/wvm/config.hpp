#pragma once
#include <string>
#include <vector>
#include <unordered_map>
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

struct RoiGridConfig {
  int rows = 1;
  int cols = 1;
  // Number of TVs to generate. 0 or negative => rows*cols
  int count = 0;
  // Currently only "row_major" is supported (tv1..tvN left→right, top→bottom)
  std::string order = "row_major";
};

struct RoiAutoConfig {
  int max_tvs = 6;
  int downscale_w = 640;
  int downscale_h = 360;
  int detect_interval_ms = 2000;
  float min_area_pct = 0.03f;
  float max_area_pct = 0.85f;
  float aspect_min = 1.1f;
  float aspect_max = 2.3f;
  float rectangularity_min = 0.75f;
  float commit_iou_min = 0.20f;
  int stable_frames = 2;
  std::string save_path = "/storage/sd/rois.json";
  // After this many ms with no committed ROIs, fall back to full-frame as tv1.
  // Covers TVs that extend to the frame edge (no closed contour visible).
  // Set to 0 to disable fallback.
  int detect_fallback_ms = 15000;
  // Reject candidate rectangles whose interior mean luminance exceeds this value.
  // Walls/ceilings in wide-angle/fisheye views are bright (~120-180) while TV screens
  // are dark (~20-80 when off, or lower when showing content). Set to 0 to disable.
  float max_interior_luma = 150.0f;
  // Path to a YOLOX-S RKNN model for NPU-based TV detection.
  // If set and the file exists, YOLOX is used instead of the OpenCV pipeline during
  // the startup detection phase.  Once ROIs are committed the NPU context is freed.
  // Supports @TARGET_SOC@ substitution (resolved at runtime).
  // Leave empty to always use OpenCV.
  std::string yolo_model_path = "";
  float yolo_conf_thresh = 0.28f;  // Lower than training default to catch dark/off TVs
};

struct RoiConfig {
  // "rect" (manual), "grid" (auto ROIs for mosaic streams), or "auto" (OpenCV TV detection)
  std::string mode = "rect";
  std::vector<RoiRect> tvs;   // used when mode=="rect"
  RoiGridConfig grid;         // used when mode=="grid"
  RoiAutoConfig auto_cfg;     // used when mode=="auto"
};

struct HealthConfig {
  bool enabled = true;
  int analysis_fps = 5;

  // Dark pixel detection
  int dark_luma_threshold = 30;  // Pixels below this are "dark"

  // BLACK detection (dark but not necessarily off)
  float black_enter_ratio = 0.95f;  // 95% of pixels dark
  float black_exit_ratio = 0.90f;
  float black_var_enter = 300.0f;   // Low variance = uniform darkness
  int persist_black_ms = 1500;
  int persist_recover_ms = 1500;

  // TV_OFF detection — multi-feature inactive-screen detector
  // Legacy strict-darkness rule (OLED/near-black panels)
  float off_mean = 6.0f;
  float off_var  = 30.0f;
  // Enter thresholds (all must be satisfied to enter TV_OFF)
  float off_dark_ratio_min    = 0.38f;
  float off_sat_mean_max      = 28.0f;
  float off_laplacian_var_max = 360.0f;
  float off_temporal_diff_max = 2.0f;
  // Exit thresholds (any one sufficient to leave TV_OFF — looser than enter)
  float off_dark_ratio_exit    = 0.30f;
  float off_sat_mean_exit      = 35.0f;
  float off_laplacian_var_exit = 500.0f;
  float off_temporal_diff_exit = 4.0f;
  int persist_off_ms         = 3000;
  int persist_off_recover_ms = 5000;  // Must stay non-TV_OFF this long before RECOVERED

  // OSD detection (NO_SIGNAL, WRONG_INPUT)
  std::string osd_mode = "embedding_prototypes";  // or "disabled"
  std::string osd_prototypes_path = "/storage/sd/osd_prototypes.json";
  float osd_sim_min = 0.85f;            // Minimum similarity for unknown OSDs
  float osd_sim_min_no_signal = 0.35f;  // Per-frame entry threshold for NO_SIGNAL detection.
                                        // Real 8s persistence + preserve_osd_pending (>= 0.38)
                                        // guard against false positives; don't raise this.
  float tv_off_sim_min = 0.85f;         // Threshold for TV_OFF prototype match (for reflective powered-off panels)
  float osd_sim_min_wrong_input = 0.75f; // Threshold for WRONG_INPUT / INPUT_MENU (real match ~0.99, noise ~0.4-0.5)
  int persist_osd_ms = 1500;            // Persistence for WRONG_INPUT / unknown OSD
  int persist_no_signal_ms = 8000;      // Longer persistence for NO_SIGNAL: real TV-off lasts minutes, noise lasts 2-3s

  // Periodic heartbeat: re-publish current state even without change
  int heartbeat_interval_ms = 30000;  // 0 = disabled

  // Prototype capture: accumulate embeddings for labelled TVs and write JSON
  bool prototype_capture = false;
  std::unordered_map<std::string, std::string> prototype_labels;  // tv_id → label
  int prototype_capture_seconds = 8;
  std::string prototype_output_path = "/storage/sd/osd_prototypes.json";
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
  HealthConfig health;  // New health monitoring config
  MqttConfig mqtt;
  LoggingConfig logging;
};

bool load_config(const std::string& path, AppConfig& out);

} // namespace wvm
