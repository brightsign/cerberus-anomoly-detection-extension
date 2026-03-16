#pragma once
#include "wvm/types.hpp"
#include "rknn_api.h"
#include <string>
#include <vector>

namespace wvm {

// YOLOX RK NPU-based TV detector (COCO class 63 = "tv").
//
// Usage:
//   YoloTvDetector det(model_path);
//   if (det.is_loaded()) {
//     auto rois = det.detect(frame);   // blocking NPU call
//     ...
//     det.release();                   // free NPU after ROIs committed
//   }
//
// The detector is intentionally used only during the startup ROI-detection
// phase.  After ROIs are committed and saved, call release() so the NPU is
// fully available for steady-state MobileNet embedding inference.
class YoloTvDetector {
public:
  explicit YoloTvDetector(const std::string& model_path, float conf_thresh = 0.34f);
  ~YoloTvDetector();

  bool is_loaded() const { return loaded_; }

  // Detect "tv" (COCO class 63) boxes in the frame.
  // Returns candidate ROIs in original frame pixel coordinates, sorted by
  // confidence descending.  Empty when nothing detected or not loaded.
  std::vector<RoiRect> detect(const CapturedFrame& frame);

  // Destroy the RKNN context and free NPU resources.
  // Safe to call multiple times; is_loaded() returns false afterwards.
  void release();

private:
  bool   loaded_      = false;
  float  conf_thresh_;
  rknn_context ctx_   = 0;
  int    model_w_     = 640;
  int    model_h_     = 640;
  uint32_t n_outputs_ = 0;

  struct OutDims { int grid_h = 0; int grid_w = 0; rknn_tensor_format fmt = RKNN_TENSOR_NCHW; bool flat = false; bool valid = false; };
  OutDims out_dims_[3]{};

  bool init(const std::string& path);

  std::vector<RoiRect> decode(rknn_output* outputs,
                              int frame_w, int frame_h,
                              int pad_x, int pad_y, float scale);
};

} // namespace wvm
