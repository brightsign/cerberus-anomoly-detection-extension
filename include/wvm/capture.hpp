#pragma once
#include "wvm/types.hpp"

namespace wvm {

class ICapture {
public:
  virtual ~ICapture() = default;
  virtual bool start() = 0;
  virtual void stop() = 0;
  virtual bool read_frame(CapturedFrame& out) = 0;
  // Returns true if the underlying stream has broken and needs reconnection.
  // Default: false (USB/V4L2 captures don't need reconnection).
  virtual bool is_broken() const { return false; }
};

} // namespace wvm
