#pragma once
#include "wvm/config.hpp"
#include "wvm/types.hpp"
#include <rknn_api.h>
#include <vector>
#include <string>

namespace wvm {

class RknnMobileNet {
public:
  RknnMobileNet() = default;
  ~RknnMobileNet();

  bool load(const ModelConfig& cfg);
  int embedding_dim() const { return embedding_dim_; }

  // Input: RGB888 NHWC (w,h,3). Output: embedding_dim floats.
  bool infer(const uint8_t* rgb, int w, int h, std::vector<float>& out_embedding);

private:
  rknn_context ctx_ = 0;
  int embedding_dim_ = 0;
  int input_w_ = 224, input_h_ = 224;

  std::vector<uint8_t> model_;
};

} // namespace wvm
