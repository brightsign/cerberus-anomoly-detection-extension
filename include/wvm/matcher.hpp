#pragma once
#include "wvm/reference_db.hpp"
#include <string>
#include <unordered_map>

namespace wvm {

struct MatchResult {
  int64_t k_best = -1;
  float sim_best = -1.0f;
};

class Matcher {
public:
  Matcher(const ReferenceDB& ref, int sample_fps, double window_seconds, bool start_at_end=false);

  MatchResult match(const std::string& tv_id, const float* emb, int dim);

  // Consensus helper
  static int64_t median_k(const std::vector<MatchResult>& m);

private:
  const ReferenceDB& ref_;
  int sample_fps_;
  int window_k_;
  bool start_at_end_ = false;
  std::unordered_map<std::string,int64_t> k_prev_;

  static float dot(const float* a, const float* b, int n);
  static void l2_normalize(std::vector<float>& v);
};

} // namespace wvm
