#include "wvm/gst_rtsp_capture.hpp"
#include "wvm/logger.hpp"
#include <cstring>
#include <chrono>

namespace wvm {

GstRtspCapture::GstRtspCapture(const DeviceConfig& cfg) : cfg_(cfg) {
  Logger::instance().log(LogLevel::INFO, "=== GstRtspCapture BUILD: %s %s ===", __DATE__, __TIME__);
  Logger::instance().log(LogLevel::INFO, "GstRtspCapture configured for RTSP URL: %s", cfg_.camera_device.c_str());
  Logger::instance().log(LogLevel::INFO, "  Target resolution: %dx%d @ %d fps", cfg_.width, cfg_.height, cfg_.fps);
  Logger::instance().log(LogLevel::INFO, "  Output format: YUYV");
  
  // Initialize GStreamer
  if (!gst_is_initialized()) {
    gst_init(nullptr, nullptr);
    Logger::instance().log(LogLevel::INFO, "GStreamer initialized");
  }
  
  frame_count_ = 0;
  last_stats_time_ = 0;
}

GstRtspCapture::~GstRtspCapture() {
  stop();
}

bool GstRtspCapture::init_pipeline() {
  fprintf(stderr, "[RTSP] Creating GStreamer pipeline for RTSP...\n");
  Logger::instance().log(LogLevel::INFO, "Creating GStreamer pipeline for RTSP...");
  
  // Create pipeline
  fprintf(stderr, "[RTSP] Creating pipeline object...\n");
  pipeline_ = gst_pipeline_new("rtsp-pipeline");
  if (!pipeline_) {
    fprintf(stderr, "[RTSP] ERROR: Failed to create GStreamer pipeline\n");
    Logger::instance().log(LogLevel::ERROR, "Failed to create GStreamer pipeline");
    return false;
  }
  fprintf(stderr, "[RTSP] Pipeline object created\n");
  
  // Create elements
  // rtspsrc for RTSP stream input
  fprintf(stderr, "[RTSP] Creating GStreamer elements...\n");
  fprintf(stderr, "[RTSP]   - rtspsrc\n");
  source_ = gst_element_factory_make("rtspsrc", "source");
  
  // rtph264depay for H.264 RTP depayloading (most common for RTSP)
  fprintf(stderr, "[RTSP]   - rtph264depay\n");
  depay_ = gst_element_factory_make("rtph264depay", "depay");
  
  // avdec_h264 for H.264 decoding
  fprintf(stderr, "[RTSP]   - avdec_h264\n");
  decoder_ = gst_element_factory_make("avdec_h264", "decoder");
  
  // videoconvert for format conversion
  fprintf(stderr, "[RTSP]   - videoconvert\n");
  convert_ = gst_element_factory_make("videoconvert", "convert");
  
  // videoscale for resolution scaling
  fprintf(stderr, "[RTSP]   - videoscale\n");
  GstElement* scale = gst_element_factory_make("videoscale", "scale");
  
  // appsink to receive frames in our application
  fprintf(stderr, "[RTSP]   - appsink\n");
  appsink_ = gst_element_factory_make("appsink", "sink");
  
  if (!source_ || !depay_ || !decoder_ || !convert_ || !scale || !appsink_) {
    fprintf(stderr, "[RTSP] ERROR: Failed to create GStreamer elements\n");
    fprintf(stderr, "[RTSP]   source=%p depay=%p decoder=%p convert=%p scale=%p appsink=%p\n",
      source_, depay_, decoder_, convert_, scale, appsink_);
    Logger::instance().log(LogLevel::ERROR, "Failed to create GStreamer elements");
    Logger::instance().log(LogLevel::ERROR, "  source=%p depay=%p decoder=%p convert=%p scale=%p appsink=%p",
      source_, depay_, decoder_, convert_, scale, appsink_);
    
    // Check which plugins are missing
    if (!source_) fprintf(stderr, "[RTSP] ERROR: rtspsrc element not found - missing gst-plugins-good or gst-plugins-bad?\n");
    if (!depay_) fprintf(stderr, "[RTSP] ERROR: rtph264depay element not found - missing gst-plugins-good?\n");
    if (!decoder_) fprintf(stderr, "[RTSP] ERROR: avdec_h264 element not found - missing gst-libav?\n");
    if (!convert_) fprintf(stderr, "[RTSP] ERROR: videoconvert element not found - missing gst-plugins-base?\n");
    if (!scale) fprintf(stderr, "[RTSP] ERROR: videoscale element not found - missing gst-plugins-base?\n");
    if (!appsink_) fprintf(stderr, "[RTSP] ERROR: appsink element not found - missing gst-plugins-base?\n");
    
    cleanup_pipeline();
    return false;
  }
  fprintf(stderr, "[RTSP] All elements created successfully\n");
  fprintf(stderr, "[RTSP] All elements created successfully\n");
  
  // Configure rtspsrc
  fprintf(stderr, "[RTSP] Configuring rtspsrc: %s\n", cfg_.camera_device.c_str());
  g_object_set(G_OBJECT(source_),
    "location", cfg_.camera_device.c_str(),
    "latency", 200,  // 200ms latency
    "protocols", 4,  // TCP (more reliable than UDP for many networks)
    "timeout", (guint64)5000000,  // 5 second timeout
    nullptr);
  fprintf(stderr, "[RTSP] rtspsrc configured\n");
  
  // Configure appsink
  fprintf(stderr, "[RTSP] Configuring appsink for %dx%d YUY2\n", cfg_.width, cfg_.height);
  GstCaps* caps = gst_caps_new_simple("video/x-raw",
    "format", G_TYPE_STRING, "YUY2",  // YUYV format
    "width", G_TYPE_INT, cfg_.width,
    "height", G_TYPE_INT, cfg_.height,
    nullptr);
  
  g_object_set(G_OBJECT(appsink_),
    "emit-signals", TRUE,
    "caps", caps,
    "max-buffers", 2,  // Keep queue small to minimize latency
    "drop", TRUE,  // Drop old frames if we can't process fast enough
    nullptr);
  
  gst_caps_unref(caps);
  fprintf(stderr, "[RTSP] appsink configured\n");
  
  // Set callback for new samples
  fprintf(stderr, "[RTSP] Setting appsink callbacks\n");
  GstAppSinkCallbacks callbacks = {nullptr, nullptr, on_new_sample};
  gst_app_sink_set_callbacks(GST_APP_SINK(appsink_), &callbacks, this, nullptr);
  
  // Add elements to pipeline
  fprintf(stderr, "[RTSP] Adding elements to pipeline\n");
  gst_bin_add_many(GST_BIN(pipeline_), source_, depay_, decoder_, convert_, scale, appsink_, nullptr);
  
  // Link elements (except source, which uses dynamic pads)
  fprintf(stderr, "[RTSP] Linking elements (depay -> decoder -> convert -> scale -> appsink)\n");
  if (!gst_element_link_many(depay_, decoder_, convert_, scale, appsink_, nullptr)) {
    fprintf(stderr, "[RTSP] ERROR: Failed to link GStreamer elements\n");
    Logger::instance().log(LogLevel::ERROR, "Failed to link GStreamer elements");
    cleanup_pipeline();
    return false;
  }
  fprintf(stderr, "[RTSP] Elements linked successfully\n");
  
  // Connect to pad-added signal for rtspsrc (dynamic pads)
  fprintf(stderr, "[RTSP] Connecting pad-added signal for dynamic linking\n");
  g_signal_connect(source_, "pad-added", G_CALLBACK(on_pad_added), depay_);
  
  // Add bus watch for error messages
  fprintf(stderr, "[RTSP] Adding bus watch for error messages\n");
  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
  gst_bus_add_watch(bus, on_bus_message, this);
  gst_object_unref(bus);
  
  fprintf(stderr, "[RTSP] GStreamer pipeline created successfully\n");
  Logger::instance().log(LogLevel::INFO, "GStreamer pipeline created successfully");
  return true;
}

void GstRtspCapture::cleanup_pipeline() {
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }
  source_ = nullptr;
  depay_ = nullptr;
  decoder_ = nullptr;
  convert_ = nullptr;
  appsink_ = nullptr;
}

bool GstRtspCapture::start() {
  fprintf(stderr, "[RTSP] Starting RTSP capture from: %s\n", cfg_.camera_device.c_str());
  Logger::instance().log(LogLevel::INFO, "Starting RTSP capture from: %s", cfg_.camera_device.c_str());
  
  fprintf(stderr, "[RTSP] Initializing pipeline...\n");
  if (!init_pipeline()) {
    fprintf(stderr, "[RTSP] ERROR: Pipeline initialization failed!\n");
    Logger::instance().log(LogLevel::ERROR, "Pipeline initialization failed");
    return false;
  }
  fprintf(stderr, "[RTSP] Pipeline initialized successfully\n");
  
  // Start playing
  fprintf(stderr, "[RTSP] Setting pipeline to PLAYING state...\n");
  Logger::instance().log(LogLevel::INFO, "Setting pipeline to PLAYING state...");
  GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    fprintf(stderr, "[RTSP] ERROR: Failed to start GStreamer pipeline (state change failed)\n");
    Logger::instance().log(LogLevel::ERROR, "Failed to start GStreamer pipeline");
    cleanup_pipeline();
    return false;
  }
  fprintf(stderr, "[RTSP] Pipeline state change return: %d\n", ret);
  Logger::instance().log(LogLevel::INFO, "Pipeline state change return: %d", ret);
  
  streaming_ = true;
  fprintf(stderr, "[RTSP] Stream started successfully\n");
  Logger::instance().log(LogLevel::INFO, "RTSP stream started successfully");
  return true;
}

void GstRtspCapture::stop() {
  if (!streaming_.exchange(false)) return;
  
  Logger::instance().log(LogLevel::INFO, "Stopping RTSP capture...");
  cleanup_pipeline();
  Logger::instance().log(LogLevel::INFO, "RTSP capture stopped");
}

bool GstRtspCapture::read_frame(CapturedFrame& out) {
  if (!streaming_) return false;
  
  std::lock_guard<std::mutex> lock(frame_mutex_);
  
  if (!frame_ready_) {
    return false;
  }
  
  out = std::move(current_frame_);
  frame_ready_ = false;
  
  // Update statistics
  frame_count_++;
  uint64_t now = now_ms();
  
  if (last_stats_time_ == 0) {
    last_stats_time_ = now;
  } else if (now - last_stats_time_ >= 10000) {  // Every 10 seconds
    double elapsed_sec = (now - last_stats_time_) / 1000.0;
    double fps = frame_count_ / elapsed_sec;
    Logger::instance().log(LogLevel::INFO, "RTSP capture stats: %.2f fps, %llu total frames", 
      fps, (unsigned long long)frame_count_);
    last_stats_time_ = now;
  }
  
  return true;
}

// Static callback when new sample is available
GstFlowReturn GstRtspCapture::on_new_sample(GstAppSink* appsink, gpointer user_data) {
  GstRtspCapture* self = static_cast<GstRtspCapture*>(user_data);
  
  GstSample* sample = gst_app_sink_pull_sample(appsink);
  if (!sample) {
    return GST_FLOW_ERROR;
  }
  
  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstCaps* caps = gst_sample_get_caps(sample);
  
  if (buffer && caps) {
    GstStructure* structure = gst_caps_get_structure(caps, 0);
    int width, height;
    gst_structure_get_int(structure, "width", &width);
    gst_structure_get_int(structure, "height", &height);
    
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      std::lock_guard<std::mutex> lock(self->frame_mutex_);
      
      self->current_frame_.ts_ms = now_ms();
      self->current_frame_.width = width;
      self->current_frame_.height = height;
      self->current_frame_.fmt = PixelFormat::YUYV;
      self->current_frame_.data.assign(map.data, map.data + map.size);
      self->frame_ready_ = true;
      
      gst_buffer_unmap(buffer, &map);
    }
  }
  
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

// Static callback for dynamic pad connection
void GstRtspCapture::on_pad_added(GstElement* element, GstPad* pad, gpointer user_data) {
  GstElement* depay = static_cast<GstElement*>(user_data);
  
  GstPad* sink_pad = gst_element_get_static_pad(depay, "sink");
  if (sink_pad) {
    if (!gst_pad_is_linked(sink_pad)) {
      GstPadLinkReturn ret = gst_pad_link(pad, sink_pad);
      if (ret != GST_PAD_LINK_OK) {
        Logger::instance().log(LogLevel::ERROR, "Failed to link dynamic pad");
      } else {
        Logger::instance().log(LogLevel::INFO, "Dynamic pad linked successfully");
      }
    }
    gst_object_unref(sink_pad);
  }
}

// Static callback for bus messages
gboolean GstRtspCapture::on_bus_message(GstBus* bus, GstMessage* msg, gpointer user_data) {
  GstRtspCapture* self = static_cast<GstRtspCapture*>(user_data);
  
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err;
      gchar* debug_info;
      gst_message_parse_error(msg, &err, &debug_info);
      Logger::instance().log(LogLevel::ERROR, "GStreamer error: %s", err->message);
      if (debug_info) {
        Logger::instance().log(LogLevel::ERROR, "Debug info: %s", debug_info);
      }
      g_clear_error(&err);
      g_free(debug_info);
      self->streaming_ = false;
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError* err;
      gchar* debug_info;
      gst_message_parse_warning(msg, &err, &debug_info);
      Logger::instance().log(LogLevel::WARN, "GStreamer warning: %s", err->message);
      g_clear_error(&err);
      g_free(debug_info);
      break;
    }
    case GST_MESSAGE_EOS:
      Logger::instance().log(LogLevel::INFO, "GStreamer EOS (End of Stream)");
      self->streaming_ = false;
      break;
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->pipeline_)) {
        GstState old_state, new_state, pending_state;
        gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
        Logger::instance().log(LogLevel::INFO, "Pipeline state changed from %s to %s",
          gst_element_state_get_name(old_state),
          gst_element_state_get_name(new_state));
      }
      break;
    default:
      break;
  }
  
  return TRUE;
}

} // namespace wvm
