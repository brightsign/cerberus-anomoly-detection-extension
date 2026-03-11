#include "wvm/config.hpp"
#include "wvm/logger.hpp"
#include <fstream>

// Expect nlohmann/json in your SDK; add it as a recipe dependency if needed.
#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace wvm {

static PixelFormat parse_pixfmt(const std::string& s) {
  if (s == "NV12") return PixelFormat::NV12;
  if (s == "RGB888") return PixelFormat::RGB888;
  return PixelFormat::YUYV;
}

bool load_config(const std::string& path, AppConfig& out) {
  std::ifstream ifs(path);
  if (!ifs) {
    Logger::instance().log(LogLevel::ERROR, "Failed to open config: %s", path.c_str());
    return false;
  }
  json j; ifs >> j;

  if (j.contains("device")) {
    auto d = j["device"];
    out.device.camera_device = d.value("camera_device", out.device.camera_device);
    out.device.width = d.value("width", out.device.width);
    out.device.height = d.value("height", out.device.height);
    out.device.fps = d.value("fps", out.device.fps);
    out.device.pixel_format = parse_pixfmt(d.value("pixel_format", "YUYV"));
  }

  if (j.contains("model")) {
    auto m = j["model"];
    out.model.rknn_path = m.value("rknn_path", out.model.rknn_path);
    out.model.input_w = m.value("input_w", out.model.input_w);
    out.model.input_h = m.value("input_h", out.model.input_h);
    out.model.embedding_dim = m.value("embedding_dim", out.model.embedding_dim);
  }

  if (j.contains("reference")) {
    auto r = j["reference"];
    out.reference.enabled = r.value("enabled", out.reference.enabled);
    out.reference.ref_fps = r.value("ref_fps", out.reference.ref_fps);
    out.reference.mode = r.value("mode", out.reference.mode);
    out.reference.embeddings_path = r.value("embeddings_path", out.reference.embeddings_path);
    out.reference.index_path = r.value("index_path", out.reference.index_path);
    out.reference.search_window_seconds = r.value("search_window_seconds", out.reference.search_window_seconds);

    // Auto mode
    out.reference.auto_source = r.value("auto_source", out.reference.auto_source);
    out.reference.ref_tv_id = r.value("ref_tv_id", out.reference.ref_tv_id);
    out.reference.auto_min_ref_seconds = r.value("auto_min_ref_seconds", out.reference.auto_min_ref_seconds);
    out.reference.auto_persist = r.value("auto_persist", out.reference.auto_persist);
    out.reference.auto_persist_path = r.value("auto_persist_path", out.reference.auto_persist_path);
    out.reference.auto_max_seconds = r.value("auto_max_seconds", out.reference.auto_max_seconds);
  }

  if (j.contains("roi")) {
    auto rr = j["roi"];
    out.roi.mode = rr.value("mode", out.roi.mode);
    out.roi.tvs.clear();

    // Manual rect ROIs
    if (rr.contains("tvs")) {
      for (auto& tv : rr["tvs"]) {
        RoiRect r;
        r.id = tv.value("id", "");
        r.x  = tv.value("x", 0);
        r.y  = tv.value("y", 0);
        r.w  = tv.value("w", 0);
        r.h  = tv.value("h", 0);
        out.roi.tvs.push_back(r);
      }
    }

    // Grid ROIs (auto-generated for mosaic streams)
    if (rr.contains("grid")) {
      auto g = rr["grid"];
      out.roi.grid.rows = g.value("rows", out.roi.grid.rows);
      out.roi.grid.cols = g.value("cols", out.roi.grid.cols);
      out.roi.grid.count = g.value("count", out.roi.grid.count);
      out.roi.grid.order = g.value("order", out.roi.grid.order);
    }
  }

  if (j.contains("anomaly")) {
    auto a = j["anomaly"];
    out.anomaly.enabled = a.value("enabled", out.anomaly.enabled);
    out.anomaly.sample_fps = a.value("sample_fps", out.anomaly.sample_fps);
    
    // BLACK detection with hysteresis (backward compatible with old config keys)
    out.anomaly.black_enter_mean = a.value("black_enter_mean", 
                                           a.value("black_luma_mean", out.anomaly.black_enter_mean));
    out.anomaly.black_exit_mean  = a.value("black_exit_mean", out.anomaly.black_exit_mean);
    out.anomaly.black_enter_var  = a.value("black_enter_var", 
                                           a.value("black_luma_var", out.anomaly.black_enter_var));
    out.anomaly.black_exit_var   = a.value("black_exit_var", out.anomaly.black_exit_var);
    out.anomaly.persist_black_ms = a.value("persist_black_ms", out.anomaly.persist_black_ms);
    out.anomaly.persist_black_recover_ms = a.value("persist_black_recover_ms", 
                                                    out.anomaly.persist_black_recover_ms);
    
    out.anomaly.persist_freeze_ms = a.value("persist_freeze_ms", out.anomaly.persist_freeze_ms);
    out.anomaly.persist_mismatch_ms = a.value("persist_mismatch_ms", out.anomaly.persist_mismatch_ms);
    out.anomaly.lag_threshold_s = a.value("lag_threshold_s", out.anomaly.lag_threshold_s);
    out.anomaly.stutter_window_ms = a.value("stutter_window_ms", out.anomaly.stutter_window_ms);
    out.anomaly.similarity_min = a.value("similarity_min", out.anomaly.similarity_min);
    
    // Basic anomaly detection (reference-less)
    out.anomaly.freeze_similarity = a.value("freeze_similarity", out.anomaly.freeze_similarity);
    if (a.contains("freeze_ignore_tvs") && a["freeze_ignore_tvs"].is_array()) {
      out.anomaly.freeze_ignore_tvs.clear();
      for (auto& v : a["freeze_ignore_tvs"]) {
        if (v.is_string()) out.anomaly.freeze_ignore_tvs.push_back(v.get<std::string>());
      }
    }
    out.anomaly.camera_timeout_ms = a.value("camera_timeout_ms", out.anomaly.camera_timeout_ms);
    out.anomaly.peer_enabled = a.value("peer_enabled", out.anomaly.peer_enabled);
    out.anomaly.peer_similarity_min = a.value("peer_similarity_min", out.anomaly.peer_similarity_min);
    out.anomaly.persist_outlier_ms = a.value("persist_outlier_ms", out.anomaly.persist_outlier_ms);
  }

  if (j.contains("health")) {
    auto h = j["health"];
    out.health.enabled = h.value("enabled", out.health.enabled);
    out.health.analysis_fps = h.value("analysis_fps", out.health.analysis_fps);
    out.health.dark_luma_threshold = h.value("dark_luma_threshold", out.health.dark_luma_threshold);
    
    out.health.black_enter_ratio = h.value("black_enter_ratio", out.health.black_enter_ratio);
    out.health.black_exit_ratio = h.value("black_exit_ratio", out.health.black_exit_ratio);
    out.health.black_var_enter = h.value("black_var_enter", out.health.black_var_enter);
    out.health.persist_black_ms = h.value("persist_black_ms", out.health.persist_black_ms);
    out.health.persist_recover_ms = h.value("persist_recover_ms", out.health.persist_recover_ms);
    
    out.health.off_mean = h.value("off_mean", out.health.off_mean);
    out.health.off_var = h.value("off_var", out.health.off_var);
    out.health.persist_off_ms = h.value("persist_off_ms", out.health.persist_off_ms);
    
    out.health.osd_mode = h.value("osd_mode", out.health.osd_mode);
    out.health.osd_prototypes_path = h.value("osd_prototypes_path", out.health.osd_prototypes_path);
    out.health.osd_sim_min = h.value("osd_sim_min", out.health.osd_sim_min);
    out.health.osd_sim_min_no_signal = h.value("osd_sim_min_no_signal", out.health.osd_sim_min_no_signal);
    out.health.osd_sim_min_wrong_input = h.value("osd_sim_min_wrong_input", out.health.osd_sim_min_wrong_input);
    out.health.persist_no_signal_ms = h.value("persist_no_signal_ms", out.health.persist_no_signal_ms);
    out.health.persist_osd_ms = h.value("persist_osd_ms", out.health.persist_osd_ms);

    out.health.heartbeat_interval_ms = h.value("heartbeat_interval_ms", out.health.heartbeat_interval_ms);

    out.health.prototype_capture = h.value("prototype_capture", out.health.prototype_capture);
    out.health.prototype_capture_seconds = h.value("prototype_capture_seconds", out.health.prototype_capture_seconds);
    out.health.prototype_output_path = h.value("prototype_output_path", out.health.prototype_output_path);
    if (h.contains("prototype_labels")) {
      for (auto& [tv_id, label] : h["prototype_labels"].items()) {
        out.health.prototype_labels[tv_id] = label.get<std::string>();
      }
    }
  }

  if (j.contains("mqtt")) {
    auto m = j["mqtt"];
    out.mqtt.enabled = m.value("enabled", out.mqtt.enabled);
    out.mqtt.host = m.value("host", out.mqtt.host);
    out.mqtt.port = m.value("port", out.mqtt.port);
    out.mqtt.topic = m.value("topic", out.mqtt.topic);
    out.mqtt.client_id = m.value("client_id", out.mqtt.client_id);
  }

  if (j.contains("logging")) {
    auto l = j["logging"];
    out.logging.level = l.value("level", out.logging.level);
    out.logging.path = l.value("path", out.logging.path);
  }

  return true;
}

} // namespace wvm
