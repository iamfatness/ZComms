// The one clock.
//
// Every timestamp in this harness -- emission, capture anchors, calibration --
// comes from here and from nowhere else. steady_clock is QPC-backed on Windows,
// is monotonic, and is unaffected by wall-clock adjustments. The brief's
// instruction not to trust two wall clocks is enforced structurally: there is
// no second source of time to accidentally reach for.
#pragma once

#include <chrono>
#include <cstdint>

namespace zc {

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline double NsToMs(int64_t ns) { return static_cast<double>(ns) / 1e6; }

}  // namespace zc
