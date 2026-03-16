// YOLOX NPU-based TV detector for RK3568/RK3576/RK3588.
//
// Loads a YOLOX-S RKNN model, runs it during startup to find TV rectangles,
// then releases the NPU context so MobileNet can use it for steady-state
// embedding inference.
//
// Decode logic mirrors the Rockchip YOLOX Model Zoo postprocess (rknpu2 path)
// for the 3-output optimised model: [1,85,80,80] / [1,85,40,40] / [1,85,20,20]
// All outputs are requested as float32 (want_float=1) so we avoid dealing with
// quantisation parameters at this level.

#include "wvm/yolo_tv_detector.hpp"
#include "wvm/logger.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace wvm {

// ── COCO constants ───────────────────────────────────────────────────────────
static constexpr int   COCO_TV_CLASS  = 63;   // "tv" in COCO80
static constexpr int   NUM_CLASSES    = 80;
static constexpr float NMS_IOU_THRESH = 0.30f;

// ── frame conversion ─────────────────────────────────────────────────────────
//
// Returns an RGB888 cv::Mat from a CapturedFrame.
// We need RGB (not BGR) because YOLOX expects RGB input with mean/std=[0,0,0]/[1,1,1].
static cv::Mat frame_to_rgb(const CapturedFrame& frame) {
  if (frame.data.empty() || frame.width <= 0 || frame.height <= 0)
    return {};

  if (frame.fmt == PixelFormat::RGB888) {
    // Already RGB – clone so caller owns memory
    cv::Mat rgb(frame.height, frame.width, CV_8UC3,
                const_cast<uint8_t*>(frame.data.data()));
    return rgb.clone();
  }

  if (frame.fmt == PixelFormat::YUYV) {
    if ((int)frame.data.size() < frame.width * frame.height * 2) return {};
    cv::Mat yuyv(frame.height, frame.width, CV_8UC2,
                 const_cast<uint8_t*>(frame.data.data()));
    cv::Mat bgr, rgb;
    cv::cvtColor(yuyv, bgr, cv::COLOR_YUV2BGR_YUY2);
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    return rgb;
  }

  if (frame.fmt == PixelFormat::NV12) {
    if ((int)frame.data.size() < (frame.width * frame.height * 3) / 2) return {};
    cv::Mat yuv(frame.height + frame.height / 2, frame.width, CV_8UC1,
                const_cast<uint8_t*>(frame.data.data()));
    cv::Mat bgr, rgb;
    cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_NV12);
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    return rgb;
  }

  return {};
}

// ── NMS helper ───────────────────────────────────────────────────────────────
static float box_iou(float x1, float y1, float w1, float h1,
                     float x2, float y2, float w2, float h2) {
  float ix  = std::max(x1, x2);
  float iy  = std::max(y1, y2);
  float ix2 = std::min(x1 + w1, x2 + w2);
  float iy2 = std::min(y1 + h1, y2 + h2);
  float iw  = std::max(0.f, ix2 - ix);
  float ih  = std::max(0.f, iy2 - iy);
  float inter = iw * ih;
  float uni   = w1 * h1 + w2 * h2 - inter;
  return uni > 0.f ? inter / uni : 0.f;
}

// ── construction / init ──────────────────────────────────────────────────────
YoloTvDetector::YoloTvDetector(const std::string& model_path, float conf_thresh)
    : conf_thresh_(conf_thresh) {
  loaded_ = init(model_path);
}

bool YoloTvDetector::init(const std::string& path) {
  // Read model file
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    Logger::instance().log(LogLevel::WARN,
      "YoloTvDetector: model not found: %s (set roi.auto.yolo_model_path or leave empty to use OpenCV)",
      path.c_str());
    return false;
  }
  auto fsize = (std::streamsize)f.tellg();
  f.seekg(0);
  std::vector<char> buf(fsize);
  f.read(buf.data(), fsize);
  if (!f) {
    Logger::instance().log(LogLevel::ERROR,
      "YoloTvDetector: read error for model: %s", path.c_str());
    return false;
  }

  int ret = rknn_init(&ctx_, buf.data(), (uint32_t)fsize, 0, nullptr);
  if (ret < 0) {
    Logger::instance().log(LogLevel::ERROR,
      "YoloTvDetector: rknn_init failed ret=%d for %s", ret, path.c_str());
    return false;
  }

  // Pin YOLOX to NPU Core 0 (MobileNet uses Core 1 for steady-state inference).
  ret = rknn_set_core_mask(ctx_, RKNN_NPU_CORE_0);
  if (ret != RKNN_SUCC) {
    Logger::instance().log(LogLevel::WARN,
      "YoloTvDetector: rknn_set_core_mask(Core0) failed ret=%d — using auto", ret);
  } else {
    Logger::instance().log(LogLevel::INFO, "YoloTvDetector: pinned to NPU Core 0");
  }

  // Query I/O count
  rknn_input_output_num io{};
  rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
  n_outputs_ = io.n_output;

  // Query input shape
  rknn_tensor_attr in_attr{};
  in_attr.index = 0;
  rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
  if (in_attr.fmt == RKNN_TENSOR_NCHW) {
    model_h_ = in_attr.dims[2];
    model_w_ = in_attr.dims[3];
  } else {
    // NHWC: [batch, h, w, c]
    model_h_ = in_attr.dims[1];
    model_w_ = in_attr.dims[2];
  }

  // Query output shapes to get grid_h, grid_w and tensor format for each level.
  // RKNN may compile outputs as NCHW [1,85,H,W] or NHWC [1,H,W,85] —
  // we detect this from oa.fmt and index accordingly in decode().
  for (uint32_t i = 0; i < std::min(n_outputs_, 3u); ++i) {
    rknn_tensor_attr oa{};
    oa.index = i;
    rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa));

    // Always log actual dims so we can see the real layout in the log
    Logger::instance().log(LogLevel::INFO,
      "[YOLO TV] output[%u]: n_dims=%u fmt=%d dims=[%u,%u,%u,%u]",
      i, oa.n_dims, (int)oa.fmt,
      oa.dims[0], oa.dims[1], oa.dims[2], oa.dims[3]);

    if (oa.n_dims == 3) {
      // Standard YOLOX ONNX flat format: [1, H*W, 85]
      // e.g. [1,6400,85] / [1,1600,85] / [1,400,85] for strides 8/16/32
      int flat_hw = (int)oa.dims[1];
      int sq = (int)std::round(std::sqrt((float)flat_hw));
      out_dims_[i].grid_h = sq;
      out_dims_[i].grid_w = sq;
      out_dims_[i].flat   = true;
      out_dims_[i].valid  = true;
      Logger::instance().log(LogLevel::INFO, "[YOLO TV]   -> flat [1,%d,85] grid=%dx%d", flat_hw, sq, sq);
    } else if (oa.n_dims >= 4) {
      out_dims_[i].fmt = oa.fmt;
      if (oa.fmt == RKNN_TENSOR_NHWC) {
        // [1, H, W, 85]
        out_dims_[i].grid_h = (int)oa.dims[1];
        out_dims_[i].grid_w = (int)oa.dims[2];
        Logger::instance().log(LogLevel::INFO, "[YOLO TV]   -> NHWC [1,%d,%d,85]",
                out_dims_[i].grid_h, out_dims_[i].grid_w);
      } else {
        // NCHW [1, 85, H, W]
        out_dims_[i].grid_h = (int)oa.dims[2];
        out_dims_[i].grid_w = (int)oa.dims[3];
        Logger::instance().log(LogLevel::INFO, "[YOLO TV]   -> NCHW [1,85,%d,%d]",
                out_dims_[i].grid_h, out_dims_[i].grid_w);
      }
      out_dims_[i].valid = true;
    } else {
      Logger::instance().log(LogLevel::WARN,
        "YoloTvDetector: output[%u] has unexpected %u dims — skipping", i, oa.n_dims);
    }
  }

  Logger::instance().log(LogLevel::INFO,
    "YoloTvDetector: loaded %s  input=%dx%d  outputs=%u  conf_thresh=%.2f",
    path.c_str(), model_w_, model_h_, n_outputs_, conf_thresh_);
  return true;
}

YoloTvDetector::~YoloTvDetector() { release(); }

void YoloTvDetector::release() {
  if (ctx_ != 0) {
    rknn_destroy(ctx_);
    ctx_ = 0;
  }
  loaded_ = false;
}

// ── inference ────────────────────────────────────────────────────────────────
std::vector<RoiRect> YoloTvDetector::detect(const CapturedFrame& frame) {
  if (!loaded_ || ctx_ == 0) return {};

  cv::Mat rgb = frame_to_rgb(frame);
  if (rgb.empty()) {
    Logger::instance().log(LogLevel::WARN,
      "YoloTvDetector: frame_to_rgb failed (fmt=%d %dx%d)",
      (int)frame.fmt, frame.width, frame.height);
    return {};
  }

  // Letterbox: scale to model size preserving aspect ratio, pad remainder with 114
  float scale = std::min((float)model_w_ / frame.width,
                         (float)model_h_ / frame.height);
  int new_w = (int)(frame.width  * scale);
  int new_h = (int)(frame.height * scale);
  int pad_x = (model_w_ - new_w) / 2;
  int pad_y = (model_h_ - new_h) / 2;

  cv::Mat lb(model_h_, model_w_, CV_8UC3, cv::Scalar(114, 114, 114));
  cv::Mat resized;
  cv::resize(rgb, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
  resized.copyTo(lb(cv::Rect(pad_x, pad_y, new_w, new_h)));

  // Feed to RKNN (uint8 RGB NHWC)
  rknn_input rin{};
  rin.index = 0;
  rin.type  = RKNN_TENSOR_UINT8;
  rin.size  = (uint32_t)(model_w_ * model_h_ * 3);
  rin.fmt   = RKNN_TENSOR_NHWC;
  rin.buf   = lb.data;

  int ret = rknn_inputs_set(ctx_, 1, &rin);
  if (ret < 0) {
    Logger::instance().log(LogLevel::WARN, "YoloTvDetector: rknn_inputs_set failed ret=%d", ret);
    return {};
  }

  ret = rknn_run(ctx_, nullptr);
  if (ret < 0) {
    Logger::instance().log(LogLevel::WARN, "YoloTvDetector: rknn_run failed ret=%d", ret);
    return {};
  }

  // Retrieve outputs as float32 (want_float=1 auto-dequantises quantised models)
  const uint32_t NO = std::min(n_outputs_, 3u);
  std::vector<rknn_output> outputs(NO);
  memset(outputs.data(), 0, NO * sizeof(rknn_output));
  for (uint32_t i = 0; i < NO; ++i) {
    outputs[i].want_float    = 1;
    outputs[i].is_prealloc   = 0;
  }
  ret = rknn_outputs_get(ctx_, NO, outputs.data(), nullptr);
  if (ret < 0) {
    Logger::instance().log(LogLevel::WARN, "YoloTvDetector: rknn_outputs_get failed ret=%d", ret);
    return {};
  }

  auto result = decode(outputs.data(), frame.width, frame.height, pad_x, pad_y, scale);
  rknn_outputs_release(ctx_, NO, outputs.data());
  return result;
}

// ── decode + NMS ─────────────────────────────────────────────────────────────
//
// YOLOX optimised model outputs (rknpu2 variant):
//   output[0]: [1, 85, 80, 80]  stride=8
//   output[1]: [1, 85, 40, 40]  stride=16
//   output[2]: [1, 85, 20, 20]  stride=32
//
// Decode (from zoo postprocess.cc, process_fp32 path):
//   box_x = (data[0*GHW + i*GW + j] + j) * stride   <- decoded cx
//   box_y = (data[1*GHW + i*GW + j] + i) * stride   <- decoded cy
//   box_w = exp(data[2*GHW + i*GW + j]) * stride
//   box_h = exp(data[3*GHW + i*GW + j]) * stride
//   obj_c = data[4*GHW + i*GW + j]
//   cls63 = data[(5+63)*GHW + i*GW + j]
//   conf  = obj_c * cls63
std::vector<RoiRect> YoloTvDetector::decode(rknn_output* outputs,
                                             int frame_w, int frame_h,
                                             int pad_x, int pad_y, float scale) {
  struct Det { float lx, ly, bw, bh, conf; };
  std::vector<Det> dets;

  for (int s = 0; s < 3; ++s) {
    auto* data = static_cast<float*>(outputs[s].buf);
    if (!data) continue;
    if (!out_dims_[s].valid) continue;

    const int gh     = out_dims_[s].grid_h;
    const int gw     = out_dims_[s].grid_w;
    if (gh <= 0 || gw <= 0) continue;

    const int glen   = gh * gw;
    const int stride = model_h_ / gh;
    // flat=[1,H*W,85] and NHWC=[1,H,W,85] both use row-major [off*85+ch] indexing.
    // NCHW=[1,85,H,W] uses [ch*glen+ch] indexing.
    const bool row_major = out_dims_[s].flat || (out_dims_[s].fmt == RKNN_TENSOR_NHWC);

    // Validate buffer size before ANY data access to prevent SIGSEGV.
    // Both NCHW [1,85,H,W] and flat/NHWC [1,H*W,85] require exactly 85*H*W floats.
    const size_t expected_bytes = (size_t)(85 * glen) * sizeof(float);
    if (outputs[s].size < expected_bytes) {
      Logger::instance().log(LogLevel::WARN, "[YOLO TV] output[%d] buffer too small: %u < %zu bytes — skipping",
              s, outputs[s].size, expected_bytes);
      continue;
    }

    // Format-aware channel accessor.
    // row-major (flat/NHWC): data[off * 85 + ch]
    // NCHW:                  data[ch  * glen + off]
    auto get = [&](int ch, int off) -> float {
      return row_major ? data[off * 85 + ch] : data[ch * glen + off];
    };
    // YOLOX fp32 rknn model outputs raw logits — apply sigmoid before thresholding.
    auto sigmoid = [](float x) -> float { return 1.0f / (1.0f + std::exp(-x)); };

    // Diagnostic: find max obj_conf in this head to check if values are sensible.
    float max_obj = -1e9f, max_cls = -1e9f;
    for (int k = 0; k < glen; ++k) {
      float v = sigmoid(get(4, k));
      if (v > max_obj) max_obj = v;
      float c = sigmoid(get(5 + COCO_TV_CLASS, k));
      if (c > max_cls) max_cls = c;
    }
    Logger::instance().log(LogLevel::INFO,
      "[YOLO TV] head[%d] stride=%d max_obj=%.3f max_tv_cls=%.3f thresh=%.2f",
      s, stride, max_obj, max_cls, conf_thresh_);

    for (int i = 0; i < gh; ++i) {
      for (int j = 0; j < gw; ++j) {
        const int off = i * gw + j;

        float obj_conf = sigmoid(get(4, off));
        if (obj_conf < conf_thresh_) continue;

        // Check only COCO class 63 ("tv") — no full argmax needed
        float cls_score = sigmoid(get(5 + COCO_TV_CLASS, off));
        if (cls_score < conf_thresh_) continue;

        float conf = obj_conf * cls_score;
        if (conf < conf_thresh_) continue;

        // Clamp raw exp() inputs to prevent float overflow (exp(>88) = +inf)
        float raw_bw = std::min(get(2, off), 8.0f);
        float raw_bh = std::min(get(3, off), 8.0f);

        // Decode centre + size back to letterbox pixel space
        float cx = (get(0, off) + j) * (float)stride;
        float cy = (get(1, off) + i) * (float)stride;
        float bw = std::exp(raw_bw) * stride;
        float bh = std::exp(raw_bh) * stride;

        dets.push_back({cx - bw * 0.5f, cy - bh * 0.5f, bw, bh, conf});
      }
    }
  }

  if (dets.empty()) {
    Logger::instance().log(LogLevel::WARN,
      "[YOLO TV] no detections above conf_thresh=%.2f after scanning all heads", conf_thresh_);
    return {};
  }

  // Sort by confidence descending for greedy NMS
  std::sort(dets.begin(), dets.end(),
    [](const Det& a, const Det& b) { return a.conf > b.conf; });

  // Greedy NMS (class-agnostic — we already filtered to one class)
  std::vector<bool> suppressed(dets.size(), false);
  for (size_t i = 0; i < dets.size(); ++i) {
    if (suppressed[i]) continue;
    for (size_t k = i + 1; k < dets.size(); ++k) {
      if (!suppressed[k] &&
          box_iou(dets[i].lx, dets[i].ly, dets[i].bw, dets[i].bh,
                  dets[k].lx, dets[k].ly, dets[k].bw, dets[k].bh) > NMS_IOU_THRESH) {
        suppressed[k] = true;
      }
    }
  }

  // Unmap letterbox → original frame coordinates
  std::vector<RoiRect> rois;
  for (size_t i = 0; i < dets.size(); ++i) {
    if (suppressed[i]) continue;
    const auto& d = dets[i];

    int x = (int)std::round((d.lx - pad_x) / scale);
    int y = (int)std::round((d.ly - pad_y) / scale);
    int w = (int)std::round(d.bw / scale);
    int h = (int)std::round(d.bh / scale);

    x = std::max(0, x);
    y = std::max(0, y);
    w = std::min(frame_w - x, w);
    h = std::min(frame_h - y, h);

    if (w < 20 || h < 10) continue;

    RoiRect roi;
    roi.x = x; roi.y = y; roi.w = w; roi.h = h;
    rois.push_back(roi);

    Logger::instance().log(LogLevel::INFO, "[YOLO TV] class=tv conf=%.2f box=%d,%d +%dx%d",
            d.conf, x, y, w, h);
  }

  // Summary line — always logged
  Logger::instance().log(LogLevel::INFO, "[YOLO TV] === Total: %zu TV(s) detected ===", rois.size());
  if (rois.size() == 2) {
    Logger::instance().log(LogLevel::INFO,
      "[YOLO TV] *** YOLOX detected TWO TVs: "
      "tv1=[x=%d y=%d w=%d h=%d]  tv2=[x=%d y=%d w=%d h=%d] ***",
      rois[0].x, rois[0].y, rois[0].w, rois[0].h,
      rois[1].x, rois[1].y, rois[1].w, rois[1].h);
  } else if (rois.size() == 1) {
    Logger::instance().log(LogLevel::WARN,
      "[YOLO TV] only 1 TV found [x=%d y=%d w=%d h=%d] — is the second TV in frame?",
      rois[0].x, rois[0].y, rois[0].w, rois[0].h);
  } else if (rois.empty()) {
    Logger::instance().log(LogLevel::WARN,
      "[YOLO TV] no TVs detected — check conf_thresh (%.2f) and model path", conf_thresh_);
  }

  return rois;
}

} // namespace wvm
