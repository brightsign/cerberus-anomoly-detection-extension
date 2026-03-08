#include "wvm/gst_rtsp_capture.hpp"
#include "wvm/logger.hpp"
#include <cstring>
#include <chrono>

namespace wvm {

// Helper to get current time in ms
static uint64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

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
  
  // Pre-allocate frame buffer (worst case: 1920x1080 YUY2 = 4MB)
  frame_buffer_.resize(1920 * 1080 * 2);
  
  frame_count_ = 0;
  last_stats_time_ = 0;
}

GstRtspCapture::~GstRtspCapture() {
  stop();
}

// Try hardware decoder first, fallback to software
GstElement* GstRtspCapture::create_decoder() {
  // Try hardware decoders in order of preference
  const char* hw_decoders[] = {
    "mpph264dec",      // Rockchip MPP decoder (best for RK3588)
    "v4l2h264dec",     // V4L2 stateless decoder
    "rkvdec",          // Older Rockchip decoder
    nullptr
  };
  
  for (int i = 0; hw_decoders[i]; i++) {
    GstElement* dec = gst_element_factory_make(hw_decoders[i], "decoder");
    if (dec) {
      fprintf(stderr, "[RTSP] Using hardware decoder: %s\n", hw_decoders[i]);
      Logger::instance().log(LogLevel::INFO, "Using hardware decoder: %s", hw_decoders[i]);
      return dec;
    }
  }
  
  // Fallback to software decoder
  fprintf(stderr, "[RTSP] No hardware decoder found, using avdec_h264 (software)\n");
  Logger::instance().log(LogLevel::WARN, "No hardware decoder found, using avdec_h264 (CPU decode)");
  return gst_element_factory_make("avdec_h264", "decoder");
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
  fprintf(stderr, "[RTSP] Creating GStreamer elements...\n");
  
  // rtspsrc for RTSP stream input
  fprintf(stderr, "[RTSP]   - rtspsrc\n");
  source_ = gst_element_factory_make("rtspsrc", "source");
  
  // rtph264depay for H.264 RTP depayloading
  fprintf(stderr, "[RTSP]   - rtph264depay\n");
  depay_ = gst_element_factory_make("rtph264depay", "depay");
  
  // h264parse for SPS/PPS stability (MUST-HAVE for reliable RTSP)
  fprintf(stderr, "[RTSP]   - h264parse\n");
  parser_ = gst_element_factory_make("h264parse", "parser");
  
  // H.264 decoder (try HW first, fallback to SW)
  fprintf(stderr, "[RTSP]   - H.264 decoder\n");
  decoder_ = create_decoder();
  
  // videoconvert for format conversion
  fprintf(stderr, "[RTSP]   - videoconvert\n");
  convert_ = gst_element_factory_make("videoconvert", "convert");
  
  // videoscale for resolution scaling
  fprintf(stderr, "[RTSP]   - videoscale\n");
  scale_ = gst_element_factory_make("videoscale", "scale");
  
  // capsfilter for explicit negotiation
  fprintf(stderr, "[RTSP]   - capsfilter\n");
  capsfilter_ = gst_element_factory_make("capsfilter", "capsfilter");
  
  // appsink to receive frames in our application
  fprintf(stderr, "[RTSP]   - appsink\n");
  appsink_ = gst_element_factory_make("appsink", "sink");
  
  // Check all elements created successfully
  if (!source_ || !depay_ || !parser_ || !decoder_ || !convert_ || !scale_ || !capsfilter_ || !appsink_) {
    fprintf(stderr, "[RTSP] ERROR: Failed to create GStreamer elements\n");
    fprintf(stderr, "[RTSP]   source=%p depay=%p parser=%p decoder=%p convert=%p scale=%p capsfilter=%p appsink=%p\n",
      source_, depay_, parser_, decoder_, convert_, scale_, capsfilter_, appsink_);
    Logger::instance().log(LogLevel::ERROR, "Failed to create GStreamer elements");
    
    // Check which plugins are missing
    if (!source_) fprintf(stderr, "[RTSP] ERROR: rtspsrc element not found - missing gst-plugins-good?\n");
    if (!depay_) fprintf(stderr, "[RTSP] ERROR: rtph264depay element not found - missing gst-plugins-good?\n");
    if (!parser_) fprintf(stderr, "[RTSP] ERROR: h264parse element not found - missing gst-plugins-bad?\n");
    if (!decoder_) fprintf(stderr, "[RTSP] ERROR: No H.264 decoder found!\n");
    if (!convert_) fprintf(stderr, "[RTSP] ERROR: videoconvert element not found - missing gst-plugins-base?\n");
    if (!scale_) fprintf(stderr, "[RTSP] ERROR: videoscale element not found - missing gst-plugins-base?\n");
    if (!capsfilter_) fprintf(stderr, "[RTSP] ERROR: capsfilter element not found - missing gst-plugins-base?\n");
    if (!appsink_) fprintf(stderr, "[RTSP] ERROR: appsink element not found - missing gst-plugins-base?\n");
    
    cleanup_pipeline();
    return false;
  }
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
  
  // Configure capsfilter with explicit format/size/framerate
  fprintf(stderr, "[RTSP] Configuring capsfilter for %dx%d YUY2 @ %d fps\n", cfg_.width, cfg_.height, cfg_.fps);
  GstCaps* caps = gst_caps_new_simple("video/x-raw",
    "format", G_TYPE_STRING, "YUY2",  // YUYV format
    "width", G_TYPE_INT, cfg_.width,
    "height", G_TYPE_INT, cfg_.height,
    "framerate", GST_TYPE_FRACTION, cfg_.fps, 1,
    nullptr);
  g_object_set(G_OBJECT(capsfilter_), "caps", caps, nullptr);
  gst_caps_unref(caps);
  fprintf(stderr, "[RTSP] capsfilter configured\n");
  
  // Configure appsink
  fprintf(stderr, "[RTSP] Configuring appsink\n");
  g_object_set(G_OBJECT(appsink_),
    "max-buffers", 2,  // Keep queue small to minimize latency
    "drop", TRUE,  // Drop old frames if we can't process fast enough
    nullptr);
  
  // Set callback for new samples
  fprintf(stderr, "[RTSP] Setting appsink callbacks\n");
  GstAppSinkCallbacks callbacks = {nullptr, nullptr, on_new_sample};
  gst_app_sink_set_callbacks(GST_APP_SINK(appsink_), &callbacks, this, nullptr);
  
  // Add elements to pipeline
  fprintf(stderr, "[RTSP] Adding elements to pipeline\n");
  gst_bin_add_many(GST_BIN(pipeline_), source_, depay_, parser_, decoder_, 
                   convert_, scale_, capsfilter_, appsink_, nullptr);
  
  // Link elements (except source, which uses dynamic pads)
  // Pipeline: depay -> parser -> decoder -> convert -> scale -> capsfilter -> appsink
  fprintf(stderr, "[RTSP] Linking elements\n");
  if (!gst_element_link_many(depay_, parser_, decoder_, convert_, scale_, capsfilter_, appsink_, nullptr)) {
    fprintf(stderr, "[RTSP] ERROR: Failed to link GStreamer elements\n");
    Logger::instance().log(LogLevel::ERROR, "Failed to link GStreamer elements");
    cleanup_pipeline();
    return false;
  }
  fprintf(stderr, "[RTSP] Elements linked successfully\n");
  
  // Connect to pad-added signal for rtspsrc (dynamic pads)
  fprintf(stderr, "[RTSP] Connecting pad-added signal for dynamic linking\n");
  g_signal_connect(source_, "pad-added", G_CALLBACK(on_pad_added), depay_);
  
  fprintf(stderr, "[RTSP] GStreamer pipeline created successfully\n");
  Logger::instance().log(LogLevel::INFO, "GStreamer pipeline created successfully");
  Logger::instance().log(LogLevel::INFO, "Pipeline: rtspsrc ! rtph264depay ! h264parse ! decoder ! videoconvert ! videoscale ! capsfilter ! appsink");
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
  parser_ = nullptr;
  decoder_ = nullptr;
  convert_ = nullptr;
  scale_ = nullptr;
  capsfilter_ = nullptr;
  appsink_ = nullptr;
}

// Dedicated thread for bus message handling (MUST-FIX #1)
void GstRtspCapture::bus_watch_thread() {
  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
  if (!bus) {
    Logger::instance().log(LogLevel::ERROR, "Failed to get pipeline bus");
    return;
  }
  
  fprintf(stderr, "[RTSP] Bus watch thread started\n");
  Logger::instance().log(LogLevel::INFO, "Bus watch thread started");
  
  while (bus_thread_running_) {
    // Poll for messages with 500ms timeout
    GstMessage* msg = gst_bus_timed_pop_filtered(bus, 500 * GST_MSECOND,
      static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | 
                                   GST_MESSAGE_WARNING | GST_MESSAGE_STATE_CHANGED));
    
    if (!msg) continue;
    
    switch (GST_MESSAGE_TYPE(msg)) {
      case GST_MESSAGE_ERROR: {
        GError* err;
        gchar* debug_info;
        gst_message_parse_error(msg, &err, &debug_info);
        fprintf(stderr, "[RTSP] ERROR from %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
        fprintf(stderr, "[RTSP] Debug: %s\n", debug_info ? debug_info : "none");
        Logger::instance().log(LogLevel::ERROR, "GStreamer error from %s: %s", 
          GST_OBJECT_NAME(msg->src), err->message);
        if (debug_info) {
          Logger::instance().log(LogLevel::ERROR, "Debug info: %s", debug_info);
        }
        g_error_free(err);
        g_free(debug_info);
        streaming_ = false;  // Signal error to stop()
        break;
      }
      
      case GST_MESSAGE_EOS:
        fprintf(stderr, "[RTSP] End-of-stream reached\n");
        Logger::instance().log(LogLevel::INFO, "End-of-stream reached");
        streaming_ = false;
        break;
      
      case GST_MESSAGE_WARNING: {
        GError* err;
        gchar* debug_info;
        gst_message_parse_warning(msg, &err, &debug_info);
        fprintf(stderr, "[RTSP] WARNING from %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
        Logger::instance().log(LogLevel::WARN, "GStreamer warning from %s: %s",
          GST_OBJECT_NAME(msg->src), err->message);
        g_error_free(err);
        g_free(debug_info);
        break;
      }
      
      case GST_MESSAGE_STATE_CHANGED: {
        // Only log pipeline state changes
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
          GstState old_state, new_state, pending_state;
          gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
          fprintf(stderr, "[RTSP] Pipeline state changed from %s to %s\n",
            gst_element_state_get_name(old_state), gst_element_state_get_name(new_state));
          Logger::instance().log(LogLevel::INFO, "Pipeline state: %s -> %s",
            gst_element_state_get_name(old_state), gst_element_state_get_name(new_state));
        }
        break;
      }
      
      default:
        break;
    }
    
    gst_message_unref(msg);
  }
  
  gst_object_unref(bus);
  fprintf(stderr, "[RTSP] Bus watch thread stopped\n");
  Logger::instance().log(LogLevel::INFO, "Bus watch thread stopped");
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
  
  // Start bus watch thread BEFORE playing (MUST-FIX #1)
  bus_thread_running_ = true;
  bus_thread_ = std::thread(&GstRtspCapture::bus_watch_thread, this);
  
  // Start playing
  fprintf(stderr, "[RTSP] Setting pipeline to PLAYING state...\n");
  Logger::instance().log(LogLevel::INFO, "Setting pipeline to PLAYING state...");
  GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    fprintf(stderr, "[RTSP] ERROR: Failed to start GStreamer pipeline (state change failed)\n");
    Logger::instance().log(LogLevel::ERROR, "Failed to start GStreamer pipeline");
    
    // Stop bus thread
    bus_thread_running_ = false;
    if (bus_thread_.joinable()) bus_thread_.join();
    
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
  
  // Stop bus watch thread
  bus_thread_running_ = false;
  if (bus_thread_.joinable()) {
    bus_thread_.join();
  }
  
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
    // Use GstVideoInfo for proper stride handling (MUST-FIX #4)
    GstVideoInfo video_info;
    if (!gst_video_info_from_caps(&video_info, caps)) {
      gst_sample_unref(sample);
      return GST_FLOW_ERROR;
    }
    
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      std::lock_guard<std::mutex> lock(self->frame_mutex_);
      
      self->current_frame_.ts_ms = now_ms();
      self->current_frame_.width = GST_VIDEO_INFO_WIDTH(&video_info);
      self->current_frame_.height = GST_VIDEO_INFO_HEIGHT(&video_info);
      self->current_frame_.fmt = PixelFormat::YUYV;
      
      // Handle stride properly (MUST-FIX #4)
      int width = self->current_frame_.width;
      int height = self->current_frame_.height;
      int stride = GST_VIDEO_INFO_PLANE_STRIDE(&video_info, 0);
      int expected_size = width * height * 2;  // YUY2 = 2 bytes per pixel
      
      // If stride == width*2, we can copy directly
      if (stride == width * 2) {
        // Reuse pre-allocated buffer (RECOMMENDED #8)
        if (self->frame_buffer_.size() < (size_t)expected_size) {
          self->frame_buffer_.resize(expected_size);
        }
        memcpy(self->frame_buffer_.data(), map.data, expected_size);
        self->current_frame_.data.assign(self->frame_buffer_.data(), 
                                         self->frame_buffer_.data() + expected_size);
      } else {
        // Stride differs, need to copy line-by-line
        fprintf(stderr, "[RTSP] WARNING: Frame has padding (stride=%d, expected=%d), copying line-by-line\n",
          stride, width * 2);
        
        if (self->frame_buffer_.size() < (size_t)expected_size) {
          self->frame_buffer_.resize(expected_size);
        }
        
        uint8_t* src = map.data;
        uint8_t* dst = self->frame_buffer_.data();
        int line_size = width * 2;
        
        for (int y = 0; y < height; y++) {
          memcpy(dst, src, line_size);
          src += stride;
          dst += line_size;
        }
        
        self->current_frame_.data.assign(self->frame_buffer_.data(),
                                         self->frame_buffer_.data() + expected_size);
      }
      
      self->frame_ready_ = true;
      
      gst_buffer_unmap(buffer, &map);
    }
  }
  
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

// Pad-added callback with proper filtering (MUST-FIX #2)
void GstRtspCapture::on_pad_added(GstElement* element, GstPad* pad, gpointer user_data) {
  GstElement* depay = static_cast<GstElement*>(user_data);
  
  // Get pad caps to check media type
  GstCaps* caps = gst_pad_query_caps(pad, nullptr);
  if (!caps) {
    fprintf(stderr, "[RTSP] Failed to get pad caps\n");
    return;
  }
  
  GstStructure* structure = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(structure);
  
  fprintf(stderr, "[RTSP] Pad added with caps: %s\n", name);
  
  // Only link RTP video pads with H.264 (MUST-FIX #2)
  if (g_str_has_prefix(name, "application/x-rtp")) {
    const gchar* media = gst_structure_get_string(structure, "media");
    const gchar* encoding = gst_structure_get_string(structure, "encoding-name");
    
    fprintf(stderr, "[RTSP] RTP pad: media=%s, encoding=%s\n", 
      media ? media : "unknown", encoding ? encoding : "unknown");
    
    if (media && g_strcmp0(media, "video") == 0 && 
        encoding && g_strcmp0(encoding, "H264") == 0) {
      
      // This is H.264 video, link it
      GstPad* sink_pad = gst_element_get_static_pad(depay, "sink");
      if (sink_pad) {
        if (gst_pad_is_linked(sink_pad)) {
          fprintf(stderr, "[RTSP] Depay sink already linked, ignoring\n");
        } else {
          GstPadLinkReturn ret = gst_pad_link(pad, sink_pad);
          if (ret == GST_PAD_LINK_OK) {
            fprintf(stderr, "[RTSP] H.264 video pad linked successfully\n");
          } else {
            fprintf(stderr, "[RTSP] ERROR: Failed to link H.264 video pad: %d\n", ret);
          }
        }
        gst_object_unref(sink_pad);
      }
    } else {
      fprintf(stderr, "[RTSP] Ignoring non-H.264 video pad\n");
    }
  } else {
    fprintf(stderr, "[RTSP] Ignoring non-RTP pad\n");
  }
  
  gst_caps_unref(caps);
}

} // namespace wvm
