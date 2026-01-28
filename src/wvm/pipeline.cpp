#include "wvm/pipeline.hpp"
#include "wvm/v4l2_capture.hpp"
#include "wvm/logger.hpp"
#include <chrono>

#include <algorithm>
#include <cmath>


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

  ref_auto_ = cfg_.reference.enabled && (cfg_.reference.mode == "auto");
  last_ref_append_ts_ = 0;
  ref_armed_ = false;

  if (cfg_.reference.enabled) {
    if (!ref_auto_) {
      // Offline (file) reference
      if (!ref_.load_f32(cfg_.reference.embeddings_path, cfg_.model.embedding_dim, cfg_.reference.ref_fps))
        return false;

      matcher_ = std::make_unique<Matcher>(ref_, cfg_.anomaly.sample_fps, cfg_.reference.search_window_seconds, /*start_at_end=*/false);
      anomaly_ = std::make_unique<AnomalyEngine>(cfg_.anomaly, ref_.ref_fps());
    } else {
      // Online (auto) reference: build while detecting.
      if (!ref_.init_online(cfg_.model.embedding_dim, cfg_.reference.ref_fps, cfg_.reference.auto_max_seconds))
        return false;

      matcher_ = std::make_unique<Matcher>(ref_, cfg_.anomaly.sample_fps, cfg_.reference.search_window_seconds, /*start_at_end=*/true);
      anomaly_ = std::make_unique<AnomalyEngine>(cfg_.anomaly, ref_.ref_fps());

      Logger::instance().log(LogLevel::INFO, "Reference auto mode enabled: source=%s ref_tv_id=%s min_ref_seconds=%.1f persist=%d path=%s",
                             cfg_.reference.auto_source.c_str(), cfg_.reference.ref_tv_id.c_str(),
                             cfg_.reference.auto_min_ref_seconds, (int)cfg_.reference.auto_persist,
                             cfg_.reference.auto_persist_path.c_str());

      if (cfg_.reference.auto_persist) {
        ref_record_ofs_.open(cfg_.reference.auto_persist_path, std::ios::binary | std::ios::out);
        if (!ref_record_ofs_) {
          Logger::instance().log(LogLevel::WARN, "Failed to open reference record file: %s (continuing without recording)",
                                 cfg_.reference.auto_persist_path.c_str());
        } else {
          Logger::instance().log(LogLevel::INFO, "Recording online reference embeddings to: %s", cfg_.reference.auto_persist_path.c_str());
        }
      }
    }
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

  if (cfg_.anomaly.enabled || ref_auto_) {
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

  if (ref_record_ofs_.is_open()) {
    ref_record_ofs_.flush();
    ref_record_ofs_.close();
  }

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
      // Online (auto) reference append (record + detect in a single run).
      if (ref_auto_) {
        const uint64_t period_ms = (uint64_t)(1000.0 / std::max(1, ref_.ref_fps()));
        const bool due = (!last_ref_append_ts_) || ((oeb->ts_ms - last_ref_append_ts_) >= period_ms);

        if (due && !oeb->embeddings.empty()) {
          // Choose which ROI to use as reference for this timestep.
          int ref_idx = -1;
          if (cfg_.reference.auto_source == "tv_id") {
            for (size_t i=0;i<oeb->embeddings.size();++i) {
              if (oeb->embeddings[i].tv_id == cfg_.reference.ref_tv_id) {
                ref_idx = (int)i;
                break;
              }
            }
          } else {
            // "consensus": choose the medoid embedding (max avg cosine similarity to others),
            // excluding obvious black frames.
            const float be_mean = cfg_.anomaly.black_enter_mean;
            const float be_var  = cfg_.anomaly.black_enter_var;

            std::vector<std::vector<float>> norm;
            norm.reserve(oeb->embeddings.size());
            std::vector<size_t> cand;
            cand.reserve(oeb->embeddings.size());

            auto l2_norm_inplace = [](float* v, int n) {
              double s=0.0; for (int i=0;i<n;i++) s += (double)v[i]*v[i];
              double inv = (s>1e-12)?(1.0/std::sqrt(s)):1.0;
              for (int i=0;i<n;i++) v[i] = (float)(v[i]*inv);
            };
            for (size_t i=0;i<oeb->embeddings.size();++i) {
              auto& e = oeb->embeddings[i];
              if (e.luma_mean < be_mean && e.luma_var < be_var) continue;
              norm.push_back(e.vec);
              l2_norm_inplace(norm.back().data(), (int)norm.back().size());
              cand.push_back(i);
            }
            if (!cand.empty()) {
              auto dot = [](const float* a, const float* b, int n) {
                double s=0.0; for (int i=0;i<n;i++) s += (double)a[i]*b[i]; return (float)s;
              };
              float best_score = -1e9f;
              size_t best_j = 0;
              for (size_t j=0;j<cand.size();++j) {
                float score=0.0f;
                for (size_t k=0;k<cand.size();++k) {
                  if (j==k) continue;
                  score += dot(norm[j].data(), norm[k].data(), (int)norm[j].size());
                }
                if (score > best_score) { best_score=score; best_j=j; }
              }
              ref_idx = (int)cand[best_j];
            }
          }

          if (ref_idx >= 0) {
            auto& re = oeb->embeddings[(size_t)ref_idx];
            int64_t k = ref_.append(re.vec.data(), (int)re.vec.size());
            last_ref_append_ts_ = oeb->ts_ms;

            if (k >= 0 && ref_record_ofs_.is_open()) {
              const float* vn = ref_.at_k(k);
              ref_record_ofs_.write(reinterpret_cast<const char*>(vn), sizeof(float) * (size_t)ref_.embedding_dim());
              if (!last_ref_flush_ts_) last_ref_flush_ts_ = oeb->ts_ms;
              const uint64_t flush_period_ms = 5000; // Flush every 5 seconds
              if ((oeb->ts_ms - last_ref_flush_ts_) >= flush_period_ms) {
                ref_record_ofs_.flush();
                last_ref_flush_ts_ = oeb->ts_ms;
              }
            }
          }
        }

        // Arm detection only after we have enough reference history.
        const int min_ref_frames = (int)std::max(1.0, std::round(cfg_.reference.auto_min_ref_seconds * ref_.ref_fps()));
        if (!ref_armed_ && ref_.count() >= min_ref_frames) {
          ref_armed_ = true;
          Logger::instance().log(LogLevel::INFO, "Reference armed: %d frames (%0.1fs) collected", ref_.count(), (double)ref_.count()/ref_.ref_fps());
        }
      }

      std::vector<MatchResult> matches;
      matches.reserve(oeb->embeddings.size());

      // 1) match each TV
      const int64_t k_wall_auto = (ref_.count() > 0) ? ref_.last_k() : -1;
      for (auto& e : oeb->embeddings) {
        // If the reference is coming from a designated TV, pin that TV to the current wall index.
        if (ref_auto_ && cfg_.reference.auto_source == "tv_id" && e.tv_id == cfg_.reference.ref_tv_id && k_wall_auto >= 0) {
          MatchResult mr;
          mr.k_best = k_wall_auto;
          mr.sim_best = 1.0f;
          matches.push_back(mr);
          continue;
        }
        auto mr = matcher_->match(e.tv_id, e.vec.data(), (int)e.vec.size());
        matches.push_back(mr);
      }

      // 2) wall position
      int64_t k_wall = -1;
      if (ref_auto_) {
        k_wall = k_wall_auto;
      } else {
        k_wall = Matcher::median_k(matches);
      }

      // 3) anomalies (BLACK always; content anomalies only when enabled/armed)
      const bool content_enabled = cfg_.anomaly.enabled && (!ref_auto_ || ref_armed_);
      for (size_t i=0;i<oeb->embeddings.size();++i) {
        auto& e = oeb->embeddings[i];
        auto& mr = matches[i];
        auto evs = anomaly_->update(oeb->ts_ms, e.tv_id, e.luma_mean, e.luma_var,
                                   mr.k_best, mr.sim_best, k_wall, content_enabled);
        for (auto& ev : evs) {
          Logger::instance().log(LogLevel::INFO, "MQTT_ENQUEUE type=%d tv=%s ts=%llu",
                                (int)ev.type, ev.tv_id.c_str(), (unsigned long long)ev.ts_ms);
          mqtt_q_.push(std::move(ev));
        }
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
        Logger::instance().log(LogLevel::INFO, "MQTT_ENQUEUE type=%d tv=%s ts=%llu",
                              (int)ev.type, ev.tv_id.c_str(), (unsigned long long)ev.ts_ms);
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
