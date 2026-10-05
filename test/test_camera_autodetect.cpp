// Host unit test for wvm::resolve_camera_device token classification.
//
// The V4L2 ioctl enumeration in auto_detect_usb_device_v4l2() needs real
// hardware and is verified on-device; here we inject a fake detector to test
// only the sentinel-vs-passthrough routing, which is pure string logic.
//
// Build & run on the host (no cross toolchain, no RKNN deps):
//   g++ -std=c++17 -Iinclude test/test_camera_autodetect.cpp
//       src/wvm/camera_autodetect.cpp src/wvm/logger.cpp -o /tmp/t && /tmp/t

#include "wvm/camera_autodetect.hpp"

#include <cstdio>
#include <string>

static int g_failures = 0;

static void expect_eq(const std::string& label, const std::string& got,
                      const std::string& want) {
  if (got == want) {
    std::printf("  ok   %-28s => '%s'\n", label.c_str(), got.c_str());
  } else {
    std::printf("  FAIL %-28s got '%s' want '%s'\n", label.c_str(),
                got.c_str(), want.c_str());
    ++g_failures;
  }
}

int main() {
  // A detector that always "finds" a camera on /dev/video3.
  const auto found = []() -> std::string { return "/dev/video3"; };
  // A detector that finds nothing.
  const auto none = []() -> std::string { return std::string{}; };

  std::printf("resolve_camera_device:\n");

  // Sentinel tokens route to the detector.
  expect_eq("usb_camera", wvm::resolve_camera_device("usb_camera", found),
            "/dev/video3");
  expect_eq("usb", wvm::resolve_camera_device("usb", found), "/dev/video3");
  expect_eq("camera", wvm::resolve_camera_device("camera", found),
            "/dev/video3");

  // Sentinels are case-insensitive.
  expect_eq("USB_Camera (mixed case)",
            wvm::resolve_camera_device("USB_Camera", found), "/dev/video3");

  // Empty string is treated as a request to auto-detect.
  expect_eq("empty string",
            wvm::resolve_camera_device(std::string{}, found), "/dev/video3");

  // Detection finding nothing falls back to /dev/video0.
  expect_eq("sentinel + no device",
            wvm::resolve_camera_device("usb_camera", none), "/dev/video0");

  // Non-sentinel values pass through verbatim; the detector must not run.
  expect_eq("rtsp url passthrough",
            wvm::resolve_camera_device("rtsp://10.0.0.5:8554/s", found),
            "rtsp://10.0.0.5:8554/s");
  expect_eq("device node passthrough",
            wvm::resolve_camera_device("/dev/video2", found), "/dev/video2");
  expect_eq("file path passthrough",
            wvm::resolve_camera_device("/storage/sd/clip.mp4", found),
            "/storage/sd/clip.mp4");

  if (g_failures == 0) {
    std::printf("ALL PASS\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_failures);
  return 1;
}
