#include "wvm/rga_preproc.hpp"
#include "wvm/logger.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>

#ifdef WVM_USE_RGA
// Rockchip RGA im2d API (common on RK BSPs)
#include <im2d.h>
#include <RgaUtils.h>
#endif

namespace wvm {

static inline uint8_t clip_u8(int x) {
  return (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x));
}

// BT.601 integer YUV->RGB (good enough for embeddings + luma checks)
static inline void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
  int C = (int)y - 16;
  int D = (int)u - 128;
  int E = (int)v - 128;
  if (C < 0) C = 0;
  int Rt = (298 * C + 409 * E + 128) >> 8;
  int Gt = (298 * C - 100 * D - 208 * E + 128) >> 8;
  int Bt = (298 * C + 516 * D + 128) >> 8;
  r = clip_u8(Rt);
  g = clip_u8(Gt);
  b = clip_u8(Bt);
}

RgaPreprocessor::RgaPreprocessor(const ModelConfig& model_cfg) : mcfg_(model_cfg) {
#ifdef WVM_USE_RGA
  Logger::instance().log(LogLevel::INFO, "RgaPreprocessor: RGA enabled (WVM_USE_RGA=1)");
#else
  Logger::instance().log(LogLevel::WARN, "RgaPreprocessor: RGA disabled (WVM_USE_RGA=0) - using CPU fallback");
#endif
}

void RgaPreprocessor::compute_luma_stats(const uint8_t* rgb, int w, int h, float& mean, float& var) {
  // Compute luma from RGB: Y ≈ 0.299R + 0.587G + 0.114B
  const int n = w*h;
  double sum = 0.0;
  for (int i=0;i<n;++i) {
    const uint8_t r = rgb[i*3+0];
    const uint8_t g = rgb[i*3+1];
    const uint8_t b = rgb[i*3+2];
    double y = 0.299*r + 0.587*g + 0.114*b;
    sum += y;
  }
  double mu = sum / n;
  double v = 0.0;
  for (int i=0;i<n;++i) {
    const uint8_t r = rgb[i*3+0];
    const uint8_t g = rgb[i*3+1];
    const uint8_t b = rgb[i*3+2];
    double y = 0.299*r + 0.587*g + 0.114*b;
    double d = y - mu;
    v += d*d;
  }
  mean = (float)mu;
  var  = (float)(v / n);
}

bool RgaPreprocessor::extract_roi_rgb224(const CapturedFrame& frame, const RoiRect& roi, RoiInput& out) {
  out.tv_id = roi.id;
  out.w = mcfg_.input_w;
  out.h = mcfg_.input_h;
  out.rgb.resize(out.w*out.h*3);

  // Validate ROI bounds
  if (roi.w <= 0 || roi.h <= 0) return false;
  if (roi.x < 0 || roi.y < 0) return false;
  if (roi.x + roi.w > frame.width) return false;
  if (roi.y + roi.h > frame.height) return false;

  // Shrink ROI inward by 6% on each side to exclude bezel, stand, and wall leakage.
  // This improves all health features: dark_ratio, sat_mean, laplacian_var, temporal_diff.
  const int shrink_x = std::max(1, roi.w * 6 / 100);
  const int shrink_y = std::max(1, roi.h * 6 / 100);
  const int inner_x = roi.x + shrink_x;
  const int inner_y = roi.y + shrink_y;
  const int inner_w = roi.w - 2 * shrink_x;
  const int inner_h = roi.h - 2 * shrink_y;
  // Use a temporary RoiRect wrapper for the inner bounds
  struct InnerRoi { int x, y, w, h; std::string id; } inner{inner_x, inner_y, inner_w, inner_h, roi.id};

#ifdef WVM_USE_RGA
  // RGA path: requires DMA-capable (physically contiguous) buffers.
  // wrapbuffer_virtualaddr with a plain heap vector typically fails with imcheck -3.
  // We attempt it and fall through to CPU on failure.
  do {
    int src_format = RK_FORMAT_YUYV_422;
    int dst_format = RK_FORMAT_RGB_888;

    // RGA/YUYV requires 2-pixel alignment on all ROI fields
    int rx = inner.x & ~1;
    int ry = inner.y & ~1;
    int rw = (inner.w + 1) & ~1;
    int rh = (inner.h + 1) & ~1;
    if (rx + rw > frame.width)  rw = (frame.width  - rx) & ~1;
    if (ry + rh > frame.height) rh = (frame.height - ry) & ~1;

    rga_buffer_t rga_src = wrapbuffer_virtualaddr((void*)frame.data.data(), frame.width, frame.height, src_format);
    rga_buffer_t rga_dst = wrapbuffer_virtualaddr((void*)out.rgb.data(), out.w, out.h, dst_format);

    im_rect src_rect = { rx, ry, rw, rh };
    im_rect dst_rect = { 0, 0, out.w, out.h };

    int ret = imcheck(rga_src, rga_dst, src_rect, dst_rect);
    if (ret != IM_STATUS_NOERROR) {
      static bool s_rga_warned = false;
      if (!s_rga_warned) {
        s_rga_warned = true;
        Logger::instance().log(LogLevel::WARN,
          "RGA imcheck failed (ret=%d) — heap buffer not DMA-capable; using CPU fallback for all frames", ret);
      }
      break; // fall through to CPU path
    }

    ret = improcess(rga_src, rga_dst, {}, src_rect, dst_rect, {}, IM_SYNC);
    if (ret != IM_STATUS_SUCCESS) {
      static bool s_rga_proc_warned = false;
      if (!s_rga_proc_warned) {
        s_rga_proc_warned = true;
        Logger::instance().log(LogLevel::WARN, "RGA improcess failed (ret=%d) — using CPU fallback", ret);
      }
      break; // fall through to CPU path
    }

    compute_luma_stats(out.rgb.data(), out.w, out.h, out.luma_mean, out.luma_var);
    return true;
  } while (false);
  // RGA failed — fall through to CPU YUYV path below
#endif

  // CPU fallback: YUYV crop + nearest-neighbour resize to RGB224.
  if (frame.fmt != PixelFormat::YUYV) {
    Logger::instance().log(LogLevel::WARN, "CPU fallback: unsupported frame fmt=%d (need YUYV)", (int)frame.fmt);
    return false;
  }
  const uint8_t* src = frame.data.data();
  const int src_stride_bytes = frame.width * 2; // YUYV 16bpp

  // Welford for luma mean/var using Y samples
  double mean = 0.0;
  double m2 = 0.0;
  int n = 0;

  for (int y = 0; y < out.h; ++y) {
    int sy = inner.y + (y * inner.h) / out.h;
    const uint8_t* row = src + sy * src_stride_bytes;
    for (int x = 0; x < out.w; ++x) {
      int sx = inner.x + (x * inner.w) / out.w;
      int sx_even = sx & ~1;
      const uint8_t* p = row + sx_even * 2; // 2 bytes per pixel

      // YUYV layout: [Y0 U Y1 V]
      uint8_t yv = (sx & 1) ? p[2] : p[0];
      uint8_t u  = p[1];
      uint8_t v  = p[3];

      uint8_t r,g,b;
      yuv_to_rgb(yv, u, v, r, g, b);

      uint8_t* q = out.rgb.data() + (y * out.w + x) * 3;
      q[0] = r; q[1] = g; q[2] = b;

      // luma stats from Y
      n++;
      double dy = (double)yv - mean;
      mean += dy / n;
      m2 += dy * ((double)yv - mean);
    }
  }

  out.luma_mean = (float)mean;
  out.luma_var  = (n > 0) ? (float)(m2 / n) : 0.0f;
  Logger::instance().log(LogLevel::DEBUG, "CPU luma stats for %s: mean=%.2f var=%.2f", 
                        roi.id.c_str(), out.luma_mean, out.luma_var);
  return true;
}

} // namespace wvm
