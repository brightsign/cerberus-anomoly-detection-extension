#include "wvm/reference_db.hpp"
#include "wvm/logger.hpp"
#include <fstream>
#include <cmath>

#include <algorithm>

namespace wvm {

static void l2_normalize(float* v, int n) {
  double s=0.0;
  for (int i=0;i<n;i++) s += (double)v[i]*v[i];
  double inv = (s > 1e-12) ? (1.0/std::sqrt(s)) : 1.0;
  for (int i=0;i<n;i++) v[i] = (float)(v[i]*inv);
}

bool ReferenceDB::load_f32(const std::string& path, int embedding_dim, int ref_fps) {
  dim_ = embedding_dim;
  ref_fps_ = ref_fps;

  online_ = false;
  max_vecs_ = 0;
  size_vecs_ = 0;
  head_ = 0;
  base_k_ = 0;
  next_k_ = 0;
  ring_.clear();

  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    Logger::instance().log(LogLevel::ERROR, "Failed to open ref embeddings: %s", path.c_str());
    return false;
  }
  ifs.seekg(0, std::ios::end);
  size_t sz = (size_t)ifs.tellg();
  ifs.seekg(0, std::ios::beg);

  if (sz % (sizeof(float)*dim_) != 0) {
    Logger::instance().log(LogLevel::ERROR, "Ref embeddings size not divisible by dim");
    return false;
  }

  emb_.resize(sz/sizeof(float));
  ifs.read((char*)emb_.data(), sz);

  // Normalize each vector
  const int nvec = (int)(emb_.size()/dim_);
  for (int k=0;k<nvec;k++) l2_normalize(emb_.data()+ (size_t)k*dim_, dim_);

  Logger::instance().log(LogLevel::INFO, "Loaded reference embeddings: %d vectors", nvec);
  return true;
}

bool ReferenceDB::init_online(int embedding_dim, int ref_fps, int max_seconds) {
  if (embedding_dim <= 0 || ref_fps <= 0) {
    Logger::instance().log(LogLevel::ERROR, "init_online: invalid dim/ref_fps");
    return false;
  }
  dim_ = embedding_dim;
  ref_fps_ = ref_fps;
  online_ = true;
  emb_.clear();

  max_vecs_ = std::max(1, max_seconds * ref_fps_);
  ring_.assign((size_t)max_vecs_ * (size_t)dim_, 0.0f);
  size_vecs_ = 0;
  head_ = 0;
  base_k_ = 0;
  next_k_ = 0;

  Logger::instance().log(LogLevel::INFO, "Online reference initialized: dim=%d ref_fps=%d max_vecs=%d",
                         dim_, ref_fps_, max_vecs_);
  return true;
}

int ReferenceDB::count() const {
  if (!online_) return (int)emb_.size() / std::max(1, dim_);
  return size_vecs_;
}

int64_t ReferenceDB::first_k() const {
  if (!online_) return 0;
  return base_k_;
}

int64_t ReferenceDB::last_k() const {
  const int n = count();
  if (n <= 0) return -1;
  if (!online_) return (int64_t)n - 1;
  return (base_k_ + (int64_t)n - 1);
}

bool ReferenceDB::has_k(int64_t k) const {
  if (k < 0) return false;
  if (!online_) {
    return k >= 0 && k < (int64_t)count();
  }
  if (size_vecs_ <= 0) return false;
  return (k >= base_k_) && (k <= (base_k_ + (int64_t)size_vecs_ - 1));
}

const float* ReferenceDB::at_k(int64_t k) const {
  if (!has_k(k)) return nullptr;
  if (!online_) {
    return emb_.data() + (size_t)k * (size_t)dim_;
  }
  const int64_t off = k - base_k_;
  const int slot = (head_ + (int)off) % max_vecs_;
  return ring_.data() + (size_t)slot * (size_t)dim_;
}

int64_t ReferenceDB::append(const float* emb_in, int dim) {
  if (!online_) {
    // In file mode, append is unsupported by design.
    return -1;
  }
  if (!emb_in || dim != dim_) return -1;

  // Determine write slot.
  int slot = 0;
  if (size_vecs_ < max_vecs_) {
    slot = (head_ + size_vecs_) % max_vecs_;
    size_vecs_++;
  } else {
    // Overwrite oldest.
    slot = head_;
    head_ = (head_ + 1) % max_vecs_;
    base_k_++;
  }

  float* dst = ring_.data() + (size_t)slot * (size_t)dim_;
  std::copy(emb_in, emb_in + dim_, dst);
  l2_normalize(dst, dim_);

  const int64_t k = next_k_;
  next_k_++;

  // Keep base_k_ consistent when we haven't wrapped yet.
  if (size_vecs_ < max_vecs_) {
    // base_k_ remains 0 until buffer becomes full.
    // For correctness when max_vecs_ is small, recompute base_k_:
    base_k_ = next_k_ - (int64_t)size_vecs_;
  }
  return k;
}

} // namespace wvm
