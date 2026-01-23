#pragma once
#include "wvm/capture.hpp"
#include "wvm/config.hpp"
#include <string>

namespace wvm {

class V4L2Capture : public ICapture {
public:
  explicit V4L2Capture(const DeviceConfig& cfg);
  ~V4L2Capture() override;

  bool start() override;
  void stop() override;
  bool read_frame(CapturedFrame& out) override;

private:
  DeviceConfig cfg_;
  int fd_ = -1;
  bool is_mplane_ = false;

  struct Buf { void* ptr=nullptr; size_t len=0; };
  Buf bufs_[4];
  int nbufs_ = 0;
  bool streaming_ = false;
  
  // Frame statistics
  uint64_t frame_count_ = 0;
  uint64_t last_stats_time_ = 0;

  bool init_device();
  bool init_mmap();
};

} // namespace wvm
