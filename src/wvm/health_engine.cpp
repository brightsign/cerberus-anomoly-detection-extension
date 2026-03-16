#include "wvm/health_engine.hpp"
#include "wvm/config.hpp"
#include "wvm/logger.hpp"
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>

namespace wvm {

const char* health_state_to_string(HealthState state) {
  switch (state) {
    case HealthState::OK: return "OK";
    case HealthState::TV_OFF: return "TV_OFF";
    case HealthState::BLACK: return "BLACK";
    case HealthState::NO_SIGNAL: return "NO_SIGNAL";
    case HealthState::WRONG_INPUT: return "WRONG_INPUT";
    case HealthState::UNKNOWN: return "UNKNOWN";
    default: return "UNKNOWN";
  }
}

HealthEngine::HealthEngine(const HealthConfig& cfg) : cfg_(cfg) {
  Logger::instance().log(LogLevel::INFO, 
    "HealthEngine created: analysis_fps=%d, black_enter_ratio=%.2f, off_mean=%.1f",
    cfg_.analysis_fps, cfg_.black_enter_ratio, cfg_.off_mean);
}

bool HealthEngine::load_osd_prototypes(const std::string& path) {
  if (cfg_.osd_mode == "disabled") {
    Logger::instance().log(LogLevel::INFO, "OSD detection disabled");
    return true;
  }

  try {
    std::ifstream f(path);
    if (!f.is_open()) {
      Logger::instance().log(LogLevel::WARN, "OSD prototypes file not found: %s (continuing without OSD detection)", path.c_str());
      return false;
    }

    nlohmann::json j;
    f >> j;

    osd_prototypes_.clear();
    for (const auto& item : j["prototypes"]) {
      OsdPrototype proto;
      proto.label = item["label"].get<std::string>();
      proto.sample_count = item.value("sample_count", 1);
      
      auto& emb_array = item["embedding"];
      proto.embedding.reserve(emb_array.size());
      for (const auto& val : emb_array) {
        proto.embedding.push_back(val.get<float>());
      }

      osd_prototypes_.push_back(std::move(proto));
      Logger::instance().log(LogLevel::INFO, "Loaded OSD prototype: %s (dim=%zu, samples=%d)",
        proto.label.c_str(), proto.embedding.size(), proto.sample_count);
    }

    Logger::instance().log(LogLevel::INFO, "Loaded %zu OSD prototypes from %s", 
      osd_prototypes_.size(), path.c_str());
    return true;

  } catch (const std::exception& e) {
    Logger::instance().log(LogLevel::ERROR, "Failed to load OSD prototypes: %s", e.what());
    return false;
  }
}

float HealthEngine::calculate_dark_ratio(const uint8_t* rgb, int width, int height) const {
  if (!rgb || width <= 0 || height <= 0) return 0.0f;

  int dark_count = 0;
  int total_pixels = width * height;
  int threshold = cfg_.dark_luma_threshold;

  for (int i = 0; i < total_pixels; ++i) {
    int idx = i * 3;
    int r = rgb[idx];
    int g = rgb[idx + 1];
    int b = rgb[idx + 2];
    
    // Simple luma approximation: Y = 0.299*R + 0.587*G + 0.114*B
    int luma = (299 * r + 587 * g + 114 * b) / 1000;
    
    if (luma < threshold) {
      dark_count++;
    }
  }

  return (float)dark_count / total_pixels;
}

float HealthEngine::calculate_sat_mean(const uint8_t* rgb, int width, int height) const {
  if (!rgb || width <= 0 || height <= 0) return 0.0f;
  // Use brightness-weighted saturation: S_weighted = S * V
  // For near-black pixels (V≈0), raw HSV S is undefined/noisy — a 1-unit colour
  // difference on a pixel at luma=8 gives S≈0.6, producing spuriously high sat_mean
  // on a powered-off panel.  Multiplying by V suppresses these dark-pixel artefacts
  // while preserving the contribution of genuinely saturated bright pixels.
  double sum_s = 0.0;
  const int n = width * height;
  for (int i = 0; i < n; ++i) {
    float r = rgb[i*3 + 0] / 255.0f;
    float g = rgb[i*3 + 1] / 255.0f;
    float b = rgb[i*3 + 2] / 255.0f;
    float maxv = std::max(r, std::max(g, b));
    float minv = std::min(r, std::min(g, b));
    float s = (maxv <= 1e-6f) ? 0.0f : ((maxv - minv) / maxv);
    // Weight by brightness so dark pixels (V≈0) don't inflate the mean
    sum_s += s * maxv * 255.0f;
  }
  return static_cast<float>(sum_s / n);
}

float HealthEngine::calculate_laplacian_var(const uint8_t* rgb, int width, int height) const {
  if (!rgb || width < 3 || height < 3) return 0.0f;
  std::vector<float> gray(width * height);
  for (int i = 0; i < width * height; ++i) {
    gray[i] = (299.0f * rgb[i*3] + 587.0f * rgb[i*3+1] + 114.0f * rgb[i*3+2]) / 1000.0f;
  }
  double sum = 0.0, sum2 = 0.0;
  int count = 0;
  for (int y = 1; y < height - 1; ++y) {
    for (int x = 1; x < width - 1; ++x) {
      int idx = y * width + x;
      float lap = gray[idx - width] + gray[idx + width]
                + gray[idx - 1]     + gray[idx + 1]
                - 4.0f * gray[idx];
      sum  += lap;
      sum2 += lap * lap;
      ++count;
    }
  }
  if (count == 0) return 0.0f;
  double mean = sum / count;
  return static_cast<float>(sum2 / count - mean * mean);
}

float HealthEngine::calculate_temporal_diff(TvHealthState& tv, const uint8_t* rgb, int width, int height) const {
  if (!rgb || width <= 0 || height <= 0) return 0.0f;
  std::vector<uint8_t> gray(width * height);
  for (int i = 0; i < width * height; ++i) {
    gray[i] = static_cast<uint8_t>(
      (299 * rgb[i*3] + 587 * rgb[i*3+1] + 114 * rgb[i*3+2]) / 1000);
  }
  float diff = 0.0f;
  if (!tv.prev_gray.empty() && tv.prev_w == width && tv.prev_h == height) {
    double sum = 0.0;
    for (int i = 0; i < width * height; ++i)
      sum += std::abs(int(gray[i]) - int(tv.prev_gray[i]));
    diff = static_cast<float>(sum / (width * height));
  }
  tv.prev_gray = std::move(gray);
  tv.prev_w = width;
  tv.prev_h = height;
  return diff;
}

void HealthEngine::classify_osd(const float* embedding, int dim,
                                 float& best_sim, std::string& best_label) const {
  best_sim = 0.0f;
  best_label = "";

  if (osd_prototypes_.empty() || !embedding) {
    return;
  }

  for (const auto& proto : osd_prototypes_) {
    if ((int)proto.embedding.size() != dim) {
      Logger::instance().log(LogLevel::WARN, 
        "Prototype %s dimension mismatch: expected %d, got %zu",
        proto.label.c_str(), dim, proto.embedding.size());
      continue;
    }

    // Cosine similarity
    float dot = 0.0f;
    float norm_a = 0.0f;
    float norm_b = 0.0f;

    for (int i = 0; i < dim; ++i) {
      dot += embedding[i] * proto.embedding[i];
      norm_a += embedding[i] * embedding[i];
      norm_b += proto.embedding[i] * proto.embedding[i];
    }

    float sim = 0.0f;
    if (norm_a > 0 && norm_b > 0) {
      sim = dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
    }

    if (sim > best_sim) {
      best_sim = sim;
      best_label = proto.label;
    }
  }
}

HealthState HealthEngine::determine_state(
  float luma_mean, float luma_var, float dark_ratio,
  float sat_mean, float laplacian_var, float temporal_diff,
  float osd_sim, const std::string& osd_label) const {

  // Priority 1: TRUE BLACK — panel is near-zero luminance (OLED off / blanked)
  if (dark_ratio > 0.99f && luma_mean < cfg_.off_mean && luma_var < cfg_.off_var) {
    return HealthState::TV_OFF;
  }

  // Priority 2: BLACK — dark but not necessarily off (dark content, blanked display)
  if (dark_ratio > cfg_.black_enter_ratio && luma_var < cfg_.black_var_enter) {
    return HealthState::BLACK;
  }

  // Priority 3: OSD states — only if screen is clearly illuminated
  if (cfg_.osd_mode != "disabled" && !osd_label.empty()) {
    if (osd_label == "NO_SIGNAL" && osd_sim >= cfg_.osd_sim_min_no_signal) {
      return HealthState::NO_SIGNAL;
    }
    if ((osd_label == "WRONG_INPUT" || osd_label == "HDMI_MENU" ||
         osd_label == "INPUT_MENU") && osd_sim >= cfg_.osd_sim_min_wrong_input) {
      return HealthState::WRONG_INPUT;
    }
    if ((osd_label == "TV_OFF" || osd_label == "OFF" || osd_label == "POWER_OFF") &&
        osd_sim >= cfg_.tv_off_sim_min &&
        laplacian_var <= cfg_.off_laplacian_var_max * 10.0f) { // corroborate: active screen has lap>>threshold
      return HealthState::TV_OFF;
    }
    if (!osd_label.empty() && osd_sim >= cfg_.osd_sim_min &&
        osd_label != "NO_SIGNAL" && osd_label != "WRONG_INPUT" &&
        osd_label != "HDMI_MENU" && osd_label != "INPUT_MENU" &&
        osd_label != "TV_OFF" && osd_label != "OFF" && osd_label != "POWER_OFF") {
      return HealthState::UNKNOWN;
    }
  }

  // Priority 4: TV_OFF — inactive-screen detector for reflective powered-off panels.
  // These panels show ambient room reflection: dark-ish, grey/unsaturated, smooth,
  // and temporally static. All four conditions together avoid false positives from
  // dark video content (which has higher temporal diff and/or saturation).
  if (dark_ratio    >= cfg_.off_dark_ratio_min &&
      sat_mean      <= cfg_.off_sat_mean_max &&
      laplacian_var <= cfg_.off_laplacian_var_max &&
      temporal_diff <= cfg_.off_temporal_diff_max) {
    return HealthState::TV_OFF;
  }

  return HealthState::OK;
}

int HealthEngine::get_persistence_ms(HealthState from, HealthState to) const {
  // Transitioning TO an anomaly state
  if (to == HealthState::TV_OFF) {
    return cfg_.persist_off_ms;
  }
  if (to == HealthState::BLACK) {
    return cfg_.persist_black_ms;
  }
  if (to == HealthState::NO_SIGNAL || to == HealthState::WRONG_INPUT || to == HealthState::UNKNOWN) {
    if (to == HealthState::NO_SIGNAL) {
      return cfg_.persist_no_signal_ms;  // Longer: real TV-off lasts minutes, noise lasts 2-3s
    }
    return cfg_.persist_osd_ms;
  }

  // TV_OFF → OK: off_active hysteresis already required persist_off_recover_ms of
  // sustained on-screen signal; no additional hold-off needed here.
  if (from == HealthState::TV_OFF && to == HealthState::OK) {
    return 0;
  }

  // Transitioning FROM anomaly back to OK (recovery)
  if (from != HealthState::OK && to == HealthState::OK) {
    return cfg_.persist_recover_ms;
  }

  return 0; // No persistence needed
}

void HealthEngine::write_prototypes() {
  // Build normalized mean embeddings and write to JSON
  nlohmann::json j;
  j["prototypes"] = nlohmann::json::array();

  for (auto& [tv_id, acc] : proto_accum_) {
    if (acc.count == 0) continue;

    // Compute mean and L2-normalize
    int dim = (int)acc.sum.size();
    std::vector<float> mean(dim);
    double norm_sq = 0.0;
    for (int i = 0; i < dim; ++i) {
      mean[i] = (float)(acc.sum[i] / acc.count);
      norm_sq += (double)mean[i] * mean[i];
    }
    double inv_norm = (norm_sq > 1e-12) ? (1.0 / std::sqrt(norm_sq)) : 1.0;
    for (int i = 0; i < dim; ++i)
      mean[i] = (float)(mean[i] * inv_norm);

    nlohmann::json proto;
    proto["label"] = acc.label;
    proto["sample_count"] = acc.count;
    proto["tv_id"] = tv_id;
    proto["embedding"] = mean;
    j["prototypes"].push_back(proto);

    Logger::instance().log(LogLevel::INFO,
      "[ProtoCapture] Wrote prototype: label=%s tv=%s samples=%d",
      acc.label.c_str(), tv_id.c_str(), acc.count);
  }

  std::ofstream ofs(cfg_.prototype_output_path);
  if (ofs) {
    ofs << j.dump(2);
    ofs.close();
    Logger::instance().log(LogLevel::INFO,
      "[ProtoCapture] ✅ Prototypes written to %s (%zu entries)",
      cfg_.prototype_output_path.c_str(), proto_accum_.size());

    // Immediately load them so OSD detection activates without restart
    osd_prototypes_.clear();
    for (auto& item : j["prototypes"]) {
      OsdPrototype p;
      p.label = item["label"].get<std::string>();
      p.sample_count = item.value("sample_count", 1);
      for (auto& v : item["embedding"])
        p.embedding.push_back(v.get<float>());
      osd_prototypes_.push_back(std::move(p));
    }
    Logger::instance().log(LogLevel::INFO,
      "[ProtoCapture] OSD prototypes hot-loaded (%zu entries)", osd_prototypes_.size());
  } else {
    Logger::instance().log(LogLevel::ERROR,
      "[ProtoCapture] ❌ Failed to write prototypes to %s", cfg_.prototype_output_path.c_str());
  }
}

bool HealthEngine::should_transition(const TvHealthState& tv, HealthState pending, uint64_t ts_ms) const {
  if (tv.pending_state != pending) {
    return false; // Pending state changed, reset timer
  }

  int required_ms = get_persistence_ms(tv.current_state, pending);
  if (required_ms <= 0) {
    return true; // No persistence required
  }

  uint64_t elapsed = ts_ms - tv.pending_enter_ts_ms;
  return elapsed >= (uint64_t)required_ms;
}

std::vector<HealthEngine::HealthEvent> HealthEngine::update(
  uint64_t ts_ms,
  const std::string& tv_id,
  float luma_mean,
  float luma_var,
  const uint8_t* rgb_data,
  int width,
  int height,
  const float* embedding,
  int embedding_dim) {
  
  std::vector<HealthEvent> events;

  // Get or create TV state
  auto& tv = tv_states_[tv_id];
  if (tv.tv_id.empty()) {
    tv.tv_id = tv_id;
    tv.current_state = HealthState::OK;
    tv.pending_state = HealthState::OK;
    tv.state_enter_ts_ms = ts_ms;
    tv.pending_enter_ts_ms = ts_ms;
    tv.last_heartbeat_ts_ms = ts_ms;
  }

  // Update measurements
  tv.luma_mean = luma_mean;
  tv.luma_var = luma_var;
  tv.dark_ratio = calculate_dark_ratio(rgb_data, width, height);
  tv.sat_mean = calculate_sat_mean(rgb_data, width, height);
  tv.laplacian_var = calculate_laplacian_var(rgb_data, width, height);
  tv.temporal_diff = calculate_temporal_diff(tv, rgb_data, width, height);

  // EMA smoothing (alpha=0.2 — ~5-frame memory, reduces jitter near thresholds)
  constexpr float EMA_A = 0.2f;
  auto ema_init = [&](float& e, float v) { if (e < 0.0f) e = v; else e = EMA_A*v + (1.0f-EMA_A)*e; };
  ema_init(tv.dark_ratio_ema,    tv.dark_ratio);
  ema_init(tv.sat_mean_ema,      tv.sat_mean);
  ema_init(tv.laplacian_var_ema, tv.laplacian_var);
  ema_init(tv.temporal_diff_ema, tv.temporal_diff);

  // Classify OSD / TV_OFF prototypes if enabled. We keep the raw best similarity
  // for debugging, but suppress low-confidence labels so MQTT/logs do not show
  // misleading labels such as INPUT_MENU@0.40 for a normal playing screen.
  float best_sim = 0.0f;
  std::string best_label;
  if (cfg_.osd_mode != "disabled" && embedding && embedding_dim > 0) {
    classify_osd(embedding, embedding_dim, best_sim, best_label);
  }
  tv.osd_similarity = best_sim;
  tv.osd_label.clear();
  if (!best_label.empty()) {
    const bool is_tv_off = (best_label == "TV_OFF" || best_label == "OFF" || best_label == "POWER_OFF");
    const bool is_no_signal = (best_label == "NO_SIGNAL");
    const bool is_wrong_input = (best_label == "WRONG_INPUT" || best_label == "HDMI_MENU" || best_label == "INPUT_MENU");

    if ((is_tv_off && best_sim >= cfg_.tv_off_sim_min) ||
        (is_no_signal && best_sim >= cfg_.osd_sim_min_no_signal) ||
        (is_wrong_input && best_sim >= cfg_.osd_sim_min_wrong_input) ||
        (!is_tv_off && !is_no_signal && !is_wrong_input && best_sim >= cfg_.osd_sim_min)) {
      tv.osd_label = best_label;
    }
  }

  // --- Prototype capture mode ---
  if (cfg_.prototype_capture && !proto_capture_done_ && embedding && embedding_dim > 0) {
    auto it = cfg_.prototype_labels.find(tv_id);
    if (it != cfg_.prototype_labels.end()) {
      if (proto_capture_start_ms_ == 0) {
        proto_capture_start_ms_ = ts_ms;
        Logger::instance().log(LogLevel::INFO, "[ProtoCapture] Started capturing prototypes for %zu TVs",
          cfg_.prototype_labels.size());
      }

      auto& acc = proto_accum_[tv_id];
      if (acc.label.empty()) {
        acc.label = it->second;
        acc.sum.assign(embedding_dim, 0.0);
        acc.count = 0;
      }
      for (int i = 0; i < embedding_dim; ++i)
        acc.sum[i] += embedding[i];
      acc.count++;

      uint64_t elapsed_ms = ts_ms - proto_capture_start_ms_;
      uint64_t target_ms  = (uint64_t)(cfg_.prototype_capture_seconds * 1000);

      if (elapsed_ms >= target_ms && !proto_accum_.empty()) {
        // Check all expected TVs have been captured
        bool all_ready = true;
        for (auto& [tid, lbl] : cfg_.prototype_labels) {
          if (proto_accum_.find(tid) == proto_accum_.end() || proto_accum_[tid].count == 0)
            all_ready = false;
        }
        if (all_ready) {
          proto_capture_done_ = true;
          write_prototypes();
        }
      }
    }
  }

  // TV_OFF hysteresis state machine (runs before determine_state so it intercepts OK/BLACK)
  {
    const float dr  = tv.dark_ratio_ema;
    const float sat = tv.sat_mean_ema;
    const float lap = tv.laplacian_var_ema;
    const float td  = tv.temporal_diff_ema;

    bool off_enter = (dr  >= cfg_.off_dark_ratio_min &&
                      sat <= cfg_.off_sat_mean_max &&
                      lap <= cfg_.off_laplacian_var_max &&
                      td  <= cfg_.off_temporal_diff_max);
    bool off_exit  = (dr  <  cfg_.off_dark_ratio_exit ||
                      sat >  cfg_.off_sat_mean_exit ||
                      lap >  cfg_.off_laplacian_var_exit ||
                      td  >  cfg_.off_temporal_diff_exit);

    if (!tv.off_active) {
      if (off_enter) {
        if (tv.off_enter_ts_ms == 0) tv.off_enter_ts_ms = ts_ms;
        if ((ts_ms - tv.off_enter_ts_ms) >= (uint64_t)cfg_.persist_off_ms) {
          tv.off_active = true;
          tv.off_recover_ts_ms = 0;
        }
      } else {
        tv.off_enter_ts_ms = 0;
      }
    } else {
      if (off_exit) {
        if (tv.off_recover_ts_ms == 0) tv.off_recover_ts_ms = ts_ms;
      } else {
        tv.off_recover_ts_ms = 0;  // still off — reset recovery clock
      }
      if (tv.off_recover_ts_ms != 0 &&
          (ts_ms - tv.off_recover_ts_ms) >= (uint64_t)cfg_.persist_off_recover_ms) {
        tv.off_active = false;
        tv.off_enter_ts_ms = 0;
        tv.off_recover_ts_ms = 0;
      }
    }
  }

  // Determine new state based on measurements.
  // Pass best_label (raw, unsuppressed) so prototype TV_OFF detection works even
  // when similarity is below the MQTT label-suppression threshold.
  HealthState new_state = determine_state(tv.luma_mean, tv.luma_var, tv.dark_ratio,
                                          tv.sat_mean, tv.laplacian_var, tv.temporal_diff,
                                          tv.osd_similarity, best_label);
  // Override with hysteresis TV_OFF if state machine says so
  if (tv.off_active) new_state = HealthState::TV_OFF;

  // Check if this is the first time we're seeing this TV
  bool is_first_update = (first_seen_.find(tv_id) == first_seen_.end());
  if (is_first_update) {
    first_seen_.insert(tv_id);
    
    // Emit initial state event
    HealthEvent evt;
    evt.ts_ms = ts_ms;
    evt.tv_id = tv_id;
    evt.old_state = HealthState::UNKNOWN;
    evt.new_state = new_state;
    evt.luma_mean = luma_mean;
    evt.luma_var = luma_var;
    evt.dark_ratio = tv.dark_ratio;
    evt.sat_mean = tv.sat_mean;
    evt.laplacian_var = tv.laplacian_var;
    evt.temporal_diff = tv.temporal_diff;
    evt.osd_similarity = tv.osd_similarity;
    evt.osd_label = tv.osd_label;
    events.push_back(evt);

    Logger::instance().log(LogLevel::INFO, 
      "[%s] Initial state: UNKNOWN → %s (luma=%.1f, var=%.1f, dark=%.2f sat=%.1f lap=%.0f tdiff=%.1f osd=%s@%.2f)",
      tv_id.c_str(),
      health_state_to_string(new_state),
      luma_mean, luma_var, tv.dark_ratio,
      tv.sat_mean, tv.laplacian_var, tv.temporal_diff,
      tv.osd_label.c_str(), tv.osd_similarity);

    // Update TV's current state to the detected state
    tv.current_state = new_state;
    tv.pending_state = new_state;
    tv.state_enter_ts_ms = ts_ms;
    tv.pending_enter_ts_ms = ts_ms;
    
    return events;  // Return immediately after initial event
  }

  // Update pending state
  if (new_state != tv.pending_state) {
    // Don't reset the NO_SIGNAL pending timer when a frame returns OK only because
    // an OSD-anomaly label (NO_SIGNAL or INPUT_MENU) scored below its individual
    // threshold. Label flips between these two are normal in wide-angle camera setups
    // where the scene embedding is noisy — the sustained signal is what matters.
    // Gate on a minimum similarity (0.38) so genuinely low-signal frames still reset.
    // Also gate on luma_var: if the tile is clearly active video (high variance) it
    // cannot be in a TV-off/OSD state — never preserve pending in that case.
    // Threshold: black_var_enter * 20 (default 300*20=6000). Covers physical-camera
    // TV-off with room background (~4500 var) but not active video (~10000+ var).
    auto is_osd_anomaly_label = [](const std::string& lbl) {
      return lbl == "NO_SIGNAL" || lbl == "INPUT_MENU" ||
             lbl == "WRONG_INPUT" || lbl == "HDMI_MENU";
    };
    bool preserve_osd_pending =
      (tv.pending_state == HealthState::NO_SIGNAL ||
       tv.pending_state == HealthState::WRONG_INPUT) &&
      new_state == HealthState::OK &&
      tv.osd_similarity >= 0.38f &&
      luma_var < cfg_.black_var_enter * 20.0f &&
      is_osd_anomaly_label(tv.osd_label);

    if (!preserve_osd_pending) {
      tv.pending_state = new_state;
      tv.pending_enter_ts_ms = ts_ms;
    }
  }

  // Check if we should transition
  if (should_transition(tv, new_state, ts_ms)) {
    if (new_state != tv.current_state) {
      // State transition!
      HealthEvent evt;
      evt.ts_ms = ts_ms;
      evt.tv_id = tv_id;
      evt.old_state = tv.current_state;
      evt.new_state = new_state;
      evt.luma_mean = luma_mean;
      evt.luma_var = luma_var;
      evt.dark_ratio = tv.dark_ratio;
      evt.sat_mean = tv.sat_mean;
      evt.laplacian_var = tv.laplacian_var;
      evt.temporal_diff = tv.temporal_diff;
      evt.osd_similarity = tv.osd_similarity;
      evt.osd_label = tv.osd_label;
      events.push_back(evt);

      Logger::instance().log(LogLevel::INFO, 
        "[%s] State: %s → %s (luma=%.1f, var=%.1f, dark=%.2f sat=%.1f lap=%.0f tdiff=%.1f osd=%s@%.2f)",
        tv_id.c_str(),
        health_state_to_string(tv.current_state),
        health_state_to_string(new_state),
        luma_mean, luma_var, tv.dark_ratio,
        tv.sat_mean, tv.laplacian_var, tv.temporal_diff,
        tv.osd_label.c_str(), tv.osd_similarity);

      tv.current_state = new_state;
      tv.state_enter_ts_ms = ts_ms;
      tv.last_heartbeat_ts_ms = ts_ms;  // Reset heartbeat timer on real state change
    }
  }

  // Periodic heartbeat: re-publish current state so subscribers know we're alive
  if (cfg_.heartbeat_interval_ms > 0 && events.empty()) {
    if ((ts_ms - tv.last_heartbeat_ts_ms) >= (uint64_t)cfg_.heartbeat_interval_ms) {
      HealthEvent hb;
      hb.ts_ms = ts_ms;
      hb.tv_id = tv_id;
      hb.old_state = tv.current_state;  // same state (no change)
      hb.new_state = tv.current_state;
      hb.luma_mean = luma_mean;
      hb.luma_var = luma_var;
      hb.dark_ratio = tv.dark_ratio;
      hb.sat_mean = tv.sat_mean;
      hb.laplacian_var = tv.laplacian_var;
      hb.temporal_diff = tv.temporal_diff;
      hb.osd_similarity = tv.osd_similarity;
      hb.osd_label = tv.osd_label;
      hb.is_heartbeat = true;
      events.push_back(hb);
      tv.last_heartbeat_ts_ms = ts_ms;

      Logger::instance().log(LogLevel::INFO,
        "[%s] Heartbeat: state=%s (luma=%.1f, var=%.1f)",
        tv_id.c_str(), health_state_to_string(tv.current_state), luma_mean, luma_var);
    }
  }

  return events;
}

const TvHealthState* HealthEngine::get_state(const std::string& tv_id) const {
  auto it = tv_states_.find(tv_id);
  if (it != tv_states_.end()) {
    return &it->second;
  }
  return nullptr;
}

} // namespace wvm
