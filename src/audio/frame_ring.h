// Bounded frame queue between the signal generator and the TX thread.
//
// Plan §6.2 specifies drop-oldest with counted drops, and that is the contract
// implemented here. The harness keeps the producer/consumer split rather than
// letting the TX thread generate its own signal inline, because a TX thread
// that manufactures its own frames can never underrun -- the underrun counter
// would be dead code and the fixed-clock behaviour under starvation, which is
// the thing §6.1 actually needs proven, would never be exercised.
//
// Deliberate deviation from §6.2: this is mutex-guarded, not lock-free.
// Drop-oldest means the producer must advance the read index, so the producer
// and consumer both write it, and the producer can overwrite a slot the
// consumer is mid-read of. Making that genuinely safe lock-free needs slot
// versioning. At 50 pushes/second with an uncontended critical section of tens
// of nanoseconds, lock-free buys nothing measurable here -- and a subtly wrong
// lock-free ring would corrupt the very measurement this harness exists to
// take. §6.1's real component should revisit it under real capture load.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "audio_defs.h"

namespace zc {

struct TxFrame {
  int16_t pcm[kFrameSamples];
  uint64_t seq = 0;

  // An optional opaque marker riding with the frame. -1 means unmarked, which
  // is the normal case for live audio.
  //
  // It exists so a producer can ask "tell me the exact instant this particular
  // audio was handed over", and get an answer taken at the send() call itself
  // rather than inferred from a schedule. Spike A uses it to timestamp probe
  // bursts; the meaning of the id and the flag belong entirely to whoever set
  // them.
  int32_t mark_id = -1;
  int32_t mark_offset = 0;  // offset of the marked instant within this frame
  bool mark_flag = false;   // one bit of producer-defined payload
};

class FrameRing {
 public:
  explicit FrameRing(size_t capacity_frames);

  // Never blocks and never fails. On overflow the oldest frame is discarded
  // and drops_ is incremented, so a slow consumer costs freshness rather than
  // costing the producer time.
  void Push(const TxFrame& f);

  bool Pop(TxFrame* out);

  size_t size() const;
  uint64_t drops() const;

 private:
  mutable std::mutex m_;
  std::vector<TxFrame> buf_;
  size_t head_ = 0;   // read position
  size_t count_ = 0;
  uint64_t drops_ = 0;
};

}  // namespace zc
