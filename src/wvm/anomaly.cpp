#include "wvm/anomaly.hpp"
#include "wvm/logger.hpp"
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
                                         int64_t k_best,
                                         float sim_best,
                                         int64_t k_wall,
                                         bool content_anomalies) {
  std::vector<Event> evs;
  auto& s = st_[tv_id];
  s.last_ts = ts;

  // BLACK detection with hysteresis
  const bool black_enter = (luma_mean < cfg_.black_enter_mean) && (luma_var < cfg_.black_enter_var);
  const bool black_exit  = (luma_mean > cfg_.black_exit_mean) || (luma_var > cfg_.black_exit_var);
  
  // Debug BLACK detection every 2s for tv2
  static uint64_t last_black_debug_log = 0;
  if (tv_id == "tv2" && (ts - last_black_debug_log) >= 2000) {
    Logger::instance().log(LogLevel::INFO, 
      "BLACK_DEBUG_REF %s: luma_mean=%.1f (enter<%.1f exit>%.1f) luma_var=%.1f (enter<%.1f exit>%.1f) black_enter=%d black_exit=%d black_active=%d",
      tv_id.c_str(), luma_mean, cfg_.black_enter_mean, cfg_.black_exit_mean,
      luma_var, cfg_.black_enter_var, cfg_.black_exit_var,
      (int)black_enter, (int)black_exit, (int)s.black_active);
    last_black_debug_log = ts;
  }
  
  if (!s.black_active) {
    // Not currently black - check for entering black state
    if (black_enter) {
      if (!s.black_since) s.black_since = ts;
      if ((ts - s.black_since) >= (uint64_t)cfg_.persist_black_ms) {
        s.black_active = true;
        s.black_recover_since = 0;
        evs.push_back(make_event(ts, tv_id, EventType::BLACK, "{\"reason\":\"luma\"}"));
      }
    } else {
      s.black_since = 0;
    }
  } else {
    // Currently black - check for exit condition with persistence
    if (black_exit) {
      if (!s.black_recover_since) s.black_recover_since = ts;
      if ((ts - s.black_recover_since) >= (uint64_t)cfg_.persist_black_recover_ms) {
        s.black_active = false;
        s.black_since = 0;
        s.black_recover_since = 0;
        evs.push_back(make_event(ts, tv_id, EventType::RECOVERED, "{\"from\":\"BLACK\"}"));
      }
    } else {
      // Still black - reset recovery timer
      s.black_recover_since = 0;
    }
  }

  // If content anomalies are disabled, stop after BLACK logic.
  if (!content_anomalies) {
    s.last_k = k_best;
    return evs;
  }

  // MISMATCH (skip if BLACK is active to reduce noise)
  if (!s.black_active) {
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
  } else {
    // While black, clear mismatch timers/state.
    s.mismatch_since = 0;
    if (s.mismatch_active) {
      s.mismatch_active = false;
      evs.push_back(make_event(ts, tv_id, EventType::RECOVERED, "{\"from\":\"MISMATCH\"}"));
    }
  }

  // While black, suppress content anomalies (freeze/lag/stutter) to reduce noise.
  if (s.black_active) {
    s.freeze_since = 0;
    s.freeze_active = false;
    s.lag_active = false;
    s.stutter_active = false;
    s.dk_hist.clear();
    s.dk_ts.clear();
    s.last_k = k_best;
    return evs;
  }

  // FREEZE (k not advancing while wall advances)
  if (s.last_k >= 0 && k_best >= 0) {
    int dk = (int)std::max<int64_t>(-2147483648LL, std::min<int64_t>(2147483647LL, (k_best - s.last_k)));
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
