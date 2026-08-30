# AI Companion Controls: Detect, Surface Loudly, Turn Off Where the Role Allows

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Goal

Detect AI Companion activity in the meeting the desk is in, surface it as a loud indicator in the panel (crew comms are being transcribed/summarized — a privacy fact the operator must never miss), and turn it off from the desk where the SDK and the client's role permit, with the co-host/host permission boundary documented and surfaced honestly.

## Architecture

Detection is two-source: the already-overridden (currently empty) `ZoomClient::onAICompanionActiveChangeNotice(bool)` on `IMeetingServiceEvent`, plus capability polling on `IMeetingAICompanionController` (`CanTurnOffAllAICompanions` / `CanRequestTurnoffAllAICompanions`). A pure state machine (`ai_watch`) folds those inputs into one panel-facing state — OFF / ACTIVE+CAN-STOP / ACTIVE+REQUEST-ONLY / ACTIVE+LOCKED — and the operator-facing text for each transition. A thin glue class (`AICompanionGuard`) implements `IMeetingAICompanionCtrlEvent`, executes `TurnOffAllAICompanions(false)` or `RequestTurnoffAllAICompanions()`, and reports request outcomes from `onAICompanionFeatureSwitchRequestResponse`. The panel gets a red AI lamp in the rail and an `ai off` verb.

## Tech Stack

- C++17, MSVC, CMake (existing build).
- Zoom Meeting SDK 7.1.5 vendored headers: `meeting_service_interface.h` (`IMeetingServiceEvent::onAICompanionActiveChangeNotice`, `IMeetingService::GetMeetingAICompanionController()`), `meeting_service_components/meeting_ai_companion_interface.h` (`IMeetingAICompanionController`, `IMeetingAICompanionCtrlEvent`, `IAICompanionFeatureSwitchHandler`, `IAICompanionFeatureTurnOnAgainHandler`, `AICompanionFeature`).
- zctest harness, `zcomms_audio_tests` exe.

## Spec

This document doubles as the spec.

### Requirements

1. Within one notification of AI Companion becoming active, the panel shows a red `AI` lamp in the rail and an ops line states which control path is available (`can stop`, `can request stop`, or `locked`).
2. `POST /act "ai off"`: if `CanTurnOffAllAICompanions()` — call `TurnOffAllAICompanions(false)` (assets kept: `bDeleteAssets=false`; deleting a client's meeting assets is not the intercom's call). Else if `CanRequestTurnoffAllAICompanions()` — call `RequestTurnoffAllAICompanions()` and say so (`requested -- host must approve`). Else — ops line `AI Companion cannot be stopped from this seat`.
3. Request outcomes are reported: `onAICompanionFeatureSwitchRequestResponse(bTimeout, bAgree, bTurnOn)` maps to `host approved`, `host declined`, `request timed out`.
4. If the desk is host/co-host, inbound requests (`onAICompanionFeatureSwitchRequested`) are surfaced as an ops line with the requester's NAME (never a stored id — and `GetRequestUserID()` legitimately returns 0 cross-room, so `GetRequestUserName()` is the primary identity, per the header note).
5. The features-that-cannot-stop case is honest: `onAICompanionFeatureCanNotBeTurnedOff(IList<AICompanionFeature>*)` produces an ops line naming the stuck features (SMART_SUMMARY / QUERY / SMART_RECORDING).
6. All decision logic is unit-tested SDK-free; the glue is compile-verified plus a live checklist.
7. The co-host boundary is documented in this plan (below) and in CLAUDE.md, so the operator's expectations match the SDK's.

### What the role can and cannot do (from the vendored headers; verify live)

- The capability is **query-based, not role-enum-based**: the SDK exposes `IsTurnoffAllAICompanionsSupported()` (meeting supports it at all) and `CanTurnOffAllAICompanions()` (this user, now). Host — and co-host, where Zoom grants it — get direct turn-off; everyone else falls back to `CanRequestTurnoffAllAICompanions()` → `RequestTurnoffAllAICompanions()`, which the host must approve. ZComms therefore never assumes; it polls and displays what it finds.
- Only host/co-host receive `onAICompanionFeatureTurnOffByParticipant` (a participant killed auto-start AI before the host joined; the handler offers `TurnOnAgain()` / `AgreeTurnOff()` — ZComms calls `AgreeTurnOff()`, never `TurnOnAgain()`: the desk's bias is privacy).
- `TurnOffAllAICompanions(bool bDeleteAssets)` turns off smart summary, smart recording, and query at once; `TurnOnAllAICompanions()` exists but ZComms deliberately never calls it.
- Breakout interaction: `BOOption::isAICompanionEnabled` + `IBOCreator::IsAICompanionSupported()` control AI auto-start in breakout rooms — the sub-production plan (2026-08-29-breakout-subproduction-comms.md) must leave `isAICompanionEnabled` at its default `false`.

### Interfaces looked for and NOT found in the vendored SDK

- There is **no** `onAICompanionActiveChangeNotice` on the AI Companion controller event — it lives on `IMeetingServiceEvent` (`meeting_service_interface.h`), which `ZoomClient` already implements with an empty body. Plan uses that.
- There is **no** per-feature "who enabled it" attribution and no way for a non-host to force-stop; the request path is the designed fallback and the plan surfaces it as such.

## Global Constraints

- Build: `cmake -S . -B build`; `cmake --build build --config Release`. Tests: `build\Release\zcomms_audio_tests.exe` / `ctest --test-dir build -C Release`.
- Pure modules include no SDK headers (must compile into `zcomms_audio_tests` without `sdk.lib`); Zoom SDK headers require `windows.h` first in any TU that does include them.
- All SDK callbacks and controller calls run on the SDK pump thread; the panel action thread only sets an intent the pump thread executes (same pattern as every existing verb).
- Zoom user ids are meeting-scoped and recycled; log names.
- `SDKERR_TOO_FREQUENT_CALL` (18) exists on back-to-back SDK calls: `ai off` is a single call per operator press, with a 5 s local re-press guard.
- Update `CLAUDE.md` in the same change as any substantive work.

## Tasks

### Task 1: `ai_watch` — indicator state machine (pure)

**Files**
- Create: `src/zoom/ai_watch.h`
- Create: `src/zoom/ai_watch.cpp`
- Test: `tests/zoom/test_ai_watch.cpp`
- Modify: `CMakeLists.txt`, `tests/audio/test_util.h`, `tests/audio/test_main.cpp`

**Interfaces**
- Produces:
  ```cpp
  // src/zoom/ai_watch.h  (no SDK includes)
  namespace zc {
  enum class AiLamp { kOff, kActiveCanStop, kActiveRequestOnly, kActiveLocked };
  class AiWatch {
   public:
    struct Update {
      bool changed = false;      // lamp state moved -- publish + ops line
      AiLamp lamp = AiLamp::kOff;
      std::string ops_line;      // "" when !changed
    };
    // Inputs are level-triggered; call on every poll and on every notice.
    Update Fold(bool active, bool can_turn_off, bool can_request);
    // Request lifecycle -> operator text (pure mapping, no state).
    static std::string RequestOutcomeText(bool timeout, bool agree);
    AiLamp lamp() const;
  };
  }
  ```
- Consumes: booleans only.

**Steps**

- [ ] Write the failing test `tests/zoom/test_ai_watch.cpp`:
  ```cpp
  #include "ai_watch.h"
  #include "test_util.h"

  void TestAiWatch() {
    ZC_TEST("ai_watch: off -> active/can-stop announces once");
    zc::AiWatch w;
    auto u = w.Fold(true, true, false);
    ZC_CHECK(u.changed);
    ZC_CHECK(u.lamp == zc::AiLamp::kActiveCanStop);
    ZC_CHECK(!u.ops_line.empty());
    u = w.Fold(true, true, false);      // same inputs: silent
    ZC_CHECK(!u.changed);

    ZC_TEST("ai_watch: capability shift while active re-announces");
    u = w.Fold(true, false, true);
    ZC_CHECK(u.changed && u.lamp == zc::AiLamp::kActiveRequestOnly);
    u = w.Fold(true, false, false);
    ZC_CHECK(u.changed && u.lamp == zc::AiLamp::kActiveLocked);

    ZC_TEST("ai_watch: deactivation clears");
    u = w.Fold(false, false, false);
    ZC_CHECK(u.changed && u.lamp == zc::AiLamp::kOff);

    ZC_TEST("ai_watch: inactive ignores capabilities");
    zc::AiWatch w2;
    ZC_CHECK(!w2.Fold(false, true, true).changed);
    ZC_CHECK(w2.lamp() == zc::AiLamp::kOff);

    ZC_TEST("ai_watch: request outcome texts");
    ZC_CHECK(zc::AiWatch::RequestOutcomeText(true, false) ==
             "AI stop request timed out");
    ZC_CHECK(zc::AiWatch::RequestOutcomeText(false, true) ==
             "host approved -- AI Companion stopping");
    ZC_CHECK(zc::AiWatch::RequestOutcomeText(false, false) ==
             "host declined the AI stop request");
  }
  ```
- [ ] Register `TestAiWatch` in `tests/audio/test_util.h` and `tests/audio/test_main.cpp`; add `tests/zoom/test_ai_watch.cpp` and `src/zoom/ai_watch.cpp` to `zcomms_audio_tests` sources in `CMakeLists.txt` (plus `target_include_directories(zcomms_audio_tests PRIVATE tests/audio src/zoom)` if not already present from a sibling plan).
- [ ] `cmake --build build --config Release --target zcomms_audio_tests` — failing.
- [ ] Implement `AiWatch`: store `AiLamp lamp_ = kOff`; `Fold` computes the target lamp (`!active → kOff`, else `can_turn_off → kActiveCanStop`, else `can_request → kActiveRequestOnly`, else `kActiveLocked`), and when it differs from `lamp_`, updates it and fills `ops_line` (`"AI COMPANION ACTIVE -- this seat can stop it (ai off)"` / `"AI COMPANION ACTIVE -- stop requires host approval (ai off to request)"` / `"AI COMPANION ACTIVE -- cannot be stopped from this seat"` / `"AI Companion inactive"`). `RequestOutcomeText` is the three-way mapping in the test.
- [ ] Run `build\Release\zcomms_audio_tests.exe`; green.
- [ ] Commit: `git add src/zoom/ai_watch.h src/zoom/ai_watch.cpp tests/zoom/test_ai_watch.cpp tests/audio/test_util.h tests/audio/test_main.cpp CMakeLists.txt && git commit -m "feat(ai): AiWatch lamp state machine for AI Companion activity (pure)"`

### Task 2: `AICompanionGuard` — controller glue

**Files**
- Create: `src/zoom/ai_guard.h`
- Create: `src/zoom/ai_guard.cpp`
- Modify: `CMakeLists.txt` (add `src/zoom/ai_guard.cpp` to `zcomms_zoom` sources)
- Modify: `src/zoom/zoom_client.h`, `src/zoom/zoom_client.cpp`

**Interfaces**
- `ZoomClient` gains, mirroring `GetTalkbackController()` / `GetBOController()`:
  ```cpp
  ZOOM_SDK_NAMESPACE::IMeetingAICompanionController* GetAICompanionController();
  // impl: return meeting_ ? meeting_->GetMeetingAICompanionController() : nullptr;
  // plus a notice hook the app installs:
  void SetAiActiveCallback(std::function<void(bool)> fn);  // stored, invoked by
                                                           // onAICompanionActiveChangeNotice
  ```
  and `onAICompanionActiveChangeNotice(bool active)` (currently `{}` at `zoom_client.cpp:555`) becomes: log `[sdk] AI Companion active: %d` and invoke the stored callback if set. The callback fires on the pump thread — same thread the app's loop runs on, so no marshalling.
- Produces:
  ```cpp
  // src/zoom/ai_guard.h
  namespace zc {
  class AICompanionGuard : public ZOOM_SDK_NAMESPACE::IMeetingAICompanionCtrlEvent {
   public:
    using OpsFn = std::function<void(const std::string&)>;
    void Attach(ZOOM_SDK_NAMESPACE::IMeetingAICompanionController* controller,
                OpsFn log_op);
    // Poll capabilities; call ~1 Hz from the app loop. Returns the current
    // (can_turn_off, can_request) pair for AiWatch::Fold.
    void PollCaps(bool* can_turn_off, bool* can_request);
    // The 'ai off' verb. Returns the ops line describing what happened.
    std::string StopAi();
    // IMeetingAICompanionCtrlEvent
    void onAICompanionFeatureTurnOffByParticipant(
        ZOOM_SDK_NAMESPACE::IAICompanionFeatureTurnOnAgainHandler* handler) override;
    void onAICompanionFeatureSwitchRequested(
        ZOOM_SDK_NAMESPACE::IAICompanionFeatureSwitchHandler* handler) override;
    void onAICompanionFeatureSwitchRequestResponse(bool bTimeout, bool bAgree,
                                                   bool bTurnOn) override;
    void onAICompanionFeatureCanNotBeTurnedOff(
        ZOOM_SDK_NAMESPACE::IList<ZOOM_SDK_NAMESPACE::AICompanionFeature>* features) override;
    void onHostUnsupportedStopNotesRequest() override;
   private:
    ZOOM_SDK_NAMESPACE::IMeetingAICompanionController* controller_ = nullptr;
    OpsFn log_op_;
    int64_t last_stop_ms_ = 0;  // 5 s re-press guard
  };
  }
  ```

**Steps**

- [ ] Add `GetAICompanionController()` + `SetAiActiveCallback` to `zoom_client.{h,cpp}` and fill in `onAICompanionActiveChangeNotice` exactly as above.
- [ ] Implement `ai_guard.cpp`:
  - `Attach`: store controller, `controller_->SetEvent(this)`.
  - `PollCaps`: `*can_turn_off = controller_ && controller_->CanTurnOffAllAICompanions(); *can_request = controller_ && controller_->CanRequestTurnoffAllAICompanions();`
  - `StopAi`:
    ```cpp
    std::string AICompanionGuard::StopAi() {
      if (controller_ == nullptr) return "AI controls unavailable (no meeting)";
      const int64_t now = NowMs();
      if (now - last_stop_ms_ < 5000) return "ai off already in flight -- wait";
      last_stop_ms_ = now;
      if (controller_->CanTurnOffAllAICompanions()) {
        const auto err = controller_->TurnOffAllAICompanions(false /*keep assets*/);
        return err == ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS
                   ? "AI Companion: turn-off issued"
                   : "AI Companion turn-off failed, SDK error " + std::to_string((int)err);
      }
      if (controller_->CanRequestTurnoffAllAICompanions()) {
        const auto err = controller_->RequestTurnoffAllAICompanions();
        return err == ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS
                   ? "AI stop requested -- host must approve"
                   : "AI stop request failed, SDK error " + std::to_string((int)err);
      }
      return "AI Companion cannot be stopped from this seat";
    }
    ```
  - `onAICompanionFeatureTurnOffByParticipant`: `handler->AgreeTurnOff();` + ops line `"a participant had turned AI off pre-join -- keeping it off"` (privacy bias, requirement/spec above).
  - `onAICompanionFeatureSwitchRequested`: ops line `std::string("AI ") + (handler->IsTurnOn() ? "start" : "stop") + " requested by " + Narrow(handler->GetRequestUserName()) + " -- answer in the Zoom client"` — ZComms surfaces but does not auto-approve someone else's request; do not call `Agree`/`Decline` here.
  - `onAICompanionFeatureSwitchRequestResponse`: `log_op_(AiWatch::RequestOutcomeText(bTimeout, bAgree))` when `!bTurnOn` (only stop requests are ours); ignore turn-on responses.
  - `onAICompanionFeatureCanNotBeTurnedOff`: walk the `IList`, map `SMART_SUMMARY/QUERY/SMART_RECORDING` to names, ops line `"AI features that cannot be turned off: ..."`.
  - `onHostUnsupportedStopNotesRequest`: ops line `"host client too old to process the stop request"`.
- [ ] Add `src/zoom/ai_guard.cpp` to `zcomms_zoom` sources in `CMakeLists.txt`; `cmake --build build --config Release --target zcomms` — compile-clean gate.
- [ ] Commit: `git add src/zoom/ai_guard.h src/zoom/ai_guard.cpp src/zoom/zoom_client.h src/zoom/zoom_client.cpp CMakeLists.txt && git commit -m "feat(ai): AICompanionGuard -- capability poll, stop/request-stop, event surfacing"`

### Task 3: panel lamp + `ai off` verb + docs

**Files**
- Modify: `src/app/main.cpp`
- Modify: `src/app/ui_html.h`
- Modify: `CLAUDE.md`

**Interfaces**
- Consumes: `AiWatch`, `AICompanionGuard`, the session loop's `log_op` and published-state JSON.
- Produces: state JSON field `"ai":"off"|"can_stop"|"request_only"|"locked"`; panel rail lamp `AI` rendered red whenever the value is not `"off"`, with the tooltip text from the ops line; `/act` verb `ai off`.

**Steps**

- [ ] In the session lambda: construct `AiWatch watch; AICompanionGuard guard;` (session-scoped, never static); `guard.Attach(client.GetAICompanionController(), log_op);` and `client.SetAiActiveCallback([&](bool a){ ai_active = a; })` where `ai_active` is a session-scoped bool.
- [ ] In the ~1 Hz housekeeping branch of the loop: `bool cts, crq; guard.PollCaps(&cts, &crq); auto u = watch.Fold(ai_active, cts, crq); if (u.changed) log_op(u.ops_line);` and serialize `watch.lamp()` into the state JSON.
- [ ] Add the `ai` verb branch: `if (verb == "ai" && arg == "off") log_op(guard.StopAi());`
- [ ] Panel HTML: add the `AI` lamp beside the existing MTG MIC lamp in the rail; red + pulsing CSS class when state != `off`, plus the `request_only`/`locked` variants as subtitle text (`REQ ONLY` / `LOCKED`). Follow the exact markup pattern of the MTG MIC lamp.
- [ ] Live checklist (meeting from the authorizing account with AI Companion available): (a) host starts AI Companion → lamp turns red within a couple of seconds and the ops line names the control path; (b) as co-host, `ai off` → observe direct stop or request path and the outcome line; (c) as plain participant, `ai off` → request path; host declines → `host declined the AI stop request`; (d) confirm the lamp clears on deactivation.
- [ ] Update `CLAUDE.md`: an "AI Companion guard" subsection — detection sources, the query-based (not role-based) permission truth, the keep-assets choice, the never-turn-on bias.
- [ ] Run `build\Release\zcomms_audio_tests.exe`; green.
- [ ] Commit: `git add src/app/main.cpp src/app/ui_html.h CLAUDE.md && git commit -m "feat(ai): red AI lamp, ai-off verb, co-host boundary surfaced in ops"`

## Self-review checklist (fix inline before PR)

- [ ] No `TBD`; every callback listed in `IMeetingAICompanionCtrlEvent` has an override in the sketch (the vendored header has exactly five).
- [ ] `Fold` input/lamp types consistent across test, header, and app wiring.
- [ ] `TurnOffAllAICompanions(false)` — the bool is `bDeleteAssets` and stays `false` everywhere.
- [ ] Verify with `build\Release\zcomms_audio_tests.exe` output before claiming green.
