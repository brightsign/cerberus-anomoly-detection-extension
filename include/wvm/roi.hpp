#pragma once
#include <vector>
#include <memory>
#include "wvm/config.hpp"

namespace wvm { class YoloTvDetector; }  // forward declaration

namespace wvm {

class RoiManager {
public:
  explicit RoiManager(const RoiConfig& cfg);
  ~RoiManager();  // defined in roi.cpp where YoloTvDetector is complete

  // For roi.mode=="grid", generate tv1..tvN ROIs based on the incoming frame size.
  // For roi.mode=="auto", detect TVs from the incoming frame using OpenCV.
  // Returns true if ROIs were regenerated or updated.
  bool update_from_frame(const CapturedFrame& frame);

  const std::vector<RoiRect>& tvs() const { return tvs_; }

private:
  RoiConfig cfg_;
  std::vector<RoiRect> tvs_;

  // Remember last frame size used to generate/update ROIs.
  int frame_w_ = 0;
  int frame_h_ = 0;
  uint64_t last_auto_detect_ts_ms_ = 0;
  uint64_t first_auto_frame_ts_ms_ = 0;  // time of first auto-detect attempt
  std::vector<RoiRect> pending_auto_rois_;
  int pending_auto_count_ = 0;
  bool auto_locked_ = false;              // true after first successful commit
  uint64_t last_good_auto_commit_ts_ = 0; // ts of last IoU-validated commit

  bool load_saved_auto_rois();
  void save_auto_rois() const;
  bool update_grid_rois(int frame_w, int frame_h);
  bool update_auto_rois(const CapturedFrame& frame);

  std::unique_ptr<YoloTvDetector> yolo_detector_;
};

} // namespace wvm
