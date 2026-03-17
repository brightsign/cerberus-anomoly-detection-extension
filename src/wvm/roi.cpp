#include "wvm/roi.hpp"
#include "wvm/yolo_tv_detector.hpp"
#include "wvm/logger.hpp"
#include <algorithm>
#include <fstream>
#include <set>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <nlohmann/json.hpp>

namespace wvm {
namespace {

static float compute_iou(const RoiRect& a, const RoiRect& b) {
  int x1 = std::max(a.x, b.x);
  int y1 = std::max(a.y, b.y);
  int x2 = std::min(a.x + a.w, b.x + b.w);
  int y2 = std::min(a.y + a.h, b.y + b.h);
  int iw = std::max(0, x2 - x1);
  int ih = std::max(0, y2 - y1);
  float inter = static_cast<float>(iw * ih);
  float uni = static_cast<float>(a.w * a.h + b.w * b.h) - inter;
  return uni > 0.0f ? (inter / uni) : 0.0f;
}

static cv::Mat frame_to_bgr(const CapturedFrame& frame) {
  if (frame.width <= 0 || frame.height <= 0 || frame.data.empty()) return cv::Mat();

  if (frame.fmt == PixelFormat::RGB888) {
    cv::Mat rgb(frame.height, frame.width, CV_8UC3, const_cast<uint8_t*>(frame.data.data()));
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    return bgr.clone();
  }

  if (frame.fmt == PixelFormat::YUYV) {
    if ((int)frame.data.size() < frame.width * frame.height * 2) return cv::Mat();
    cv::Mat yuyv(frame.height, frame.width, CV_8UC2, const_cast<uint8_t*>(frame.data.data()));
    cv::Mat bgr;
    cv::cvtColor(yuyv, bgr, cv::COLOR_YUV2BGR_YUY2);
    return bgr;
  }

  if (frame.fmt == PixelFormat::NV12) {
    if ((int)frame.data.size() < (frame.width * frame.height * 3) / 2) return cv::Mat();
    cv::Mat yuv(frame.height + frame.height / 2, frame.width, CV_8UC1, const_cast<uint8_t*>(frame.data.data()));
    cv::Mat bgr;
    cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_NV12);
    return bgr;
  }

  return cv::Mat();
}

struct Candidate {
  RoiRect roi;
  float score = 0.0f;
};

static float rectangularity(const std::vector<cv::Point>& contour, const cv::Rect& r) {
  double area = cv::contourArea(contour);
  double rect_area = static_cast<double>(r.area());
  if (rect_area <= 1.0) return 0.0f;
  return static_cast<float>(area / rect_area);
}

}

RoiManager::~RoiManager() = default;

RoiManager::RoiManager(const RoiConfig& cfg)
  : cfg_(cfg), tvs_(cfg.tvs) {
  if (cfg_.mode == "grid") {
    tvs_.clear();
  } else if (cfg_.mode == "auto") {
    tvs_.clear();
    if (load_saved_auto_rois()) {
      Logger::instance().log(LogLevel::INFO, "ROI auto: loaded %zu persisted ROIs from %s",
                             tvs_.size(), cfg_.auto_cfg.save_path.c_str());
    }
    if (!cfg_.auto_cfg.yolo_model_path.empty()) {
      yolo_detector_ = std::make_unique<YoloTvDetector>(cfg_.auto_cfg.yolo_model_path, cfg_.auto_cfg.yolo_conf_thresh);
      if (!yolo_detector_->is_loaded()) {
        Logger::instance().log(LogLevel::WARN,
          "YoloTvDetector: not loaded — using OpenCV fallback for TV detection");
        yolo_detector_.reset();
      }
    }
  }
}

bool RoiManager::load_saved_auto_rois() {
  if (cfg_.auto_cfg.save_path.empty()) return false;
  std::ifstream f(cfg_.auto_cfg.save_path);
  if (!f.is_open()) return false;
  try {
    nlohmann::json j;
    f >> j;
    if (!j.contains("tvs") || !j["tvs"].is_array()) return false;
    tvs_.clear();
    for (const auto& tv : j["tvs"]) {
      RoiRect r;
      r.id = tv.value("id", "");
      r.x = tv.value("x", 0);
      r.y = tv.value("y", 0);
      r.w = tv.value("w", 0);
      r.h = tv.value("h", 0);
      if (!r.id.empty() && r.w > 0 && r.h > 0) tvs_.push_back(r);
    }
    return !tvs_.empty();
  } catch (...) {
    return false;
  }
}

void RoiManager::save_auto_rois() const {
  if (cfg_.auto_cfg.save_path.empty() || tvs_.empty()) return;
  try {
    nlohmann::json j;
    j["tvs"] = nlohmann::json::array();
    for (const auto& r : tvs_) {
      j["tvs"].push_back({{"id", r.id}, {"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}});
    }
    std::ofstream out(cfg_.auto_cfg.save_path);
    if (out.is_open()) {
      out << j.dump(2);
      out.flush();
      Logger::instance().log(LogLevel::INFO, "ROI auto: saved %zu ROI(s) to %s",
                             tvs_.size(), cfg_.auto_cfg.save_path.c_str());
    } else {
      Logger::instance().log(LogLevel::ERROR,
        "ROI auto: failed to open %s for writing — check path exists and is writable",
        cfg_.auto_cfg.save_path.c_str());
    }
  } catch (const std::exception& e) {
    Logger::instance().log(LogLevel::ERROR, "ROI auto: exception saving rois.json: %s", e.what());
  }
}

bool RoiManager::update_grid_rois(int frame_w, int frame_h) {
  if (frame_w <= 0 || frame_h <= 0) return false;
  if (!tvs_.empty() && frame_w == frame_w_ && frame_h == frame_h_) return false;

  const int rows = std::max(1, cfg_.grid.rows);
  const int cols = std::max(1, cfg_.grid.cols);
  const int max_tiles = rows * cols;
  int count = cfg_.grid.count;
  if (count <= 0) count = max_tiles;
  count = std::min(count, max_tiles);

  const int tile_w = frame_w / cols;
  const int tile_h = frame_h / rows;
  if (tile_w <= 0 || tile_h <= 0) return false;

  tvs_.clear();
  for (int i = 0; i < count; ++i) {
    int r = i / cols;
    int c = i % cols;
    int x = c * tile_w;
    int y = r * tile_h;
    int w = (c == cols - 1) ? (frame_w - x) : tile_w;
    int h = (r == rows - 1) ? (frame_h - y) : tile_h;
    tvs_.push_back({"tv" + std::to_string(i + 1), x, y, std::max(1, w), std::max(1, h)});
  }

  frame_w_ = frame_w;
  frame_h_ = frame_h;
  Logger::instance().log(LogLevel::INFO, "ROI grid generated: frame=%dx%d rows=%d cols=%d count=%d",
                         frame_w, frame_h, rows, cols, count);
  return true;
}

bool RoiManager::update_auto_rois(const CapturedFrame& frame) {
  if (frame.width <= 0 || frame.height <= 0) return false;

  // If we already have the target number of TVs committed, use the longer
  // fallback interval instead of detect_interval_ms — prevents OpenCV from
  // overwriting a good YOLOX 2-TV commit 2 seconds later with a 1-TV result.
  const int desired = std::max(1, cfg_.auto_cfg.max_tvs);
  const uint64_t redetect_interval =
      ((int)tvs_.size() >= desired && cfg_.auto_cfg.detect_fallback_ms > 0)
        ? (uint64_t)cfg_.auto_cfg.detect_fallback_ms
        : (uint64_t)std::max(250, cfg_.auto_cfg.detect_interval_ms);

  if (!tvs_.empty() && (frame.ts_ms - last_auto_detect_ts_ms_) < redetect_interval) {
    return false;
  }
  // Track how long we've been trying to detect
  if (first_auto_frame_ts_ms_ == 0) first_auto_frame_ts_ms_ = frame.ts_ms;
  last_auto_detect_ts_ms_ = frame.ts_ms;

  std::vector<Candidate> candidates;
  bool used_yolo = false;

  if (yolo_detector_ && yolo_detector_->is_loaded()) {
    used_yolo = true;
    // ── YOLO NPU path ──────────────────────────────────────────────────────────
    // Semantically correct: YOLOX was trained on COCO "tv" (class 63) which
    // covers flat-panel monitors and televisions.  Robust to fisheye distortion
    // and room geometry false-positives that trip up the OpenCV heuristic.
    auto rois = yolo_detector_->detect(frame);
    const float yolo_fa = static_cast<float>(frame.width * frame.height);
    int rejected_area = 0, rejected_aspect = 0;
    for (size_t ri = 0; ri < rois.size(); ++ri) {
      const auto& r = rois[ri];
      float area_pct       = static_cast<float>(r.w * r.h) / yolo_fa;
      float aspect         = static_cast<float>(r.w) / std::max(1, r.h);

      // Apply the same area + aspect filters as the OpenCV path.
      if (area_pct < cfg_.auto_cfg.min_area_pct || area_pct > cfg_.auto_cfg.max_area_pct) {
        ++rejected_area;
        continue;
      }
      if (aspect < cfg_.auto_cfg.aspect_min || aspect > cfg_.auto_cfg.aspect_max) {
        ++rejected_aspect;
        continue;
      }

      // Interior brightness checks — filter impossible candidates before scoring.
      // max_interior_luma: rejects bright walls/ceilings (OpenCV path uses this too).
      // min_interior_luma: rejects dark/off computer monitors that YOLOX also
      //   classifies as "tv" but that should not be monitored.
      if (cfg_.auto_cfg.max_interior_luma > 0.0f || cfg_.auto_cfg.min_interior_luma > 0.0f) {
        cv::Mat bgr_full = frame_to_bgr(frame);
        if (!bgr_full.empty()) {
          int sx = std::max(0, r.x);
          int sy = std::max(0, r.y);
          int sw = std::min(r.w, bgr_full.cols - sx);
          int sh = std::min(r.h, bgr_full.rows - sy);
          if (sw > 0 && sh > 0) {
            cv::Scalar mean_val = cv::mean(bgr_full(cv::Rect(sx, sy, sw, sh)));
            float mean_luma = 0.299f * mean_val[2] + 0.587f * mean_val[1] + 0.114f * mean_val[0];
            if (cfg_.auto_cfg.max_interior_luma > 0.0f && mean_luma > cfg_.auto_cfg.max_interior_luma) {
              Logger::instance().log(LogLevel::DEBUG,
                "[ROI YOLO] Rejected: luma=%.1f > max=%.1f (wall/ceiling?)",
                mean_luma, cfg_.auto_cfg.max_interior_luma);
              ++rejected_area;
              continue;
            }
            if (cfg_.auto_cfg.min_interior_luma > 0.0f && mean_luma < cfg_.auto_cfg.min_interior_luma) {
              Logger::instance().log(LogLevel::DEBUG,
                "[ROI YOLO] Rejected: luma=%.1f < min=%.1f (screen off/dark monitor?)",
                mean_luma, cfg_.auto_cfg.min_interior_luma);
              ++rejected_area;
              continue;
            }
          }
        }
      }

      float aspect_penalty = std::fabs(aspect - 16.0f / 9.0f);
      float score          = (area_pct * 4.0f) - (aspect_penalty * 0.5f);

      // Wide-box splitter: if a single detected box is far too wide to be one
      // TV (e.g. three adjacent monitors merged into one detection), split it
      // at vertical luma valleys inside the box.  Threshold: aspect > 2.4.
      if (aspect > 2.4f) {
        cv::Mat bgr_split = frame_to_bgr(frame);
        bool split_done = false;
        if (!bgr_split.empty()) {
          int sx = std::max(0, r.x), sy = std::max(0, r.y);
          int sw = std::min(r.w, bgr_split.cols - sx);
          int sh = std::min(r.h, bgr_split.rows - sy);
          if (sw > 20 && sh > 10) {
            cv::Mat roi_bgr = bgr_split(cv::Rect(sx, sy, sw, sh));
            cv::Mat gray;
            cv::cvtColor(roi_bgr, gray, cv::COLOR_BGR2GRAY);
            // Column mean luma profile
            std::vector<float> col_luma(sw);
            for (int x = 0; x < sw; ++x) {
              double s = 0;
              for (int y = 0; y < sh; ++y) s += gray.at<uint8_t>(y, x);
              col_luma[x] = static_cast<float>(s / sh);
            }
            // Smooth profile (5-pixel boxcar)
            std::vector<float> smooth(sw, 0.f);
            for (int x = 2; x < sw - 2; ++x)
              smooth[x] = (col_luma[x-2]+col_luma[x-1]+col_luma[x]+col_luma[x+1]+col_luma[x+2]) / 5.f;
            // Find local minima that are darker than 60% of the mean
            float mean_luma_col = 0;
            for (float v : smooth) mean_luma_col += v;
            mean_luma_col /= sw;
            float valley_thresh = mean_luma_col * 0.60f;
            std::vector<int> valleys;
            int min_gap = sw / (cfg_.auto_cfg.max_tvs + 1);
            for (int x = 5; x < sw - 5; ++x) {
              if (smooth[x] < valley_thresh &&
                  smooth[x] <= smooth[x-1] && smooth[x] <= smooth[x+1]) {
                if (valleys.empty() || (x - valleys.back()) > min_gap)
                  valleys.push_back(x);
              }
            }
            if (!valleys.empty()) {
              Logger::instance().log(LogLevel::INFO,
                "[ROI YOLO] Wide box (aspect=%.2f) — splitting at %zu valley(s)",
                aspect, valleys.size());
              // Build split x-boundaries
              std::vector<int> splits = {0};
              for (int v : valleys) splits.push_back(v);
              splits.push_back(sw);
              for (size_t si = 0; si + 1 < splits.size(); ++si) {
                int sub_x = sx + splits[si];
                int sub_w = splits[si+1] - splits[si];
                if (sub_w < 20) continue;
                float sub_area = static_cast<float>(sub_w * sh) / yolo_fa;
                float sub_asp  = static_cast<float>(sub_w) / std::max(1, sh);
                if (sub_area < cfg_.auto_cfg.min_area_pct) continue;
                if (sub_asp < cfg_.auto_cfg.aspect_min || sub_asp > cfg_.auto_cfg.aspect_max) continue;
                RoiRect sub; sub.x = sub_x; sub.y = sy; sub.w = sub_w; sub.h = sh;
                float sub_penalty = std::fabs(sub_asp - 16.0f / 9.0f);
                float sub_score   = (sub_area * 4.0f) - (sub_penalty * 0.5f);
                candidates.push_back({sub, sub_score});
                Logger::instance().log(LogLevel::INFO,
                  "[ROI YOLO]   split sub-ROI: x=%d y=%d w=%d h=%d asp=%.2f score=%.3f",
                  sub_x, sy, sub_w, sh, sub_asp, sub_score);
              }
              split_done = true;
            }
          }
        }
        if (split_done) continue;  // don't push the merged box
      }

      candidates.push_back({r, score});
      Logger::instance().log(LogLevel::INFO,
        "[ROI YOLO]   accepted: box=[x=%d y=%d w=%d h=%d] area_pct=%.3f aspect=%.2f score=%.3f",
        r.x, r.y, r.w, r.h, area_pct, aspect, score);
    }
    Logger::instance().log(LogLevel::INFO,
      "[ROI YOLO] Filter: %zu from YOLOX -> %zu accepted (%d rejected area, %d rejected aspect) (frame %dx%d)",
      rois.size(), candidates.size(), rejected_area, rejected_aspect, frame.width, frame.height);
    if (candidates.size() == 2) {
      Logger::instance().log(LogLevel::INFO, "[ROI YOLO] *** YOLOX supplied 2 TV candidates — both will be committed as ROIs ***");
    }
  } else {
  // ── OpenCV fallback path ───────────────────────────────────────────────────
  cv::Mat bgr = frame_to_bgr(frame);
  if (bgr.empty()) {
    Logger::instance().log(LogLevel::WARN, "ROI auto: failed to convert frame to BGR for detection");
    Logger::instance().log(LogLevel::ERROR, "[ROI AUTO] frame_to_bgr failed (fmt=%d w=%d h=%d sz=%zu)",
            (int)frame.fmt, frame.width, frame.height, frame.data.size());
    return false;
  }

  cv::Mat small;
  cv::resize(bgr, small, cv::Size(cfg_.auto_cfg.downscale_w, cfg_.auto_cfg.downscale_h), 0, 0, cv::INTER_AREA);

  // Add a thin constant border so TVs whose edges reach the camera frame boundary
  // form closed contours and are detected by findContours.
  const int border = 3;
  cv::Mat bordered;
  cv::copyMakeBorder(small, bordered, border, border, border, border,
                     cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));

  cv::Mat gray;
  cv::cvtColor(bordered, gray, cv::COLOR_BGR2GRAY);
  cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
  clahe->apply(gray, gray);
  cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0.0);

  cv::Mat edges;
  cv::Canny(gray, edges, 50, 150);
  cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5));
  cv::morphologyEx(edges, edges, cv::MORPH_CLOSE, kernel, cv::Point(-1, -1), 2);

  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(edges, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);

  const float frame_area = static_cast<float>(small.cols * small.rows);

  for (const auto& contour : contours) {
    double peri = cv::arcLength(contour, true);
    if (peri < 50.0) continue;

    std::vector<cv::Point> poly;
    cv::approxPolyDP(contour, poly, 0.02 * peri, true);
    // Accept 4–6 sided polygons: fisheye lenses curve straight edges so screens
    // approx as pentagons/hexagons rather than strict quads.
    if (poly.size() < 4 || poly.size() > 6) continue;

    cv::Rect r = cv::boundingRect(poly);
    float area_pct = static_cast<float>(r.area()) / frame_area;
    if (area_pct < cfg_.auto_cfg.min_area_pct || area_pct > cfg_.auto_cfg.max_area_pct) continue;

    float aspect = static_cast<float>(r.width) / std::max(1, r.height);
    if (aspect < cfg_.auto_cfg.aspect_min || aspect > cfg_.auto_cfg.aspect_max) continue;

    float rectness = rectangularity(contour, r);
    if (rectness < cfg_.auto_cfg.rectangularity_min) continue;

    float aspect_target = 16.0f / 9.0f;
    float aspect_penalty = std::fabs(aspect - aspect_target);
    float score = (area_pct * 4.0f) + (rectness * 2.0f) - (aspect_penalty * 0.5f);

    RoiRect rr;
    // Interior brightness check: reject bright regions (walls, ceilings, other room geometry).
    // TV screens are dark relative to room surfaces in wide-angle/fisheye views.
    if (cfg_.auto_cfg.max_interior_luma > 0.0f) {
      int sx = std::max(0, r.x - border);
      int sy = std::max(0, r.y - border);
      int sw = std::min(r.width,  small.cols - sx);
      int sh = std::min(r.height, small.rows - sy);
      if (sw > 0 && sh > 0) {
        cv::Scalar mean_val = cv::mean(small(cv::Rect(sx, sy, sw, sh)));
        float mean_luma = 0.299f * mean_val[2] + 0.587f * mean_val[1] + 0.114f * mean_val[0];
        if (mean_luma > cfg_.auto_cfg.max_interior_luma) {
          Logger::instance().log(LogLevel::DEBUG, "[ROI AUTO] Rejected bright candidate luma=%.1f > %.1f (wall/ceiling?)",
                  mean_luma, cfg_.auto_cfg.max_interior_luma);
          continue;
        }
      }
    }

    // r is in bordered space; subtract border offset before scaling to frame coords
    rr.x = static_cast<int>(std::round((double)(r.x - border) * frame.width / small.cols));
    rr.y = static_cast<int>(std::round((double)(r.y - border) * frame.height / small.rows));
    rr.w = static_cast<int>(std::round((double)r.width * frame.width / small.cols));
    rr.h = static_cast<int>(std::round((double)r.height * frame.height / small.rows));
    rr.x = std::max(0, rr.x);
    rr.y = std::max(0, rr.y);
    rr.w = std::min(frame.width - rr.x, rr.w);
    rr.h = std::min(frame.height - rr.y, rr.h);

    candidates.push_back({rr, score});
  }

  Logger::instance().log(LogLevel::INFO, "[ROI AUTO] Detection: %zu contours -> %zu candidates (frame %dx%d)",
          contours.size(), candidates.size(), frame.width, frame.height);
  } // end detection branch (OpenCV fallback)

  if (candidates.empty()) {
    Logger::instance().log(LogLevel::INFO, "ROI auto: no screen candidates found; keeping existing ROIs (%zu)", tvs_.size());

    // Fallback: if we've been trying for detect_fallback_ms with no result, use full frame as tv1
    if (cfg_.auto_cfg.detect_fallback_ms > 0 && tvs_.empty() &&
        (frame.ts_ms - first_auto_frame_ts_ms_) >= (uint64_t)cfg_.auto_cfg.detect_fallback_ms) {
      Logger::instance().log(LogLevel::WARN,
        "ROI auto: no screen detected after %dms — falling back to full-frame as tv1. "
        "Check that the TV is fully within the camera frame or switch to roi.mode=\"rect\".",
        cfg_.auto_cfg.detect_fallback_ms);
      Logger::instance().log(LogLevel::WARN, "[ROI AUTO] no screen detected after %dms — using full frame as tv1 fallback",
              cfg_.auto_cfg.detect_fallback_ms);
      RoiRect fb;
      fb.id = "tv1";
      fb.x = 0; fb.y = 0;
      fb.w = frame.width; fb.h = frame.height;
      tvs_ = {fb};
      frame_w_ = frame.width;
      frame_h_ = frame.height;
      // Don't save the fallback — we want to keep trying on next startup
      return true;
    }
    return false;
  }

  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    return a.score > b.score;
  });

  std::vector<RoiRect> selected;
  for (const auto& c : candidates) {
    bool overlaps = false;
    for (const auto& s : selected) {
      if (compute_iou(c.roi, s) > 0.35f) {
        overlaps = true;
        break;
      }
    }
    if (!overlaps) selected.push_back(c.roi);
    if ((int)selected.size() >= std::max(1, cfg_.auto_cfg.max_tvs)) break;
  }

  std::sort(selected.begin(), selected.end(), [](const RoiRect& a, const RoiRect& b) {
    int acy = a.y + a.h / 2;
    int bcy = b.y + b.h / 2;
    if (std::abs(acy - bcy) > std::min(a.h, b.h) / 2) return acy < bcy;
    return a.x < b.x;
  });

  // Never downgrade below the committed count when OpenCV ran — it can
  // temporarily miss dark/off screens that YOLOX found before.
  // When YOLOX is the detector, let it freely update ROIs so it can
  // self-correct any bad initial commits (wall/door false positives).
  if (!used_yolo &&
      (int)selected.size() < (int)tvs_.size() && (int)tvs_.size() >= desired) {
    Logger::instance().log(LogLevel::INFO,
      "ROI auto: detected %zu TV(s) < committed %zu (max_tvs=%d) — keeping existing ROIs",
      selected.size(), tvs_.size(), desired);
    return false;
  }

  // Preserve existing ids when overlap is strong, otherwise assign fresh row-major ids.
  // Track all assigned ids to prevent duplicates across both IoU-match and fallback paths.
  std::vector<RoiRect> labeled = selected;
  std::vector<bool> old_used(tvs_.size(), false);
  std::set<std::string> assigned_ids;
  for (size_t i = 0; i < labeled.size(); ++i) {
    float best_iou = 0.0f;
    int best_j = -1;
    for (size_t j = 0; j < tvs_.size(); ++j) {
      if (old_used[j]) continue;
      float iou = compute_iou(labeled[i], tvs_[j]);
      if (iou > best_iou) {
        best_iou = iou;
        best_j = (int)j;
      }
    }
    if (best_j >= 0 && best_iou >= cfg_.auto_cfg.commit_iou_min &&
        assigned_ids.find(tvs_[best_j].id) == assigned_ids.end()) {
      labeled[i].id = tvs_[best_j].id;
      assigned_ids.insert(labeled[i].id);
      old_used[best_j] = true;
    } else {
      // Assign next available row-major id (skip any already taken by IoU-match).
      int idx = (int)i + 1;
      std::string cand;
      do { cand = "tv" + std::to_string(idx++); } while (assigned_ids.count(cand));
      labeled[i].id = cand;
      assigned_ids.insert(labeled[i].id);
    }
  }

  // Stabilize detections across multiple intervals.
  bool same_as_pending = pending_auto_rois_.size() == labeled.size();
  if (same_as_pending) {
    for (size_t i = 0; i < labeled.size(); ++i) {
      if (compute_iou(labeled[i], pending_auto_rois_[i]) < cfg_.auto_cfg.commit_iou_min) {
        same_as_pending = false;
        break;
      }
    }
  }

  if (same_as_pending) {
    pending_auto_count_++;
  } else {
    pending_auto_rois_ = labeled;
    pending_auto_count_ = 1;
  }

  if (pending_auto_count_ < std::max(1, cfg_.auto_cfg.stable_frames)) {
    Logger::instance().log(LogLevel::INFO, "ROI auto: found %zu candidate screens, waiting for stability (%d/%d)",
                           labeled.size(), pending_auto_count_, std::max(1, cfg_.auto_cfg.stable_frames));
    return false;
  }

  tvs_ = pending_auto_rois_;
  frame_w_ = frame.width;
  frame_h_ = frame.height;
  save_auto_rois();

  // Do NOT release YOLOX here — it runs on NPU Core 0, MobileNet on Core 1,
  // so they are fully independent. Keeping YOLOX alive allows it to re-run at
  // detect_fallback_ms intervals and self-correct any bad initial ROI commits
  // (e.g. a wall/door detectedas a TV on first frame).

  Logger::instance().log(LogLevel::INFO, "ROI auto: committed %zu TVs", tvs_.size());
  for (size_t i = 0; i < std::min<size_t>(tvs_.size(), 8); ++i) {
    const auto& r = tvs_[i];
    Logger::instance().log(LogLevel::INFO, "  ROI auto %s: x=%d y=%d w=%d h=%d", r.id.c_str(), r.x, r.y, r.w, r.h);
  }
  return true;
}

bool RoiManager::update_from_frame(const CapturedFrame& frame) {
  if (cfg_.mode == "grid") return update_grid_rois(frame.width, frame.height);
  if (cfg_.mode == "auto") return update_auto_rois(frame);
  return false;
}

} // namespace wvm
