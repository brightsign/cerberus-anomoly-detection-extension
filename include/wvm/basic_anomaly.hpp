#pragma once
#include "wvm/types.hpp"
#include "wvm/config.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wvm {

/**
 * BasicAnomalyEngine - Reference-free anomaly detection
 * 
 * Detects common anomalies without requiring reference embeddings:
 * - BLACK: Low luma mean/variance
 * - FREEZE: High embedding similarity between consecutive frames
 * - PEER OUTLIER: TV diverges from wall consensus (reference-less mismatch)
 * 
 * Works with MJPEG cameras and processes embeddings from NPU inference.
 */
class BasicAnomalyEngine {
public:
  explicit BasicAnomalyEngine(const AnomalyConfig& cfg);

  /**
   * Process one batch of embeddings (one embedding per ROI/TV).
   * Returns BLACK, FREEZE, and peer-outlier events.
   */
  std::vector<Event> update_batch(const EmbeddingBatch& batch);

private:
  struct TvState {
    uint64_t last_ts = 0;
    uint64_t last_metrics_log = 0;  // Per-TV metrics logging timestamp

    // BLACK detection with hysteresis
    uint64_t black_since = 0;
    uint64_t black_recover_since = 0;
    bool black_active = false;

    // FREEZE detection (embedding-based)
    std::vector<float> last_emb_norm;
    uint64_t freeze_since = 0;
    bool freeze_active = false;

    // PEER outlier (reference-less mismatch)
    uint64_t outlier_since = 0;
    bool outlier_active = false;
  };

  AnomalyConfig cfg_;
  std::unordered_map<std::string, TvState> st_;

  // Fast lookup for basic-mode FREEZE suppression per tv.
  std::unordered_set<std::string> freeze_ignore_;

  // Utility functions
  static void l2_normalize_inplace(std::vector<float>& v);
  static float dot(const std::vector<float>& a, const std::vector<float>& b);
  static float cosine_normed(const std::vector<float>& a_norm, const std::vector<float>& b_norm);
  static Event make_event(uint64_t ts, const std::string& tv_id, EventType t, const std::string& details);
};

} // namespace wvm
