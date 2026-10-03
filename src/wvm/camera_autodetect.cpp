#include "wvm/camera_autodetect.hpp"
#include "wvm/logger.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <vector>

namespace wvm {

namespace {

std::string to_lower(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool is_sentinel(const std::string& low) {
  // Empty means "no device configured" -> auto-detect, matching the Argus flow
  // where an unset registry value becomes the usb_camera token.
  return low.empty() || low == "usb_camera" || low == "usb" || low == "camera";
}

// Retry an ioctl across EINTR/EAGAIN, as the Argus extension does.
bool xioctl(int fd, unsigned long request, void* arg) {
  for (int i = 0; i < 4; ++i) {
    if (ioctl(fd, request, arg) != -1) return true;
    if (errno == EINTR || errno == EAGAIN) continue;
    break;
  }
  return false;
}

}  // namespace

std::string auto_detect_usb_device_v4l2() {
  std::vector<std::string> devices;
  if (DIR* dir = ::opendir("/dev")) {
    while (dirent* entry = ::readdir(dir)) {
      std::string name = entry->d_name;
      if (name.rfind("video", 0) == 0) devices.emplace_back("/dev/" + name);
    }
    ::closedir(dir);
  }
  std::sort(devices.begin(), devices.end());

  for (const std::string& device : devices) {
    int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) continue;

    v4l2_capability cap{};
    if (!xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
      ::close(fd);
      continue;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
      ::close(fd);
      continue;
    }

    // Attempt an NV12 640x480 format negotiation; the device is accepted
    // regardless of the result, matching the Argus probe.
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 640;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    xioctl(fd, VIDIOC_S_FMT, &fmt);

    ::close(fd);
    Logger::instance().log(LogLevel::INFO,
                           "USB auto-detect: selected %s (driver='%s' card='%s')",
                           device.c_str(), cap.driver, cap.card);
    return device;
  }

  Logger::instance().log(LogLevel::WARN,
                         "USB auto-detect: no suitable V4L2 device found");
  return std::string{};
}

std::string resolve_camera_device(
    const std::string& configured,
    const std::function<std::string()>& detector) {
  if (!is_sentinel(to_lower(configured))) return configured;

  std::string detected = detector();
  if (detected.empty()) {
    Logger::instance().log(LogLevel::WARN,
                           "USB auto-detect: falling back to /dev/video0");
    return "/dev/video0";
  }
  return detected;
}

}  // namespace wvm
