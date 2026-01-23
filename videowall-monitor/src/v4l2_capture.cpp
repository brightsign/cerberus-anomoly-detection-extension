#include "wvm/v4l2_capture.hpp"
#include "wvm/logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cstring>

namespace wvm {

static uint32_t to_v4l2_pixfmt(PixelFormat fmt) {
  switch (fmt) {
    case PixelFormat::NV12: return V4L2_PIX_FMT_NV12;
    case PixelFormat::RGB888: return V4L2_PIX_FMT_RGB24;
    case PixelFormat::YUYV:
    default: return V4L2_PIX_FMT_YUYV;
  }
}

V4L2Capture::V4L2Capture(const DeviceConfig& cfg) : cfg_(cfg) {}
V4L2Capture::~V4L2Capture() { stop(); }

bool V4L2Capture::start() {
  fd_ = ::open(cfg_.camera_device.c_str(), O_RDWR | O_NONBLOCK, 0);
  if (fd_ < 0) {
    Logger::instance().log(LogLevel::ERROR, "V4L2 open failed: %s", cfg_.camera_device.c_str());
    return false;
  }
  if (!init_device()) return false;
  if (!init_mmap()) return false;

  // Queue buffers
  for (int i=0;i<nbufs_;++i) {
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
      Logger::instance().log(LogLevel::ERROR, "VIDIOC_QBUF failed");
      return false;
    }
  }

  // Stream on
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
    Logger::instance().log(LogLevel::ERROR, "VIDIOC_STREAMON failed");
    return false;
  }
  streaming_ = true;
  return true;
}

void V4L2Capture::stop() {
  if (fd_ >= 0) {
    if (streaming_) {
      int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
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
  if (fd_ < 0) return false;

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd_, &fds);
  timeval tv{0, 200000}; // 200ms
  int r = select(fd_+1, &fds, nullptr, nullptr, &tv);
  if (r <= 0) return false;

  v4l2_buffer buf{};
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

  if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) return false;

  // Copy for MVP (optimize to dmabuf later if needed)
  out.ts_ms = now_ms();
  out.width = cfg_.width;
  out.height = cfg_.height;
  out.fmt = cfg_.pixel_format;
  out.data.resize(buf.bytesused);
  std::memcpy(out.data.data(), bufs_[buf.index].ptr, buf.bytesused);

  // Requeue
  ioctl(fd_, VIDIOC_QBUF, &buf);
  return true;
}

bool V4L2Capture::init_device() {
  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = cfg_.width;
  fmt.fmt.pix.height = cfg_.height;
  fmt.fmt.pix.pixelformat = to_v4l2_pixfmt(cfg_.pixel_format);
  fmt.fmt.pix.field = V4L2_FIELD_ANY;

  if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
    Logger::instance().log(LogLevel::ERROR, "VIDIOC_S_FMT failed");
    return false;
  }

  v4l2_streamparm sp{};
  sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  sp.parm.capture.timeperframe.numerator = 1;
  sp.parm.capture.timeperframe.denominator = cfg_.fps;
  ioctl(fd_, VIDIOC_S_PARM, &sp);

  return true;
}

bool V4L2Capture::init_mmap() {
  v4l2_requestbuffers req{};
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
    Logger::instance().log(LogLevel::ERROR, "VIDIOC_REQBUFS failed");
    return false;
  }
  nbufs_ = (int)req.count;

  for (int i=0;i<nbufs_;++i) {
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
      Logger::instance().log(LogLevel::ERROR, "VIDIOC_QUERYBUF failed");
      return false;
    }

    void* ptr = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
    if (ptr == MAP_FAILED) {
      Logger::instance().log(LogLevel::ERROR, "mmap failed");
      return false;
    }
    bufs_[i].ptr = ptr;
    bufs_[i].len = buf.length;
  }
  return true;
}

} // namespace wvm
