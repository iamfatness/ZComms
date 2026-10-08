# macOS Port P1-A — ZoomClient and VirtualMic Seams — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put `ZoomClient` and the virtual mic behind abstract seams, so P1-B can write macOS backends behind them, and give the virtual mic's send-window law its first tests.

**Architecture:** Two more seams in the shape P0 proved with `TalkbackSdk`. `VirtualMic` is a `FrameSink` plus a four-callback lifecycle; `ZoomClient` is the SDK lifecycle — init, auth, join, voip, mute, participants, pump. Both get an abstract header with no Zoom includes and a Windows backend that owns every SDK type. Two of `ZoomClient`'s current getters become **factories** (`MakeTalkbackSdk`, `InstallVirtualMic`) so no raw SDK pointer crosses the seam; the three getters that feed Windows-only consumers stay on the concrete Windows class.

**Tech Stack:** C++17, Zoom Meeting SDK (Windows `IZoomSDKVirtualAudioMicEvent` / `IAuthService` / `IMeetingService`), CMake, the repo's plain-executable test harness (`tests/audio/test_util.h`, no framework).

**Design spec:** `docs/plans/2026-09-04-macos-port.md` §3.1. **This plan writes no macOS code** — P1-B does that.

## Global Constraints

- **Zero behaviour change on Windows.** This is a move-only refactor plus two factory methods. Same order of operations, same guards, same counters, same lock scopes. **A diff hunk that changes control flow is a defect.** Verified by CI, never locally — this machine is an Apple Silicon Mac with no Windows toolchain. Never attempt a Windows build; keep the change mechanical and let the `Windows build & test` gate answer.
- **`send()` is legal only between `onMicStartSend` and `onMicStopSend`.** Hold the sender pointer from `onMicInitialize` until `onMicUninitialized` and gate every call on the window (CLAUDE.md, load-bearing). The teardown order in `onMicUninitialized` — shut the gate, then take the lock `Send()` takes, then drop the pointer — is what stops a TX thread that already passed `CanSend()` reaching a revoked pointer. **It must survive this refactor byte-for-byte.**
- **Mono is a law.** `send()` gets `ZoomSDKAudioChannel_Mono`, hardcoded below the seam.
- **No raw SDK type crosses a seam.** Not as a parameter, not as a return, not as a member of anything the seam declares.
- **Never assert a branch unreachable.** Standing policy in `CLAUDE.md`; two Majors in the talkback feature lived behind exactly that claim.
- **Tests pin invariants, not implementations,** and every new pin is mutation-proved: break the thing, watch the test fail, revert.
- Every OS object carries the `ZComms` prefix.
- Comment style: state the constraint the code cannot show. When motivated by a real failure, say what happened, with numbers.

---

## File Structure

**Create:**
- `src/audio/virtual_mic.h` — the `VirtualMic` seam. `FrameSink` plus lifecycle status. No Zoom headers, no `windows.h`.
- `src/audio/mic_source_win.h` / `.cpp` — Windows backend. Owns `IZoomSDKVirtualAudioMicEvent` and the sender pointer.
- `src/zoom/zoom_client.h` — **rewritten** as the abstract seam. Portable subset only.
- `src/zoom/zoom_client_win.h` / `.cpp` — Windows backend, from today's `zoom_client.{h,cpp}`.
- `tests/audio/fake_mic_sender.h` — a fake SDK sender for the mic tests.
- `tests/audio/test_virtual_mic.cpp` — the send-window law's first tests.

**Delete (moved, not lost):**
- `src/audio/mic_source.h` / `.cpp` → `mic_source_win.*` plus the new seam.
- The old concrete `src/zoom/zoom_client.cpp` → `zoom_client_win.cpp`.

**Modify:**
- `src/app/main.cpp` — construct `ZoomClientWin`, take the two factories' results, keep the three Windows-only getters on the concrete type.
- `CMakeLists.txt` — new sources; the seam headers must land in an SDK-free target.
- `tests/audio/test_util.h`, `tests/audio/test_main.cpp` — declare and call `TestVirtualMic()`.

**Deliberately not touched:** `src/zoom/talkback_*` (P0's work), `roster.*`, `breakout.*`, `chat_signals.*`, `talkback_source.*`, and everything else in `src/audio/`.

---

### Task 1: The `VirtualMic` seam and its Windows backend

`ZoomMicSource` today is one class doing two jobs: it implements Zoom's `IZoomSDKVirtualAudioMicEvent` **and** it is the `FrameSink` the TX pacer drives. The second job is already portable — `FrameSink` is declared in `tx_pacer.h` with no SDK dependency. This task splits them.

**Files:**
- Create: `src/audio/virtual_mic.h`, `src/audio/mic_source_win.h`, `src/audio/mic_source_win.cpp`
- Delete: `src/audio/mic_source.h`, `src/audio/mic_source.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `zc::FrameSink` (from `src/audio/tx_pacer.h`).
- Produces: `zc::VirtualMic` (abstract); `zc::ZoomMicSourceWin` (Windows backend, default-constructible, implements both `VirtualMic` and `IZoomSDKVirtualAudioMicEvent`).

- [ ] **Step 1: Write the seam header**

Create `src/audio/virtual_mic.h`:

```cpp
// Zoom's virtual microphone, as the engine needs it.
//
// Two jobs used to live in one class: implementing Zoom's
// IZoomSDKVirtualAudioMicEvent, and being the FrameSink the TX pacer drives.
// Only the second is portable, so only the second is up here. The four-
// callback lifecycle (onMicInitialize/StartSend/StopSend/Uninitialized) and
// the sender pointer belong to whichever backend the build selected.
//
// The status accessors exist because "the send window never opened" is the
// single most common talkback failure and the operator needs the reason
// attached: initialised() distinguishes "Zoom never handed us a sender"
// (usually a missing raw-data entitlement) from sending() false, which is
// "we have a sender but the window is shut" (usually a muted meeting mic).
#pragma once

#include <cstdint>

#include "tx_pacer.h"

namespace zc {

class VirtualMic : public FrameSink {
 public:
  ~VirtualMic() override = default;

  // Zoom handed us a sender (onMicInitialize fired, onMicUninitialized has
  // not). Says nothing about whether sending is legal right now.
  virtual bool initialised() const = 0;
  // The send window is OPEN -- between onMicStartSend and onMicStopSend.
  virtual bool sending() const = 0;
  virtual uint64_t send_failures() const = 0;
  // The platform SDK's own last error code, for humans. Never branched on.
  virtual int last_error() const = 0;
};

}  // namespace zc
```

- [ ] **Step 2: Move the Windows implementation behind it**

Create `src/audio/mic_source_win.h` — this is today's `mic_source.h` with the class renamed and `VirtualMic` added as a base. **Keep the entire header comment verbatim**; it documents the four-callback contract and the crash it prevents.

```cpp
// The virtual mic. Seeds ZoomMicSource in plan section 6.1.
//
// IZoomSDKVirtualAudioMicEvent is a four-callback lifecycle and the rules are
// not advisory:
//
//   onMicInitialize(pSender)  -- the sender pointer arrives; hold it
//   onMicStartSend()          -- send() becomes legal
//   onMicStopSend()           -- send() becomes illegal again
//   onMicUninitialized()      -- the pointer is revoked; drop it
//
// So every send is gated on a flag that only those callbacks move, and the
// pointer is read under a lock that onMicUninitialized also takes. The failure
// this prevents is calling into a revoked pointer from the TX thread, which is
// a crash rather than a glitch, and which a 20 ms cadence would find quickly.
//
// This class is a VirtualMic, which is what lets the same TxPacer drive Zoom,
// a local output device, or a synthetic sink without knowing the difference.
#pragma once

// windows.h must precede the SDK headers: zoom_sdk_def.h uses HWND, RECT and
// UINT64 bare, without including anything that declares them.
// clang-format off
#include <windows.h>
// clang-format on

#include <atomic>
#include <cstdint>
#include <mutex>

#include "rawdata/rawdata_audio_helper_interface.h"
#include "virtual_mic.h"
#include "zoom_sdk.h"

namespace zc {

class ZoomMicSourceWin : public VirtualMic,
                         public ZOOM_SDK_NAMESPACE::IZoomSDKVirtualAudioMicEvent {
 public:
  // IZoomSDKVirtualAudioMicEvent
  void onMicInitialize(ZOOM_SDK_NAMESPACE::IZoomSDKAudioRawDataSender* sender) override;
  void onMicStartSend() override;
  void onMicStopSend() override;
  void onMicUninitialized() override;

  // FrameSink
  bool CanSend() override;
  bool Send(const int16_t* pcm, int samples) override;

  // VirtualMic
  bool initialised() const override { return initialised_.load(); }
  bool sending() const override { return can_send_.load(); }
  uint64_t send_failures() const override { return send_failures_.load(); }
  int last_error() const override { return last_error_.load(); }

 private:
  mutable std::mutex sender_m_;
  ZOOM_SDK_NAMESPACE::IZoomSDKAudioRawDataSender* sender_ = nullptr;
  std::atomic<bool> can_send_{false};
  std::atomic<bool> initialised_{false};
  std::atomic<uint64_t> send_failures_{0};
  std::atomic<int> last_error_{0};
};

}  // namespace zc
```

- [ ] **Step 3: Move the .cpp**

Create `src/audio/mic_source_win.cpp` as today's `src/audio/mic_source.cpp` with `#include "mic_source_win.h"` and every `ZoomMicSource::` renamed to `ZoomMicSourceWin::`. **Change nothing else** — not the lock scopes, not the store order in `onMicUninitialized`, not the printf text, not the comments.

The teardown body is the part that must not drift:

```cpp
void ZoomMicSourceWin::onMicUninitialized() {
  // Order matters. Shut the gate before dropping the pointer, so a TX thread
  // that has already passed CanSend() cannot reach a null sender_ -- and take
  // the same lock Send() takes, so one that is already inside finishes first.
  can_send_.store(false);
  initialised_.store(false);
  {
    std::lock_guard<std::mutex> lock(sender_m_);
    sender_ = nullptr;
  }
  std::printf("[mic] onMicUninitialized -- sender revoked\n");
}
```

Then delete `src/audio/mic_source.h` and `src/audio/mic_source.cpp`.

- [ ] **Step 4: Update the build**

In `CMakeLists.txt`, in the Windows-only `target_sources(zcomms_zoom PRIVATE ...)` block, replace `src/audio/mic_source.cpp` with `src/audio/mic_source_win.cpp`. The new `virtual_mic.h` is header-only and needs no source entry, but confirm `src/audio` is already on the include path for `zcomms_zoom` (it is, via `zcomms_audio`'s `PUBLIC` include dirs).

- [ ] **Step 5: Confirm the seam is Zoom-free on macOS**

```bash
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac
ctest --test-dir build-mac --output-on-failure
```

Expected: PASS. `virtual_mic.h` is not yet included by anything in the macOS build, so this only proves nothing regressed — Task 2 is what actually compiles the seam on macOS.

- [ ] **Step 6: Commit and let CI judge Windows**

```bash
git add src/audio/virtual_mic.h src/audio/mic_source_win.h src/audio/mic_source_win.cpp CMakeLists.txt
git add -u src/audio/
git commit -m "refactor(mic): split the virtual mic into a seam and a Windows backend

ZoomMicSource did two jobs -- Zoom's IZoomSDKVirtualAudioMicEvent and the
FrameSink the TX pacer drives. Only the second is portable, so only the
second is above the seam now. The four-callback contract, the teardown
order and the lock discipline move to the Windows backend unchanged; the
crash they prevent (a TX thread reaching a revoked sender at 20 ms
cadence) is the reason none of it was allowed to drift."
git push
```

Then confirm both green:

```bash
gh api repos/iamfatness/ZComms/commits/$(git rev-parse HEAD)/check-runs \
  --jq '[.check_runs[] | .name + ":" + (.conclusion // "running")]'
```

Expected: `["Windows build & test:success","macOS build & test:success"]`. **If Windows goes red, the move changed something — fix forward or revert; do not proceed.**

---

### Task 2: The send-window law's first tests

`mic_source.cpp` has never had a test, for the same reason `talkback_channels.cpp` never did: it inherited a Zoom interface and could not be constructed without the SDK. Task 1 fixed that for the `FrameSink` half. These tests pin the contract CLAUDE.md calls load-bearing.

The Windows backend still cannot be constructed on macOS (it inherits the SDK interface). So the tests drive a **fake `VirtualMic`** that reproduces the same gate-and-lock discipline — which pins the *contract* the pacer depends on, not one backend's implementation of it.

**Files:**
- Create: `tests/audio/fake_mic_sender.h`, `tests/audio/test_virtual_mic.cpp`
- Modify: `tests/audio/test_util.h`, `tests/audio/test_main.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `zc::VirtualMic`, `zc::FrameSink`.
- Produces: `zctest::FakeVirtualMic`, `void TestVirtualMic()`.

- [ ] **Step 1: Write the fake**

Create `tests/audio/fake_mic_sender.h`:

```cpp
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
```

- [ ] **Step 2: Write the tests**

Create `tests/audio/test_virtual_mic.cpp`:

```cpp
#include "fake_mic_sender.h"
#include "test_util.h"

using zctest::FakeVirtualMic;

void TestVirtualMic() {
  ZC_TEST("a send before onMicInitialize is refused");
  {
    // Zoom hands the sender over in onMicInitialize. Before that there is
    // nothing to send into, and the pacer must be told so rather than
    // discovering it by dereferencing null on the 20 ms tick.
    FakeVirtualMic mic;
    const int16_t pcm[160] = {0};
    ZC_CHECK(!mic.CanSend());
    ZC_CHECK(!mic.Send(pcm, 160));
    ZC_CHECK(mic.accepted.empty());
  }

  ZC_TEST("initialised is not the same as sending");
  {
    // The two states answer different operator questions: no sender at all
    // (usually a missing raw-data entitlement) versus a sender whose window
    // is shut (usually a muted meeting mic). Collapsing them loses the
    // distinction that tells the operator what to fix.
    FakeVirtualMic mic;
    mic.Initialize();
    ZC_CHECK(mic.initialised());
    ZC_CHECK(!mic.sending());
    ZC_CHECK(!mic.CanSend());
  }

  ZC_TEST("send is legal only between StartSend and StopSend");
  {
    // The law, from CLAUDE.md: send() is legal only between onMicStartSend
    // and onMicStopSend.
    FakeVirtualMic mic;
    const int16_t pcm[160] = {7};
    mic.Initialize();
    ZC_CHECK(!mic.Send(pcm, 160));      // window shut
    mic.StartSend();
    ZC_CHECK(mic.CanSend());
    ZC_CHECK(mic.Send(pcm, 160));       // window open
    ZC_CHECK(mic.accepted.size() == 1u);
    mic.StopSend();
    ZC_CHECK(!mic.CanSend());
    ZC_CHECK(!mic.Send(pcm, 160));      // window shut again
    ZC_CHECK(mic.accepted.size() == 1u);
  }

  ZC_TEST("a revoked sender refuses sends, it does not crash");
  {
    // onMicUninitialized drops the pointer. A TX thread that had already
    // passed CanSend() must find a closed gate rather than a revoked
    // pointer -- that failure is a crash, not a glitch, and a 20 ms cadence
    // finds it quickly.
    FakeVirtualMic mic;
    const int16_t pcm[160] = {0};
    mic.Initialize();
    mic.StartSend();
    ZC_CHECK(mic.Send(pcm, 160));
    mic.Uninitialize();
    ZC_CHECK(!mic.initialised());
    ZC_CHECK(!mic.CanSend());
    ZC_CHECK(!mic.Send(pcm, 160));
    ZC_CHECK(mic.accepted.size() == 1u);
  }

  ZC_TEST("a refused send is counted and its code kept");
  {
    // fails=0 alone cannot distinguish "Zoom accepted audio" from "nothing
    // was ever sent" -- the 2026-08-29 no-audio hunt stalled on exactly
    // that ambiguity in the talkback path.
    FakeVirtualMic mic;
    const int16_t pcm[160] = {0};
    mic.Initialize();
    mic.StartSend();
    mic.fail_with = 3;
    ZC_CHECK(!mic.Send(pcm, 160));
    ZC_CHECK(mic.send_failures() == 1u);
    ZC_CHECK(mic.last_error() == 3);
    ZC_CHECK(mic.accepted.empty());
  }

  ZC_TEST("re-initialising reopens the mic from a clean state");
  {
    // Zoom can tear the virtual mic down and hand it back within one
    // session. A stale can_send_ across that boundary would make the pacer
    // send into a sender Zoom has not opened yet.
    FakeVirtualMic mic;
    const int16_t pcm[160] = {0};
    mic.Initialize();
    mic.StartSend();
    mic.Uninitialize();
    mic.Initialize();
    ZC_CHECK(mic.initialised());
    ZC_CHECK(!mic.sending());
    ZC_CHECK(!mic.Send(pcm, 160));
    mic.StartSend();
    ZC_CHECK(mic.Send(pcm, 160));
  }
}
```

- [ ] **Step 3: Wire into the harness**

In `tests/audio/test_util.h`, add to the declarations at the bottom:

```cpp
void TestVirtualMic();
```

In `tests/audio/test_main.cpp`, add the call after `TestTalkbackChannels();`:

```cpp
  TestVirtualMic();
```

In `CMakeLists.txt`, add to the `zcomms_audio_tests` source list after `tests/zoom/test_talkback_channels.cpp`:

```cmake
  tests/audio/test_virtual_mic.cpp
```

- [ ] **Step 4: Run**

```bash
cmake --build build-mac && ./build-mac/zcomms_audio_tests
```

Expected: `ALL TESTS PASSED`. If a check fails, work out whether the test's expectation or the fake is wrong — and if the *fake* diverges from `mic_source_win.cpp`'s real discipline, fix the fake, because its whole value is mirroring it.

- [ ] **Step 5: Mutation-prove every pin**

For each of the six tests, break the thing it pins in `FakeVirtualMic`, confirm that test fails, revert. Suggested mutations, one per test:

| Test | Mutation |
|---|---|
| send before initialize | drop `!have_sender_` from `Send`'s guard |
| initialised ≠ sending | make `CanSend()` return `initialised_.load()` only |
| legal only between Start/Stop | drop `!can_send_.load()` from `Send`'s guard |
| revoked sender refuses | in `Uninitialize`, skip `can_send_.store(false)` |
| refused send counted | in the `fail_with` branch, `return false` without `fetch_add` |
| re-init is clean | in `Initialize`, add `can_send_.store(true)` |

Record each mutation and the exact failure it produced in your report. **A pin that has never failed is not a pin.**

- [ ] **Step 6: Commit**

```bash
git add tests/audio/fake_mic_sender.h tests/audio/test_virtual_mic.cpp \
        tests/audio/test_util.h tests/audio/test_main.cpp CMakeLists.txt
git commit -m "test(mic): pin the virtual mic's send-window law

First tests for this path -- it could not be constructed without the SDK
until the seam landed. Pins the contract CLAUDE.md calls load-bearing:
send() is legal only between onMicStartSend and onMicStopSend, a revoked
sender refuses rather than crashing, initialised and sending answer
different operator questions, and a refusal is counted with its code so
'fails=0' can never again mean both 'all good' and 'nothing was sent'.
Every pin mutation-proved."
git push
```

Confirm both checks green before moving on.

---

### Task 3: The `ZoomClient` seam

A header only, plus its two factory return types. Nothing implements it yet, so nothing can break — but Task 4 and all of P1-B are written against this exact shape.

**The scope decision this encodes** (owner ruling 2026-09-05): the seam carries only what both platforms can do. `GetTalkbackController()` becomes a portable `MakeTalkbackSdk()` factory; `InstallVirtualMic()` becomes a factory returning a `VirtualMic`. `GetParticipantsController()`, `GetBOController()` and `GetChatController()` return raw Windows SDK types and their only consumers (`roster`, `breakout`, `chat_signals`) are Windows-only files this phase does not port — so they **stay off the seam**, on the concrete Windows class.

**Files:**
- Create: `src/zoom/zoom_client.h` (replacing today's concrete declaration — Task 4 moves that to `zoom_client_win.h`)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `zc::TalkbackSdk` (`src/zoom/talkback_sdk.h`), `zc::VirtualMic` (`src/audio/virtual_mic.h`).
- Produces: `zc::ZoomClient` (abstract), `zc::JoinResult`, `zc::MeetingState`.

- [ ] **Step 1: Write the seam header**

Replace `src/zoom/zoom_client.h` entirely:

```cpp
// The Zoom Meeting SDK's lifecycle, as ZComms needs it -- and nothing else.
//
// Two implementations: Windows (IAuthService/IMeetingService, callbacks
// delivered by pumping a Win32 message loop) and macOS (the ObjC framework,
// callbacks delivered on a run loop). Neither shape is visible here.
//
// WHAT IS DELIBERATELY NOT ON THIS INTERFACE. The Windows class also hands
// out IMeetingParticipantsController, IMeetingBOController and
// IMeetingChatController. Those are raw Windows SDK types, and their only
// consumers -- roster.cpp, breakout.cpp, chat_signals.cpp -- are themselves
// Windows-only (they derive from ZOOM_SDK_NAMESPACE interfaces; see
// docs/plans/2026-09-04-macos-port.md section 2's 2026-09-05 amendment).
// Putting them here would drag the whole Windows SDK across the seam to
// serve code that cannot run on the other side of it. They stay on
// ZoomClientWin, fetched once at construction.
//
// Talkback and the virtual mic go the other way: they WERE raw-pointer
// getters and are now FACTORIES, so each backend builds its own adapter and
// no SDK pointer crosses this line.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "talkback_sdk.h"
#include "virtual_mic.h"

namespace zc {

// Where a meeting is, normalised. The platforms enumerate these differently
// and ZComms only ever asks two questions of them -- see in_meeting() and
// session_alive() below.
enum class MeetingState {
  Idle,
  Connecting,
  WaitingForHost,
  InWaitingRoom,
  InMeeting,
  Reconnecting,
  JoiningBreakout,
  LeavingBreakout,
  Failed,
  Ended,
};

const char* MeetingStateName(MeetingState s);

// The passcode conversation's state. Zoom asks for a passcode through a
// callback; with nobody listening a passcode-protected meeting dies as a
// misleading "meeting ended" join failure (live-diagnosed 2026-08-29).
enum class PasscodeState {
  NotAsked,
  Needed,
  WasWrong,
};

class ZoomClient {
 public:
  virtual ~ZoomClient() = default;

  virtual bool Init(std::string* error) = 0;

  // Auth with a ready-made Meeting SDK JWT (the broker mints it from the
  // operator's OAuth session). Paired with a ZAK on Join, the client is the
  // operator's account rather than an anonymous guest, which is what lifts
  // the cross-account join refusal (fail 504). Blocks, pumping, until the
  // SDK answers or `timeout_ms` elapses.
  virtual bool AuthenticateWithJwt(const std::string& jwt, int timeout_ms,
                                   std::string* error) = 0;

  // `on_tick` runs every pump iteration of the join wait so the caller can
  // surface progress, supply a passcode -- and CANCEL: returning false
  // aborts the join. Before that existed, the operator's only exit from a
  // stuck waiting room was killing the app (2026-08-30).
  // `zak` non-empty joins as the signed-in user.
  virtual bool Join(uint64_t meeting_number, const std::string& password,
                    const std::string& display_name, int timeout_ms,
                    std::string* error,
                    const std::function<bool()>& on_tick = nullptr,
                    const std::string& zak = std::string()) = 0;

  virtual PasscodeState passcode_state() const = 0;
  virtual bool SubmitPasscode(const std::string& passcode) = 0;

  // Installs a never-fed virtual mic. This is the auto-suppress trick: an
  // open-but-silent meeting mic, so talkback stays deliverable (delivery law
  // 1: talkback arrives ONLY while this client's meeting audio is open)
  // while the room hears nothing. Returns null on failure with `error` set --
  // typically an auth tier without the raw-data entitlement, where the
  // callbacks never fire and the operator must point Zoom at a dead input.
  // The caller owns the result and must keep it alive for the session.
  virtual std::unique_ptr<VirtualMic> InstallVirtualMic(std::string* error) = 0;

  // The talkback controller for this meeting, already wrapped. Null when the
  // meeting has none. The caller owns the result and must keep it alive for
  // as long as anything holds a TalkbackSdk* into it.
  virtual std::unique_ptr<TalkbackSdk> MakeTalkbackSdk() = 0;

  virtual bool JoinVoip(std::string* error) = 0;
  virtual bool LeaveVoip(std::string* error) = 0;

  // Unmutes this client. A meeting with mute-on-entry admits the client
  // muted, and a muted client's virtual mic never receives onMicStartSend --
  // the send window simply stays shut. Host-side unmute of an SDK client can
  // require a consent handshake ZComms does not implement, so it unmutes
  // itself.
  virtual bool UnmuteSelf(std::string* error) = 0;
  virtual bool SelfMuted() = 0;
  // Logs this client's audio connection and mute state, so "the send window
  // never opened" comes with the reason attached instead of being a mystery.
  virtual void LogSelfAudioState(const char* tag) = 0;

  // Everyone except this client -- the people a talkback channel addresses.
  virtual std::vector<unsigned int> GetOtherParticipants() = 0;
  // Host/co-host only; a no-permission failure is expected when not host and
  // is not worth surfacing every tick.
  virtual bool AdmitAllWaiting() = 0;

  virtual void Leave() = 0;
  virtual void Cleanup() = 0;

  // Runs the platform's callback loop for `ms`. Anything that waits in this
  // codebase waits by calling this -- a harness that simply slept would
  // authenticate and join exactly never, which presents as a silent hang.
  virtual void Pump(int ms) = 0;

  virtual MeetingState state() const = 0;
  // The local failure code from the last failed join, or 0. Kept distinct
  // from state(): Zoom's FAILED code gets clobbered by the ENDED that
  // follows it (ENDED carries result 0), so the code is latched.
  virtual int last_fail_code() const = 0;
  virtual std::string FailReason(int code) const = 0;

  bool in_meeting() const { return state() == MeetingState::InMeeting; }

  // The session-loop liveness test, distinct from in_meeting(): a breakout
  // move transitions through Join/LeaveBreakout and Reconnecting, and a
  // network blip through Reconnecting. Treating those as "meeting over" tore
  // the whole session down on the FIRST live room hop (2026-08-30).
  // Connecting appears mid-session during a breakout move's rejoin leg; by
  // construction the session loop only runs after the first join, so here it
  // never means "still joining".
  bool session_alive() const {
    switch (state()) {
      case MeetingState::InMeeting:
      case MeetingState::JoiningBreakout:
      case MeetingState::LeavingBreakout:
      case MeetingState::Reconnecting:
      case MeetingState::Connecting:
        return true;
      default:
        return false;
    }
  }

  // Local failure code: the signed-in account is already in a meeting on
  // another device and we refused to end it. Chosen outside Zoom's
  // MeetingFailCode range.
  static constexpr int kFailAccountBusyElsewhere = 909001;
};

}  // namespace zc
```

- [ ] **Step 2: Write the one name helper**

Create `src/zoom/zoom_client.cpp`:

```cpp
#include "zoom_client.h"

namespace zc {

const char* MeetingStateName(MeetingState s) {
  switch (s) {
    case MeetingState::Idle: return "IDLE";
    case MeetingState::Connecting: return "CONNECTING";
    case MeetingState::WaitingForHost: return "WAITING_FOR_HOST";
    case MeetingState::InWaitingRoom: return "IN_WAITING_ROOM";
    case MeetingState::InMeeting: return "IN_MEETING";
    case MeetingState::Reconnecting: return "RECONNECTING";
    case MeetingState::JoiningBreakout: return "JOIN_BREAKOUT_ROOM";
    case MeetingState::LeavingBreakout: return "LEAVE_BREAKOUT_ROOM";
    case MeetingState::Failed: return "FAILED";
    case MeetingState::Ended: return "ENDED";
  }
  return "UNKNOWN";
}

}  // namespace zc
```

No `default:` label, deliberately: adding an enumerator should draw `-Wswitch` (on by default in Clang; verified by mutation during P0) rather than falling silently into "UNKNOWN". The trailing `return` satisfies compilers that cannot see the switch is exhaustive.

- [ ] **Step 3: Add it to the SDK-free test target**

In `CMakeLists.txt`, add to the `zcomms_audio_tests` source list after `src/zoom/talkback_channels.cpp`:

```cmake
  src/zoom/zoom_client.cpp
```

- [ ] **Step 4: Prove the header is SDK-free on macOS**

```bash
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target zcomms_audio_tests
```

Expected: PASS. **This is the proof the seam is portable.** If it fails on a Zoom or Windows symbol, the header is pulling something it must not — fix it here, because Task 4 and all of P1-B depend on this compiling with no SDK.

Note: Windows CI will be RED after this commit — `zoom_client_win.*` does not exist yet, so `zcomms_zoom` no longer has a `ZoomClient` implementation. That is expected and Task 4 closes it. **Commit this and Task 4 together if you prefer a green history**; otherwise push and accept one red Windows run, then confirm green after Task 4.

- [ ] **Step 5: Commit**

```bash
git add src/zoom/zoom_client.h src/zoom/zoom_client.cpp CMakeLists.txt
git commit -m "feat(zoom): the ZoomClient seam

Portable subset only. GetTalkbackController and InstallVirtualMic were
raw-pointer getters and are now factories, so no SDK pointer crosses the
line. The participants/BO/chat controllers deliberately do NOT come with
them: they return Windows SDK types and their only consumers are
Windows-only files this phase does not port, so they stay on the concrete
Windows class rather than dragging the SDK across the seam.

MeetingStatus collapses to a normalised MeetingState -- session_alive()
keeps its exact 2026-08-30 shape, because treating a breakout hop as
'meeting over' tore the session down on the first live room change."
```

---

### Task 4: The Windows backend, and rewiring the app

Moves today's `ZoomClient` implementation behind the seam and adds the two factories. This is the task where Windows can break, and CI is the only thing that will say so.

**Files:**
- Create: `src/zoom/zoom_client_win.h`, `src/zoom/zoom_client_win.cpp`
- Delete: the old concrete declaration and definition (moved)
- Modify: `src/app/main.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `zc::ZoomClient`, `zc::MeetingState`, `zc::PasscodeState`, `zc::VirtualMic`, `zc::TalkbackSdk`, `zc::ZoomMicSourceWin`, `zc::TalkbackSdkWin`.
- Produces: `zc::ZoomClientWin` — implements `ZoomClient`, plus three Windows-only accessors: `GetParticipantsController()`, `GetBOController()`, `GetChatController()`.

- [ ] **Step 1: Create the Windows header**

`src/zoom/zoom_client_win.h` is today's `zoom_client.h` with these changes and no others:

1. Class head becomes:
   ```cpp
   class ZoomClientWin : public ZoomClient,
                         public ZOOM_SDK_NAMESPACE::IAuthServiceEvent,
                         public ZOOM_SDK_NAMESPACE::IMeetingServiceEvent,
                         public ZOOM_SDK_NAMESPACE::IMeetingAudioCtrlEvent,
                         public ZOOM_SDK_NAMESPACE::IMeetingConfigurationEvent {
   ```
2. `#include "zoom_client.h"` added.
3. Every method that is on the seam gains `override` and its seam signature. Concretely: `Init`, `AuthenticateWithJwt`, `Join`, `SubmitPasscode`, `JoinVoip`, `LeaveVoip`, `UnmuteSelf`, `SelfMuted`, `LogSelfAudioState`, `GetOtherParticipants`, `AdmitAllWaiting`, `Leave`, `Cleanup`, `Pump`.
4. `passcode_state()` returns `PasscodeState` instead of `int`, and `state()` returns `MeetingState` — both `override`.
5. `InstallVirtualMic(ZoomMicSource*, std::string*)` becomes `std::unique_ptr<VirtualMic> InstallVirtualMic(std::string* error) override;`
6. `GetTalkbackController()` is **deleted** and replaced by `std::unique_ptr<TalkbackSdk> MakeTalkbackSdk() override;` (Step 3 inlines its one-line body into the factory).
7. `GetParticipantsController()`, `GetBOController()`, `GetChatController()` stay exactly as they are — **not** `override`, they are Windows-only extras.
8. `int last_fail_code() const override;` and `std::string FailReason(int code) const override;` replace the free `MeetingFailReason`.
9. `status()`, `in_meeting()` and `session_alive()` are **deleted** — the seam provides the latter two, and `status()` is replaced by `state()`.

Keep `Authenticate(public_app_key, sdk_key, sdk_secret, ...)` as a Windows-only extra if `main.cpp` still calls it (check with `grep -n "\.Authenticate(" src/app/main.cpp`); it is the `--anon` guest path and is not on the seam.

- [ ] **Step 2: Move the implementation**

```bash
git mv src/zoom/zoom_client.cpp src/zoom/zoom_client_win.cpp
```

Then in `zoom_client_win.cpp`: change the include to `zoom_client_win.h`, rename every `ZoomClient::` to `ZoomClientWin::`, and apply exactly these behavioural-neutral adaptations:

- `status_` stays a `std::atomic<MeetingStatus>` internally. Add a private mapper and have `state()` return through it:

  ```cpp
  namespace {
  MeetingState ToState(MeetingStatus s) {
    switch (s) {
      case MEETING_STATUS_INMEETING:            return MeetingState::InMeeting;
      case MEETING_STATUS_CONNECTING:           return MeetingState::Connecting;
      case MEETING_STATUS_WAITINGFORHOST:       return MeetingState::WaitingForHost;
      case MEETING_STATUS_IN_WAITING_ROOM:      return MeetingState::InWaitingRoom;
      case MEETING_STATUS_RECONNECTING:         return MeetingState::Reconnecting;
      case MEETING_STATUS_JOIN_BREAKOUT_ROOM:   return MeetingState::JoiningBreakout;
      case MEETING_STATUS_LEAVE_BREAKOUT_ROOM:  return MeetingState::LeavingBreakout;
      case MEETING_STATUS_FAILED:               return MeetingState::Failed;
      case MEETING_STATUS_ENDED:                return MeetingState::Ended;
      default:                                  return MeetingState::Idle;
    }
  }
  }  // namespace
  ```

  A `default:` **is** correct here — `MeetingStatus` is Zoom's enum with many values ZComms does not model, and they all mean "not one of the states we act on".

  **Verify every enumerator name against `third_party/zoom-sdk/h/meeting_service_interface.h` before building.** Names above are from the existing `MeetingStatusName` in the file you are moving — read them out of it rather than trusting this list.

- `passcode_state()` returns `PasscodeState`: `0 → NotAsked`, `1 → Needed`, `2 → WasWrong`. The internal `std::atomic<int>` is unchanged.
- `FailReason(int)` is the existing free `MeetingFailReason(int)` made a member. Keep the free function too if anything else calls it (`grep -rn "MeetingFailReason" src/`).

- [ ] **Step 3: Write the two factories**

Append to `zoom_client_win.cpp`:

Replace the old `InstallVirtualMic(ZoomMicSource*, std::string*)` with the factory. The body is the old one unchanged except that it creates the mic instead of receiving it, and returns it instead of a bool:

```cpp
std::unique_ptr<VirtualMic> ZoomClientWin::InstallVirtualMic(
    std::string* error) {
  if (!HasRawdataLicense()) {
    // Worth checking explicitly. Without the raw-data entitlement the calls
    // below can succeed and simply never fire a callback, which looks like a
    // hang rather than like a licensing problem.
    std::printf("[sdk] WARNING: HasRawdataLicense() is false -- the virtual "
                "mic callbacks may never fire\n");
  }
  IZoomSDKAudioRawDataHelper* helper = GetAudioRawdataHelper();
  if (helper == nullptr) {
    *error = "GetAudioRawdataHelper returned null";
    return nullptr;
  }
  auto mic = std::make_unique<ZoomMicSourceWin>();
  const SDKError err = helper->setExternalAudioSource(mic.get());
  if (err != SDKERR_SUCCESS) {
    *error = "setExternalAudioSource failed: " +
             std::to_string(static_cast<int>(err));
    return nullptr;
  }
  // The SDK now holds a bare pointer to *mic as its event sink, so the
  // caller owning this unique_ptr is what keeps that pointer valid. Same
  // contract the old signature had, made explicit by ownership.
  return mic;
}

std::unique_ptr<TalkbackSdk> ZoomClientWin::MakeTalkbackSdk() {
  IMeetingTalkbackController* c =
      meeting_ != nullptr ? meeting_->GetMeetingTalkbackController() : nullptr;
  if (c == nullptr) return nullptr;
  return std::make_unique<TalkbackSdkWin>(c);
}
```

**Note the ordering change inside `InstallVirtualMic`:** the mic is constructed *after* the helper null-check rather than by the caller beforehand. That is the only sequencing difference, and it is unobservable — nothing else touches the mic between those two points.

Delete the old `GetTalkbackController()` member entirely; its one-line body is now inlined above and nothing else calls it (verify with `grep -rn "GetTalkbackController" src/`).

Add includes: `#include "mic_source_win.h"`, `#include "talkback_sdk_win.h"`, `#include <memory>`.

- [ ] **Step 4: Rewire `main.cpp`**

Four edits, all at known sites:

1. `main.cpp:1151` — `ZoomClient zoom;` becomes `ZoomClientWin zoom;`. Include `zoom_client_win.h` instead of `zoom_client.h`.
2. `main.cpp:1245-1252` — the virtual mic block becomes:
   ```cpp
   // never-fed virtual mic gives an open-but-silent meeting mic, so the
   // room hears nothing while the channels stay deliverable. Under an auth
   // tier without the raw-data entitlement the callbacks never fire -- the
   // meeting then hears whatever device Zoom captures, and the operator
   // must point Zoom at a dead input; say which world we are in.
   std::unique_ptr<VirtualMic> silent_mic =  // deliberately never fed
       zoom.InstallVirtualMic(&err);
   if (silent_mic) {
     log_op("meeting mic auto-suppressed (open but silent to the room)");
   } else {
     log_op("mic auto-suppress unavailable (" + err +
            ") -- the room may hear Zoom's mic device");
   }
   ```
   `silent_mic` must stay in scope for the whole session — it owns the SDK's event sink.
3. `main.cpp:1305` — the talkback adapter becomes:
   ```cpp
   // The adapter must outlive TalkbackChannels -- it holds the SDK's event
   // registration and forwards into it.
   auto talkback_sdk = zoom.MakeTalkbackSdk();
   TalkbackChannels bank(talkback_sdk.get());
   ```
   Guard the null case the way the old code guarded a null controller.
4. Every `zoom.status()` becomes `zoom.state()` and every `MeetingStatusName(...)` on that value becomes `MeetingStateName(...)`. Find them with `grep -n "zoom\.status()\|MeetingStatusName" src/app/main.cpp`. `in_meeting()` and `session_alive()` are unchanged — the seam provides both.

The three Windows-only getters at `main.cpp:1254/1260/1272` are **unchanged** — `zoom` is a `ZoomClientWin`, which still has them.

- [ ] **Step 5: Update the build**

In `CMakeLists.txt`'s Windows-only `target_sources(zcomms_zoom PRIVATE ...)` block, replace `src/zoom/zoom_client.cpp` with `src/zoom/zoom_client_win.cpp`. `src/zoom/zoom_client.cpp` (the seam's name helper) belongs in the **shared** list alongside `talkback_sdk.cpp`.

- [ ] **Step 6: Confirm the SDK-free build still passes**

```bash
cmake --build build-mac && ctest --test-dir build-mac --output-on-failure
```

Expected: PASS. The seam compiles on macOS with no SDK; the Windows backend is not in the macOS build at all.

- [ ] **Step 7: Commit and let CI judge Windows**

```bash
git add src/zoom/zoom_client_win.h src/zoom/zoom_client_win.cpp \
        src/app/main.cpp CMakeLists.txt
git add -u src/zoom/
git commit -m "refactor(zoom): put the app on the ZoomClient seam

ZoomClient's implementation becomes ZoomClientWin behind the abstract
seam. Two raw-pointer getters become factories -- MakeTalkbackSdk and
InstallVirtualMic -- so no SDK pointer crosses the line and each backend
builds its own adapter. The participants/BO/chat getters stay on the
concrete class, because their consumers are Windows-only and putting
them on the seam would drag the Windows SDK across it to serve code that
cannot run on the far side.

Behaviour is unchanged: the status enum is mapped, not reinterpreted,
and session_alive() keeps its exact 2026-08-30 shape."
git push
gh api repos/iamfatness/ZComms/commits/$(git rev-parse HEAD)/check-runs \
  --jq '[.check_runs[] | .name + ":" + (.conclusion // "running")]'
```

Expected: both green. **If Windows goes red, read the log and fix forward — this is the task the gate exists for.**

---

## Done when

- Both CI checks green on the final commit.
- `virtual_mic.h` and `zoom_client.h` compile on macOS with no Zoom SDK present.
- The virtual mic's send-window law has six tests, every one mutation-proved.
- `main.cpp` builds against the seam, with the three Windows-only getters still served by the concrete class.
- Windows behaviour is unchanged — no logic edited, only types and one enum mapping.

**Not done, deliberately:** no macOS backend exists for either seam, and nothing has talked to a live meeting. That is P1-B: `mic_source_mac.mm`, `zoom_client_mac.mm`, the headless probe, and the live proof that a real Zoom client hears audio keyed from the Mac.
