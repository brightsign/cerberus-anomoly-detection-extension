#pragma once
#include <functional>
#include <string>

namespace wvm {

// Probe /dev/video* via V4L2 VIDIOC_QUERYCAP and return the first node that
// advertises both VIDEO_CAPTURE and STREAMING, or "" if none qualifies.
// Mirrors the Argus audience-measurement extension's autoDetectUsbDeviceV4L2():
// nodes are tried in ascending numeric order, so /dev/video0 wins when eligible.
// Hardware-dependent; exercised on the player, not in host unit tests.
std::string auto_detect_usb_device_v4l2();

// Resolve a configured camera_device string to a concrete capture source.
//
// The sentinel tokens "usb_camera", "usb", and "camera" (case-insensitive), as
// well as the empty string, trigger V4L2 auto-detection via `detector`, falling
// back to "/dev/video0" when detection finds nothing. Everything else -- rtsp://
// URLs, device node paths, and file paths -- passes through unchanged so the
// downstream RTSP-vs-V4L2 branch keeps working.
//
// `detector` is injectable so the token-classification logic can be unit-tested
// without camera hardware; it defaults to auto_detect_usb_device_v4l2.
std::string resolve_camera_device(
    const std::string& configured,
    const std::function<std::string()>& detector = auto_detect_usb_device_v4l2);

}  // namespace wvm
