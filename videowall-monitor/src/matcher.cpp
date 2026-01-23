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

Matcher::Matcher(const ReferenceDB& ref, int sample_fps, double window_seconds)
: ref_(ref), sample_fps_(sample_fps) {
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

  int nref = ref_.count();
  int k_prev = 0;
  auto it = k_prev_.find(tv_id);
  if (it != k_prev_.end()) k_prev = it->second;

  int step = (int)std::round((double)ref_.ref_fps() / (double)sample_fps_);
  if (step < 1) step = 1;

  int k_pred = k_prev + step;

  int k0 = std::max(0, k_pred - window_k_);
  int k1 = std::min(nref-1, k_pred + window_k_);

  MatchResult mr;
  for (int k=k0;k<=k1;k++) {
    float s = dot(emb.data(), ref_.at(k), dim);
    if (s > mr.sim_best) {
      mr.sim_best = s;
      mr.k_best = k;
    }
  }

  if (mr.k_best >= 0) k_prev_[tv_id] = mr.k_best;
  return mr;
}

int Matcher::median_k(const std::vector<MatchResult>& m) {
  std::vector<int> ks;
  ks.reserve(m.size());
  for (auto& x : m) if (x.k_best >= 0) ks.push_back(x.k_best);
  if (ks.empty()) return -1;
  std::nth_element(ks.begin(), ks.begin() + ks.size()/2, ks.end());
  return ks[ks.size()/2];
}

} // namespace wvm
