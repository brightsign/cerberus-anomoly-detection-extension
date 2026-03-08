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
  float osd_sim, const std::string& osd_label) const {
  
  // Priority 1: TV_OFF (strictest black)
  if (dark_ratio > 0.99f && luma_mean < cfg_.off_mean && luma_var < cfg_.off_var) {
    return HealthState::TV_OFF;
  }

  // Priority 2: BLACK (dark but not necessarily off)
  if (dark_ratio > cfg_.black_enter_ratio && luma_var < cfg_.black_var_enter) {
    return HealthState::BLACK;
  }

  // Priority 3: OSD detection (only if not black/off)
  if (cfg_.osd_mode != "disabled" && osd_sim >= cfg_.osd_sim_min && !osd_label.empty()) {
    if (osd_label == "NO_SIGNAL") {
      return HealthState::NO_SIGNAL;
    } else if (osd_label == "WRONG_INPUT" || osd_label == "HDMI_MENU" || 
               osd_label == "INPUT_MENU") {
      return HealthState::WRONG_INPUT;
    } else {
      return HealthState::UNKNOWN;
    }
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
    return cfg_.persist_osd_ms;
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

  // Classify OSD if enabled
  if (cfg_.osd_mode != "disabled" && embedding && embedding_dim > 0) {
    classify_osd(embedding, embedding_dim, tv.osd_similarity, tv.osd_label);
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

  // Determine new state based on measurements
  HealthState new_state = determine_state(tv.luma_mean, tv.luma_var, tv.dark_ratio,
                                          tv.osd_similarity, tv.osd_label);

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
    evt.osd_similarity = tv.osd_similarity;
    evt.osd_label = tv.osd_label;
    events.push_back(evt);

    Logger::instance().log(LogLevel::INFO, 
      "[%s] Initial state: UNKNOWN → %s (luma=%.1f, var=%.1f, dark=%.2f, osd=%s@%.2f)",
      tv_id.c_str(),
      health_state_to_string(new_state),
      luma_mean, luma_var, tv.dark_ratio,
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
    tv.pending_state = new_state;
    tv.pending_enter_ts_ms = ts_ms;
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
      evt.osd_similarity = tv.osd_similarity;
      evt.osd_label = tv.osd_label;
      events.push_back(evt);

      Logger::instance().log(LogLevel::INFO, 
        "[%s] State: %s → %s (luma=%.1f, var=%.1f, dark=%.2f, osd=%s@%.2f)",
        tv_id.c_str(),
        health_state_to_string(tv.current_state),
        health_state_to_string(new_state),
        luma_mean, luma_var, tv.dark_ratio,
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
      hb.osd_similarity = tv.osd_similarity;
      hb.osd_label = tv.osd_label;
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
