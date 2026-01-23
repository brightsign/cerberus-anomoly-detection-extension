#pragma once
#include "wvm/types.hpp"

namespace wvm {

class ICapture {
public:
  virtual ~ICapture() = default;
  virtual bool start() = 0;
  virtual void stop() = 0;
  virtual bool read_frame(CapturedFrame& out) = 0;
};

} // namespace wvm
