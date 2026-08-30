# Multi-Meeting Talkback Bridge: Feasibility Spike First, Two-Process Architecture Second

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Goal

Determine — by spike, before any product code — whether one Windows machine can hold SDK clients in two Zoom meetings at once (production meeting + comms meeting), and, contingent on the answer, build a two-process talkback bridge that carries intercom audio between the two meetings with local audio routing between the processes.

## Architecture

The Windows Meeting SDK offers no multi-instance support: one `InitSDK` per process, one `CreateMeetingService` client, one joined meeting — and the live-observed `SDKERR_OTHER_SDK_INSTANCE_RUNNING` (14), triggered by an *unrelated process's* SDK engine, suggests the singleton may be machine-wide, which would kill even a two-process design on one box. So this plan is gated: **Spike C** (the spike CLAUDE.md already calls for by name) proves or refutes two processes ≙ two meetings on one machine. If it passes, the bridge is two `zcomms`-family processes — the existing desk in the production meeting, plus a headless `zcomms-bridge.exe` in the comms meeting — exchanging 20 ms PCM frames over a loopback UDP socket using a tiny sequenced frame protocol (pure, unit-tested). Each process feeds received frames into its own engine ring exactly as if they were mic capture, so pacing, limiting, and the talkback delivery laws all apply unchanged. If the spike fails, the documented fallbacks are (a) a second machine running the bridge process, or (b) no bridge — chat signaling as the cross-meeting cue path.

This is a deliberate, argued exception to CLAUDE.md's "no IPC layer" rule: that rule forbids IPC *on the assumption* that Zoom media apps need helpers; here two SDK singletons are the one thing a single process provably cannot hold, which is exactly the "worker-per-meeting" deferred idea in plan §3.2 — and it stays out of the product until the spike says the platform allows it.

## Tech Stack

- C++17, MSVC, CMake (existing build).
- Zoom Meeting SDK 7.1.5 vendored: `zoom_sdk.h` (`InitSDK`, `CreateMeetingService`, `DestroyMeetingService`, `CleanUPSDK`), `zoom_sdk_def.h` (`SDKERR_OTHER_SDK_INSTANCE_RUNNING`), existing `ZoomClient` wrapper.
- Winsock UDP on 127.0.0.1 (`ws2_32`, already linked by `zcomms`).
- Existing audio engine seams: `FrameSink` (TX side) and the 20 ms frame ring (`src/audio/frame_ring.{h,cpp}`).
- zctest harness, `zcomms_audio_tests` exe; spike lives under `spikes/` like `spikes/a-tx-latency/`.

## Spec

This document doubles as the spec.

### What the vendored SDK does and does not allow (header-verified)

- `InitSDK(InitParam&)` / `CleanUPSDK()` are free functions — process-global state, no handle, no instance object: **there is no multi-instance interface in the vendored SDK.** `CreateMeetingService(IMeetingService**)` creates the one meeting service; `IMeetingService::Join(JoinParam&)` joins the one meeting.
- **Looked for and not found:** any "multi meeting", "second instance", or per-instance SDK context in any vendored header; any documented scope for `SDKERR_OTHER_SDK_INSTANCE_RUNNING` (the enum exists in `zoom_sdk_def.h`; whether it is per-process or machine-wide is exactly what Spike C measures — the 2026-08-29 live incident where OBS's ZoomObsEngine blocked `InitSDK` in zcomms is one data point that says wider-than-process, but that engine is a different SDK generation, so it is evidence, not proof).
- Talkback channels live *inside* one meeting (`IMeetingTalkbackController`); nothing in the SDK spans meetings. Cross-meeting audio must therefore be carried by us between two clients.

### Requirements

1. **Spike C ships first and alone.** It answers, with a machine-readable verdict file: can process B `InitSDK` + `Authenticate` + `Join` meeting 2 while process A sits in meeting 1 on the same machine? Secondary: does the answer change if B starts before A joins VoIP? Both directions logged.
2. The verdict has three named outcomes: `TWO_PROCESS_OK`, `INIT_BLOCKED` (B gets error 14), `JOIN_DEGRADED` (init passes, join or audio fails). Product work beyond Task 2 is authorized only on `TWO_PROCESS_OK`.
3. The bridge frame protocol is pure and unit-tested: 16-bit mono 32 kHz, 640-sample (20 ms) payloads, sequence numbers, drop/duplicate/reorder tolerance, underrun-as-silence with a counted stat (never a stall — the TX pacer law).
4. The bridge process is headless, joins the comms meeting signed-in (the auth rules apply unchanged), keys a configured talkback channel, and transmits whatever frames arrive on its socket; the desk process mirrors the reverse path. Direction one (desk → comms meeting) is the MVP; the return path reuses the same protocol on a second port.
5. Every bridge failure is loud: socket bind failure, frame gap > 1 s, and mic-closed (accepted-but-silent law) each produce ops lines on the desk.
6. All OS objects carry the `ZComms` prefix; the bridge binary is named `zcomms-bridge.exe` (never generic).

## Global Constraints

- Build: `cmake -S . -B build`; `cmake --build build --config Release`. Tests: `build\Release\zcomms_audio_tests.exe` / `ctest --test-dir build -C Release`.
- SDK laws in force: mic must be OPEN or sends are accepted-but-silent (the bridge's comms-meeting client must `UnmuteSelf` + auto-suppress like the desk); per-call rate limit (`SDKERR_TOO_FREQUENT_CALL`, 18) on CreateChannel and invites — the bridge pre-provisions once; stereo is silently discarded by talkback → the bridge sends mono; talkback does not cross breakout rooms; `sdk.dll` fastfails on normal `main` return → both processes exit via the established `HardExit()` pattern (`spikes/a-tx-latency/src/main.cpp`); SDK headers need `windows.h` first.
- Same-account collision law: two clients of one account must not host-join the same PMI; the spike uses **two different signed-in accounts or one account + one `--anon` same-account guest**, meetings started per CLAUDE.md's test-meeting rules (no waiting room, 40-min limit awareness).
- Pure modules include no SDK headers; they compile into `zcomms_audio_tests` without `sdk.lib`.
- Update `CLAUDE.md` in the same change as any substantive work — the spike verdict especially (it settles plan §3.2's open question either way).

## Tasks

### Task 1: bridge frame protocol (pure)

**Files**
- Create: `src/zoom/bridge_proto.h`
- Create: `src/zoom/bridge_proto.cpp`
- Test: `tests/zoom/test_bridge_proto.cpp`
- Modify: `CMakeLists.txt`, `tests/audio/test_util.h`, `tests/audio/test_main.cpp`

**Interfaces**
- Produces:
  ```cpp
  // src/zoom/bridge_proto.h  (no SDK includes, no winsock includes)
  #include <cstdint>
  #include <vector>
  namespace zc {
  constexpr int kBridgeSamplesPerFrame = 640;   // 20 ms @ 32 kHz mono
  constexpr uint32_t kBridgeMagic = 0x5A43'4231; // "ZCB1"
  struct BridgeFrame {
    uint32_t seq = 0;
    int16_t pcm[kBridgeSamplesPerFrame] = {};
  };
  // Wire form: magic(4) | seq(4) | pcm(1280), little-endian, 1288 bytes total.
  std::vector<uint8_t> EncodeBridgeFrame(const BridgeFrame& f);
  bool DecodeBridgeFrame(const uint8_t* data, size_t len, BridgeFrame* out);
  // Reassembly window: absorbs loss/dup/reorder, hands the pacer exactly one
  // frame per Pull. Missing frame -> silence + underrun count (pacer law).
  class BridgeJitterBuffer {
   public:
    explicit BridgeJitterBuffer(int depth_frames = 3);
    void Push(const BridgeFrame& f);
    // Fills pcm[kBridgeSamplesPerFrame]; returns false when it substituted
    // silence (underrun or gap).
    bool Pull(int16_t* pcm);
    uint64_t underruns() const;
    uint64_t late_drops() const;
  };
  }
  ```

**Steps**

- [ ] Write the failing test `tests/zoom/test_bridge_proto.cpp`:
  ```cpp
  #include <cstring>
  #include "bridge_proto.h"
  #include "test_util.h"

  void TestBridgeProto() {
    ZC_TEST("bridge: frame round-trips at exactly 1288 bytes");
    zc::BridgeFrame f;
    f.seq = 42;
    for (int i = 0; i < zc::kBridgeSamplesPerFrame; ++i)
      f.pcm[i] = static_cast<int16_t>(i - 320);
    auto wire = zc::EncodeBridgeFrame(f);
    ZC_CHECK(wire.size() == 1288);
    zc::BridgeFrame back;
    ZC_CHECK(zc::DecodeBridgeFrame(wire.data(), wire.size(), &back));
    ZC_CHECK(back.seq == 42);
    ZC_CHECK(std::memcmp(back.pcm, f.pcm, sizeof(f.pcm)) == 0);

    ZC_TEST("bridge: wrong magic and short packets rejected");
    auto bad = wire;
    bad[0] ^= 0xFF;
    ZC_CHECK(!zc::DecodeBridgeFrame(bad.data(), bad.size(), &back));
    ZC_CHECK(!zc::DecodeBridgeFrame(wire.data(), 100, &back));

    ZC_TEST("bridge: jitter buffer plays in order across reorder");
    zc::BridgeJitterBuffer jb(3);
    zc::BridgeFrame a, b, c;
    a.seq = 1; a.pcm[0] = 100;
    b.seq = 2; b.pcm[0] = 200;
    c.seq = 3; c.pcm[0] = 300;
    jb.Push(a); jb.Push(c); jb.Push(b);   // reordered arrival
    int16_t out[zc::kBridgeSamplesPerFrame];
    ZC_CHECK(jb.Pull(out) && out[0] == 100);
    ZC_CHECK(jb.Pull(out) && out[0] == 200);
    ZC_CHECK(jb.Pull(out) && out[0] == 300);

    ZC_TEST("bridge: a lost frame becomes counted silence, stream continues");
    zc::BridgeJitterBuffer jb2(3);
    zc::BridgeFrame d, e2;
    d.seq = 1;  d.pcm[0] = 100;
    e2.seq = 3; e2.pcm[0] = 300;          // seq 2 lost
    jb2.Push(d); jb2.Push(e2);
    ZC_CHECK(jb2.Pull(out) && out[0] == 100);
    ZC_CHECK(!jb2.Pull(out));             // silence substituted
    ZC_CHECK(out[0] == 0);
    ZC_CHECK(jb2.underruns() == 1);
    ZC_CHECK(jb2.Pull(out) && out[0] == 300);

    ZC_TEST("bridge: duplicates are dropped and counted");
    zc::BridgeJitterBuffer jb3(3);
    jb3.Push(d); jb3.Push(d);
    ZC_CHECK(jb3.Pull(out) && out[0] == 100);
    ZC_CHECK(jb3.late_drops() == 1);
  }
  ```
- [ ] Register `TestBridgeProto` in `tests/audio/test_util.h` + `tests/audio/test_main.cpp`; add `tests/zoom/test_bridge_proto.cpp` and `src/zoom/bridge_proto.cpp` to `zcomms_audio_tests` sources in `CMakeLists.txt` (plus `target_include_directories(zcomms_audio_tests PRIVATE tests/audio src/zoom)` if not already present from a sibling plan).
- [ ] `cmake --build build --config Release --target zcomms_audio_tests` — failing.
- [ ] Implement `bridge_proto.cpp`: encode/decode with explicit little-endian byte writes (no struct-cast serialization — MSVC padding is not a wire format); `BridgeJitterBuffer` keeps a small `std::map<uint32_t, BridgeFrame>` window keyed by seq with a `next_seq_` cursor: `Pull` emits `next_seq_` if present (advance), else zero-fills and counts an underrun, advancing past a seq confirmed missing only once a newer seq has been seen (so a merely-late frame still plays); `Push` of a seq `< next_seq_` increments `late_drops_`.
- [ ] Run `build\Release\zcomms_audio_tests.exe`; green.
- [ ] Commit: `git add src/zoom/bridge_proto.h src/zoom/bridge_proto.cpp tests/zoom/test_bridge_proto.cpp tests/audio/test_util.h tests/audio/test_main.cpp CMakeLists.txt && git commit -m "feat(bridge): sequenced 20 ms frame protocol + jitter buffer (pure)"`

### Task 2: Spike C — SDK exclusivity harness

**Files**
- Create: `spikes/c-sdk-exclusivity/CMakeLists.txt`
- Create: `spikes/c-sdk-exclusivity/README.md`
- Create: `spikes/c-sdk-exclusivity/src/main.cpp`
- Create: `spikes/c-sdk-exclusivity/src/verdict.h`
- Create: `spikes/c-sdk-exclusivity/src/verdict.cpp`
- Test: `spikes/c-sdk-exclusivity/tests/test_verdict.cpp`, `spikes/c-sdk-exclusivity/tests/test_main.cpp`
- Modify: root `CMakeLists.txt` (append `add_subdirectory(spikes/c-sdk-exclusivity)` after the existing `add_subdirectory(spikes/a-tx-latency)`)

**Interfaces**
- Produces (pure, mirrors the spike-a stats/report pattern):
  ```cpp
  // spikes/c-sdk-exclusivity/src/verdict.h  (no SDK includes)
  #include <string>
  namespace zc_spike_c {
  struct Observation {
    int init_b_err = -1;        // SDKError from process B's InitSDK, -1 = not run
    bool auth_b_ok = false;
    bool join_b_ok = false;
    bool a_still_in_meeting = false;  // A unaffected while B ran
    bool b_started_first = false;     // ordering variant
  };
  enum class Verdict { kTwoProcessOk, kInitBlocked, kJoinDegraded, kInconclusive };
  Verdict Judge(const Observation& o);
  // One-line machine-readable record appended to verdict.txt, e.g.
  // "C|order=AB|init_b=0|auth=1|join=1|a_alive=1|verdict=TWO_PROCESS_OK"
  std::string FormatRecord(const Observation& o);
  }
  ```
- Consumes (SDK side, in `main.cpp` only): the existing `zc::ZoomClient` (`Init`, `Authenticate`/`AuthenticateWithJwt`, `Join`, `JoinVoip`, `Pump`, `in_meeting`, `Leave`, `Cleanup`) and `HardExit()` per the spike-a pattern; config via `local.env` exactly like `spikes/a-tx-latency` (`spikes/a-tx-latency/local.env.example` is the template — copy the loading code from `spikes/a-tx-latency/src/config.{h,cpp}`).

**Steps**

- [ ] Write the failing test `spikes/c-sdk-exclusivity/tests/test_verdict.cpp` (`test_main.cpp` mirrors the spike-a test runner shape):
  ```cpp
  #include "verdict.h"
  #include "test_util.h"

  void TestVerdict() {
    ZC_TEST("verdict: clean run both ways is TWO_PROCESS_OK");
    zc_spike_c::Observation o;
    o.init_b_err = 0;  // SDKERR_SUCCESS
    o.auth_b_ok = o.join_b_ok = o.a_still_in_meeting = true;
    ZC_CHECK(zc_spike_c::Judge(o) == zc_spike_c::Verdict::kTwoProcessOk);

    ZC_TEST("verdict: error 14 is INIT_BLOCKED regardless of the rest");
    zc_spike_c::Observation b;
    b.init_b_err = 14;  // SDKERR_OTHER_SDK_INSTANCE_RUNNING
    ZC_CHECK(zc_spike_c::Judge(b) == zc_spike_c::Verdict::kInitBlocked);

    ZC_TEST("verdict: init ok but join failed or A disturbed is JOIN_DEGRADED");
    zc_spike_c::Observation d;
    d.init_b_err = 0;
    d.auth_b_ok = true;
    d.join_b_ok = false;
    ZC_CHECK(zc_spike_c::Judge(d) == zc_spike_c::Verdict::kJoinDegraded);
    d.join_b_ok = true;
    d.a_still_in_meeting = false;
    ZC_CHECK(zc_spike_c::Judge(d) == zc_spike_c::Verdict::kJoinDegraded);

    ZC_TEST("verdict: not-run stays INCONCLUSIVE");
    ZC_CHECK(zc_spike_c::Judge({}) == zc_spike_c::Verdict::kInconclusive);

    ZC_TEST("verdict: record is grep-able");
    zc_spike_c::Observation r = b;
    ZC_CHECK(zc_spike_c::FormatRecord(r).find("verdict=INIT_BLOCKED") !=
             std::string::npos);
  }
  ```
- [ ] `spikes/c-sdk-exclusivity/CMakeLists.txt`: a `spike_c_tests` exe from `tests/*.cpp` + `src/verdict.cpp` (always built; include dir `../a-tx-latency/tests` is NOT reused — copy `test_util.h`'s pattern into this spike's `tests/`), plus the `spike-c` exe from `src/main.cpp` + `src/verdict.cpp` linked against `zcomms_zoom`, guarded by `if(TARGET zcomms_zoom)`. Register `add_test(NAME spike_c_tests COMMAND spike_c_tests)`.
- [ ] Build to failure, implement `verdict.cpp` (the truth table above; `FormatRecord` with `snprintf`), rerun `ctest --test-dir build -C Release -R spike_c_tests` — green.
- [ ] Implement `src/main.cpp` roles: `spike-c --role a --meeting <n1>` (init, auth, join, `JoinVoip`, then pump forever printing a heartbeat), `spike-c --role b --meeting <n2>` (init → record error code → if ok: auth, join, hold 30 s, leave; append `FormatRecord` to `verdict.txt` in the CWD) — both exiting via `HardExit()`.
- [ ] Live run (the spike's whole point; needs two meetings started per the same-account collision rules): run A into meeting 1; run B into meeting 2; then the reverse order (B first, A second) with `b_started_first=true`. Append both records; note in the README whether OBS/CoreVideo engines were confirmed dead first (`SdkConflictHint()`'s known-hosts list is the sweep checklist).
- [ ] Record the verdict in `CLAUDE.md` (this closes the "Spike C should test it directly" item and plan §3.2's open question) and in `spikes/c-sdk-exclusivity/README.md` with the raw records.
- [ ] Commit: `git add spikes/c-sdk-exclusivity CMakeLists.txt CLAUDE.md && git commit -m "feat(spike-c): two-process SDK exclusivity harness + live verdict"`

### Task 3: `zcomms-bridge.exe` — comms-meeting leg (GATED on TWO_PROCESS_OK)

**Files**
- Create: `src/bridge/main.cpp`
- Create: `src/bridge/udp_frame_port.h`
- Create: `src/bridge/udp_frame_port.cpp`
- Modify: `CMakeLists.txt`

**Interfaces**
- Produces:
  ```cpp
  // src/bridge/udp_frame_port.h  (winsock lives in the .cpp only)
  namespace zc {
  class UdpFramePort {
   public:
    // Receiver: binds 127.0.0.1:<port>. Sender: connects to it. ZComms-only
    // machine-local traffic; port default 7361 (desk->bridge), 7362 (return).
    bool OpenRecv(uint16_t port, std::string* error);
    bool OpenSend(uint16_t port, std::string* error);
    bool Send(const BridgeFrame& f);
    bool Recv(BridgeFrame* f, int timeout_ms);  // false on timeout
    void Close();
  };
  }
  ```
- Consumes: `BridgeFrame`/`BridgeJitterBuffer` (Task 1), `ZoomClient` (init/auth/join/`UnmuteSelf`/auto-suppress virtual mic exactly as the desk does), `TalkbackChannels` (`CreateChannels(1)` once, then `SendToKeyed` from the pacer with the bridge channel permanently keyed), `TxPacer`'s 20 ms clock.
- CMake: new target
  ```cmake
  if(ZCOMMS_HAVE_SDK)
    add_executable(zcomms-bridge src/bridge/main.cpp src/bridge/udp_frame_port.cpp)
    target_link_libraries(zcomms-bridge PRIVATE zcomms_zoom winmm ws2_32)
    if(MSVC)
      target_compile_options(zcomms-bridge PRIVATE /W3 /utf-8)
    endif()
    add_custom_command(TARGET zcomms-bridge POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
              "${ZOOM_SDK_DIR}/bin" "$<TARGET_FILE_DIR:zcomms-bridge>"
      COMMENT "Staging Zoom SDK runtime beside zcomms-bridge")
  endif()
  ```

**Steps**

- [ ] Confirm `verdict.txt` says `TWO_PROCESS_OK`; if not, stop here and file the fallback decision (second machine vs chat-signaling-only) as a follow-up plan — do not build the bridge against a refuted platform.
- [ ] Implement `UdpFramePort` (plain blocking socket with `SO_RCVTIMEO` for the timeout; every failure fills `*error` with `WSAGetLastError()`).
- [ ] Implement `src/bridge/main.cpp`: parse `--meeting <n> --port 7361 --channel-name <label>`; sign-in credentials via the same `local.env`/broker path as the desk (headless is fine for scripted runs under `--anon` per the auth rules); join, `UnmuteSelf`, install the auto-suppress virtual mic; `CreateChannels(1)`; run the TX pacer with a `FrameSink` whose `OnFrame` pulls `BridgeJitterBuffer::Pull` output — the receive thread just `Recv`s and `Push`es. Frame-gap watchdog: no frame for 1 s → printf ops line + send silence (never stall). Exit via `HardExit()`.
- [ ] Desk side (one small `main.cpp` change, kept behind `--bridge-to <port>`): a second `FrameSink` tee after the PTT envelope sends every frame to `UdpFramePort::Send` when a new `BRIDGE` key is latched.
- [ ] Build both targets: `cmake --build build --config Release --target zcomms zcomms-bridge`; run `build\Release\zcomms_audio_tests.exe` — green.
- [ ] Live gate (two meetings, matched-filter e2e): desk in meeting 1 with `--test-signal --bridge-to 7361`, bridge in meeting 2, a native listener in the bridge channel of meeting 2, `zcomms-tap` at the listener. Detection at the listener = the bridge exists; record latency medians with the spike-a bracket discipline (no number quoted without `--self-test` first).
- [ ] Update `CLAUDE.md` ("multi-meeting bridge" section: the spike verdict, the IPC-exception rationale, ports, and the live result).
- [ ] Commit: `git add src/bridge CMakeLists.txt src/app/main.cpp CLAUDE.md && git commit -m "feat(bridge): zcomms-bridge comms-meeting leg over loopback UDP (spike-gated)"`

## Self-review checklist (fix inline before PR)

- [ ] No `TBD`; the gate between Task 2 and Task 3 is explicit and blocking.
- [ ] `kBridgeSamplesPerFrame`/byte sizes consistent everywhere (640 samples, 1280 PCM bytes, 1288 wire bytes).
- [ ] No SDK or winsock includes in `bridge_proto.*`; winsock confined to `udp_frame_port.cpp`.
- [ ] Both processes exit through `HardExit()`; no bare `return` from `main` in SDK-linked binaries.
- [ ] The IPC exception is argued against CLAUDE.md's rule in prose, not slipped in silently.
