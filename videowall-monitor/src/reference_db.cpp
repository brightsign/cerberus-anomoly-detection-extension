#include "wvm/reference_db.hpp"
#include "wvm/logger.hpp"
#include <fstream>
#include <cmath>

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

} // namespace wvm
