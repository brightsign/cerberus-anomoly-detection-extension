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

  Logger::instance().log(LogLevel::INFO, "=== PIPELINE BUILD: %s %s ===", __DATE__, __TIME__);
  Logger::instance().log(LogLevel::INFO, "Pipeline start. anomaly.enabled=%d", (int)cfg_.anomaly.enabled);

  if (!cap_->start()) return false;
  if (!npu_.load(cfg_.model)) return false;

  if (cfg_.reference.enabled) {
    if (!ref_.load_f32(cfg_.reference.embeddings_path, cfg_.model.embedding_dim, cfg_.reference.ref_fps))
      return false;

    matcher_ = std::make_unique<Matcher>(ref_, cfg_.anomaly.sample_fps, cfg_.reference.search_window_seconds);
    anomaly_ = std::make_unique<AnomalyEngine>(cfg_.anomaly, ref_.ref_fps());
  } else {
    // Use basic anomaly detection (reference-less)
    basic_anomaly_ = std::make_unique<BasicAnomalyEngine>(cfg_.anomaly);
  }

  if (!mqtt_.connect()) {
    Logger::instance().log(LogLevel::WARN, "MQTT connect failed (continuing)");
  }

  t_cap_ = std::thread(&Pipeline::capture_loop, this);
  t_pre_ = std::thread(&Pipeline::preprocess_loop, this);
  t_inf_ = std::thread(&Pipeline::inference_loop, this);

  if (cfg_.anomaly.enabled) {
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
  int frame_count = 0;
  int loop_iterations = 0;
  while (!stop_) {
    loop_iterations++;
    if (loop_iterations == 1) {
      Logger::instance().log(LogLevel::INFO, "Capture loop: About to call read_frame for the first time");
    }
    if (loop_iterations % 500 == 0) {
      Logger::instance().log(LogLevel::INFO, "Capture loop: %d iterations, %d frames captured", loop_iterations, frame_count);
    }
    
    CapturedFrame f;
    if (cap_->read_frame(f)) {
      frame_count++;
      last_frame_ts_ = f.ts_ms; // Update last frame timestamp
      if (frame_count == 1) {
        Logger::instance().log(LogLevel::INFO, "Capture loop: First frame read from camera!");
      }
      if (frame_count <= 5) {
        Logger::instance().log(LogLevel::INFO, "Capture loop: Frame %d captured successfully", frame_count);
      }
      
      // Camera offline recovery
      if (camera_offline_active_) {
        camera_offline_active_ = false;
        Event e;
        e.ts_ms = f.ts_ms;
        e.tv_id = "camera";
        e.type = EventType::RECOVERED;
        e.details = "{\"from\":\"CAMERA_OFFLINE\"}";
        mqtt_q_.push(std::move(e));
        Logger::instance().log(LogLevel::INFO, "Camera online");
      }
      
      if (!frame_q_.push(std::move(f))) {
        Logger::instance().log(LogLevel::WARN, "Capture loop: Frame queue full, exiting");
        break;
      }
    }
  }
  Logger::instance().log(LogLevel::INFO, "Capture loop exited after %d iterations, %d frames", loop_iterations, frame_count);
}

void Pipeline::preprocess_loop() {
  Logger::instance().log(LogLevel::INFO, "Preprocess loop started");
  Logger::instance().log(LogLevel::INFO, "Preprocess: ROI count = %zu", roi_.tvs().size());
  
  const int target_period_ms = 1000 / std::max(1, cfg_.anomaly.sample_fps);
  uint64_t last_emit = 0;
  int batch_count = 0;

  while (!stop_) {
    auto of = frame_q_.pop();
    if (!of) break;

    // Rate-limit analysis frames to sample_fps
    if (last_emit && (of->ts_ms - last_emit) < (uint64_t)target_period_ms) {
      continue;
    }
    last_emit = of->ts_ms;

    batch_count++;
    RoiBatch batch;
    batch.ts_ms = of->ts_ms;
    batch.rois.reserve(roi_.tvs().size());

    if (batch_count <= 5) {
      Logger::instance().log(LogLevel::INFO, "Preprocess: Processing frame, ROI count = %zu", roi_.tvs().size());
    }

    for (auto& r : roi_.tvs()) {
      if (batch_count <= 5) {
        Logger::instance().log(LogLevel::INFO, "Preprocess: Extracting ROI id=%s x=%d y=%d w=%d h=%d", 
                              r.id.c_str(), r.x, r.y, r.w, r.h);
      }
      RoiInput ri;
      if (pre_.extract_roi_rgb224(*of, r, ri)) {
        if (batch_count <= 5) {
          Logger::instance().log(LogLevel::INFO, "Preprocess: ROI extracted successfully for %s", r.id.c_str());
        }
        batch.rois.push_back(std::move(ri));
      } else {
        if (batch_count <= 5) {
          Logger::instance().log(LogLevel::WARN, "Preprocess: Failed to extract ROI for %s", r.id.c_str());
        }
      }
    }

    if (batch_count <= 5) {
      Logger::instance().log(LogLevel::INFO, "Preprocess: Batch %d has %zu ROIs, pushing to infer_q", 
                            batch_count, batch.rois.size());
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

    // Push embeddings to analysis queue if anomaly detection is enabled (reference or basic mode)
    if (cfg_.anomaly.enabled) {
      if (!analyze_q_.push(std::move(eb))) break;
    }
  }
}

void Pipeline::analysis_loop() {
  Logger::instance().log(LogLevel::INFO, "Analysis loop started");
  int batch_count = 0;
  while (!stop_) {
    auto oeb = analyze_q_.pop();
    if (!oeb) break;
    
    batch_count++;
    if (batch_count <= 5 || batch_count % 20 == 0) {
      Logger::instance().log(LogLevel::INFO, "Analysis: Batch %d received with %zu embeddings", 
                            batch_count, oeb->embeddings.size());
    }

    // Check for camera offline (no frames received within timeout)
    uint64_t now = now_ms();
    if (cfg_.anomaly.enabled && !cfg_.reference.enabled && basic_anomaly_) {
      if (last_frame_ts_ > 0 && (now - last_frame_ts_) > (uint64_t)cfg_.anomaly.camera_timeout_ms) {
        if (!camera_offline_active_) {
          camera_offline_active_ = true;
          Event e;
          e.ts_ms = now;
          e.tv_id = "camera";
          e.type = EventType::CAMERA_OFFLINE;
          e.details = "{}";
          mqtt_q_.push(std::move(e));
          Logger::instance().log(LogLevel::WARN, "Camera offline (no frames for %dms)", 
                                cfg_.anomaly.camera_timeout_ms);
        }
      }
    }

    // Reference-based anomaly detection
    if (cfg_.reference.enabled && matcher_ && anomaly_) {
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
    // Basic anomaly detection (reference-less)
    else if (basic_anomaly_) {
      if (batch_count <= 5) {
        Logger::instance().log(LogLevel::INFO, "Analysis: Calling BasicAnomalyEngine::update_batch for batch %d", batch_count);
      }
      auto evs = basic_anomaly_->update_batch(*oeb);
      if (batch_count <= 5) {
        Logger::instance().log(LogLevel::INFO, "Analysis: BasicAnomalyEngine returned %zu events", evs.size());
      }
      for (auto& ev : evs) {
        Logger::instance().log(LogLevel::INFO, "BasicAnomaly: Event type=%d tv_id=%s", (int)ev.type, ev.tv_id.c_str());
        mqtt_q_.push(std::move(ev));
      }
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
