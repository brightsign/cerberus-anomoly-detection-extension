#pragma once
#include "wvm/config.hpp"
#include "wvm/ts_queue.hpp"
#include "wvm/types.hpp"
#include "wvm/capture.hpp"
#include "wvm/roi.hpp"
#include "wvm/rga_preproc.hpp"
#include "wvm/rknn_mobilenet.hpp"
#include "wvm/reference_db.hpp"
#include "wvm/matcher.hpp"
#include "wvm/anomaly.hpp"
#include "wvm/basic_anomaly.hpp"
#include "wvm/health_engine.hpp"
#include "wvm/mqtt.hpp"

#include <atomic>
#include <thread>
#include <memory>
#include <fstream>

namespace wvm {

class Pipeline {
public:
  explicit Pipeline(const AppConfig& cfg);
  ~Pipeline();

  bool start();
  void stop();

private:
  AppConfig cfg_;
  std::atomic<bool> stop_{false};

  // Components
  std::unique_ptr<ICapture> cap_;
  RoiManager roi_;
  RgaPreprocessor pre_;
  RknnMobileNet npu_;
  ReferenceDB ref_;
  std::unique_ptr<Matcher> matcher_;
  std::unique_ptr<AnomalyEngine> anomaly_;
  std::unique_ptr<BasicAnomalyEngine> basic_anomaly_;
  std::unique_ptr<HealthEngine> health_;  // New health monitoring engine
  MqttPublisher mqtt_;

  // Camera offline detection
  uint64_t last_frame_ts_ = 0;
  bool camera_offline_active_ = false;

  // Auto-reference (record+detect in a single run)
  bool ref_auto_ = false;
  uint64_t last_ref_append_ts_ = 0;
  uint64_t last_ref_flush_ts_ = 0;
  bool ref_armed_ = false;
  std::ofstream ref_record_ofs_;

  // Queues
  TsQueue<CapturedFrame> frame_q_{2};
  TsQueue<RoiBatch> infer_q_{2};
  TsQueue<EmbeddingBatch> analyze_q_{2};
  TsQueue<Event> mqtt_q_{64};

  // Threads
  std::thread t_cap_;
  std::thread t_pre_;
  std::thread t_inf_;
  std::thread t_ana_;
  std::thread t_mqtt_;

  // Loops
  void capture_loop();
  void preprocess_loop();
  void inference_loop();
  void analysis_loop();
  void mqtt_loop();
  
  // Frame streaming helper
  void save_frame_composite(const CapturedFrame& f);
};

} // namespace wvm
