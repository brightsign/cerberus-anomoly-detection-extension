#pragma once
#include "wvm/config.hpp"

namespace wvm {

class RoiManager {
public:
  explicit RoiManager(const RoiConfig& cfg);
  const std::vector<RoiRect>& tvs() const { return tvs_; }

private:
  std::vector<RoiRect> tvs_;
};

} // namespace wvm
