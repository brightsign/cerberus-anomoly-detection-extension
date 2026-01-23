#pragma once
#include "wvm/types.hpp"
#include "wvm/config.hpp"

namespace wvm {

class RgaPreprocessor {
public:
  explicit RgaPreprocessor(const ModelConfig& model_cfg);

  // Extract ROI from captured frame and return RGB model input.
  bool extract_roi_rgb224(const CapturedFrame& frame, const RoiRect& roi, RoiInput& out);

private:
  ModelConfig mcfg_;

  void compute_luma_stats(const uint8_t* rgb, int w, int h, float& mean, float& var);
};

} // namespace wvm
