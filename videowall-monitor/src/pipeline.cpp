#include "wvm/pipeline.hpp"
#include "wvm/v4l2_capture.hpp"
#include "wvm/logger.hpp"
#include <chrono>

namespace wvm {

Pipeline::Pipeline(const AppConfig& cfg)
: cfg_(cfg),
  cap_(std::make_unique<V4L2Capture>(cfg.device)),
  roi_(cfg.roi),
  pre_(cfg.model),
  mqtt_(cfg.mqtt) {}

Pipeline::~Pipeline() { stop(); }

bool Pipeline::start() {
  stop_ = false;

  Logger::instance().log(LogLevel::INFO, "Pipeline start. anomaly.enabled=%d", (int)cfg_.anomaly.enabled);

  if (!cap_->start()) return false;
  if (!npu_.load(cfg_.model)) return false;

  if (cfg_.reference.enabled) {
    if (!ref_.load_f32(cfg_.reference.embeddings_path, cfg_.model.embedding_dim, cfg_.reference.ref_fps))
      return false;

    matcher_ = std::make_unique<Matcher>(ref_, cfg_.anomaly.sample_fps, cfg_.reference.search_window_seconds);
    anomaly_ = std::make_unique<AnomalyEngine>(cfg_.anomaly, ref_.ref_fps());
  }

  if (!mqtt_.connect()) {
    Logger::instance().log(LogLevel::WARN, "MQTT connect failed (continuing)");
  }

  t_cap_ = std::thread(&Pipeline::capture_loop, this);
  t_pre_ = std::thread(&Pipeline::preprocess_loop, this);
  t_inf_ = std::thread(&Pipeline::inference_loop, this);

  if (cfg_.anomaly.enabled && cfg_.reference.enabled) {
    t_ana_ = std::thread(&Pipeline::analysis_loop, this);
  }

  if (cfg_.mqtt.enabled) {
    t_mqtt_ = std::thread(&Pipeline::mqtt_loop, this);
  }

  return true;
}

void Pipeline::stop() {
  if (stop_.exchange(true)) return;

  frame_q_.stop();
  infer_q_.stop();
  analyze_q_.stop();
  mqtt_q_.stop();

  if (t_cap_.joinable()) t_cap_.join();
  if (t_pre_.joinable()) t_pre_.join();
  if (t_inf_.joinable()) t_inf_.join();
  if (t_ana_.joinable()) t_ana_.join();
  if (t_mqtt_.joinable()) t_mqtt_.join();

  cap_->stop();
  mqtt_.disconnect();
}

void Pipeline::capture_loop() {
  Logger::instance().log(LogLevel::INFO, "Capture loop started");
  while (!stop_) {
    CapturedFrame f;
    if (cap_->read_frame(f)) {
      if (!frame_q_.push(std::move(f))) break;
    }
  }
}

void Pipeline::preprocess_loop() {
  Logger::instance().log(LogLevel::INFO, "Preprocess loop started");
  const int target_period_ms = 1000 / std::max(1, cfg_.anomaly.sample_fps);
  uint64_t last_emit = 0;

  while (!stop_) {
    auto of = frame_q_.pop();
    if (!of) break;

    // Rate-limit analysis frames to sample_fps
    if (last_emit && (of->ts_ms - last_emit) < (uint64_t)target_period_ms) {
      continue;
    }
    last_emit = of->ts_ms;

    RoiBatch batch;
    batch.ts_ms = of->ts_ms;
    batch.rois.reserve(roi_.tvs().size());

    for (auto& r : roi_.tvs()) {
      RoiInput ri;
      if (pre_.extract_roi_rgb224(*of, r, ri)) {
        batch.rois.push_back(std::move(ri));
      }
    }

    if (!infer_q_.push(std::move(batch))) break;
  }
}

void Pipeline::inference_loop() {
  Logger::instance().log(LogLevel::INFO, "Inference loop started");
  while (!stop_) {
    auto ob = infer_q_.pop();
    if (!ob) break;

    EmbeddingBatch eb;
    eb.ts_ms = ob->ts_ms;
    eb.embeddings.reserve(ob->rois.size());

    for (auto& ri : ob->rois) {
      std::vector<float> emb;
      if (npu_.infer(ri.rgb.data(), ri.w, ri.h, emb)) {
        Embedding e;
        e.tv_id = ri.tv_id;
        e.vec = std::move(emb);
        e.luma_mean = ri.luma_mean;
        e.luma_var = ri.luma_var;
        eb.embeddings.push_back(std::move(e));
      }
    }

    if (cfg_.anomaly.enabled && cfg_.reference.enabled) {
      if (!analyze_q_.push(std::move(eb))) break;
    }
  }
}

void Pipeline::analysis_loop() {
  Logger::instance().log(LogLevel::INFO, "Analysis loop started");
  while (!stop_) {
    auto oeb = analyze_q_.pop();
    if (!oeb) break;

    std::vector<MatchResult> matches;
    matches.reserve(oeb->embeddings.size());

    // 1) match each TV
    for (auto& e : oeb->embeddings) {
      auto mr = matcher_->match(e.tv_id, e.vec.data(), (int)e.vec.size());
      matches.push_back(mr);
    }

    // 2) consensus
    int k_wall = Matcher::median_k(matches);

    // 3) anomalies
    for (size_t i=0;i<oeb->embeddings.size();++i) {
      auto& e = oeb->embeddings[i];
      auto& mr = matches[i];
      auto evs = anomaly_->update(oeb->ts_ms, e.tv_id, e.luma_mean, e.luma_var,
                                 mr.k_best, mr.sim_best, k_wall);
      for (auto& ev : evs) mqtt_q_.push(std::move(ev));
    }
  }
}

void Pipeline::mqtt_loop() {
  Logger::instance().log(LogLevel::INFO, "MQTT loop started");
  while (!stop_) {
    auto oe = mqtt_q_.pop();
    if (!oe) break;
    mqtt_.publish(*oe);
  }
}

} // namespace wvm
