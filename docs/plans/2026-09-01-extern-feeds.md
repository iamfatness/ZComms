# Extern feeds: latched multichannel sources into talkback channels

2026-09-01. Owner request: the ZoomISO capability — route a source from a
multichannel capture device into a comms channel, latched, so a larger
intercom system (matrix frame, Dante network) can use Zoom as its last
mile. Roadmap item "extern feeds latched into a channel" plus decision B
(director barge), refined live: ZoomISO ships 4 latched channels and ducks
only on ACTUAL audio, so both ducks here are signal-gated, not state-gated.

## Decisions (owner, 2026-09-01)

- Source = capture **device + channel number(s)**: one channel, or a
  stereo pair downmixed to mono (SDK mono-only law #5).
- **At most one feed per talkback channel**; several channels may each
  carry their own feed. Mixing multiple sources into one channel belongs
  to the upstream console.
- **Send-only.** The return direction (channel audio back out to the
  external system) is future work with its own SDK unknowns.
- **Both ducks signal-gated**:
  - members' meeting-audio duck (law #4) engages only while a channel
    carries actual audio — a latched-but-silent feed leaves the meeting
    at unity (the ZoomISO behavior);
  - the in-channel barge duck drops the feed to ~30% only while the
    operator's mic actually carries voice (short hang), not merely while
    the key is held.

## Components

- `src/audio/signal_gate.{h,cpp}` — pure. Per-frame peak vs threshold
  (default −50 dBFS), instant attack, ~800 ms hang so speech gaps do not
  flutter a duck. One instance on the post-envelope voice, one per feed.
- `src/audio/channel_mix.{h,cpp}` — pure. Per-slot composer:
  `out = voice(if keyed) + feed(if latched) × barge_gain`, barge_gain
  ramping (~50 ms) between 1.0 and 0.3 driven by the voice gate; float
  math, clamped to int16. The routing/duck arithmetic lives here and only
  here, unit-tested.
- `src/audio/extern_feed.{h,cpp}` — one per configured feed. miniaudio
  capture at the device's NATIVE channel count; pure
  `ExtractDownmix(interleaved, nch, ch_a, ch_b)` picks the channel or
  pair and downmixes; smoothed gain → limiter → latch ramp (Envelope) →
  FrameAccumulator → own FrameRing. No AEC (line feed), no PTT. Stats:
  frames, ring drops, decaying peak.
- `TalkbackChannels::SendToSlot(slot, pcm, samples)` — per-slot send
  beside the existing fan-out; the only SDK-boundary addition.
- `ChannelBankSink::Send()` (main.cpp) becomes the composition point: on
  each pacer tick it pulls every latched feed's frame (underrun = silence,
  counted), asks ChannelMix for each slot with content, and issues one
  per-slot send. Channels with neither voice nor feed send nothing. The
  single 20 ms pacer clock is unchanged.
- `DuckPlanner` — mechanics unchanged; the main loop now feeds it an
  ACTIVITY mask instead of the raw key mask:
  `active(slot) = (keyed && voice_gate) || (latched && feed_gate)`.

## Panel / verbs

- Verbs: `feed set <slot> <device-substr>:<ch>[-<ch2>]`,
  `feed latch <slot> on|off`, `feed gain <slot> <db>`, `feed off <slot>`.
- Settings drawer: EXTERN FEEDS section — per-channel device picker
  (same device list as the mic picker), channel number field, LATCH
  toggle, gain. Channel cell grows a small FEED lamp: amber = latched +
  flowing, dim = latched-silent, red = capture dead.
- Ops lines on: feed start/stop, device open failure, capture stall,
  first audio, latch changes.

## Persistence

`%APPDATA%\ZComms\feeds.env` (own file — config.env stays operator-owned
for auth/meeting overrides): `feedN=device:ch[-ch2],gain_db,latch` per
slot, loaded at startup, written on every feed verb. Feeds re-arm on
rejoin: latch state survives the session cycle.

## Failure modes

- Feed device vanishes mid-show (USB yank): capture stops, lamp red, ops
  line, channel keeps working for voice; re-`feed set` or device return
  re-arms. Never fatal.
- Feed ring underrun at a tick: silence into the mix, counted — the
  channel does not stall (pacer law).
- Hot feed + hot voice: mix headroom handled in float; limiter ceiling
  honored before the int16 boundary.
- Send refusals surface per-slot in `send_failures`/ops as today.

## Testing

- Unit: SignalGate (threshold, hang, re-trigger), ChannelMix (routing
  truth table: keyed/latched × voice-active/feed-active; barge ramp
  trajectory; clamp), ExtractDownmix (channel pick, pair downmix, odd
  interleaves), feeds.env round-trip.
- Bench: feed → WavSink through the real accumulator/ring/pacer with a
  scripted voice barge — the recorded WAV shows the duck engage/release.
- Live gates (need a meeting; the repo norm for SDK truths):
  1. **M0**: distinct simultaneous streams to 2+ channels from one
     client (ZoomISO proves the SDK can; ours must), and a long latched
     send into an EMPTY channel — the unexplained 2026-08-28 AppHangB1
     followed exactly that pattern, so this soak either reproduces it
     under instrumentation or retires it.
  2. Feed audible to channel members; latched-silent feed leaves meeting
     audio at unity; barge duck audible under live voice.

## Out of scope (YAGNI)

Return path / 4-wire, multi-source-per-channel mixing, per-member feed
levels, feed EQ/processing, program-audio auto-mix-minus.
