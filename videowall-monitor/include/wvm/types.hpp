#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace wvm {

using SteadyTime = std::chrono::steady_clock::time_point;

inline uint64_t now_ms() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

enum class PixelFormat {
  YUYV,
  NV12,
  RGB888
};

struct CapturedFrame {
  uint64_t ts_ms = 0;
  int width = 0;
  int height = 0;
  PixelFormat fmt = PixelFormat::YUYV;
  std::vector<uint8_t> data;   // For MVP: copied buffer. Can be optimized to dmabuf later.
};

struct RoiRect {
  std::string id;
  int x=0, y=0, w=0, h=0;
};

struct RoiInput {
  std::string tv_id;
  int w=224, h=224;
  std::vector<uint8_t> rgb; // RGB888 interleaved, size w*h*3
  // Optional: store luma stats from preprocessing
  float luma_mean = 0.0f;
  float luma_var  = 0.0f;
};

struct RoiBatch {
  uint64_t ts_ms = 0;
  std::vector<RoiInput> rois;
};

struct Embedding {
  std::string tv_id;
  std::vector<float> vec; // embedding_dim
  float luma_mean = 0.0f;
  float luma_var  = 0.0f;
};

struct EmbeddingBatch {
  uint64_t ts_ms = 0;
  std::vector<Embedding> embeddings;
};

enum class EventType {
  BLACK,
  FREEZE,
  LAG,
  STUTTER,
  MISMATCH,
  RECOVERED
};

struct Event {
  uint64_t ts_ms = 0;
  std::string tv_id;
  EventType type;
  std::string details; // JSON or simple string
};

} // namespace wvm
