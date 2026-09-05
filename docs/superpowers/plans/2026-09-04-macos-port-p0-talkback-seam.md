# ZComms macOS Port — P0 + TalkbackSdk Seam — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the Windows CI gate that every later task depends on, then put the `TalkbackSdk` seam under `TalkbackChannels` so the talkback ladder becomes platform-free, testable, and buildable on macOS.

**Architecture:** `TalkbackChannels` today inherits `IMeetingTalkbackCtrlEvent` and holds a raw `IMeetingTalkbackController*`, which is why it compiles only on Windows and has no tests. This plan introduces `TalkbackSdk` (an abstract seam owned by ZComms, no Zoom headers) plus `TalkbackSdkEvents` (callbacks in). `TalkbackChannels` keeps every line of its healer, pacing, keying and membership logic and only changes the type it calls. A Windows adapter hides the Begin/Add/Execute batch sequences; a macOS adapter wraps `ZoomSDKTalkbackController`, whose equivalents are single atomic calls.

**Tech Stack:** C++17, Objective-C++ (`.mm`) for the macOS adapter, Zoom Meeting SDK (Windows `IMeetingTalkbackController` / macOS `ZoomSDKTalkbackController`), CMake, GitHub Actions, the repo's plain-executable test harness (`tests/audio/test_util.h`, no framework).

**Design spec:** `docs/plans/2026-09-04-macos-port.md`. This plan implements §5.3 (P0) and the `TalkbackSdk` half of §3.1/§3.3.

## Global Constraints

- **Zero behaviour change on Windows.** Windows talkback works and has passed a live gate. Every Windows edit in this plan is a mechanical move or a type substitution. A task that changes Windows *behaviour* is a defect, not progress. **This is verified by CI, never locally** — the development machine is an Apple Silicon Mac with no Windows toolchain. Never ask an implementer to run a Windows build; keep the change mechanical and let Task 1's gate answer.
- **The ladder must never see a raw SDK code.** There are **two** SDK error spaces and both are normalised in the adapters: `SDKError` returned *synchronously* from calls (`18` = `SDKERR_TOO_FREQUENT_CALL`, the code Law 2's backoff keys on) and `TalkbackError` delivered *asynchronously* in callbacks (`NOPERMISSION`, `ALREADY_EXIST`, `COUNT_OVERFLOW`, `NOT_EXIST`, `REJECTED`, `TIMEOUT`).
- **Mono is a law, not a preference.** `ZoomSDKAudioChannel_Stereo` returns success and delivers nothing audible (CLAUDE.md Law 5). Each adapter hardcodes mono at the SDK call; the seam exposes no channel-count parameter, so the law cannot be violated from above.
- **Channel ids cross the seam as `std::string`.** They are ASCII GUIDs. The Windows adapter widens to `std::wstring`; macOS converts to `NSString*`. `TalkbackChannels` holds narrow strings only.
- **Never assert a branch unreachable.** Standing policy in CLAUDE.md; two Majors in this feature lived behind exactly that claim.
- **Tests pin invariants, not implementations,** and every new pin is mutation-proved: break the thing, watch the test fail, revert.
- **Every OS object carries the `ZComms` prefix** (CLAUDE.md §3.3).
- Comment style: state the constraint the code cannot show. When motivated by a real failure, say what happened, with numbers.

---

## File Structure

**Create:**
- `.github/workflows/windows.yml` — the regression gate. Builds and tests on `windows-latest` against the SDK fetched from a private release asset.
- `.github/workflows/macos.yml` — builds the SDK-free targets on `macos-14` (arm64). Extended in a later plan.
- `src/zoom/talkback_sdk.h` — the seam. Two normalised enums, the events interface, the operations interface. **No Zoom headers, no `windows.h`, no Qt.** This purity is what makes the ladder portable and testable.
- `src/zoom/talkback_sdk_win.h` / `.cpp` — Windows adapter. Owns Begin/Add/Execute and the `zchar_t` conversions.
- `src/zoom/talkback_sdk_mac.h` / `.mm` — macOS adapter over `ZoomSDKTalkbackController`, plus the delegate object forwarding into `TalkbackSdkEvents`.
- `tests/zoom/fake_talkback_sdk.h` — the test double. Records calls with timestamps, lets a test script return values.
- `tests/zoom/test_talkback_channels.cpp` — the ladder's first tests.

**Modify:**
- `src/zoom/talkback_channels.h` / `.cpp` — hold a `TalkbackSdk*`, implement `TalkbackSdkEvents`. Delete the batch bookkeeping and the `zchar_t` helpers (they move into the Windows adapter). **No logic changes.**
- `src/app/main.cpp` — construct the Windows adapter and pass it in. One site.
- `CMakeLists.txt` — add the adapters, add an Apple arm to the SDK gate, move `talkback_channels.cpp` into the SDK-free test target.
- `tests/audio/test_util.h` — declare `TestTalkbackChannels()`.
- `tests/audio/test_main.cpp` — call it.

**Deliberately not touched:** `src/zoom/duck_plan.*`, `roster.*`, `reach.*`, `room_plan.*`, `signal_*.*`, `breakout.*`, `chat_signals.*`, `talkback_source.*`, and everything in `src/audio/`. They are already portable. **The port is the boundary, not the product.**

**One deliberate deviation from the spec.** §5.1 item 5 called for a new `zcomms_zoom_tests` target. This plan puts the ladder tests in the existing `zcomms_audio_tests` instead, because that target already does exactly what was wanted: it links no `sdk.lib` and already compiles the SDK-free `src/zoom/*.cpp` directly (`CMakeLists.txt:233-237`). A second target would duplicate its include paths and its `add_test` registration for no gain. If the SDK-dependent seams later need a target that *does* link the SDK, add it then, with a reason.

---

### Task 1: The Windows CI gate

Nothing else in this plan is safe without it. The deliverable is a workflow that goes green on a healthy tree **and demonstrably red on a broken one** — an unproven gate is decoration.

The Zoom SDK is gitignored and not redistributable (CLAUDE.md Known Gates), so CI fetches it from a private release asset with `gh release download`. There is no fetch script in the repo today; this task writes the CI-side one inline.

**Files:**
- Create: `.github/workflows/windows.yml`

**Interfaces:**
- Consumes: nothing.
- Produces: a required status check named `windows` that every later task relies on. No code symbols.

- [ ] **Step 1: Confirm the release asset (already staged — verify only)**

The controller created this before execution began. Confirm it is there and unchanged:

```bash
gh release view sdk-assets --repo iamfatness/ZComms --json isDraft,assets \
  --jq '{draft: .isDraft, assets: [.assets[].name]}'
```

Expected:

```json
{"draft":true,"assets":["zoom-sdk-windows-7.1.5.43953.zip","zoom-sdk-macos-7.1.5.84750.zip"]}
```

**`draft` must be `true`.** The Meeting SDK is not redistributable; publishing this release would expose it. If it reads `false`, stop and report — do not continue.

The zips carry a version-named top directory, and the Windows one holds three architecture trees:

```
zoom-sdk-windows-7.1.5.43953/{x64,x86,arm64}/{bin,demo,h,lib}
zoom-sdk-macos-7.1.5.84750/ZoomSDK/ZoomSDK.framework
```

ZComms is x64. **Stage `x64/` explicitly** — CoreVideo shipped a broken Windows release on 2026-08-01 by letting a glob pick an arch tree, and `CMakeLists.txt` only checks that `lib/sdk.lib` exists, not which architecture it is. A wrong-arch `sdk.lib` configures fine and fails at link with unresolved externals.

- [ ] **Step 2: Write the workflow**

Use the tag and asset name confirmed in Step 1 verbatim.

```yaml
name: windows

on:
  push:
    branches: [main, macos-port]
  pull_request:

jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4

      # The Meeting SDK is not redistributable and is gitignored, so a fresh
      # clone has none and CMake falls back to "engine only" -- which would
      # make this gate silently useless: it would compile the audio library,
      # skip every SDK-dependent target, and report success. Step "verify the
      # SDK landed" below is what stops that.
      - name: Fetch the Zoom Windows SDK
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          gh release download sdk-assets --repo ${{ github.repository }} `
            --pattern zoom-sdk-windows-7.1.5.43953.zip --dir "$env:RUNNER_TEMP"
          Expand-Archive -Path "$env:RUNNER_TEMP\zoom-sdk-windows-7.1.5.43953.zip" `
            -DestinationPath "$env:RUNNER_TEMP\sdk" -Force
          New-Item -ItemType Directory -Force -Path third_party | Out-Null
          # x64 EXPLICITLY. The zip also carries x86 and arm64, and CMake only
          # checks that lib/sdk.lib exists -- not which architecture it is. A
          # wrong-arch sdk.lib configures fine and dies at link. CoreVideo
          # shipped a broken Windows release this exact way on 2026-08-01.
          Move-Item "$env:RUNNER_TEMP\sdk\zoom-sdk-windows-7.1.5.43953\x64" `
            third_party\zoom-sdk

      - name: Verify the SDK landed
        run: |
          if (-not (Test-Path third_party\zoom-sdk\lib\sdk.lib)) {
            Write-Error "sdk.lib missing -- CMake would silently build engine-only and this gate would prove nothing"
            exit 1
          }
          if (-not (Test-Path third_party\zoom-sdk\h\meeting_service_components\meeting_talkback_ctrl_interface.h)) {
            Write-Error "talkback header missing -- wrong SDK layout staged"
            exit 1
          }

      - name: Configure
        run: cmake -S . -B build -A x64

      - name: Build
        run: cmake --build build --config Release

      - name: Test
        run: ctest --test-dir build -C Release --output-on-failure
```

- [ ] **Step 3: Push the branch and confirm the gate goes GREEN**

```bash
git add .github/workflows/windows.yml
git commit -m "ci: build and test on Windows"
git push -u origin macos-port
gh run watch
```

Expected: the `windows` job succeeds, and the Test step reports `ALL TESTS PASSED`.

If the SDK download step fails on permissions, the token needs read access to releases on this repo; confirm the release is on this repository and not another.

- [ ] **Step 4: Prove the gate goes RED**

A gate that has never failed is unproven. Break one assertion, watch CI reject it, revert.

```bash
# tests/audio/test_envelope.cpp -- change any ZC_CHECK to something false.
# Example: find a line asserting a value and negate the comparison.
git commit -am "ci: TEMPORARY -- prove the gate fails"
git push
gh run watch
```

Expected: the `windows` job FAILS at the Test step, and the log lists the failing check by file and line.

- [ ] **Step 5: Revert the break**

```bash
git revert --no-edit HEAD
git push
gh run watch
```

Expected: green again.

- [ ] **Step 6: Commit**

Nothing further to commit — Steps 3–5 already pushed. Confirm the branch is clean:

```bash
git status --short
```

Expected: no output.

---

### Task 2: macOS CI, on the targets that already build

`zcomms_audio` and `zcomms_audio_tests` need no Zoom SDK — `zcomms_audio_tests` deliberately links no `sdk.lib` and compiles the SDK-free `src/zoom/*.cpp` directly. So the entire test suite can run on macOS **before any porting work at all**, once CMake links CoreAudio for miniaudio. Doing this now means every later task has a macOS gate.

`loopback.cpp` is dropped on Apple: WASAPI loopback has no macOS equivalent, and per design spec §2 it serves only the measurement tools while the echo canceller runs reference-starved on both platforms today.

**Files:**
- Modify: `CMakeLists.txt:49-70` (the `zcomms_audio` target)
- Create: `.github/workflows/macos.yml`

**Interfaces:**
- Consumes: nothing from Task 1 in code; relies on Task 1's gate existing so Windows regressions surface.
- Produces: a `macos` status check, and a macOS-buildable `zcomms_audio` / `zcomms_audio_tests`.

- [ ] **Step 1: Try the build and record what breaks**

```bash
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target zcomms_audio_tests 2>&1 | tail -40
```

Expected: FAIL. Likely `loopback.cpp` referencing WASAPI types, and/or miniaudio link errors for `AudioUnit` / `AudioToolbox` symbols. Record the actual errors — the next step fixes exactly those.

- [ ] **Step 2: Make `zcomms_audio` build on Apple**

In `CMakeLists.txt`, replace the `add_library(zcomms_audio STATIC ...)` block's source list handling and add framework links. Change the `src/audio/loopback.cpp` entry so it is conditional, and append the framework block after `target_link_libraries(zcomms_audio PUBLIC speexdsp)`:

```cmake
add_library(zcomms_audio STATIC
  src/audio/frame_ring.cpp
  src/audio/tx_pacer.cpp
  src/audio/signal.cpp
  src/audio/correlator.cpp
  src/audio/generator.cpp
  src/audio/envelope.cpp
  src/audio/limiter.cpp
  src/audio/frame_accumulator.cpp
  src/audio/aec.cpp
  src/audio/signal_gate.cpp
  src/audio/channel_mix.cpp
  src/audio/extern_feed.cpp
  src/audio/devices.cpp
  src/audio/wav_sink.cpp
  src/audio/engine.cpp
  src/audio/miniaudio_impl.cpp
)

# loopback is WASAPI render-stream capture. macOS has no OS-level equivalent
# without installing a virtual driver, and it costs nothing to drop: it feeds
# the latency-measurement tools (zcomms-tap, --calibrate), not the shipping
# audio path. engine.cpp records that the echo canceller runs reference-
# starved (passthrough) on BOTH platforms today, so macOS without loopback
# behaves exactly as Windows does. See docs/plans/2026-09-04-macos-port.md §2.
if(NOT APPLE)
  target_sources(zcomms_audio PRIVATE src/audio/loopback.cpp)
endif()

target_include_directories(zcomms_audio PUBLIC src/audio "${MINIAUDIO_DIR}")
target_link_libraries(zcomms_audio PUBLIC speexdsp)

if(APPLE)
  # miniaudio's CoreAudio backend. Without these the link fails on AudioUnit
  # and AudioObjectGetPropertyData symbols rather than at compile time.
  target_link_libraries(zcomms_audio PUBLIC
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework CoreFoundation")
endif()
```

- [ ] **Step 3: Handle whatever else Step 1 reported**

If `zcomms-tap` or `zcomms-engine` fail to link because they used loopback, guard those targets too:

```cmake
if(NOT APPLE)
  add_executable(zcomms-tap tools/tap-detect/main.cpp)
  target_link_libraries(zcomms-tap PRIVATE zcomms_audio)
endif()
```

Apply the same guard to any other target the build names. Do not guard `zcomms-engine` unless it actually fails — check its error before deciding.

- [ ] **Step 4: Build and run the suite on macOS**

```bash
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target zcomms_audio_tests
./build-mac/zcomms_audio_tests
```

Expected: `ALL TESTS PASSED`. This is the first time the suite has run on this machine.

- [ ] **Step 5: Write the macOS workflow**

```yaml
name: macos

on:
  push:
    branches: [main, macos-port]
  pull_request:

jobs:
  build:
    runs-on: macos-14
    steps:
      - uses: actions/checkout@v4

      # No Zoom SDK here yet. zcomms_audio_tests links no sdk.lib by design,
      # so the whole suite runs without it. The SDK-dependent targets arrive
      # with the macOS adapter (Task 6).
      - name: Configure
        run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

      - name: Build tests
        run: cmake --build build --target zcomms_audio_tests

      - name: Test
        run: ctest --test-dir build --output-on-failure
```

- [ ] **Step 6: Commit and confirm both gates**

```bash
git add CMakeLists.txt .github/workflows/macos.yml
git commit -m "ci+build: run the test suite on macOS

zcomms_audio_tests links no sdk.lib by design, so the suite needs no Zoom
SDK -- only CoreAudio for miniaudio. loopback.cpp is dropped on Apple: it
feeds the measurement tools, and the echo canceller runs reference-starved
on both platforms today, so macOS without it matches Windows exactly."
git push
gh run watch
```

Expected: **both** `windows` and `macos` green.

---

### Task 3: The `TalkbackSdk` seam

A header only. Pure, no Zoom types, no `windows.h`. Nothing consumes it yet, so nothing can break — but its exact shape is what Tasks 4, 5 and 6 are written against.

**Files:**
- Create: `src/zoom/talkback_sdk.h`

**Interfaces:**
- Consumes: nothing.
- Produces: `zc::TalkbackCall`, `zc::TalkbackEvent`, `zc::TalkbackSdkEvents`, `zc::TalkbackSdk`, `zc::TalkbackCallName()`, `zc::TalkbackEventName()`.

- [ ] **Step 1: Write the header**

```cpp
// The Zoom talkback controller, as ZComms needs it -- and nothing else.
//
// TalkbackChannels owns the healer, the pacing law and the key mask; this is
// the only thing under it that knows a Zoom SDK exists. Two implementations:
// Windows (IMeetingTalkbackController, whose membership calls are
// Begin/Add/Execute batch sequences) and macOS (ZoomSDKTalkbackController,
// whose equivalents are single atomic calls). Neither shape is visible here,
// which is the point -- the batch mutual-exclusion rules that produced a Major
// on the Windows side have no analogue on macOS and must not leak upward.
//
// Operations are stated SEMANTICALLY. InviteUsers() takes a list because that
// is what the operation means, not because either SDK spells it that way.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zc {

// The SYNCHRONOUS outcome of an SDK call. Distinct from TalkbackEvent below:
// Zoom answers membership calls twice, once by return value and once by
// callback, and the two use different code spaces. Law 2's backoff keys on
// TooFrequent, which only ever arrives this way (Windows SDKERR_TOO_FREQUENT_
// CALL == 18; macOS ZoomSDKError_TooFrequentCall). The ladder must never
// compare a raw integer -- the platforms disagree on the numbers.
enum class TalkbackCall {
  Ok,
  TooFrequent,   // Law 2: back off and retry the SAME item, do not advance.
  NoController,  // No SDK object at all -- not in a meeting, or not host.
  WrongUsage,    // Rejected as invalid, e.g. a cross-breakout invite.
  Failed,        // Anything else. Never silently retried.
};

// The ASYNCHRONOUS outcome, delivered to TalkbackSdkEvents.
enum class TalkbackEvent {
  Ok,
  NoPermission,   // Needs host or co-host.
  AlreadyExists,  // Confirmed presence. NEVER retried -- retrying loops.
  CountOverflow,  // Past the 16-channel cap.
  NotExist,
  Rejected,
  Timeout,
  Unknown,
};

const char* TalkbackCallName(TalkbackCall c);
const char* TalkbackEventName(TalkbackEvent e);

// Callbacks in. Channel ids are narrow ASCII GUIDs on both platforms.
class TalkbackSdkEvents {
 public:
  virtual ~TalkbackSdkEvents() = default;
  virtual void OnCreateChannelResponse(const std::string& channel_id,
                                       TalkbackEvent error) = 0;
  virtual void OnDestroyChannelResponse(const std::string& channel_id,
                                        TalkbackEvent error) = 0;
  virtual void OnChannelUserJoinResponse(const std::string& channel_id,
                                         unsigned int user_id,
                                         TalkbackEvent error) = 0;
  virtual void OnChannelUserLeaveResponse(const std::string& channel_id,
                                          unsigned int user_id,
                                          TalkbackEvent error) = 0;
  virtual void OnJoinTalkbackChannel(unsigned int inviter_id) = 0;
  virtual void OnLeaveTalkbackChannel(unsigned int inviter_id) = 0;
};

class TalkbackSdk {
 public:
  virtual ~TalkbackSdk() = default;

  virtual void SetEvents(TalkbackSdkEvents* events) = 0;
  virtual bool MeetingSupportsTalkback() = 0;

  // Asks for `count` channels in ONE call. CreateChannel is rate-limited
  // (found live by the CoreVideo talkback work), so N channels are never
  // requested as N calls.
  virtual TalkbackCall CreateChannels(unsigned int count) = 0;

  virtual TalkbackCall InviteUsers(const std::string& channel_id,
                                   const std::vector<unsigned int>& user_ids) = 0;
  virtual TalkbackCall RemoveUsers(const std::string& channel_id,
                                   const std::vector<unsigned int>& user_ids) = 0;
  virtual TalkbackCall DestroyChannels(
      const std::vector<std::string>& channel_ids) = 0;

  // One mono frame at the engine's sample rate. There is deliberately NO
  // channel-count parameter: ZoomSDKAudioChannel_Stereo returns success and
  // delivers NOTHING audible (CLAUDE.md Law 5, found live). Each adapter
  // hardcodes mono, so the law cannot be broken from above this line.
  virtual TalkbackCall SendAudio(const std::string& channel_id,
                                 const int16_t* pcm, int samples) = 0;

  // Channel-scoped meeting-audio gain, 0.0-2.0, 1.0 = unity. Zoom ducks
  // channel members BY DEFAULT, so unity is applied at creation and ducking
  // reserved for while the channel is keyed; DuckPlanner owns that policy,
  // this is only the call.
  virtual TalkbackCall SetChannelBackgroundVolume(const std::string& channel_id,
                                                  float volume) = 0;
};

}  // namespace zc
```

- [ ] **Step 2: Write the name helpers**

Create `src/zoom/talkback_sdk.cpp`:

```cpp
#include "talkback_sdk.h"

namespace zc {

const char* TalkbackCallName(TalkbackCall c) {
  switch (c) {
    case TalkbackCall::Ok: return "OK";
    case TalkbackCall::TooFrequent: return "TOO_FREQUENT (rate limited)";
    case TalkbackCall::NoController: return "NO_CONTROLLER";
    case TalkbackCall::WrongUsage: return "WRONG_USAGE";
    case TalkbackCall::Failed: return "FAILED";
  }
  return "FAILED";
}

const char* TalkbackEventName(TalkbackEvent e) {
  switch (e) {
    case TalkbackEvent::Ok: return "OK";
    case TalkbackEvent::NoPermission: return "NO_PERMISSION (need host/co-host)";
    case TalkbackEvent::AlreadyExists: return "ALREADY_EXIST";
    case TalkbackEvent::CountOverflow: return "COUNT_OVERFLOW (max 16)";
    case TalkbackEvent::NotExist: return "NOT_EXIST";
    case TalkbackEvent::Rejected: return "REJECTED";
    case TalkbackEvent::Timeout: return "TIMEOUT";
    case TalkbackEvent::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

}  // namespace zc
```

There is no `default:` label on either switch. That is deliberate: adding an enumerator should produce a compiler warning here, not fall silently into "UNKNOWN". The trailing `return` satisfies compilers that cannot see the switch is exhaustive.

- [ ] **Step 3: Add it to the SDK-free test target so it compiles on both platforms**

In `CMakeLists.txt`, add to the `zcomms_audio_tests` source list, after `src/zoom/duck_plan.cpp`:

```cmake
  src/zoom/talkback_sdk.cpp
```

- [ ] **Step 4: Build on macOS to prove the header is Zoom-free**

```bash
cmake --build build-mac --target zcomms_audio_tests
```

Expected: PASS. If it fails, the header is including something platform-specific — fix it here, because Task 5 depends on this compiling with no SDK present.

- [ ] **Step 5: Commit**

```bash
git add src/zoom/talkback_sdk.h src/zoom/talkback_sdk.cpp CMakeLists.txt
git commit -m "feat(talkback): the TalkbackSdk seam

Two normalised error spaces, not one: Zoom answers membership calls by
return value AND by callback, and they use different codes. Law 2's
backoff keys on TooFrequent, which only ever arrives synchronously.

SendAudio takes no channel-count parameter on purpose -- stereo returns
success and delivers nothing audible (Law 5), so mono is enforced below
this line where it cannot be overridden."
git push
```

Expected: both gates green (the header is unused, so nothing can regress).

---

### Task 4: The Windows adapter, and retargeting the ladder

The mechanical heart of the plan. `TalkbackChannels` stops knowing about Zoom; the batch sequences move into `TalkbackSdkWin`. **No logic changes** — same order of operations, same guards, same counters.

**Files:**
- Create: `src/zoom/talkback_sdk_win.h`, `src/zoom/talkback_sdk_win.cpp`
- Modify: `src/zoom/talkback_channels.h`, `src/zoom/talkback_channels.cpp`, `src/app/main.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `zc::TalkbackSdk`, `zc::TalkbackSdkEvents`, `zc::TalkbackCall`, `zc::TalkbackEvent` from Task 3.
- Produces: `zc::TalkbackSdkWin` (constructor takes `ZOOM_SDK_NAMESPACE::IMeetingTalkbackController*`); `TalkbackChannels(TalkbackSdk*)`.

- [ ] **Step 1: Write the Windows adapter header**

```cpp
// TalkbackSdk over the Windows Meeting SDK.
//
// This file owns two things nothing above it should know: the
// Begin/Add/Execute batch sequences (whose mutual-exclusion rules produced a
// Major), and the zchar_t conversions. Channel ids are ASCII GUIDs, so
// widening is a straight char-by-char copy.
#pragma once

// clang-format off
#include <windows.h>
// clang-format on

#include <string>
#include <vector>

#include "meeting_service_components/meeting_talkback_ctrl_interface.h"
#include "talkback_sdk.h"

namespace zc {

class TalkbackSdkWin : public TalkbackSdk,
                       public ZOOM_SDK_NAMESPACE::IMeetingTalkbackCtrlEvent {
 public:
  explicit TalkbackSdkWin(
      ZOOM_SDK_NAMESPACE::IMeetingTalkbackController* controller);

  void SetEvents(TalkbackSdkEvents* events) override;
  bool MeetingSupportsTalkback() override;
  TalkbackCall CreateChannels(unsigned int count) override;
  TalkbackCall InviteUsers(const std::string& channel_id,
                           const std::vector<unsigned int>& user_ids) override;
  TalkbackCall RemoveUsers(const std::string& channel_id,
                           const std::vector<unsigned int>& user_ids) override;
  TalkbackCall DestroyChannels(
      const std::vector<std::string>& channel_ids) override;
  TalkbackCall SendAudio(const std::string& channel_id, const int16_t* pcm,
                         int samples) override;
  TalkbackCall SetChannelBackgroundVolume(const std::string& channel_id,
                                          float volume) override;

  // IMeetingTalkbackCtrlEvent -- forwarded to events_.
  void onCreateChannelResponse(const zchar_t* channel_id,
                               TalkbackError error) override;
  void onDestroyChannelResponse(const zchar_t* channel_id,
                                TalkbackError error) override;
  void onChannelUserJoinResponse(const zchar_t* channel_id,
                                 unsigned int user_id,
                                 TalkbackError error) override;
  void onChannelUserLeaveResponse(const zchar_t* channel_id,
                                  unsigned int user_id,
                                  TalkbackError error) override;
  void onJoinTalkbackChannel(unsigned int inviter_id) override;
  void onLeaveTalkbackChannel(unsigned int inviter_id) override;
  void onInviterAudioLevel(unsigned int, unsigned int) override {}

 private:
  ZOOM_SDK_NAMESPACE::IMeetingTalkbackController* controller_;
  TalkbackSdkEvents* events_ = nullptr;
};

}  // namespace zc
```

- [ ] **Step 2: Write the Windows adapter implementation**

```cpp
#include "talkback_sdk_win.h"

#include "audio_defs.h"

using namespace ZOOM_SDK_NAMESPACE;

namespace zc {
namespace {

std::string Narrow(const zchar_t* s) {
  if (s == nullptr) return "";
  std::string out;
  for (const zchar_t* p = s; *p != 0; ++p) {
    out.push_back(*p < 128 ? static_cast<char>(*p) : '?');
  }
  return out;
}

std::wstring Widen(const std::string& s) {
  return std::wstring(s.begin(), s.end());  // channel ids are ASCII GUIDs
}

// SDKERR_TOO_FREQUENT_CALL is enum position 18. Law 2's backoff turns on
// exactly this value and nothing above the seam may see the raw number.
TalkbackCall FromSdkError(SDKError err) {
  switch (err) {
    case SDKERR_SUCCESS: return TalkbackCall::Ok;
    case SDKERR_TOO_FREQUENT_CALL: return TalkbackCall::TooFrequent;
    case SDKERR_WRONG_USAGE: return TalkbackCall::WrongUsage;
    default: return TalkbackCall::Failed;
  }
}

TalkbackEvent FromTalkbackError(TalkbackError e) {
  switch (e) {
    case TALKBACK_ERROR_OK: return TalkbackEvent::Ok;
    case TALKBACK_ERROR_NOPERMISSION: return TalkbackEvent::NoPermission;
    case TALKBACK_ERROR_ALREADY_EXIST: return TalkbackEvent::AlreadyExists;
    case TALKBACK_ERROR_COUNT_OVERFLOW: return TalkbackEvent::CountOverflow;
    case TALKBACK_ERROR_NOT_EXIST: return TalkbackEvent::NotExist;
    case TALKBACK_ERROR_REJECTED: return TalkbackEvent::Rejected;
    case TALKBACK_ERROR_TIMEOUT: return TalkbackEvent::Timeout;
    default: return TalkbackEvent::Unknown;
  }
}

}  // namespace

TalkbackSdkWin::TalkbackSdkWin(IMeetingTalkbackController* controller)
    : controller_(controller) {
  if (controller_ != nullptr) controller_->SetEvent(this);
}

void TalkbackSdkWin::SetEvents(TalkbackSdkEvents* events) { events_ = events; }

bool TalkbackSdkWin::MeetingSupportsTalkback() {
  return controller_ != nullptr && controller_->IsMeetingSupportTalkBack();
}

TalkbackCall TalkbackSdkWin::CreateChannels(unsigned int count) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  return FromSdkError(controller_->CreateChannel(count));
}

TalkbackCall TalkbackSdkWin::InviteUsers(
    const std::string& channel_id, const std::vector<unsigned int>& user_ids) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  if (user_ids.empty()) return TalkbackCall::Ok;
  const std::wstring idw = Widen(channel_id);
  SDKError err = controller_->BeginBatchInviteUsers(idw.c_str());
  for (size_t i = 0; i < user_ids.size() && err == SDKERR_SUCCESS; ++i) {
    err = controller_->AddUserToInvite(user_ids[i]);
  }
  if (err == SDKERR_SUCCESS) err = controller_->ExecuteBatchInviteUsers();
  return FromSdkError(err);
}

TalkbackCall TalkbackSdkWin::RemoveUsers(
    const std::string& channel_id, const std::vector<unsigned int>& user_ids) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  if (user_ids.empty()) return TalkbackCall::Ok;
  const std::wstring idw = Widen(channel_id);
  SDKError err = controller_->BeginBatchRemoveUsers(idw.c_str());
  for (size_t i = 0; i < user_ids.size() && err == SDKERR_SUCCESS; ++i) {
    err = controller_->AddUserToRemove(user_ids[i]);
  }
  if (err == SDKERR_SUCCESS) err = controller_->ExecuteBatchRemoveUsers();
  return FromSdkError(err);
}

TalkbackCall TalkbackSdkWin::DestroyChannels(
    const std::vector<std::string>& channel_ids) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  // The Windows controller destroys one channel per call; the seam's list
  // shape follows macOS, which takes them all at once. First refusal wins so
  // a rate limit is reported rather than swallowed by later successes.
  for (const std::string& id : channel_ids) {
    const std::wstring idw = Widen(id);
    const TalkbackCall r = FromSdkError(controller_->DestroyChannel(idw.c_str()));
    if (r != TalkbackCall::Ok) return r;
  }
  return TalkbackCall::Ok;
}

TalkbackCall TalkbackSdkWin::SendAudio(const std::string& channel_id,
                                       const int16_t* pcm, int samples) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  const std::wstring idw = Widen(channel_id);
  return FromSdkError(controller_->SendAudioDataToChannel(
      idw.c_str(), reinterpret_cast<const char*>(pcm),
      static_cast<unsigned int>(samples * static_cast<int>(sizeof(int16_t))),
      kSampleRate, ZoomSDKAudioChannel_Mono));
}

TalkbackCall TalkbackSdkWin::SetChannelBackgroundVolume(
    const std::string& channel_id, float volume) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  const std::wstring idw = Widen(channel_id);
  return FromSdkError(
      controller_->SetChannelBackgroundVolume(idw.c_str(), volume));
}

void TalkbackSdkWin::onCreateChannelResponse(const zchar_t* channel_id,
                                             TalkbackError error) {
  if (events_ != nullptr) {
    events_->OnCreateChannelResponse(Narrow(channel_id),
                                     FromTalkbackError(error));
  }
}

void TalkbackSdkWin::onDestroyChannelResponse(const zchar_t* channel_id,
                                              TalkbackError error) {
  if (events_ != nullptr) {
    events_->OnDestroyChannelResponse(Narrow(channel_id),
                                      FromTalkbackError(error));
  }
}

void TalkbackSdkWin::onChannelUserJoinResponse(const zchar_t* channel_id,
                                               unsigned int user_id,
                                               TalkbackError error) {
  if (events_ != nullptr) {
    events_->OnChannelUserJoinResponse(Narrow(channel_id), user_id,
                                       FromTalkbackError(error));
  }
}

void TalkbackSdkWin::onChannelUserLeaveResponse(const zchar_t* channel_id,
                                                unsigned int user_id,
                                                TalkbackError error) {
  if (events_ != nullptr) {
    events_->OnChannelUserLeaveResponse(Narrow(channel_id), user_id,
                                        FromTalkbackError(error));
  }
}

void TalkbackSdkWin::onJoinTalkbackChannel(unsigned int inviter_id) {
  if (events_ != nullptr) events_->OnJoinTalkbackChannel(inviter_id);
}

void TalkbackSdkWin::onLeaveTalkbackChannel(unsigned int inviter_id) {
  if (events_ != nullptr) events_->OnLeaveTalkbackChannel(inviter_id);
}

}  // namespace zc
```

**Note on `DestroyChannel`:** verify the exact Windows method name and arity against `third_party/zoom-sdk/h/meeting_service_components/meeting_talkback_ctrl_interface.h` before building. If Windows also uses a batch shape (`BeginBatchDestroyChannels`/`Add`/`Execute`), mirror the invite pattern instead of the loop above. `TalkbackChannels` never calls `DestroyChannels` today, so this method is unexercised either way — do not invent behaviour for it beyond compiling.

- [ ] **Step 3: Retarget `talkback_channels.h`**

Replace the includes and the class declaration head. Delete `#include <windows.h>` and the SDK include; add `#include "talkback_sdk.h"`.

```cpp
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "talkback_sdk.h"
```

Change the class head and the event overrides:

```cpp
class TalkbackChannels : public TalkbackSdkEvents {
 public:
  static constexpr int kMaxChannels = 16;  // SDK cap
  static constexpr int kMaxMembers = 10;   // SDK cap per channel

  explicit TalkbackChannels(TalkbackSdk* sdk);
```

Replace the `IMeetingTalkbackCtrlEvent` override block with:

```cpp
  // TalkbackSdkEvents
  void OnCreateChannelResponse(const std::string& channel_id,
                               TalkbackEvent error) override;
  void OnDestroyChannelResponse(const std::string& channel_id,
                                TalkbackEvent error) override;
  void OnChannelUserJoinResponse(const std::string& channel_id,
                                 unsigned int user_id,
                                 TalkbackEvent error) override;
  void OnChannelUserLeaveResponse(const std::string& channel_id,
                                  unsigned int user_id,
                                  TalkbackEvent error) override;
  void OnJoinTalkbackChannel(unsigned int inviter_id) override;
  void OnLeaveTalkbackChannel(unsigned int inviter_id) override;
```

Delete `static const char* ErrorName(TalkbackError e);` — callers use `TalkbackEventName()` from the seam. Change the member and the id cache:

```cpp
  TalkbackSdk* sdk_;
  int want_ = 0;
  // ...
  std::mutex send_m_;
  std::array<std::string, kMaxChannels> send_ids_;  // narrow; adapter converts
```

- [ ] **Step 4: Retarget `talkback_channels.cpp`**

Delete `Narrow`, `Widen`, `ErrorName` and the `using namespace ZOOM_SDK_NAMESPACE` (they now live in the adapter). Replace each SDK call site with its seam call. The substitutions, in order:

| Was | Becomes |
|---|---|
| `controller_->SetEvent(this)` in the ctor | `if (sdk_ != nullptr) sdk_->SetEvents(this);` |
| `controller_ != nullptr && controller_->IsMeetingSupportTalkBack()` | `sdk_ != nullptr && sdk_->MeetingSupportsTalkback()` |
| `controller_->CreateChannel(count)` + `err != SDKERR_SUCCESS` | `sdk_->CreateChannels(count)` + `!= TalkbackCall::Ok`, error text `TalkbackCallName(r)` |
| `BeginBatchInviteUsers`/`AddUserToInvite`/`ExecuteBatchInviteUsers` in `Invite` | `sdk_->InviteUsers(id, {user_id})` |
| the same three in `InviteMany` | `sdk_->InviteUsers(id, user_ids)` |
| `BeginBatchRemoveUsers`/`AddUserToRemove`/`ExecuteBatchRemoveUsers` | `sdk_->RemoveUsers(id, {user_id})` |
| `controller_->SetChannelBackgroundVolume(idw.c_str(), volume) == SDKERR_SUCCESS` | `sdk_->SetChannelBackgroundVolume(id, volume) == TalkbackCall::Ok` |
| `controller_->SendAudioDataToChannel(...)` in `SendToSlot`/`SendToKeyed` | `sdk_->SendAudio(send_ids_[slot], pcm, samples)` |
| `Widen(channels_[i].id)` in `RefreshSendIds` | `channels_[i].id` |
| `controller_ == nullptr` guards | `sdk_ == nullptr` |

`InviteMany`'s rate-limit message keeps its meaning without the magic number:

```cpp
  const TalkbackCall r = sdk_->InviteUsers(id, user_ids);
  if (r != TalkbackCall::Ok) {
    *error = TalkbackCallName(r);
    return false;
  }
  return true;
```

Rename the six event handlers to the `TalkbackSdkEvents` spelling and change their first parameter from `const zchar_t*` to `const std::string&`, deleting the `Narrow()` call at the top of each body. Change `TalkbackError` comparisons to `TalkbackEvent`: `TALKBACK_ERROR_OK` → `TalkbackEvent::Ok`, `TALKBACK_ERROR_ALREADY_EXIST` → `TalkbackEvent::AlreadyExists`, and any `ErrorName(error)` → `TalkbackEventName(error)`.

**Preserve every guard, lock, counter and early return exactly.** If a diff hunk changes control flow, it is wrong.

- [ ] **Step 5: Update the one construction site**

In `src/app/main.cpp`, find where `TalkbackChannels` is constructed from the controller and thread the adapter in. Locate it with:

```bash
grep -n "TalkbackChannels\|GetMeetingTalkbackController" src/app/main.cpp
```

Change the construction to build the adapter first and keep it alive as long as the channels object:

```cpp
  // The adapter must outlive TalkbackChannels -- it holds the SDK's event
  // registration and forwards into it.
  auto talkback_sdk = std::make_unique<TalkbackSdkWin>(talkback_controller);
  TalkbackChannels channels(talkback_sdk.get());
```

Add `#include "talkback_sdk_win.h"` alongside the existing `talkback_channels.h` include.

- [ ] **Step 6: Add the adapter to the build**

In `CMakeLists.txt`, inside the `if(EXISTS "${ZOOM_SDK_DIR}/lib/sdk.lib")` block's `add_library(zcomms_zoom STATIC ...)` source list, add after `src/zoom/talkback_channels.cpp`:

```cmake
    src/zoom/talkback_sdk.cpp
    src/zoom/talkback_sdk_win.cpp
```

- [ ] **Step 7: Confirm the seam is clean on macOS**

`talkback_channels.cpp` should now compile with no SDK present. Move it into the SDK-free test target — in the `zcomms_audio_tests` source list, after `src/zoom/talkback_sdk.cpp`:

```cmake
  src/zoom/talkback_channels.cpp
```

Then:

```bash
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target zcomms_audio_tests
```

Expected: PASS. **This is the proof the seam worked** — the ladder compiles on a machine with no Zoom SDK at all. If it fails on a Zoom symbol, a call site was missed in Step 4.

- [ ] **Step 8: Commit and let CI judge Windows**

```bash
git add src/zoom/talkback_sdk_win.h src/zoom/talkback_sdk_win.cpp \
        src/zoom/talkback_channels.h src/zoom/talkback_channels.cpp \
        src/app/main.cpp CMakeLists.txt
git commit -m "refactor(talkback): put the ladder on the TalkbackSdk seam

TalkbackChannels no longer knows a Zoom SDK exists. The healer, the
pacing law, the key mask and every guard are byte-for-byte the same
behaviour; only the type they call changed. Begin/Add/Execute and the
zchar_t conversions move into TalkbackSdkWin.

The ladder now compiles with no SDK present, which is what makes it
testable at all -- see the next commit."
git push
gh run watch
```

Expected: `windows` GREEN (behaviour unchanged) and `macos` GREEN (ladder now builds there). **If Windows goes red, the diff changed behaviour — fix forward or revert; do not proceed to Task 5.**

---

### Task 5: `FakeTalkbackSdk`, and the ladder's first tests

`talkback_channels.cpp` has never had a test, because it could not be constructed without the Windows SDK. Task 4 removed that. These tests pin laws that were found live and cost real debugging time.

**Files:**
- Create: `tests/zoom/fake_talkback_sdk.h`, `tests/zoom/test_talkback_channels.cpp`
- Modify: `tests/audio/test_util.h`, `tests/audio/test_main.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `zc::TalkbackSdk`, `zc::TalkbackResult`, `zc::TalkbackCall`, `zc::TalkbackEvent`, `zc::TalkbackChannels`.
- Produces: `zctest::FakeTalkbackSdk`, `void TestTalkbackChannels()`.

**Amendments from Task 4's review — read before writing any test:**

1. **The seam returns `TalkbackResult`, not `TalkbackCall`.** Task 4 added a raw platform error code alongside the normalised enum, so the six operation methods now return `struct TalkbackResult { TalkbackCall code; int raw; }`. It has an implicit constructor from `TalkbackCall` and `==`/`!=` against `TalkbackCall`, so `next_result = TalkbackCall::TooFrequent` and `return next_result;` both still work — but the fake's **override return types must say `zc::TalkbackResult`** or they will not compile as overrides. The code below already reflects this.

2. **A null `TalkbackSdk*` is not the old null-controller case.** Task 4 substituted `controller_ == nullptr` for `sdk_ == nullptr`, and `sdk_` is never null in the app. These paths were unreachable before and become reachable here the moment a fake returns `NoController`: `CreateChannels` will mutate `want_` and resize `channels_` *before* failing, and `SendToSlot`/`SendToKeyed` will take `send_m_` and increment `send_failures_` per keyed slot per tick where the original left it at zero. **Do not write a test that assumes the pre-refactor behaviour on these paths** — if you test them at all, pin what the code does now and say in the comment that it differs from the original.

3. **`AlreadyExists` does not record presence.** See the test below — it pins a known bug rather than the intended contract, by owner ruling. Do not "fix" it while writing the test.

- [ ] **Step 1: Write the fake**

```cpp
// A TalkbackSdk that records instead of calling Zoom.
//
// Everything the ladder does to the SDK lands in calls[]; everything the SDK
// would say back is scripted with next_result / Emit*. No timing, no threads:
// these tests pin decisions, not schedules.
#pragma once

#include <string>
#include <vector>

#include "talkback_channels.h"
#include "talkback_sdk.h"

namespace zctest {

struct FakeCall {
  std::string op;                     // "create", "invite", "remove", "send", "volume"
  std::string channel_id;
  std::vector<unsigned int> user_ids;
  float volume = 0.0f;
};

class FakeTalkbackSdk : public zc::TalkbackSdk {
 public:
  std::vector<FakeCall> calls;
  zc::TalkbackCall next_result = zc::TalkbackCall::Ok;
  bool supports = true;

  void SetEvents(zc::TalkbackSdkEvents* events) override { events_ = events; }
  bool MeetingSupportsTalkback() override { return supports; }

  zc::TalkbackResult CreateChannels(unsigned int count) override {
    FakeCall c;
    c.op = "create";
    c.user_ids.push_back(count);
    calls.push_back(c);
    return next_result;
  }

  zc::TalkbackResult InviteUsers(
      const std::string& channel_id,
      const std::vector<unsigned int>& user_ids) override {
    calls.push_back({"invite", channel_id, user_ids, 0.0f});
    return next_result;
  }

  zc::TalkbackResult RemoveUsers(
      const std::string& channel_id,
      const std::vector<unsigned int>& user_ids) override {
    calls.push_back({"remove", channel_id, user_ids, 0.0f});
    return next_result;
  }

  zc::TalkbackResult DestroyChannels(
      const std::vector<std::string>& channel_ids) override {
    for (const std::string& id : channel_ids) {
      calls.push_back({"destroy", id, {}, 0.0f});
    }
    return next_result;
  }

  zc::TalkbackResult SendAudio(const std::string& channel_id, const int16_t*,
                             int samples) override {
    FakeCall c;
    c.op = "send";
    c.channel_id = channel_id;
    c.user_ids.push_back(static_cast<unsigned int>(samples));
    calls.push_back(c);
    return next_result;
  }

  zc::TalkbackResult SetChannelBackgroundVolume(const std::string& channel_id,
                                              float volume) override {
    calls.push_back({"volume", channel_id, {}, volume});
    return next_result;
  }

  // Drive the ladder's callbacks the way Zoom would.
  void EmitChannelCreated(const std::string& id) {
    events_->OnCreateChannelResponse(id, zc::TalkbackEvent::Ok);
  }
  void EmitUserJoined(const std::string& id, unsigned int user_id,
                      zc::TalkbackEvent e = zc::TalkbackEvent::Ok) {
    events_->OnChannelUserJoinResponse(id, user_id, e);
  }

  int CountOp(const std::string& op) const {
    int n = 0;
    for (const FakeCall& c : calls) {
      if (c.op == op) ++n;
    }
    return n;
  }

 private:
  zc::TalkbackSdkEvents* events_ = nullptr;
};

}  // namespace zctest
```

- [ ] **Step 2: Write the failing tests**

```cpp
#include "fake_talkback_sdk.h"
#include "test_util.h"

using zc::TalkbackCall;
using zc::TalkbackChannels;
using zc::TalkbackEvent;
using zctest::FakeTalkbackSdk;

namespace {

// Bring N channels up and confirm them, the way Zoom does: one create call,
// then one response per channel in arrival order.
void BringUp(FakeTalkbackSdk* fake, TalkbackChannels* ch, int n) {
  std::string err;
  ch->CreateChannels(n, &err);
  for (int i = 0; i < n; ++i) {
    fake->EmitChannelCreated("guid-" + std::to_string(i));
  }
}

}  // namespace

void TestTalkbackChannels() {
  ZC_TEST("create asks for every channel in ONE call");
  {
    // N channels as N calls is what tripped the rate limit live on
    // 2026-08-29 with a 12-person roster.
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    std::string err;
    ZC_CHECK(ch.CreateChannels(8, &err));
    ZC_CHECK(fake.CountOp("create") == 1);
    ZC_CHECK(fake.calls[0].user_ids[0] == 8u);
  }

  ZC_TEST("a channel is not ready until Zoom confirms it");
  {
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    std::string err;
    ch.CreateChannels(2, &err);
    ZC_CHECK(ch.channels_ready() == 0);
    fake.EmitChannelCreated("guid-0");
    ZC_CHECK(ch.channels_ready() == 1);
    fake.EmitChannelCreated("guid-1");
    ZC_CHECK(ch.channels_ready() == 2);
  }

  ZC_TEST("invite refuses a channel that is not ready");
  {
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    std::string err;
    ch.CreateChannels(1, &err);
    ZC_CHECK(!ch.Invite(0, 101, &err));
    ZC_CHECK(fake.CountOp("invite") == 0);
  }

  ZC_TEST("InviteMany sends everyone in ONE call");
  {
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    std::string err;
    ZC_CHECK(ch.InviteMany(0, {101, 102, 103}, &err));
    ZC_CHECK(fake.CountOp("invite") == 1);
    ZC_CHECK(fake.calls.back().user_ids.size() == 3u);
  }

  ZC_TEST("a channel refuses an eleventh member");
  {
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    std::string err;
    std::vector<unsigned int> eleven;
    for (unsigned int i = 0; i < 11; ++i) eleven.push_back(100 + i);
    ZC_CHECK(!ch.InviteMany(0, eleven, &err));
    ZC_CHECK(fake.CountOp("invite") == 0);
  }

  ZC_TEST("a rate-limited invite is reported, not swallowed");
  {
    // Law 2: the caller must learn it was refused so it can back off and
    // retry the SAME item. Reporting success here would drop the member.
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    fake.next_result = TalkbackCall::TooFrequent;
    std::string err;
    ZC_CHECK(!ch.InviteMany(0, {101}, &err));
    ZC_CHECK(!err.empty());
  }

  ZC_TEST("a confirmed join is recorded as a member");
  {
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    fake.EmitUserJoined("guid-0", 101);
    ZC_CHECK(ch.Snapshot()[0].members.count(101) == 1u);
  }

  ZC_TEST("ALREADY_EXIST does NOT record presence -- pins a known bug");
  {
    // This pins what the code DOES, which is not what it should do.
    //
    // A member is recorded only on TalkbackEvent::Ok; every other response
    // just sets last_error_. So when Zoom answers an invite with
    // ALREADY_EXIST -- meaning the person IS in the channel -- the ladder
    // does not record them, `want && !have` stays true, and the healer
    // re-invites the same person every 5-60s for the life of the session,
    // spending the rate-limit budget that Law 2 exists to protect.
    //
    // It contradicts talkback_sdk.h's own contract ("Confirmed presence.
    // NEVER retried"). Fixing it is a BEHAVIOUR change and this plan is a
    // move-only port, so the fix is filed separately and needs its own live
    // verification. Owner ruling 2026-09-05: pin reality, file the bug.
    // When that fix lands, this test inverts to == 1u and the comment goes.
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    fake.EmitUserJoined("guid-0", 101, TalkbackEvent::AlreadyExists);
    ZC_CHECK(ch.Snapshot()[0].members.count(101) == 0u);
  }

  ZC_TEST("audio goes ONLY to keyed, ready channels");
  {
    // The routing rule is the entire product: a frame reaches a channel if
    // and only if that channel is keyed.
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 3);
    const int16_t pcm[160] = {0};
    ZC_CHECK(ch.SendToKeyed(pcm, 160) == 0);
    ZC_CHECK(fake.CountOp("send") == 0);
    ch.SetKey(1, true);
    ZC_CHECK(ch.SendToKeyed(pcm, 160) == 1);
    ZC_CHECK(fake.calls.back().channel_id == "guid-1");
    ch.SetKey(2, true);
    ZC_CHECK(ch.SendToKeyed(pcm, 160) == 2);
    ch.SetKey(1, false);
    ch.SetKey(2, false);
    ZC_CHECK(ch.SendToKeyed(pcm, 160) == 0);
  }

  ZC_TEST("a refused send is counted, not silently dropped");
  {
    // fails=0 alone could not distinguish "Zoom accepted audio" from
    // "nothing was ever sent" -- the 2026-08-29 no-audio hunt stalled there.
    FakeTalkbackSdk fake;
    TalkbackChannels ch(&fake);
    BringUp(&fake, &ch, 1);
    ch.SetKey(0, true);
    fake.next_result = TalkbackCall::Failed;
    const int16_t pcm[160] = {0};
    ch.SendToKeyed(pcm, 160);
    ZC_CHECK(ch.send_failures() == 1u);
    ZC_CHECK(ch.channel_sends() == 0u);
    ZC_CHECK(ch.sent_mask() == 0u);
  }
}
```

- [ ] **Step 3: Wire the test into the harness**

In `tests/audio/test_util.h`, add to the declarations at the bottom:

```cpp
void TestTalkbackChannels();
```

In `tests/audio/test_main.cpp`, add the call after `TestDuckPlan();`:

```cpp
  TestTalkbackChannels();
```

In `CMakeLists.txt`, add to the `zcomms_audio_tests` source list after `tests/zoom/test_duck_plan.cpp`:

```cmake
  tests/zoom/test_talkback_channels.cpp
```

- [ ] **Step 4: Run and watch them fail where they should**

```bash
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target zcomms_audio_tests
./build-mac/zcomms_audio_tests
```

Expected: it **builds and runs**. Some checks may fail — that is information, not defeat. Each failure means either the test's expectation is wrong or the ladder does not do what CLAUDE.md claims. **Investigate every failure before changing either side**, and if the ladder is wrong, stop and report rather than "fixing" it inside this task: a behaviour change belongs in its own commit with its own reasoning.

- [ ] **Step 5: Mutation-prove each new pin**

For each test above, break the thing it pins, confirm the test fails, revert. Example for the routing rule:

```bash
# In talkback_channels.cpp SendToKeyed, temporarily drop the key mask from
# the `live` computation so every ready channel is sent to:
#   const uint32_t live = ready_mask_.load();
cmake --build build-mac --target zcomms_audio_tests && ./build-mac/zcomms_audio_tests
```

Expected: FAIL on "audio goes ONLY to keyed, ready channels". Revert. Repeat for the 10-member cap, the rate-limit report, and the send-failure counter. **A pin that never failed is not a pin.**

- [ ] **Step 6: Commit**

```bash
git add tests/zoom/fake_talkback_sdk.h tests/zoom/test_talkback_channels.cpp \
        tests/audio/test_util.h tests/audio/test_main.cpp CMakeLists.txt
git commit -m "test(talkback): first tests for the channel ladder

This file has never had a test -- it could not be constructed without the
Windows SDK. The seam fixed that.

Pins laws that were found live and cost a day each: one create call for N
channels (N calls tripped the rate limit with a 12-person roster), the
10-member cap, a rate-limited invite reported rather than swallowed,
ALREADY_EXIST counted as presence rather than retried forever, and the
routing rule itself -- a frame reaches a channel if and only if it is
keyed. Every pin mutation-proved."
git push
gh run watch
```

Expected: both gates green.

---

### Task 6: The macOS adapter

The last piece of the seam. Compiles and links against the real macOS SDK; it is not exercised live until the next plan brings up `ZoomClient` and the probe tool.

**Files:**
- Create: `src/zoom/talkback_sdk_mac.h`, `src/zoom/talkback_sdk_mac.mm`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `zc::TalkbackSdk`, `zc::TalkbackSdkEvents`, `zc::TalkbackResult`, `zc::TalkbackCall`, `zc::TalkbackEvent`.
- Produces: `zc::TalkbackSdkMac` (constructor takes `ZoomSDKTalkbackController*` as an opaque `void*` at the C++ boundary — see Step 1).

- [ ] **Step 1: Write the header**

The header is included by C++ translation units, so it must not mention Objective-C types. The controller crosses as `void*` and is cast inside the `.mm`.

```cpp
// TalkbackSdk over the macOS Meeting SDK.
//
// The header stays Objective-C-free so plain C++ can include it; the
// controller crosses as void* and is cast in the .mm. macOS is the simpler
// platform here -- inviteUsersToChannel: and removeUsersFromChannel: are
// single atomic calls, so the Begin/Add/Execute mutual-exclusion rules the
// Windows adapter hides have no analogue.
//
// THREADING IS UNRESOLVED. The macOS SDK headers carry no guidance at all
// (every header was grepped). CoreVideo's port concluded membership calls are
// main-queue-only there; whether sendAudioDataToChannel: is too decides
// whether the 20ms TX pacer can call it directly or must hand frames across.
// The next plan answers this against a live meeting. Until then this adapter
// makes NO threading promise beyond the SDK's own.
#pragma once

#include <string>
#include <vector>

#include "talkback_sdk.h"

namespace zc {

class TalkbackSdkMac : public TalkbackSdk {
 public:
  // `controller` is a ZoomSDKTalkbackController*.
  explicit TalkbackSdkMac(void* controller);
  ~TalkbackSdkMac() override;

  void SetEvents(TalkbackSdkEvents* events) override;
  bool MeetingSupportsTalkback() override;
  TalkbackResult CreateChannels(unsigned int count) override;
  TalkbackResult InviteUsers(const std::string& channel_id,
                           const std::vector<unsigned int>& user_ids) override;
  TalkbackResult RemoveUsers(const std::string& channel_id,
                           const std::vector<unsigned int>& user_ids) override;
  TalkbackResult DestroyChannels(
      const std::vector<std::string>& channel_ids) override;
  TalkbackResult SendAudio(const std::string& channel_id, const int16_t* pcm,
                         int samples) override;
  TalkbackResult SetChannelBackgroundVolume(const std::string& channel_id,
                                          float volume) override;

 private:
  void* controller_;   // ZoomSDKTalkbackController*
  void* delegate_;     // ZComumsTalkbackDelegate*, retained
  TalkbackSdkEvents* events_ = nullptr;
};

}  // namespace zc
```

- [ ] **Step 2: Write the implementation**

```objcpp
#include "talkback_sdk_mac.h"

#import <Foundation/Foundation.h>
#import <ZoomSDK/ZoomSDK.h>

#include "audio_defs.h"

namespace {

// The normalised code the ladder branches on, PLUS the macOS SDK's own number
// carried alongside for the operator. Windows' adapter does the same from its
// own number space -- the two are not comparable, which is exactly why `raw`
// is never compared or switched on above the seam.
zc::TalkbackResult FromZoomError(ZoomSDKError err) {
  const int raw = static_cast<int>(err);
  switch (err) {
    case ZoomSDKError_Success: return {zc::TalkbackCall::Ok, raw};
    case ZoomSDKError_TooFrequentCall: return {zc::TalkbackCall::TooFrequent, raw};
    case ZoomSDKError_WrongUsage: return {zc::TalkbackCall::WrongUsage, raw};
    default: return {zc::TalkbackCall::Failed, raw};
  }
}

zc::TalkbackEvent FromZoomTalkbackError(ZoomSDKTalkbackError e) {
  switch (e) {
    case ZoomSDKTalkbackError_OK: return zc::TalkbackEvent::Ok;
    case ZoomSDKTalkbackError_NoPermission: return zc::TalkbackEvent::NoPermission;
    case ZoomSDKTalkbackError_AlreadyExist: return zc::TalkbackEvent::AlreadyExists;
    case ZoomSDKTalkbackError_CountOverflow: return zc::TalkbackEvent::CountOverflow;
    case ZoomSDKTalkbackError_NotExist: return zc::TalkbackEvent::NotExist;
    case ZoomSDKTalkbackError_Rejected: return zc::TalkbackEvent::Rejected;
    case ZoomSDKTalkbackError_Timeout: return zc::TalkbackEvent::Timeout;
    default: return zc::TalkbackEvent::Unknown;
  }
}

NSString* Ns(const std::string& s) {
  return [NSString stringWithUTF8String:s.c_str()];
}

std::string Std(NSString* s) {
  return s == nil ? std::string() : std::string([s UTF8String]);
}

}  // namespace

// The delegate. Every OS object carries the ZComms prefix (CLAUDE.md 3.3).
@interface ZCommsTalkbackDelegate : NSObject <ZoomSDKTalkbackControllerDelegate>
@property(nonatomic, assign) zc::TalkbackSdkEvents* events;
@end

@implementation ZCommsTalkbackDelegate

- (void)onCreateChannelResponse:(NSString*)channelID
                          error:(ZoomSDKTalkbackError)error {
  if (self.events) {
    self.events->OnCreateChannelResponse(Std(channelID),
                                         FromZoomTalkbackError(error));
  }
}

- (void)onDestroyChannelResponse:(NSString*)channelID
                           error:(ZoomSDKTalkbackError)error {
  if (self.events) {
    self.events->OnDestroyChannelResponse(Std(channelID),
                                          FromZoomTalkbackError(error));
  }
}

- (void)onChannelUserJoinResponse:(NSString*)channelID
                           userID:(unsigned int)userID
                            error:(ZoomSDKTalkbackError)error {
  if (self.events) {
    self.events->OnChannelUserJoinResponse(Std(channelID), userID,
                                           FromZoomTalkbackError(error));
  }
}

- (void)onChannelUserLeaveResponse:(NSString*)channelID
                            userID:(unsigned int)userID
                             error:(ZoomSDKTalkbackError)error {
  if (self.events) {
    self.events->OnChannelUserLeaveResponse(Std(channelID), userID,
                                            FromZoomTalkbackError(error));
  }
}

- (void)onJoinTalkbackChannel:(unsigned int)inviterID {
  if (self.events) self.events->OnJoinTalkbackChannel(inviterID);
}

- (void)onLeaveTalkbackChannel:(unsigned int)inviterID {
  if (self.events) self.events->OnLeaveTalkbackChannel(inviterID);
}

- (void)onInviterAudioLevel:(unsigned int)inviterID
                 audioLevel:(unsigned int)audioLevel {
  // Not consumed. The panel's level meter is local, pre-SDK.
}

@end

namespace zc {

TalkbackSdkMac::TalkbackSdkMac(void* controller) : controller_(controller) {
  ZCommsTalkbackDelegate* d = [[ZCommsTalkbackDelegate alloc] init];
  delegate_ = (__bridge_retained void*)d;
  if (controller_ != nullptr) {
    ((__bridge ZoomSDKTalkbackController*)controller_).delegate = d;
  }
}

TalkbackSdkMac::~TalkbackSdkMac() {
  if (controller_ != nullptr) {
    ((__bridge ZoomSDKTalkbackController*)controller_).delegate = nil;
  }
  if (delegate_ != nullptr) {
    CFRelease(delegate_);
    delegate_ = nullptr;
  }
}

void TalkbackSdkMac::SetEvents(TalkbackSdkEvents* events) {
  events_ = events;
  ((__bridge ZCommsTalkbackDelegate*)delegate_).events = events;
}

bool TalkbackSdkMac::MeetingSupportsTalkback() {
  if (controller_ == nullptr) return false;
  return [((__bridge ZoomSDKTalkbackController*)controller_)
      isMeetingSupportTalkBack];
}

TalkbackResult TalkbackSdkMac::CreateChannels(unsigned int count) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  return FromZoomError([((__bridge ZoomSDKTalkbackController*)controller_)
      createChannel:count]);
}

TalkbackResult TalkbackSdkMac::InviteUsers(
    const std::string& channel_id, const std::vector<unsigned int>& user_ids) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  if (user_ids.empty()) return TalkbackCall::Ok;
  NSMutableArray<NSNumber*>* ids =
      [NSMutableArray arrayWithCapacity:user_ids.size()];
  for (unsigned int id : user_ids) {
    [ids addObject:@(id)];
  }
  // One atomic call. Windows needs Begin/Add/Execute for the same effect.
  return FromZoomError([((__bridge ZoomSDKTalkbackController*)controller_)
      inviteUsersToChannel:Ns(channel_id)
                userIDList:ids]);
}

TalkbackResult TalkbackSdkMac::RemoveUsers(
    const std::string& channel_id, const std::vector<unsigned int>& user_ids) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  if (user_ids.empty()) return TalkbackCall::Ok;
  NSMutableArray<NSNumber*>* ids =
      [NSMutableArray arrayWithCapacity:user_ids.size()];
  for (unsigned int id : user_ids) {
    [ids addObject:@(id)];
  }
  return FromZoomError([((__bridge ZoomSDKTalkbackController*)controller_)
      removeUsersFromChannel:Ns(channel_id)
                  userIDList:ids]);
}

TalkbackResult TalkbackSdkMac::DestroyChannels(
    const std::vector<std::string>& channel_ids) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  if (channel_ids.empty()) return TalkbackCall::Ok;
  NSMutableArray<NSString*>* ids =
      [NSMutableArray arrayWithCapacity:channel_ids.size()];
  for (const std::string& id : channel_ids) {
    [ids addObject:Ns(id)];
  }
  return FromZoomError([((__bridge ZoomSDKTalkbackController*)controller_)
      destroyChannels:ids]);
}

TalkbackResult TalkbackSdkMac::SendAudio(const std::string& channel_id,
                                       const int16_t* pcm, int samples) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  // Mono, always. Stereo returns success and delivers nothing audible on
  // Windows (Law 5); the macOS header repeats the same "mono or stereo"
  // claim, so it is assumed to lie the same way until measured live.
  return FromZoomError([((__bridge ZoomSDKTalkbackController*)controller_)
      sendAudioDataToChannel:Ns(channel_id)
                   audioData:(char*)pcm
                  dataLength:(unsigned int)(samples * (int)sizeof(int16_t))
                  sampleRate:kSampleRate
                     channel:ZoomSDKAudioChannel_Mono]);
}

TalkbackResult TalkbackSdkMac::SetChannelBackgroundVolume(
    const std::string& channel_id, float volume) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  return FromZoomError([((__bridge ZoomSDKTalkbackController*)controller_)
      setChannelBackgroundVolume:Ns(channel_id)
                backgroundVolume:volume]);
}

}  // namespace zc
```

- [ ] **Step 3: Verify every SDK symbol you just used actually exists**

The enum spellings above are inferred from the pattern, not read. Check each against the real headers and correct any mismatch:

```bash
SDK=~/Developer/zoom-sdk-macos/ZoomSDK.framework/Versions/A/Headers
grep -nE "ZoomSDKTalkbackError_" $SDK/ZoomSDKTalkbackController.h
grep -nE "ZoomSDKError_(Success|TooFrequentCall|WrongUsage)" $SDK/ZoomSDKErrors.h
grep -nE "ZoomSDKAudioChannel_Mono" $SDK/*.h
```

Fix the `switch` labels to whatever the headers actually say. A wrong enumerator is a compile error, so this step is confirmation — but do it before building so the errors are one round, not five.

- [ ] **Step 4: Add the Apple arm to the SDK gate in CMake**

Replace the `if(EXISTS "${ZOOM_SDK_DIR}/lib/sdk.lib")` condition so both platforms can satisfy it, and select the right adapter:

```cmake
# A plain boolean, not a deferred `EXISTS <path>` list. The
# `set(VAR EXISTS path)` + `if(${VAR})` idiom does expand correctly, but it
# reads as a bug and breaks the moment anyone quotes the variable.
if(APPLE)
  if(EXISTS "${ZOOM_SDK_DIR}/ZoomSDK.framework")
    set(ZCOMMS_SDK_PRESENT TRUE)
  else()
    set(ZCOMMS_SDK_PRESENT FALSE)
  endif()
else()
  if(EXISTS "${ZOOM_SDK_DIR}/lib/sdk.lib")
    set(ZCOMMS_SDK_PRESENT TRUE)
  else()
    set(ZCOMMS_SDK_PRESENT FALSE)
  endif()
endif()

if(ZCOMMS_SDK_PRESENT)
  add_library(zcomms_zoom STATIC
    src/zoom/talkback_sdk.cpp
    src/zoom/talkback_channels.cpp
    src/zoom/roster.cpp
    src/zoom/breakout.cpp
    src/zoom/room_plan.cpp
    src/zoom/reach.cpp
    src/zoom/signal_protocol.cpp
    src/zoom/signal_outbox.cpp
    src/zoom/chat_signals.cpp
    src/zoom/duck_plan.cpp
    src/zoom/jwt.cpp
  )
  if(APPLE)
    target_sources(zcomms_zoom PRIVATE src/zoom/talkback_sdk_mac.mm)
    target_link_libraries(zcomms_zoom PUBLIC zcomms_audio
      "-framework Foundation" "-F${ZOOM_SDK_DIR}" "-framework ZoomSDK")
    target_include_directories(zcomms_zoom PUBLIC src/zoom)
  else()
    target_sources(zcomms_zoom PRIVATE
      src/audio/mic_source.cpp
      src/zoom/zoom_client.cpp
      src/zoom/talkback_source.cpp
      src/zoom/talkback_sdk_win.cpp)
    target_link_libraries(zcomms_zoom PUBLIC zcomms_audio "${ZOOM_SDK_DIR}/lib/sdk.lib")
    target_link_libraries(zcomms_zoom PRIVATE bcrypt)
    target_include_directories(zcomms_zoom PUBLIC "${ZOOM_SDK_DIR}/h" src/zoom)
  endif()
  if(MSVC)
    target_compile_options(zcomms_zoom PRIVATE /W3 /utf-8)
  endif()
  set(ZCOMMS_HAVE_SDK TRUE)
else()
  set(ZCOMMS_HAVE_SDK FALSE)
  message(STATUS
    "Zoom SDK not found at ${ZOOM_SDK_DIR} -- building the engine only.\n"
    "   The SDK is not redistributable and is fetched separately (plan §3.5).")
endif()
```

`zoom_client.cpp`, `mic_source.cpp` and `talkback_source.cpp` stay Windows-only here: their seams are the next plan's work. The macOS `zcomms_zoom` therefore builds the ladder and the adapter but no client yet — which is exactly the scope of this plan.

- [ ] **Step 5: Stage the macOS SDK locally and build**

```bash
mkdir -p third_party/zoom-sdk
cp -R ~/Developer/zoom-sdk-macos/ZoomSDK.framework third_party/zoom-sdk/
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --target zcomms_zoom
```

Expected: PASS. Fix any enum or selector mismatches Step 3 missed.

- [ ] **Step 6: Confirm the SDK-free suite still passes**

```bash
cmake --build build-mac --target zcomms_audio_tests && ./build-mac/zcomms_audio_tests
```

Expected: `ALL TESTS PASSED`. The ladder must still build without the SDK — that property is what Task 5's tests depend on, and the CMake edit in Step 4 is the kind of change that can quietly break it.

- [ ] **Step 7: Confirm the macOS SDK asset (already staged — verify only)**

The controller uploaded both platforms to the same draft release before execution began, so there is nothing to upload here. Confirm:

```bash
gh release view sdk-assets --repo iamfatness/ZComms --json assets \
  --jq '[.assets[].name] | index("zoom-sdk-macos-7.1.5.84750.zip") != null'
```

Expected: `true`. If `false`, stop and report — do not upload an SDK yourself.

- [ ] **Step 8: Extend the macOS workflow to build the adapter**

Add these steps to `.github/workflows/macos.yml`, between `checkout` and `Configure`:

```yaml
      - name: Fetch the Zoom macOS SDK
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          gh release download sdk-assets --repo ${{ github.repository }} \
            --pattern zoom-sdk-macos-7.1.5.84750.zip --dir "$RUNNER_TEMP"
          unzip -q "$RUNNER_TEMP/zoom-sdk-macos-7.1.5.84750.zip" -d "$RUNNER_TEMP/sdk"
          mkdir -p third_party/zoom-sdk
          # The framework sits under a ZoomSDK/ subdirectory inside the
          # version-named top directory -- not at the zip root.
          cp -R "$RUNNER_TEMP/sdk/zoom-sdk-macos-7.1.5.84750/ZoomSDK/ZoomSDK.framework" \
            third_party/zoom-sdk/

      - name: Verify the SDK landed
        run: |
          test -d third_party/zoom-sdk/ZoomSDK.framework
          test -f third_party/zoom-sdk/ZoomSDK.framework/Versions/A/Headers/ZoomSDKTalkbackController.h
```

And change the build step so the adapter is actually compiled:

```yaml
      - name: Build
        run: cmake --build build --target zcomms_audio_tests zcomms_zoom
```

- [ ] **Step 9: Commit**

```bash
git add src/zoom/talkback_sdk_mac.h src/zoom/talkback_sdk_mac.mm \
        CMakeLists.txt .github/workflows/macos.yml
git commit -m "feat(talkback): the macOS TalkbackSdk adapter

Compiles and links against ZoomSDKTalkbackController. macOS is the
simpler side: inviteUsersToChannel: and removeUsersFromChannel: are
single atomic calls, so the Begin/Add/Execute rules the Windows adapter
hides have no analogue here.

Not exercised live yet -- that needs ZoomClient and the probe tool, which
are the next plan. Threading is deliberately unpromised: the macOS SDK
headers carry no guidance at all, and whether sendAudioDataToChannel: is
main-queue-only is measured against a real meeting, not guessed."
git push
gh run watch
```

Expected: both gates green. `macos` now builds the ladder, the seam, and the macOS adapter.

---

## Done when

- `windows` and `macos` checks both green on `macos-port`, and the Windows gate has been demonstrated red at least once (Task 1 Step 4).
- `TalkbackChannels` contains no Zoom type and compiles with no SDK present.
- The ladder has tests for the first time, every one mutation-proved.
- Both adapters compile against their real SDKs.

**Not done, deliberately:** nothing has talked to a live meeting. `ZoomClient`, `VirtualMic` and the headless probe are the next plan, and the threading question in §4.2 of the spec is still open by design.
