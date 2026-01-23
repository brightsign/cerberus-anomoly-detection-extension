#pragma once
#include <string>
#include <vector>

namespace wvm {

class ReferenceDB {
public:
  bool load_f32(const std::string& path, int embedding_dim, int ref_fps);

  int ref_fps() const { return ref_fps_; }
  int embedding_dim() const { return dim_; }
  int count() const { return (int)emb_.size() / dim_; }

  // Return pointer to normalized embedding at index k
  const float* at(int k) const { return emb_.data() + (size_t)k*dim_; }

private:
  int dim_ = 0;
  int ref_fps_ = 0;
  std::vector<float> emb_; // normalized unit vectors
};

} // namespace wvm
