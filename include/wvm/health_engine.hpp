#pragma once
#include "wvm/config.hpp"  // For HealthConfig definition
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace wvm {

// Health anomaly types (priority order matters!)
enum class HealthState {
  OK = 0,
  TV_OFF = 1,          // Priority 1: Pure black (panel off)
  BLACK = 2,           // Priority 2: Dark/blank screen
  NO_SIGNAL = 3,       // Priority 3: "No Signal" OSD
  WRONG_INPUT = 4,     // Priority 3: "HDMI2" / Input menu OSD
  UNKNOWN = 5          // Unknown OSD/state
};

const char* health_state_to_string(HealthState state);

// OSD prototype (mean embedding for each class)
struct OsdPrototype {
  std::string label;  // "NO_SIGNAL", "WRONG_INPUT", "HDMI_MENU", etc.
  std::vector<float> embedding;
  int sample_count;
};

// Per-TV state tracking
struct TvHealthState {
  std::string tv_id;
  HealthState current_state = HealthState::OK;
  HealthState pending_state = HealthState::OK;
  uint64_t state_enter_ts_ms = 0;   // When current_state was entered
  uint64_t pending_enter_ts_ms = 0; // When pending_state started
  uint64_t last_heartbeat_ts_ms = 0; // When last heartbeat was emitted

  // Latest measurements
  float luma_mean = 0.0f;
  float luma_var = 0.0f;
  float dark_ratio = 0.0f;
  float osd_similarity = 0.0f;
  std::string osd_label;
};

// Health monitoring engine
class HealthEngine {
public:
  explicit HealthEngine(const HealthConfig& cfg);
  ~HealthEngine() = default;

  // Load OSD prototypes from JSON
  bool load_osd_prototypes(const std::string& path);

  // Process one TV ROI and return events (if state changed)
  struct HealthEvent {
    uint64_t ts_ms;
    std::string tv_id;
    HealthState new_state;
    HealthState old_state;
    float luma_mean;
    float luma_var;
    float dark_ratio;
    float osd_similarity;
    std::string osd_label;
    bool is_heartbeat = false;
  };

  std::vector<HealthEvent> update(
    uint64_t ts_ms,
    const std::string& tv_id,
    float luma_mean,
    float luma_var,
    const uint8_t* rgb_data,  // 224x224 RGB for dark_ratio calculation
    int width,
    int height,
    const float* embedding,   // MobileNetV2 embedding for OSD classification
    int embedding_dim
  );

  // Get current state for a TV
  const TvHealthState* get_state(const std::string& tv_id) const;

private:
  HealthConfig cfg_;
  std::unordered_map<std::string, TvHealthState> tv_states_;
  std::vector<OsdPrototype> osd_prototypes_;
  std::unordered_set<std::string> first_seen_;  // Track TVs we've sent initial event for

  // Prototype capture state
  struct ProtoAccum {
    std::string label;
    std::vector<double> sum;   // accumulated embedding (double for precision)
    int count = 0;
  };
  std::unordered_map<std::string, ProtoAccum> proto_accum_;  // tv_id → accumulator
  uint64_t proto_capture_start_ms_ = 0;
  bool proto_capture_done_ = false;

  // Calculate dark pixel ratio from RGB data
  float calculate_dark_ratio(const uint8_t* rgb, int width, int height) const;

  // Classify OSD by nearest prototype (cosine similarity)
  void classify_osd(const float* embedding, int dim, float& best_sim, std::string& best_label) const;

  // Determine health state based on measurements
  HealthState determine_state(float luma_mean, float luma_var, float dark_ratio,
                              float osd_sim, const std::string& osd_label) const;

  // Check if pending state should be promoted to current state
  bool should_transition(const TvHealthState& tv, HealthState pending, uint64_t ts_ms) const;

  // Get persistence threshold for a given transition
  int get_persistence_ms(HealthState from, HealthState to) const;

  // Write accumulated prototype embeddings to JSON file
  void write_prototypes();
};

} // namespace wvm
