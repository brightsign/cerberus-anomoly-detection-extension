#include "wvm/matcher.hpp"
#include <algorithm>
#include <cmath>

namespace wvm {

static void l2_norm_inplace(float* v, int n) {
  double s=0.0;
  for (int i=0;i<n;i++) s += (double)v[i]*v[i];
  double inv = (s > 1e-12) ? (1.0/std::sqrt(s)) : 1.0;
  for (int i=0;i<n;i++) v[i] = (float)(v[i]*inv);
}

Matcher::Matcher(const ReferenceDB& ref, int sample_fps, double window_seconds, bool start_at_end)
: ref_(ref), sample_fps_(sample_fps), start_at_end_(start_at_end) {
  window_k_ = (int)std::round(window_seconds * ref_.ref_fps());
  if (window_k_ < 1) window_k_ = 1;
}

float Matcher::dot(const float* a, const float* b, int n) {
  double s=0.0;
  for (int i=0;i<n;i++) s += (double)a[i]*b[i];
  return (float)s;
}

MatchResult Matcher::match(const std::string& tv_id, const float* emb_in, int dim) {
  // Copy and normalize incoming embedding (unit vector)
  std::vector<float> emb(dim);
  std::copy(emb_in, emb_in+dim, emb.begin());
  l2_norm_inplace(emb.data(), dim);

  const int nref = ref_.count();
  if (nref <= 0) {
    return MatchResult{};
  }

  const int64_t first_k = ref_.first_k();
  const int64_t last_k  = ref_.last_k();

  int64_t k_prev = start_at_end_ ? last_k : first_k;
  auto it = k_prev_.find(tv_id);
  if (it != k_prev_.end()) k_prev = it->second;

  int step = (int)std::round((double)ref_.ref_fps() / (double)sample_fps_);
  if (step < 1) step = 1;

  int64_t k_pred = k_prev + (int64_t)step;

  int64_t k0 = std::max(first_k, k_pred - (int64_t)window_k_);
  int64_t k1 = std::min(last_k,  k_pred + (int64_t)window_k_);

  MatchResult mr;
  for (int64_t k=k0;k<=k1;k++) {
    const float* r = ref_.at_k(k);
    if (!r) continue;
    float s = dot(emb.data(), r, dim);
    if (s > mr.sim_best) {
      mr.sim_best = s;
      mr.k_best = k;
    }
  }

  if (mr.k_best >= 0) k_prev_[tv_id] = mr.k_best;
  return mr;
}

int64_t Matcher::median_k(const std::vector<MatchResult>& m) {
  std::vector<int64_t> ks;
  ks.reserve(m.size());
  for (auto& x : m) if (x.k_best >= 0) ks.push_back(x.k_best);
  if (ks.empty()) return -1;
  std::nth_element(ks.begin(), ks.begin() + ks.size()/2, ks.end());
  return ks[ks.size()/2];
}

} // namespace wvm
