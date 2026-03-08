#pragma once
#include "wvm/capture.hpp"
#include "wvm/config.hpp"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>

namespace wvm {

class GstRtspCapture : public ICapture {
public:
  explicit GstRtspCapture(const DeviceConfig& cfg);
  ~GstRtspCapture() override;

  bool start() override;
  void stop() override;
  bool read_frame(CapturedFrame& out) override;

private:
  DeviceConfig cfg_;

  // GStreamer – only touched from open_pipeline() / close_pipeline() / bus thread
  GstElement* pipeline_ = nullptr;
  GstElement* appsink_  = nullptr;
  GstBus*     bus_      = nullptr;

  std::atomic<bool> broken_{false};
  std::atomic<bool> bus_running_{false};
  std::thread       bus_thread_;

  // Frame delivery – pulled from appsink, read by read_frame()
  std::mutex        frame_mutex_;
  CapturedFrame     current_frame_;
  bool              frame_ready_ = false;
  std::vector<uint8_t> frame_buffer_;   // reuse buffer to avoid allocs

  // Statistics
  uint64_t frame_count_     = 0;
  uint64_t last_stats_time_ = 0;

  // Pipeline string variants tried in order on each open attempt
  std::vector<std::string> build_pipeline_candidates() const;

  // Open one GStreamer pipeline from a string; waits for first frame.
  bool open_pipeline(const std::string& pipeline_str, int first_frame_timeout_ms);

  // Tear down the current pipeline (safe to call multiple times)
  void close_pipeline();

  // Background thread: watch bus for ERROR/EOS
  void bus_watch_thread();
};

} // namespace wvm
