#include "wvm/basic_anomaly.hpp"
#include "wvm/logger.hpp"
#include <cmath>
#include <sstream>
#include <algorithm>

namespace wvm {

BasicAnomalyEngine::BasicAnomalyEngine(const AnomalyConfig& cfg) : cfg_(cfg) {
  Logger::instance().log(LogLevel::INFO, 
    "BasicAnomalyEngine initialized: black_persist=%dms freeze_persist=%dms freeze_sim=%.4f peer=%s",
    cfg_.persist_black_ms, cfg_.persist_freeze_ms, cfg_.freeze_similarity,
    cfg_.peer_enabled ? "enabled" : "disabled");
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

    // Periodic metrics logging for threshold tuning (every 2s)
    static uint64_t s_last_metrics_log = 0;
    const bool log_metrics = (batch.ts_ms - s_last_metrics_log) >= 2000;
    if (log_metrics) s_last_metrics_log = batch.ts_ms;

    // ========================================
    // BLACK SCREEN DETECTION
    // ========================================
    const bool is_black = (e.luma_mean < cfg_.black_luma_mean) && 
                          (e.luma_var < cfg_.black_luma_var);
    
    if (is_black) {
      if (!s.black_since) {
        s.black_since = batch.ts_ms;
      }
      if (!s.black_active && (batch.ts_ms - s.black_since) >= (uint64_t)cfg_.persist_black_ms) {
        s.black_active = true;
        std::ostringstream oss;
        oss << "{\"luma_mean\":" << e.luma_mean << ",\"luma_var\":" << e.luma_var << "}";
        evs.push_back(make_event(batch.ts_ms, e.tv_id, EventType::BLACK, oss.str()));
        Logger::instance().log(LogLevel::WARN, "BLACK detected on %s (luma_mean=%.2f, luma_var=%.2f)",
                              e.tv_id.c_str(), e.luma_mean, e.luma_var);
      }
    } else {
      s.black_since = 0;
      if (s.black_active) {
        s.black_active = false;
        evs.push_back(make_event(batch.ts_ms, e.tv_id, EventType::RECOVERED, "{\"from\":\"BLACK\"}"));
        Logger::instance().log(LogLevel::INFO, "BLACK recovered on %s", e.tv_id.c_str());
      }
    }

    // ========================================
    // FREEZE DETECTION (embedding similarity)
    // ========================================
    auto it = emb_norm.find(e.tv_id);
    if (it != emb_norm.end()) {
      const auto& curr = it->second;
      
      if (!s.last_emb_norm.empty() && curr.size() == s.last_emb_norm.size()) {
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
      } else if (log_metrics) {
        // First frame for this TV - no previous embedding to compare
        Logger::instance().log(LogLevel::INFO,
          "METRICS %s: luma_mean=%.1f luma_var=%.1f sim_prev=N/A (first_frame)",
          e.tv_id.c_str(), e.luma_mean, e.luma_var);
      }
      
      // Update last embedding for next comparison
      s.last_emb_norm = curr;
    }

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
