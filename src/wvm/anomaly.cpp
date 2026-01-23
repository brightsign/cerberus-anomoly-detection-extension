#include "wvm/anomaly.hpp"
#include <sstream>
#include <algorithm>

namespace wvm {

AnomalyEngine::AnomalyEngine(const AnomalyConfig& cfg, int ref_fps)
: cfg_(cfg), ref_fps_(ref_fps) {}

Event AnomalyEngine::make_event(uint64_t ts, const std::string& tv, EventType t, const std::string& details) {
  Event e; e.ts_ms = ts; e.tv_id = tv; e.type = t; e.details = details; return e;
}

std::vector<Event> AnomalyEngine::update(uint64_t ts,
                                         const std::string& tv_id,
                                         float luma_mean,
                                         float luma_var,
                                         int k_best,
                                         float sim_best,
                                         int k_wall) {
  std::vector<Event> evs;
  auto& s = st_[tv_id];
  s.last_ts = ts;

  // BLACK
  bool is_black = (luma_mean < cfg_.black_luma_mean) && (luma_var < cfg_.black_luma_var);
  if (is_black) {
    if (!s.black_since) s.black_since = ts;
    if (!s.black_active && (ts - s.black_since) >= (uint64_t)cfg_.persist_black_ms) {
      s.black_active = true;
      evs.push_back(make_event(ts, tv_id, EventType::BLACK, "{\"reason\":\"luma\"}"));
    }
  } else {
    s.black_since = 0;
    if (s.black_active) {
      s.black_active = false;
      evs.push_back(make_event(ts, tv_id, EventType::RECOVERED, "{\"from\":\"BLACK\"}"));
    }
  }

  // MISMATCH (only if we have a match result)
  bool mismatch = (k_best < 0) || (sim_best < cfg_.similarity_min);
  if (mismatch) {
    if (!s.mismatch_since) s.mismatch_since = ts;
    if (!s.mismatch_active && (ts - s.mismatch_since) >= (uint64_t)cfg_.persist_mismatch_ms) {
      s.mismatch_active = true;
      std::ostringstream oss;
      oss << "{\"sim\":" << sim_best << "}";
      evs.push_back(make_event(ts, tv_id, EventType::MISMATCH, oss.str()));
    }
  } else {
    s.mismatch_since = 0;
    if (s.mismatch_active) {
      s.mismatch_active = false;
      evs.push_back(make_event(ts, tv_id, EventType::RECOVERED, "{\"from\":\"MISMATCH\"}"));
    }
  }

  // FREEZE (k not advancing while wall advances)
  if (s.last_k >= 0 && k_best >= 0) {
    int dk = k_best - s.last_k;
    // stutter history
    s.dk_hist.push_back(dk);
    s.dk_ts.push_back(ts);

    // prune stutter window
    while (!s.dk_ts.empty() && (ts - s.dk_ts.front()) > (uint64_t)cfg_.stutter_window_ms) {
      s.dk_ts.erase(s.dk_ts.begin());
      s.dk_hist.erase(s.dk_hist.begin());
    }

    bool no_progress = (dk <= 0);
    bool wall_progress = (k_wall >= 0 && k_best < k_wall);

    if (no_progress && wall_progress) {
      if (!s.freeze_since) s.freeze_since = ts;
      if (!s.freeze_active && (ts - s.freeze_since) >= (uint64_t)cfg_.persist_freeze_ms) {
        s.freeze_active = true;
        evs.push_back(make_event(ts, tv_id, EventType::FREEZE, "{\"reason\":\"k_stuck\"}"));
      }
    } else {
      s.freeze_since = 0;
      if (s.freeze_active) {
        s.freeze_active = false;
        evs.push_back(make_event(ts, tv_id, EventType::RECOVERED, "{\"from\":\"FREEZE\"}"));
      }
    }

    // LAG
    if (k_wall >= 0) {
      float lag_s = (float)(k_wall - k_best) / (float)ref_fps_;
      bool lagging = lag_s > cfg_.lag_threshold_s;
      if (lagging && !s.lag_active) {
        s.lag_active = true;
        std::ostringstream oss; oss << "{\"lag_s\":" << lag_s << "}";
        evs.push_back(make_event(ts, tv_id, EventType::LAG, oss.str()));
      } else if (!lagging && s.lag_active) {
        s.lag_active = false;
        evs.push_back(make_event(ts, tv_id, EventType::RECOVERED, "{\"from\":\"LAG\"}"));
      }
    }

    // STUTTER: many repeats (dk==0) with occasional jumps
    if ((int)s.dk_hist.size() >= 6) {
      int zeros = 0;
      int bigjumps = 0;
      for (int x : s.dk_hist) {
        if (x == 0) zeros++;
        if (x >= 3) bigjumps++;
      }
      bool stutter = (zeros >= (int)(0.5 * s.dk_hist.size())) && (bigjumps >= 1);
      if (stutter && !s.stutter_active) {
        s.stutter_active = true;
        evs.push_back(make_event(ts, tv_id, EventType::STUTTER, "{\"pattern\":\"repeat_jump\"}"));
      } else if (!stutter && s.stutter_active) {
        s.stutter_active = false;
        evs.push_back(make_event(ts, tv_id, EventType::RECOVERED, "{\"from\":\"STUTTER\"}"));
      }
    }
  }

  s.last_k = k_best;
  return evs;
}

} // namespace wvm
