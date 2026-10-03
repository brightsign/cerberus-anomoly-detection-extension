#include "wvm/v4l2_capture.hpp"
#include "wvm/logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cstring>
#include <cerrno>
#include <vector>
#include <algorithm>

namespace wvm {

static uint32_t to_v4l2_pixfmt(PixelFormat fmt) {
  switch (fmt) {
    case PixelFormat::NV12: return V4L2_PIX_FMT_NV12;
    case PixelFormat::RGB888: return V4L2_PIX_FMT_RGB24;
    case PixelFormat::YUYV:
    default: return V4L2_PIX_FMT_YUYV;
  }
}

static std::string fourcc_to_str(uint32_t f) {
  char s[5];
  s[0] = f & 0xFF;
  s[1] = (f >> 8) & 0xFF;
  s[2] = (f >> 16) & 0xFF;
  s[3] = (f >> 24) & 0xFF;
  s[4] = 0;
  return std::string(s);
}

static bool enum_formats(int fd, std::vector<uint32_t>& out, bool mplane) {
  out.clear();
  v4l2_fmtdesc fmtd{};
  fmtd.type = mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                     : V4L2_BUF_TYPE_VIDEO_CAPTURE;
  for (fmtd.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmtd) == 0; fmtd.index++) {
    out.push_back(fmtd.pixelformat);
  }
  return !out.empty();
}

static bool enum_framesizes(int fd, uint32_t pixfmt, std::vector<std::pair<int,int>>& out) {
  out.clear();
  v4l2_frmsizeenum fse{};
  fse.pixel_format = pixfmt;
  for (fse.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fse) == 0; fse.index++) {
    if (fse.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
      out.emplace_back((int)fse.discrete.width, (int)fse.discrete.height);
    } else if (fse.type == V4L2_FRMSIZE_TYPE_STEPWISE || fse.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
      // Choose a sensible representative set (min/max); we'll validate by S_FMT + G_FMT later.
      out.emplace_back((int)fse.stepwise.min_width, (int)fse.stepwise.min_height);
      out.emplace_back((int)fse.stepwise.max_width, (int)fse.stepwise.max_height);
      break;
    }
  }
  return !out.empty();
}

V4L2Capture::V4L2Capture(const DeviceConfig& cfg) : cfg_(cfg) {
  Logger::instance().log(LogLevel::INFO, "=== V4L2Capture BUILD: %s %s ===", __DATE__, __TIME__);
  Logger::instance().log(LogLevel::INFO, "V4L2Capture configured for device: %s", cfg_.camera_device.c_str());
  Logger::instance().log(LogLevel::INFO, "  Resolution: %dx%d @ %d fps", cfg_.width, cfg_.height, cfg_.fps);
  Logger::instance().log(LogLevel::INFO, "  Pixel format: %s", 
    cfg_.pixel_format == PixelFormat::YUYV ? "YUYV" :
    cfg_.pixel_format == PixelFormat::NV12 ? "NV12" : "RGB888");
  
  // Initialize frame statistics
  frame_count_ = 0;
  last_stats_time_ = 0;
}

V4L2Capture::~V4L2Capture() { stop(); }

bool V4L2Capture::start() {
  Logger::instance().log(LogLevel::INFO, "Attempting to open V4L2 device: %s", cfg_.camera_device.c_str());
  
  fd_ = ::open(cfg_.camera_device.c_str(), O_RDWR | O_NONBLOCK, 0);
  if (fd_ < 0) {
    Logger::instance().log(LogLevel::ERROR, "V4L2 open failed: %s (errno=%d: %s)", 
      cfg_.camera_device.c_str(), errno, strerror(errno));
    return false;
  }
  
  Logger::instance().log(LogLevel::INFO, "V4L2 device opened successfully (fd=%d)", fd_);
  if (!init_device()) return false;
  if (!init_mmap()) return false;

  // Queue buffers
  Logger::instance().log(LogLevel::INFO, "Queueing %d buffers for streaming...", nbufs_);
  for (int i=0;i<nbufs_;++i) {
    v4l2_buffer buf{};
    buf.type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                          : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    
    v4l2_plane planes[1];
    if (is_mplane_) {
      buf.m.planes = planes;
      buf.length = 1;
    }
    
    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
      Logger::instance().log(LogLevel::ERROR, "VIDIOC_QBUF failed for buffer %d (errno=%d: %s)", i, errno, strerror(errno));
      return false;
    }
    Logger::instance().log(LogLevel::INFO, "Buffer %d queued successfully", i);
  }
  Logger::instance().log(LogLevel::INFO, "All %d buffers queued", nbufs_);

  // Stream on
  Logger::instance().log(LogLevel::INFO, "Starting video stream...");
  int type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                        : V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
    Logger::instance().log(LogLevel::ERROR, "VIDIOC_STREAMON failed (errno=%d: %s)", errno, strerror(errno));
    return false;
  }
  streaming_ = true;
  Logger::instance().log(LogLevel::INFO, "Video stream started successfully");
  return true;
}

void V4L2Capture::stop() {
  if (fd_ >= 0) {
    if (streaming_) {
      int type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                            : V4L2_BUF_TYPE_VIDEO_CAPTURE;
      ioctl(fd_, VIDIOC_STREAMOFF, &type);
      streaming_ = false;
    }
    for (int i=0;i<nbufs_;++i) {
      if (bufs_[i].ptr) munmap(bufs_[i].ptr, bufs_[i].len);
      bufs_[i] = {};
    }
    ::close(fd_);
    fd_ = -1;
  }
}

bool V4L2Capture::read_frame(CapturedFrame& out) {
  static bool first_call = true;
  static int call_count = 0;
  call_count++;
  
  if (first_call) {
    Logger::instance().log(LogLevel::INFO, "read_frame: First call (fd=%d, is_mplane=%d)", fd_, (int)is_mplane_);
    first_call = false;
  }
  
  if (fd_ < 0) {
    Logger::instance().log(LogLevel::ERROR, "read_frame: fd not open");
    return false;
  }

  // Log every 100 calls to show activity
  if (call_count % 100 == 0) {
    Logger::instance().log(LogLevel::INFO, "read_frame: Called %d times", call_count);
  }

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd_, &fds);
  timeval tv{0, 200000}; // 200ms
  int r = select(fd_+1, &fds, nullptr, nullptr, &tv);
  
  if (r < 0) {
    Logger::instance().log(LogLevel::ERROR, "select failed (errno=%d: %s)", errno, strerror(errno));
    return false;
  }
  if (r == 0) {
    // Timeout - log once per 5 seconds to avoid spam
    static uint64_t last_timeout_log = 0;
    static int timeout_count = 0;
    timeout_count++;
    uint64_t now = now_ms();
    if (now - last_timeout_log >= 5000) {
      Logger::instance().log(LogLevel::WARN, "Frame read timeout (no data from camera) - %d timeouts so far", timeout_count);
      last_timeout_log = now;
    }
    return false;
  }

  // select returned > 0, data is ready
  if (call_count <= 5) {
    Logger::instance().log(LogLevel::INFO, "read_frame: select returned %d (data ready)", r);
  }

  v4l2_buffer buf{};
  buf.type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                        : V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

  v4l2_plane planes[1];
  if (is_mplane_) {
    buf.m.planes = planes;
    buf.length = 1;
  }

  if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
    Logger::instance().log(LogLevel::ERROR, "VIDIOC_DQBUF failed (errno=%d: %s)", errno, strerror(errno));
    return false;
  }
  
  if (call_count <= 5) {
    Logger::instance().log(LogLevel::INFO, "read_frame: DQBUF succeeded, buffer index=%d", buf.index);
  }

  // Copy for MVP (optimize to dmabuf later if needed)
  out.ts_ms = now_ms();
  out.width = cfg_.width;
  out.height = cfg_.height;
  out.fmt = cfg_.pixel_format;
  
  size_t bytesused;
  if (is_mplane_) {
    bytesused = planes[0].bytesused;
  } else {
    bytesused = buf.bytesused;
  }
  
  out.data.resize(bytesused);
  std::memcpy(out.data.data(), bufs_[buf.index].ptr, bytesused);

  // Requeue
  if (is_mplane_) {
    buf.m.planes = planes;
    buf.length = 1;
  }
  ioctl(fd_, VIDIOC_QBUF, &buf);
  
  // Update frame statistics
  frame_count_++;
  uint64_t now = now_ms();
  if (last_stats_time_ == 0) {
    last_stats_time_ = now;
    Logger::instance().log(LogLevel::INFO, "First frame captured! size: %zu bytes", bytesused);
  } else if (now - last_stats_time_ >= 10000) { // Log every 10 seconds
    double elapsed_sec = (now - last_stats_time_) / 1000.0;
    double fps = frame_count_ / elapsed_sec;
    Logger::instance().log(LogLevel::INFO, 
      "Capture stats: %llu frames, %.1f fps, frame size: %zu bytes",
      (unsigned long long)frame_count_, fps, bytesused);
    frame_count_ = 0;
    last_stats_time_ = now;
  }
  
  return true;
}

bool V4L2Capture::init_device() {
  // 1) Query caps
  v4l2_capability cap{};
  if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
    Logger::instance().log(LogLevel::ERROR, "VIDIOC_QUERYCAP failed (errno=%d)", errno);
    return false;
  }

  // Use this node's device_caps, not the device-wide capabilities union. A UVC
  // webcam exposes a real capture node and a paired metadata node; the union
  // reports VIDEO_CAPTURE on both, so capabilities would misidentify the metadata
  // node as capture (then "No pixel formats enumerated"). device_caps is per-node.
  const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                            ? cap.device_caps
                            : cap.capabilities;

  is_mplane_ =
      (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;

  const bool is_capture =
      (caps & V4L2_CAP_VIDEO_CAPTURE) != 0;

  const bool is_streaming =
      (caps & V4L2_CAP_STREAMING) != 0;

  Logger::instance().log(LogLevel::INFO, "V4L2 caps: driver=%s card=%s bus=%s",
                         cap.driver, cap.card, cap.bus_info);
  Logger::instance().log(LogLevel::INFO, "V4L2 caps: capture=%d mplane=%d streaming=%d",
                         (int)is_capture, (int)is_mplane_, (int)is_streaming);

  if (!is_streaming || (!is_capture && !is_mplane_)) {
    Logger::instance().log(LogLevel::ERROR, "Device does not support required capture/streaming caps");
    return false;
  }

  // 2) Enumerate formats
  std::vector<uint32_t> fmts;
  if (!enum_formats(fd_, fmts, is_mplane_)) {
    Logger::instance().log(LogLevel::ERROR, "No pixel formats enumerated");
    return false;
  }

  Logger::instance().log(LogLevel::INFO, "Supported formats:");
  for (auto f : fmts) {
    Logger::instance().log(LogLevel::INFO, "  %s (0x%08x)", fourcc_to_str(f).c_str(), f);
  }

  // 3) Requested format
  uint32_t req_fmt = to_v4l2_pixfmt(cfg_.pixel_format);
  if (std::find(fmts.begin(), fmts.end(), req_fmt) == fmts.end()) {
    Logger::instance().log(LogLevel::WARN,
      "Requested pixfmt %s not supported; falling back to %s",
      fourcc_to_str(req_fmt).c_str(), fourcc_to_str(fmts[0]).c_str());
    req_fmt = fmts[0];
  }

  // 4) Enumerate frame sizes for chosen format
  std::vector<std::pair<int,int>> sizes;
  if (enum_framesizes(fd_, req_fmt, sizes)) {
    Logger::instance().log(LogLevel::INFO, "Some supported sizes for %s:", fourcc_to_str(req_fmt).c_str());
    for (auto& s : sizes) {
      Logger::instance().log(LogLevel::INFO, "  %dx%d", s.first, s.second);
    }
  } else {
    Logger::instance().log(LogLevel::WARN, "Could not enumerate frame sizes for %s", fourcc_to_str(req_fmt).c_str());
  }

  // 5) Try S_FMT; if it fails, fall back to current G_FMT
  int w = cfg_.width;
  int h = cfg_.height;

  auto try_set_format = [&](int tw, int th) -> bool {
    v4l2_format fmt{};
    fmt.type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                         : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (is_mplane_) {
      fmt.fmt.pix_mp.width = tw;
      fmt.fmt.pix_mp.height = th;
      fmt.fmt.pix_mp.pixelformat = req_fmt;
      fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
      fmt.fmt.pix_mp.num_planes = 1; // driver may override
    } else {
      fmt.fmt.pix.width = tw;
      fmt.fmt.pix.height = th;
      fmt.fmt.pix.pixelformat = req_fmt;
      fmt.fmt.pix.field = V4L2_FIELD_NONE;
    }

    Logger::instance().log(LogLevel::INFO, "Trying S_FMT: %dx%d %s (%s)",
                           tw, th, fourcc_to_str(req_fmt).c_str(),
                           is_mplane_ ? "MPLANE" : "SPLANE");

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
      Logger::instance().log(LogLevel::ERROR, "VIDIOC_S_FMT failed (errno=%d: %s)",
                             errno, strerror(errno));
      return false;
    }

    // Read back actual format the driver accepted
    v4l2_format g{};
    g.type = fmt.type;
    if (ioctl(fd_, VIDIOC_G_FMT, &g) == 0) {
      if (is_mplane_) {
        cfg_.width  = g.fmt.pix_mp.width;
        cfg_.height = g.fmt.pix_mp.height;
        Logger::instance().log(LogLevel::INFO, "Accepted format: %dx%d %s",
          cfg_.width, cfg_.height, fourcc_to_str(g.fmt.pix_mp.pixelformat).c_str());
      } else {
        cfg_.width  = g.fmt.pix.width;
        cfg_.height = g.fmt.pix.height;
        Logger::instance().log(LogLevel::INFO, "Accepted format: %dx%d %s",
          cfg_.width, cfg_.height, fourcc_to_str(g.fmt.pix.pixelformat).c_str());
      }
    }
    return true;
  };

  // First try requested size
  if (try_set_format(w, h)) {
    // Set FPS
    v4l2_streamparm sp{};
    sp.type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                        : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sp.parm.capture.timeperframe.numerator = 1;
    sp.parm.capture.timeperframe.denominator = cfg_.fps;
    ioctl(fd_, VIDIOC_S_PARM, &sp);
    return true;
  }

  // If that failed, try the first enumerated size (if any)
  if (!sizes.empty()) {
    Logger::instance().log(LogLevel::WARN, "Retrying with first enumerated size %dx%d",
                           sizes[0].first, sizes[0].second);
    if (try_set_format(sizes[0].first, sizes[0].second)) {
      v4l2_streamparm sp{};
      sp.type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                          : V4L2_BUF_TYPE_VIDEO_CAPTURE;
      sp.parm.capture.timeperframe.numerator = 1;
      sp.parm.capture.timeperframe.denominator = cfg_.fps;
      ioctl(fd_, VIDIOC_S_PARM, &sp);
      return true;
    }
  }

  // Final fallback: use whatever the driver currently has (G_FMT only)
  {
    v4l2_format g{};
    g.type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                       : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_G_FMT, &g) == 0) {
      if (is_mplane_) {
        cfg_.width = g.fmt.pix_mp.width;
        cfg_.height = g.fmt.pix_mp.height;
        // Update pixel format to match what device is actually providing
        uint32_t actual_fmt = g.fmt.pix_mp.pixelformat;
        if (actual_fmt == V4L2_PIX_FMT_NV12) cfg_.pixel_format = PixelFormat::NV12;
        else if (actual_fmt == V4L2_PIX_FMT_RGB24 || actual_fmt == V4L2_PIX_FMT_BGR24) cfg_.pixel_format = PixelFormat::RGB888;
        else cfg_.pixel_format = PixelFormat::YUYV;
        Logger::instance().log(LogLevel::WARN, "Using existing device format: %dx%d %s",
          cfg_.width, cfg_.height, fourcc_to_str(actual_fmt).c_str());
      } else {
        cfg_.width = g.fmt.pix.width;
        cfg_.height = g.fmt.pix.height;
        // Update pixel format to match what device is actually providing
        uint32_t actual_fmt = g.fmt.pix.pixelformat;
        if (actual_fmt == V4L2_PIX_FMT_NV12) cfg_.pixel_format = PixelFormat::NV12;
        else if (actual_fmt == V4L2_PIX_FMT_RGB24 || actual_fmt == V4L2_PIX_FMT_BGR24) cfg_.pixel_format = PixelFormat::RGB888;
        else cfg_.pixel_format = PixelFormat::YUYV;
        Logger::instance().log(LogLevel::WARN, "Using existing device format: %dx%d %s",
          cfg_.width, cfg_.height, fourcc_to_str(actual_fmt).c_str());
      }
      return true;
    }
  }

  Logger::instance().log(LogLevel::ERROR, "Failed to configure camera format");
  return false;
}

bool V4L2Capture::init_mmap() {
  v4l2_requestbuffers req{};
  req.count = 4;
  req.type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                        : V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
    Logger::instance().log(LogLevel::ERROR, "VIDIOC_REQBUFS failed (errno=%d: %s)", errno, strerror(errno));
    return false;
  }
  nbufs_ = (int)req.count;

  for (int i=0;i<nbufs_;++i) {
    v4l2_buffer buf{};
    buf.type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                          : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    v4l2_plane planes[1];
    if (is_mplane_) {
      buf.m.planes = planes;
      buf.length = 1;
    }

    if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
      Logger::instance().log(LogLevel::ERROR, "VIDIOC_QUERYBUF failed (errno=%d: %s)", errno, strerror(errno));
      return false;
    }

    size_t length;
    off_t offset;
    if (is_mplane_) {
      length = planes[0].length;
      offset = planes[0].m.mem_offset;
    } else {
      length = buf.length;
      offset = buf.m.offset;
    }

    void* ptr = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, offset);
    if (ptr == MAP_FAILED) {
      Logger::instance().log(LogLevel::ERROR, "mmap failed (errno=%d: %s)", errno, strerror(errno));
      return false;
    }
    bufs_[i].ptr = ptr;
    bufs_[i].len = length;
  }
  return true;
}

} // namespace wvm
