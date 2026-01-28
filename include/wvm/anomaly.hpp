#pragma once
#include "wvm/types.hpp"
#include "wvm/config.hpp"
#include <unordered_map>

namespace wvm {

struct TvState {
  // Timing
  uint64_t last_ts = 0;

  // BLACK detection
  uint64_t black_since = 0;
  uint64_t black_recover_since = 0;
  bool black_active = false;

  // FREEZE detection via k progression
  int64_t last_k = -1;
  uint64_t freeze_since = 0;
  bool freeze_active = false;

  // MISMATCH
  uint64_t mismatch_since = 0;
  bool mismatch_active = false;

  // LAG
  bool lag_active = false;

  // STUTTER
  std::vector<int> dk_hist;
  std::vector<uint64_t> dk_ts;
  bool stutter_active = false;
};

class AnomalyEngine {
public:
  explicit AnomalyEngine(const AnomalyConfig& cfg, int ref_fps);

  // Evaluate anomalies for a TV given its match result, consensus k, and luma stats.
  // Returns 0..N events.
  std::vector<Event> update(uint64_t ts_ms,
                            const std::string& tv_id,
                            float luma_mean,
                            float luma_var,
                            int64_t k_best,
                            float sim_best,
                            int64_t k_wall,
                            bool content_anomalies = true);

private:
  AnomalyConfig cfg_;
  int ref_fps_;
  std::unordered_map<std::string,TvState> st_;

  static std::string etype_to_str(EventType t);
  static Event make_event(uint64_t ts, const std::string& tv, EventType t, const std::string& details);
};

} // namespace wvm
