#include "wvm/rknn_mobilenet.hpp"
#include "wvm/logger.hpp"
#include <fstream>
#include <cstring>

namespace wvm {

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) return false;
  ifs.seekg(0, std::ios::end);
  size_t sz = (size_t)ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  out.resize(sz);
  ifs.read((char*)out.data(), sz);
  return true;
}

RknnMobileNet::~RknnMobileNet() {
  if (ctx_) {
    rknn_destroy(ctx_);
    ctx_ = 0;
  }
}

bool RknnMobileNet::load(const ModelConfig& cfg) {
  input_w_ = cfg.input_w;
  input_h_ = cfg.input_h;

  if (!read_file(cfg.rknn_path, model_)) {
    Logger::instance().log(LogLevel::ERROR, "Failed to read rknn model: %s", cfg.rknn_path.c_str());
    return false;
  }

  int ret = rknn_init(&ctx_, model_.data(), model_.size(), 0, nullptr);
  if (ret != RKNN_SUCC) {
    Logger::instance().log(LogLevel::ERROR, "rknn_init failed: %d", ret);
    return false;
  }

  rknn_input_output_num io_num{};
  ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
  if (ret != RKNN_SUCC || io_num.n_output < 1) {
    Logger::instance().log(LogLevel::ERROR, "rknn_query in/out failed: %d", ret);
    return false;
  }

  rknn_tensor_attr out_attr{};
  out_attr.index = 0;
  ret = rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &out_attr, sizeof(out_attr));
  if (ret != RKNN_SUCC) {
    Logger::instance().log(LogLevel::ERROR, "rknn_query output attr failed: %d", ret);
    return false;
  }

  // Determine embedding dim from output dims
  // Many RKNN models show [1, 1280] with n_dims=2
  int dim = 1;
  for (uint32_t i=0;i<out_attr.n_dims;i++) dim *= out_attr.dims[i];
  embedding_dim_ = dim;

  Logger::instance().log(LogLevel::INFO, "RKNN loaded. output dim=%d", embedding_dim_);
  return true;
}

bool RknnMobileNet::infer(const uint8_t* rgb, int w, int h, std::vector<float>& out_embedding) {
  if (!ctx_) return false;
  if (w != input_w_ || h != input_h_) return false;

  rknn_input in{};
  std::memset(&in, 0, sizeof(in));
  in.index = 0;
  in.type = RKNN_TENSOR_UINT8;
  in.fmt  = RKNN_TENSOR_NHWC;
  in.size = (uint32_t)(w*h*3);
  in.buf  = (void*)rgb;

  int ret = rknn_inputs_set(ctx_, 1, &in);
  if (ret != RKNN_SUCC) return false;

  ret = rknn_run(ctx_, nullptr);
  if (ret != RKNN_SUCC) return false;

  rknn_output out{};
  std::memset(&out, 0, sizeof(out));
  out.index = 0;
  out.want_float = 1; // critical for cosine similarity stability

  ret = rknn_outputs_get(ctx_, 1, &out, nullptr);
  if (ret != RKNN_SUCC) return false;

  out_embedding.resize(embedding_dim_);
  std::memcpy(out_embedding.data(), out.buf, embedding_dim_ * sizeof(float));

  rknn_outputs_release(ctx_, 1, &out);
  return true;
}

} // namespace wvm
