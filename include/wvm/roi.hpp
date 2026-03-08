#pragma once
#include <vector>
#include "wvm/config.hpp"

namespace wvm {

class RoiManager {
public:
  explicit RoiManager(const RoiConfig& cfg);

  // For roi.mode=="grid", generate tv1..tvN ROIs based on the incoming frame size.
  // Returns true if ROIs were regenerated.
  bool update_from_frame(int frame_w, int frame_h);

  const std::vector<RoiRect>& tvs() const { return tvs_; }

private:
  RoiConfig cfg_;
  std::vector<RoiRect> tvs_;

  // Remember last frame size used to generate grid ROIs.
  int grid_frame_w_ = 0;
  int grid_frame_h_ = 0;
};

} // namespace wvm
