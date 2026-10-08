// A VirtualMic with the same gate discipline as the real backends, and no SDK.
//
// The four lifecycle calls are driven directly by the test instead of by
// Zoom. Everything else -- the CanSend/Send gating, the re-check under the
// lock, the counters -- mirrors ZoomMicSourceWin exactly, because that
// contract is what TxPacer is written against and what these tests pin.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "virtual_mic.h"

namespace zctest {

class FakeVirtualMic : public zc::VirtualMic {
 public:
  // Every frame Send() accepted, in order.
  std::vector<std::vector<int16_t>> accepted;
  // Set non-zero to make the "SDK" refuse; Send() then counts a failure.
  int fail_with = 0;

  // The lifecycle, driven by the test.
  void Initialize() {
    {
      std::lock_guard<std::mutex> lock(m_);
      have_sender_ = true;
    }
    initialised_.store(true);
  }
  void StartSend() { can_send_.store(true); }
  void StopSend() { can_send_.store(false); }
  void Uninitialize() {
    can_send_.store(false);
    initialised_.store(false);
    std::lock_guard<std::mutex> lock(m_);
    have_sender_ = false;
  }

  bool CanSend() override {
    return can_send_.load() && initialised_.load();
  }

  bool Send(const int16_t* pcm, int samples) override {
    std::lock_guard<std::mutex> lock(m_);
    if (!have_sender_ || !can_send_.load()) return false;
    if (fail_with != 0) {
      send_failures_.fetch_add(1);
      last_error_.store(fail_with);
      return false;
    }
    accepted.emplace_back(pcm, pcm + samples);
    return true;
  }

  bool initialised() const override { return initialised_.load(); }
  bool sending() const override { return can_send_.load(); }
  uint64_t send_failures() const override { return send_failures_.load(); }
  int last_error() const override { return last_error_.load(); }

 private:
  mutable std::mutex m_;
  bool have_sender_ = false;
  std::atomic<bool> can_send_{false};
  std::atomic<bool> initialised_{false};
  std::atomic<uint64_t> send_failures_{0};
  std::atomic<int> last_error_{0};
};

}  // namespace zctest
