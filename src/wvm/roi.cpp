#include "wvm/roi.hpp"
#include "wvm/logger.hpp"
#include <algorithm>

namespace wvm {

RoiManager::RoiManager(const RoiConfig& cfg)
  : cfg_(cfg), tvs_(cfg.tvs) {
  // In grid mode, tvs_ is generated once we know incoming frame size.
  if (cfg_.mode == "grid") {
    tvs_.clear();
  }
}

bool RoiManager::update_from_frame(int frame_w, int frame_h) {
  if (cfg_.mode != "grid") return false;
  if (frame_w <= 0 || frame_h <= 0) return false;

  // Only regenerate if first time or resolution changed.
  if (!tvs_.empty() && frame_w == grid_frame_w_ && frame_h == grid_frame_h_) return false;

  const int rows = std::max(1, cfg_.grid.rows);
  const int cols = std::max(1, cfg_.grid.cols);
  const int max_tiles = rows * cols;

  int count = cfg_.grid.count;
  if (count <= 0) count = max_tiles;
  count = std::min(count, max_tiles);

  const int tile_w = frame_w / cols;
  const int tile_h = frame_h / rows;

  if (tile_w <= 0 || tile_h <= 0) {
    Logger::instance().log(LogLevel::ERROR, "ROI grid invalid: frame=%dx%d rows=%d cols=%d -> tile=%dx%d",
                           frame_w, frame_h, rows, cols, tile_w, tile_h);
    return false;
  }

  tvs_.clear();
  tvs_.reserve((size_t)count);

  for (int i = 0; i < count; ++i) {
    const int r = i / cols;
    const int c = i % cols;

    const int x = c * tile_w;
    const int y = r * tile_h;

    // Give remainder pixels to last column/row so we cover the full frame.
    const int w = (c == cols - 1) ? (frame_w - x) : tile_w;
    const int h = (r == rows - 1) ? (frame_h - y) : tile_h;

    RoiRect rr;
    rr.id = "tv" + std::to_string(i + 1);
    rr.x = x;
    rr.y = y;
    rr.w = std::max(1, w);
    rr.h = std::max(1, h);
    tvs_.push_back(rr);
  }

  grid_frame_w_ = frame_w;
  grid_frame_h_ = frame_h;

  Logger::instance().log(LogLevel::INFO, "ROI grid generated: frame=%dx%d rows=%d cols=%d count=%d tile=%dx%d (last col/row absorbs remainder)",
                         frame_w, frame_h, rows, cols, count, tile_w, tile_h);
  return true;
}

} // namespace wvm
