#include "wvm/gst_rtsp_capture.hpp"
#include "wvm/logger.hpp"
#include <cstring>
#include <chrono>
#include <algorithm>
#include <thread>
#include <unistd.h>

namespace wvm {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void gst_init_once() {
  static bool done = false;
  if (done) return;
  done = true;
  // Clear stale registry so plugins are rescanned with current GST_PLUGIN_PATH
  unlink("/tmp/gst-registry.bin");
  int argc = 0; char** argv = nullptr;
  gst_init(&argc, &argv);
  if (!getenv("GST_REGISTRY"))
    setenv("GST_REGISTRY", "/tmp/gst-registry.bin", 1);
  fprintf(stderr, "[RTSP] GStreamer initialized (BUILD %s %s)\n", __DATE__, __TIME__);
  fprintf(stderr, "[RTSP] GST_PLUGIN_PATH=%s\n", getenv("GST_PLUGIN_PATH") ? getenv("GST_PLUGIN_PATH") : "(not set)");
  fprintf(stderr, "[RTSP] GST_PLUGIN_SYSTEM_PATH=%s\n", getenv("GST_PLUGIN_SYSTEM_PATH") ? getenv("GST_PLUGIN_SYSTEM_PATH") : "(not set)");
  fprintf(stderr, "[RTSP] LD_LIBRARY_PATH=%s\n", getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "(not set)");

  // Check critical plugin availability immediately after init
  const char* critical_plugins[] = {
    "rtspsrc", "rtph264depay", "h264parse", "appsink",
    "videoconvert", "videoscale", "queue",
    "mpph264dec",   // Rockchip HW decoder
    "avdec_h264",   // SW fallback
    nullptr
  };
  fprintf(stderr, "[RTSP] --- Plugin availability check ---\n");
  for (int i = 0; critical_plugins[i]; i++) {
    GstElementFactory* f = gst_element_factory_find(critical_plugins[i]);
    fprintf(stderr, "[RTSP]   %s %s\n", f ? "✓" : "✗", critical_plugins[i]);
    if (f) gst_object_unref(f);
  }
  fprintf(stderr, "[RTSP] --- End plugin check ---\n");
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
GstRtspCapture::GstRtspCapture(const DeviceConfig& cfg) : cfg_(cfg) {
  // NOTE: gst_init_once() is intentionally NOT called here.
  // It is deferred to start() so that GStreamer is only initialised when
  // the pipeline actually needs to run (avoids crashes at boot if
  // GStreamer libraries/plugins aren't fully mapped yet).
  frame_buffer_.resize(1920 * 1080 * 2);
  Logger::instance().log(LogLevel::INFO,
    "GstRtspCapture: url=%s target=%dx%d@%dfps",
    cfg_.camera_device.c_str(), cfg_.width, cfg_.height, cfg_.fps);
  fprintf(stderr, "[RTSP] GstRtspCapture constructed (gst_init deferred to start())\n");
}

GstRtspCapture::~GstRtspCapture() {
  stop();
}

// ---------------------------------------------------------------------------
// Build pipeline string candidates (argus pattern: try multiple variants)
// ---------------------------------------------------------------------------
std::vector<std::string> GstRtspCapture::build_pipeline_candidates() const {
  const std::string& url = cfg_.camera_device;
  const int w   = cfg_.width;
  const int h   = cfg_.height;

  // Output caps: YUY2 at the target size but NO framerate constraint —
  // a hard framerate= causes negotiation failure when the camera reports
  // a fractional or slightly different rate (e.g. 30000/1001 vs 30/1).
  auto caps_str = [&](bool with_scale) -> std::string {
    if (with_scale) {
      return " videoscale ! videoconvert"
             " ! video/x-raw,format=YUY2,width=" + std::to_string(w) +
             ",height=" + std::to_string(h) +
             " ! queue max-size-buffers=2 leaky=downstream"
             " ! appsink name=mysink drop=true max-buffers=2 sync=false enable-last-sample=false";
    }
    return " videoconvert"
           " ! video/x-raw,format=YUY2,width=" + std::to_string(w) +
           ",height=" + std::to_string(h) +
           " ! queue max-size-buffers=2 leaky=downstream"
           " ! appsink name=mysink drop=true max-buffers=2 sync=false enable-last-sample=false";
  };

  // rtspsrc with short connection timeout so failed attempts fail quickly
  auto src = [&](const char* proto) -> std::string {
    return std::string("rtspsrc location=") + url +
           " protocols=" + proto +
           " latency=100 drop-on-latency=true"
           " tcp-timeout=5000000000 timeout=8000000000"
           " retry=1 do-rtsp-keep-alive=true name=src"
           " src. ! ";
  };

  // NOTE: mpph264dec (RK3588 HW) confirmed absent on this device — skip it.
  // avdec_h264 (SW) is confirmed present.
  std::vector<std::string> p;

  // 1. TCP + H264 + SW decoder (no scale — only works if stream matches target res)
  p.push_back(src("tcp") +
    "application/x-rtp,media=video,encoding-name=H264 !"
    " rtph264depay ! h264parse config-interval=1 !"
    " avdec_h264 !" + caps_str(false));

  // 2. TCP + H264 + SW decoder + videoscale (handles any resolution)
  p.push_back(src("tcp") +
    "application/x-rtp,media=video,encoding-name=H264 !"
    " rtph264depay ! h264parse config-interval=1 !"
    " avdec_h264 !" + caps_str(true));

  // 3. TCP + decodebin + scale (auto-detect codec, most flexible)
  p.push_back(src("tcp") +
    "decodebin !" + caps_str(true));

  // 4. UDP + SW decoder + scale (some cameras prefer UDP)
  p.push_back(src("udp") +
    "application/x-rtp,media=video,encoding-name=H264 !"
    " rtph264depay ! h264parse config-interval=1 !"
    " avdec_h264 !" + caps_str(true));

  return p;
}

// ---------------------------------------------------------------------------
// open_pipeline: try one pipeline string; validate with first-frame pull
// ---------------------------------------------------------------------------
bool GstRtspCapture::open_pipeline(const std::string& pipeline_str, int first_frame_timeout_ms) {
  fprintf(stderr, "[RTSP] Trying pipeline: %s\n", pipeline_str.c_str());

  GError* err = nullptr;
  pipeline_ = gst_parse_launch(pipeline_str.c_str(), &err);
  if (!pipeline_) {
    fprintf(stderr, "[RTSP]   gst_parse_launch FAILED: %s\n", err ? err->message : "(unknown)");
    if (err) g_error_free(err);
    return false;
  }
  if (err) {
    fprintf(stderr, "[RTSP]   gst_parse_launch WARNING: %s\n", err->message);
    g_error_free(err);
  }
  fprintf(stderr, "[RTSP]   gst_parse_launch OK\n");

  appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mysink");
  if (!appsink_) {
    fprintf(stderr, "[RTSP]   ERROR: appsink 'mysink' not found – elements failed to link\n");
    gst_object_unref(pipeline_); pipeline_ = nullptr;
    return false;
  }
  fprintf(stderr, "[RTSP]   appsink found\n");

  // Configure appsink (pipeline string already sets most, belt-and-braces here)
  g_object_set(G_OBJECT(appsink_),
    "drop",               (gboolean)TRUE,
    "max-buffers",        (guint)2,
    "enable-last-sample", (gboolean)FALSE,
    "sync",               (gboolean)FALSE,
    nullptr);

  bus_ = gst_element_get_bus(pipeline_);
  broken_.store(false, std::memory_order_release);
  bus_running_.store(true, std::memory_order_release);
  bus_thread_ = std::thread(&GstRtspCapture::bus_watch_thread, this);

  fprintf(stderr, "[RTSP]   Setting pipeline to PLAYING...\n");
  GstStateChangeReturn scr = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  fprintf(stderr, "[RTSP]   set_state(PLAYING) returned: %d (%s)\n", scr,
    scr == GST_STATE_CHANGE_SUCCESS  ? "SUCCESS" :
    scr == GST_STATE_CHANGE_ASYNC    ? "ASYNC" :
    scr == GST_STATE_CHANGE_FAILURE  ? "FAILURE" :
    scr == GST_STATE_CHANGE_NO_PREROLL ? "NO_PREROLL" : "?");

  if (scr == GST_STATE_CHANGE_FAILURE) {
    fprintf(stderr, "[RTSP]   Pipeline failed to start – aborting this candidate\n");
    bus_running_.store(false, std::memory_order_release);
    if (bus_thread_.joinable()) bus_thread_.join();
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(appsink_); appsink_ = nullptr;
    gst_object_unref(bus_);     bus_     = nullptr;
    gst_object_unref(pipeline_); pipeline_ = nullptr;
    return false;
  }

  // Wait for first frame — poll in short slices so we catch bus errors fast
  // instead of blocking the full timeout on a stuck TCP connection.
  fprintf(stderr, "[RTSP]   Waiting up to %d ms for first frame (polling every 200ms)...\n",
          first_frame_timeout_ms);

  GstSample* sample = nullptr;
  const int poll_ms   = 200;
  const int max_polls = first_frame_timeout_ms / poll_ms;

  for (int p = 0; p < max_polls && !sample; p++) {
    // Check bus for errors before pulling — exits the loop immediately on error
    GstMessage* msg = gst_bus_timed_pop_filtered(bus_, 0,
      static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_WARNING));
    if (msg) {
      if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* e = nullptr; gchar* dbg = nullptr;
        gst_message_parse_error(msg, &e, &dbg);
        fprintf(stderr, "[RTSP]   Bus ERROR at poll %d/%d: %s | %s\n",
                p+1, max_polls, e ? e->message : "?", dbg ? dbg : "");
        if (e) g_error_free(e); if (dbg) g_free(dbg);
        gst_message_unref(msg);
        broken_.store(true, std::memory_order_release);
        break;  // No point waiting further
      } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
        GError* e = nullptr; gchar* dbg = nullptr;
        gst_message_parse_warning(msg, &e, &dbg);
        fprintf(stderr, "[RTSP]   Bus WARNING at poll %d/%d: %s\n",
                p+1, max_polls, e ? e->message : "?");
        if (e) g_error_free(e); if (dbg) g_free(dbg);
        gst_message_unref(msg);
        // Warnings don't abort — keep waiting
      } else {
        // EOS
        fprintf(stderr, "[RTSP]   Bus EOS at poll %d/%d\n", p+1, max_polls);
        gst_message_unref(msg);
        broken_.store(true, std::memory_order_release);
        break;
      }
    }

    if (broken_.load(std::memory_order_acquire)) break;

    // Non-blocking pull with 200ms window
    sample = gst_app_sink_try_pull_sample(
      GST_APP_SINK(appsink_), (GstClockTime)poll_ms * GST_MSECOND);

    if (!sample && (p % 5 == 0)) {
      fprintf(stderr, "[RTSP]   Still waiting... poll %d/%d, broken_=%d\n",
              p+1, max_polls, (int)broken_.load());
    }
  }

  if (!sample) {
    fprintf(stderr, "[RTSP]   First-frame FAILED after %d ms – broken_=%d (bus error or timeout)\n",
            first_frame_timeout_ms, (int)broken_.load());
    bus_running_.store(false, std::memory_order_release);
    if (bus_thread_.joinable()) bus_thread_.join();
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(appsink_); appsink_ = nullptr;
    gst_object_unref(bus_);     bus_     = nullptr;
    gst_object_unref(pipeline_); pipeline_ = nullptr;
    broken_.store(false, std::memory_order_release);
    return false;
  }

  // Get actual dimensions from first frame caps
  GstCaps* scaps = gst_sample_get_caps(sample);
  if (scaps) {
    const GstStructure* s = gst_caps_get_structure(scaps, 0);
    int actual_w = 0, actual_h = 0;
    gst_structure_get_int(s, "width",  &actual_w);
    gst_structure_get_int(s, "height", &actual_h);
    gchar* caps_str_dbg = gst_caps_to_string(scaps);
    fprintf(stderr, "[RTSP] ✅ First frame received: %dx%d  caps=%s\n",
            actual_w, actual_h, caps_str_dbg ? caps_str_dbg : "?");
    if (caps_str_dbg) g_free(caps_str_dbg);
    Logger::instance().log(LogLevel::INFO, "RTSP stream open: %dx%d", actual_w, actual_h);
  }
  gst_sample_unref(sample);

  return true;
}

// ---------------------------------------------------------------------------
// close_pipeline: tear down safely (no locks needed – caller serialises)
// ---------------------------------------------------------------------------
void GstRtspCapture::close_pipeline() {
  bus_running_.store(false, std::memory_order_release);
  if (bus_thread_.joinable()) bus_thread_.join();

  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_); pipeline_ = nullptr;
  }
  if (appsink_) { gst_object_unref(appsink_); appsink_ = nullptr; }
  if (bus_)     { gst_object_unref(bus_);     bus_     = nullptr; }

  broken_.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// bus_watch_thread
// ---------------------------------------------------------------------------
void GstRtspCapture::bus_watch_thread() {
  fprintf(stderr, "[RTSP] Bus watch thread started\n");
  while (bus_running_.load(std::memory_order_acquire)) {
    GstMessage* msg = gst_bus_timed_pop_filtered(bus_, 300 * GST_MSECOND,
      static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_WARNING));
    if (!msg) continue;

    switch (GST_MESSAGE_TYPE(msg)) {
      case GST_MESSAGE_ERROR: {
        GError* e = nullptr; gchar* dbg = nullptr;
        gst_message_parse_error(msg, &e, &dbg);
        fprintf(stderr, "[RTSP] GST_ERROR: %s\n", e ? e->message : "(unknown)");
        if (dbg) { fprintf(stderr, "[RTSP] Debug: %s\n", dbg); g_free(dbg); }
        if (e) g_error_free(e);
        broken_.store(true, std::memory_order_release);
        break;
      }
      case GST_MESSAGE_EOS:
        fprintf(stderr, "[RTSP] GST_EOS\n");
        broken_.store(true, std::memory_order_release);
        break;
      case GST_MESSAGE_WARNING: {
        GError* e = nullptr; gchar* dbg = nullptr;
        gst_message_parse_warning(msg, &e, &dbg);
        fprintf(stderr, "[RTSP] GST_WARNING: %s\n", e ? e->message : "(unknown)");
        if (dbg) g_free(dbg);
        if (e)   g_error_free(e);
        break;
      }
      default: break;
    }
    gst_message_unref(msg);
  }
  fprintf(stderr, "[RTSP] Bus watch thread stopped\n");
}

// ---------------------------------------------------------------------------
// start(): try each pipeline candidate; retry with backoff on failure
// ---------------------------------------------------------------------------
bool GstRtspCapture::start() {
  // Initialise GStreamer here (not in constructor) so that libraries and
  // plugins are guaranteed to be mapped before we touch them.
  gst_init_once();

  fprintf(stderr, "[RTSP] start(): url=%s\n", cfg_.camera_device.c_str());
  fprintf(stderr, "[RTSP] start(): target=%dx%d@%dfps format=YUY2\n",
          cfg_.width, cfg_.height, cfg_.fps);
  Logger::instance().log(LogLevel::INFO, "Starting RTSP capture: %s", cfg_.camera_device.c_str());

  auto candidates = build_pipeline_candidates();
  fprintf(stderr, "[RTSP] %zu pipeline candidates to try\n", candidates.size());

  // 1 round only at startup — if none of the candidates work, exit cleanly
  // so the bsext_init wrapper stops and you can SSH in to read the logs.
  // (Increase to 3+ once the correct pipeline string is confirmed working.)
  const int max_outer = 1;
  int round = 0;

  while (round < max_outer) {
    for (size_t i = 0; i < candidates.size(); i++) {
      fprintf(stderr, "[RTSP] --- Candidate %zu/%zu (round %d/%d) ---\n",
              i + 1, candidates.size(), round + 1, max_outer);
      if (open_pipeline(candidates[i], /*first_frame_timeout_ms=*/5000)) {
        fprintf(stderr, "[RTSP] ✅ Pipeline %zu opened successfully\n", i + 1);
        Logger::instance().log(LogLevel::INFO, "RTSP pipeline %zu opened successfully", i + 1);
        return true;
      }
      fprintf(stderr, "[RTSP] ✗ Candidate %zu failed\n", i + 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));  // brief gap between candidates
    }

    round++;
    fprintf(stderr, "[RTSP] All %zu candidates failed (round %d/%d)\n",
            candidates.size(), round, max_outer);
  }

  fprintf(stderr, "[RTSP] ❌ All candidates failed after %d rounds. Exiting to stop restart loop.\n", max_outer);
  fprintf(stderr, "[RTSP] ❌ Check: is rtsp://%s reachable? Is the RTSP server running?\n",
          cfg_.camera_device.c_str());
  Logger::instance().log(LogLevel::ERROR,
    "Failed to open RTSP stream after %d rounds — exiting with code 1", max_outer);
  // Write crash marker so it's visible after exit
  FILE* mf = fopen("/tmp/anomaly_crash.txt", "w");
  if (mf) {
    fprintf(mf, "RTSP failed after %d rounds: %s\n", max_outer, cfg_.camera_device.c_str());
    fprintf(mf, "Check /tmp/anomaly_detection.log for [RTSP] lines\n");
    fclose(mf);
  }
  fflush(stderr);
  _exit(1);  // Hard exit — stops the bsext_init restart loop so you can SSH in
  return false; // unreachable
}

// ---------------------------------------------------------------------------
// stop()
// ---------------------------------------------------------------------------
void GstRtspCapture::stop() {
  fprintf(stderr, "[RTSP] stop() called\n");
  close_pipeline();
  Logger::instance().log(LogLevel::INFO, "RTSP capture stopped");
  fprintf(stderr, "[RTSP] stop() complete\n");
}

// ---------------------------------------------------------------------------
// read_frame(): pull next frame from appsink (called from capture_loop)
// ---------------------------------------------------------------------------
bool GstRtspCapture::read_frame(CapturedFrame& out) {
  if (!appsink_) {
    fprintf(stderr, "[RTSP] read_frame(): appsink_ is NULL\n");
    return false;
  }
  if (broken_.load(std::memory_order_acquire)) {
    static std::atomic<int> broken_log_count{0};
    int n = broken_log_count.fetch_add(1, std::memory_order_relaxed);
    if (n < 5) {
      fprintf(stderr, "[RTSP] read_frame(): broken_ is SET (#%d), returning false\n", n + 1);
    } else if (n == 5) {
      fprintf(stderr, "[RTSP] read_frame(): broken_ log suppressed after 5\n");
    }
    return false;
  }

  // Non-blocking pull (100ms timeout)
  GstSample* sample = gst_app_sink_try_pull_sample(
    GST_APP_SINK(appsink_), 100 * GST_MSECOND);
  if (!sample) return false;

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstCaps*   caps   = gst_sample_get_caps(sample);

  if (!buffer || !caps) {
    gst_sample_unref(sample);
    return false;
  }

  GstVideoInfo vinfo;
  if (!gst_video_info_from_caps(&vinfo, caps)) {
    gst_sample_unref(sample);
    return false;
  }

  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return false;
  }

  const int width  = GST_VIDEO_INFO_WIDTH(&vinfo);
  const int height = GST_VIDEO_INFO_HEIGHT(&vinfo);
  const int stride = GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 0);
  const int expected_size = width * height * 2;  // YUY2

  if (frame_buffer_.size() < (size_t)expected_size)
    frame_buffer_.resize(expected_size);

  if (stride == width * 2) {
    memcpy(frame_buffer_.data(), map.data, expected_size);
  } else {
    // Copy line-by-line to strip padding
    const uint8_t* src = map.data;
    uint8_t*       dst = frame_buffer_.data();
    const int line_bytes = width * 2;
    for (int y = 0; y < height; y++) {
      memcpy(dst, src, line_bytes);
      src += stride;
      dst += line_bytes;
    }
  }

  gst_buffer_unmap(buffer, &map);
  gst_sample_unref(sample);

  out.ts_ms  = now_ms();
  out.width  = width;
  out.height = height;
  out.fmt    = PixelFormat::YUYV;
  out.data.assign(frame_buffer_.data(), frame_buffer_.data() + expected_size);

  // Log first few frames delivered to confirm pipeline is working end-to-end
  static std::atomic<int> first_frame_log{0};
  int fn = first_frame_log.fetch_add(1, std::memory_order_relaxed);
  if (fn < 3) {
    fprintf(stderr, "[RTSP] read_frame(): ✅ frame #%d delivered %dx%d stride=%d size=%d\n",
            fn + 1, width, height, stride, expected_size);
  }

  // Statistics every 10s
  frame_count_++;
  uint64_t now = out.ts_ms;
  if (last_stats_time_ == 0) {
    last_stats_time_ = now;
  } else if (now - last_stats_time_ >= 10000) {
    double elapsed = (now - last_stats_time_) / 1000.0;
    Logger::instance().log(LogLevel::INFO,
      "RTSP capture: %.1f fps (%llu frames total)", frame_count_ / elapsed,
      (unsigned long long)frame_count_);
    last_stats_time_ = now;
  }

  return true;
}

} // namespace wvm
