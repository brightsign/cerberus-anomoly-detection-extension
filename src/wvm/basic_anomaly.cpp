#include "wvm/basic_anomaly.hpp"
#include "wvm/logger.hpp"
#include <cmath>
#include <sstream>
#include <algorithm>

namespace wvm {

BasicAnomalyEngine::BasicAnomalyEngine(const AnomalyConfig& cfg) : cfg_(cfg) {
  for (const auto& id : cfg_.freeze_ignore_tvs) {
    if (!id.empty()) freeze_ignore_.insert(id);
  }
  Logger::instance().log(LogLevel::INFO, 
    "BasicAnomalyEngine initialized: black_persist=%dms freeze_persist=%dms freeze_sim=%.4f peer=%s",
    cfg_.persist_black_ms, cfg_.persist_freeze_ms, cfg_.freeze_similarity,
    cfg_.peer_enabled ? "enabled" : "disabled");
  if (!freeze_ignore_.empty()) {
    std::string ids;
    for (auto it = freeze_ignore_.begin(); it != freeze_ignore_.end(); ++it) {
      if (!ids.empty()) ids += ",";
      ids += *it;
    }
    Logger::instance().log(LogLevel::INFO, "BasicAnomalyEngine: freeze_ignore_tvs=%s", ids.c_str());
  }
}

Event BasicAnomalyEngine::make_event(uint64_t ts, const std::string& tv_id, EventType t, const std::string& details) {
  Event e; 
  e.ts_ms = ts; 
  e.tv_id = tv_id; 
  e.type = t; 
  e.details = details; 
  return e;
}

void BasicAnomalyEngine::l2_normalize_inplace(std::vector<float>& v) {
  double s = 0.0;
  for (float x : v) s += (double)x * x;
  double inv = (s > 1e-12) ? (1.0 / std::sqrt(s)) : 1.0;
  for (float& x : v) x = (float)(x * inv);
}

float BasicAnomalyEngine::dot(const std::vector<float>& a, const std::vector<float>& b) {
  double s = 0.0;
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) s += (double)a[i] * (double)b[i];
  return (float)s;
}

float BasicAnomalyEngine::cosine_normed(const std::vector<float>& a_norm, const std::vector<float>& b_norm) {
  return dot(a_norm, b_norm); // both are unit vectors
}

std::vector<Event> BasicAnomalyEngine::update_batch(const EmbeddingBatch& batch) {
  std::vector<Event> evs;

  if (batch.embeddings.empty()) {
    return evs;
  }

  // Normalize all embeddings and compute consensus (if peer mode enabled)
  std::unordered_map<std::string, std::vector<float>> emb_norm;
  emb_norm.reserve(batch.embeddings.size());

  std::vector<float> consensus;
  if (cfg_.peer_enabled) {
    consensus.assign(batch.embeddings[0].vec.size(), 0.0f);
  }

  for (const auto& e : batch.embeddings) {
    std::vector<float> v = e.vec;
    l2_normalize_inplace(v);
    emb_norm[e.tv_id] = v;

    if (cfg_.peer_enabled && !consensus.empty()) {
      for (size_t i = 0; i < consensus.size(); ++i) {
        consensus[i] += v[i];
      }
    }
  }

  // Normalize consensus to unit vector
  if (cfg_.peer_enabled && !consensus.empty()) {
    l2_normalize_inplace(consensus);
  }

  // Per-TV anomaly detection
  for (const auto& e : batch.embeddings) {
    auto& s = st_[e.tv_id];
    s.last_ts = batch.ts_ms;

    // Periodic metrics logging for threshold tuning (every 2s per TV)
    const bool log_metrics = (batch.ts_ms - s.last_metrics_log) >= 2000;
    if (log_metrics) s.last_metrics_log = batch.ts_ms;

    // ========================================
    // BLACK SCREEN DETECTION (with hysteresis)
    // ========================================
    const bool black_enter = (e.luma_mean < cfg_.black_enter_mean) && 
                             (e.luma_var < cfg_.black_enter_var);
    const bool black_exit  = (e.luma_mean > cfg_.black_exit_mean) || 
                             (e.luma_var > cfg_.black_exit_var);
    
    // Debug logging for BLACK detection
    static uint64_t black_debug_ctr = 0;
    if (++black_debug_ctr % 10 == 0) {
      Logger::instance().log(LogLevel::INFO,
        "BLACK_DEBUG %s: luma_mean=%.1f (enter<%.1f exit>%.1f) luma_var=%.1f (enter<%.1f exit>%.1f) "
        "black_enter=%d black_exit=%d black_active=%d black_since=%llu recover_since=%llu",
        e.tv_id.c_str(), 
        e.luma_mean, cfg_.black_enter_mean, cfg_.black_exit_mean,
        e.luma_var, cfg_.black_enter_var, cfg_.black_exit_var,
        black_enter, black_exit, s.black_active, 
        (unsigned long long)s.black_since, (unsigned long long)s.black_recover_since);
    }
    
    if (!s.black_active) {
      // Not currently black - check for entering black state
      if (black_enter) {
        if (!s.black_since) {
          s.black_since = batch.ts_ms;
          Logger::instance().log(LogLevel::INFO, "BLACK enter condition met, starting timer for %s", e.tv_id.c_str());
        }
        if ((batch.ts_ms - s.black_since) >= (uint64_t)cfg_.persist_black_ms) {
          s.black_active = true;
          s.black_recover_since = 0;
          std::ostringstream oss;
          oss << "{\"luma_mean\":" << e.luma_mean << ",\"luma_var\":" << e.luma_var << "}";
          evs.push_back(make_event(batch.ts_ms, e.tv_id, EventType::BLACK, oss.str()));
          Logger::instance().log(LogLevel::WARN, "BLACK EVENT EMIT tv=%s ts=%llu mean=%.1f var=%.1f",
                                e.tv_id.c_str(), (unsigned long long)batch.ts_ms, e.luma_mean, e.luma_var);
        }
      } else {
        s.black_since = 0;
      }
    } else {
      // Currently black - check for exit condition with persistence
      if (black_exit) {
        if (!s.black_recover_since) {
          s.black_recover_since = batch.ts_ms;
          Logger::instance().log(LogLevel::INFO, "BLACK exit condition met, starting recovery timer for %s", e.tv_id.c_str());
        }
        if ((batch.ts_ms - s.black_recover_since) >= (uint64_t)cfg_.persist_black_recover_ms) {
          s.black_active = false;
          s.black_since = 0;
          s.black_recover_since = 0;
          evs.push_back(make_event(batch.ts_ms, e.tv_id, EventType::RECOVERED, "{\"from\":\"BLACK\"}"));
          Logger::instance().log(LogLevel::WARN, "RECOVERED EVENT EMIT (from BLACK) tv=%s ts=%llu",
                                e.tv_id.c_str(), (unsigned long long)batch.ts_ms);
        }
      } else {
        // Still black - reset recovery timer
        s.black_recover_since = 0;
      }
    }

    // ========================================
    // FREEZE DETECTION (embedding similarity)
    // Skip if BLACK is active (black screens appear frozen with sim=1.0)
    // ========================================
    if (!s.black_active) {
      auto it = emb_norm.find(e.tv_id);
      if (it != emb_norm.end()) {
        const auto& curr = it->second;

        // Optionally suppress FREEZE for selected TVs (e.g. the reference screen).
        const bool freeze_ignored = (freeze_ignore_.find(e.tv_id) != freeze_ignore_.end());
        if (freeze_ignored) {
          // Keep state warm to avoid spikes if the ignore list is changed at runtime.
          s.freeze_since = 0;
          s.freeze_active = false;
          if (log_metrics) {
            Logger::instance().log(LogLevel::INFO,
              "METRICS %s: luma_mean=%.1f luma_var=%.1f FREEZE_IGNORED",
              e.tv_id.c_str(), e.luma_mean, e.luma_var);
          }
        } else if (!s.last_emb_norm.empty() && curr.size() == s.last_emb_norm.size()) {
          float sim = cosine_normed(curr, s.last_emb_norm);
          
          // Log metrics every 2s for threshold tuning
          if (log_metrics) {
            Logger::instance().log(LogLevel::INFO,
              "METRICS %s: luma_mean=%.1f luma_var=%.1f sim_prev=%.5f (freeze_thresh=%.4f)",
              e.tv_id.c_str(), e.luma_mean, e.luma_var, sim, cfg_.freeze_similarity);
          }
          
          if (sim >= cfg_.freeze_similarity) {
            if (!s.freeze_since) {
              s.freeze_since = batch.ts_ms;
            }
            if (!s.freeze_active && (batch.ts_ms - s.freeze_since) >= (uint64_t)cfg_.persist_freeze_ms) {
              s.freeze_active = true;
              std::ostringstream oss; 
              oss << "{\"similarity\":" << sim << "}";
              evs.push_back(make_event(batch.ts_ms, e.tv_id, EventType::FREEZE, oss.str()));
              Logger::instance().log(LogLevel::WARN, "FREEZE detected on %s (similarity=%.4f)", 
                                    e.tv_id.c_str(), sim);
            }
          } else {
            s.freeze_since = 0;
            if (s.freeze_active) {
              s.freeze_active = false;
              evs.push_back(make_event(batch.ts_ms, e.tv_id, EventType::RECOVERED, "{\"from\":\"FREEZE\"}"));
              Logger::instance().log(LogLevel::INFO, "FREEZE recovered on %s", e.tv_id.c_str());
            }
          }
        } else if (log_metrics && !freeze_ignored) {
          // First frame for this TV - no previous embedding to compare
          Logger::instance().log(LogLevel::INFO,
            "METRICS %s: luma_mean=%.1f luma_var=%.1f sim_prev=N/A (first_frame)",
            e.tv_id.c_str(), e.luma_mean, e.luma_var);
        }
        
        // Always update last embedding for next comparison (for all non-ignored TVs)
        if (!freeze_ignored) {
          s.last_emb_norm = curr;
        }
      } else if (log_metrics && e.tv_id == "tv2") {
        // Diagnostic: tv2 not found in embedding map
        Logger::instance().log(LogLevel::WARN, "FREEZE_DIAGNOSTIC tv2 not in emb_norm map, map_size=%zu", emb_norm.size());
      }
    } else if (log_metrics && e.tv_id == "tv2") {
      // Diagnostic: BLACK active, skipping freeze detection
      Logger::instance().log(LogLevel::INFO, "FREEZE_DIAGNOSTIC tv2 skipped (black_active=1)");
    } // end if (!s.black_active) - skip FREEZE when BLACK

    // ========================================
    // PEER OUTLIER DETECTION (reference-less mismatch)
    // ========================================
    if (cfg_.peer_enabled && !consensus.empty()) {
      const auto& curr = emb_norm[e.tv_id];
      
      if (curr.size() == consensus.size()) {
        float sim_peer = cosine_normed(curr, consensus);

        if (sim_peer < cfg_.peer_similarity_min) {
          if (!s.outlier_since) {
            s.outlier_since = batch.ts_ms;
          }
          if (!s.outlier_active && (batch.ts_ms - s.outlier_since) >= (uint64_t)cfg_.persist_outlier_ms) {
            s.outlier_active = true;
            std::ostringstream oss; 
            oss << "{\"peer_similarity\":" << sim_peer << "}";
            evs.push_back(make_event(batch.ts_ms, e.tv_id, EventType::MISMATCH, oss.str()));
            Logger::instance().log(LogLevel::WARN, "PEER OUTLIER detected on %s (similarity=%.4f)", 
                                  e.tv_id.c_str(), sim_peer);
          }
        } else {
          s.outlier_since = 0;
          if (s.outlier_active) {
            s.outlier_active = false;
            evs.push_back(make_event(batch.ts_ms, e.tv_id, EventType::RECOVERED, "{\"from\":\"MISMATCH\"}"));
            Logger::instance().log(LogLevel::INFO, "PEER OUTLIER recovered on %s", e.tv_id.c_str());
          }
        }
      }
    }
  }

  return evs;
}

} // namespace wvm
