#pragma once
#include <string>
#include <vector>

namespace wvm {

class ReferenceDB {
public:
  bool load_f32(const std::string& path, int embedding_dim, int ref_fps);

  // Online (auto mode) initialization: allocate a ring buffer of max_seconds.
  // Embeddings are stored as unit-normalized vectors.
  bool init_online(int embedding_dim, int ref_fps, int max_seconds);

  // Append one embedding (dim must match). Returns the global reference index k.
  // In file mode, k is 0-based. In online mode, k is monotonic increasing.
  int64_t append(const float* emb, int dim);

  bool is_ready() const { return dim_ > 0 && ref_fps_ > 0; }

  int ref_fps() const { return ref_fps_; }
  int embedding_dim() const { return dim_; }
  int count() const;

  // Global index range for online mode.
  int64_t first_k() const;
  int64_t last_k() const;
  bool has_k(int64_t k) const;

  // Return pointer to normalized embedding at global index k.
  const float* at_k(int64_t k) const;

private:
  int dim_ = 0;
  int ref_fps_ = 0;

  // File mode storage (0..N-1)
  std::vector<float> emb_; // normalized unit vectors

  // Online ring storage
  bool online_ = false;
  int max_vecs_ = 0;
  int size_vecs_ = 0;
  int head_ = 0;            // index of oldest element [0..max_vecs_-1]
  int64_t base_k_ = 0;      // global index of head_
  int64_t next_k_ = 0;      // next global index to assign
  std::vector<float> ring_; // size = max_vecs_*dim_
};

} // namespace wvm
