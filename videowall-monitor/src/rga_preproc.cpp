#include "wvm/rga_preproc.hpp"
#include "wvm/logger.hpp"
#include <cmath>
#include <cstring>

#ifdef WVM_USE_RGA
// Rockchip RGA im2d API (common on RK BSPs)
#include <im2d.h>
#include <RgaUtils.h>
#endif

namespace wvm {

RgaPreprocessor::RgaPreprocessor(const ModelConfig& model_cfg) : mcfg_(model_cfg) {}

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

#ifdef WVM_USE_RGA
  // NOTE: You must ensure the RGA format mapping matches your camera format.
  // This MVP assumes YUYV camera and outputs RGB888.
  int src_format = RK_FORMAT_YUYV_422; // verify your librga defines this
  int dst_format = RK_FORMAT_RGB_888;

  rga_buffer_t src = wrapbuffer_virtualaddr((void*)frame.data.data(), frame.width, frame.height, src_format);
  rga_buffer_t dst = wrapbuffer_virtualaddr((void*)out.rgb.data(), out.w, out.h, dst_format);

  im_rect src_rect = { roi.x, roi.y, roi.w, roi.h };
  im_rect dst_rect = { 0, 0, out.w, out.h };

  int ret = imcheck(src, dst, src_rect, dst_rect);
  if (ret != IM_STATUS_NOERROR) {
    Logger::instance().log(LogLevel::WARN, "RGA imcheck failed for %s (ret=%d)", roi.id.c_str(), ret);
    return false;
  }

  ret = imresize(src, dst, src_rect, dst_rect);
  if (ret != IM_STATUS_SUCCESS) {
    Logger::instance().log(LogLevel::WARN, "RGA imresize failed for %s (ret=%d)", roi.id.c_str(), ret);
    return false;
  }
#else
  // CPU fallback: nearest-neighbor crop+resize for MVP (replace with RGA on target)
  if (roi.x < 0 || roi.y < 0 || roi.x+roi.w > frame.width || roi.y+roi.h > frame.height) return false;
  if (frame.fmt != PixelFormat::RGB888) return false;

  const uint8_t* src = frame.data.data();
  for (int y=0;y<out.h;++y) {
    int sy = roi.y + (y * roi.h) / out.h;
    for (int x=0;x<out.w;++x) {
      int sx = roi.x + (x * roi.w) / out.w;
      const uint8_t* p = src + (sy*frame.width + sx)*3;
      uint8_t* q = out.rgb.data() + (y*out.w + x)*3;
      q[0]=p[0]; q[1]=p[1]; q[2]=p[2];
    }
  }
#endif

  compute_luma_stats(out.rgb.data(), out.w, out.h, out.luma_mean, out.luma_var);
  return true;
}

} // namespace wvm
