# Breakout Sub-Production Comms: Programmatic Rooms, Channel Re-Provisioning, Cross-Room Truth

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Goal

Turn the shipped breakout-awareness layer (`src/zoom/breakout.{h,cpp}`) into an operator tool: create and staff breakout rooms programmatically for sub-productions (green room, per-segment crews), re-provision talkback channel membership on every room transition (delivery law #2: talkback never crosses breakout rooms), and keep the roster's reachability truth exact across rooms.

## Architecture

A pure planner (`room_plan`) diffs a desired sub-production layout (room name → member display names) against the live `BreakoutState` and emits an ordered action list; a pure reachability function computes, per talkback slot, which members can actually hear the station right now. The SDK glue extends the existing `BreakoutRooms` class with creator/admin verbs (`IBOCreator::CreateBreakoutRoom` / `AssignUserToBO`, `IBOAdmin::StartBO` / `StopBO` / `AssignNewUserToRunningBO` / `SwitchAssignedUserToRunningBO`), all driven from the SDK pump thread. The existing membership healer in `main.cpp` gains a stale-flush: any room transition (mine or anyone's) marks channel membership dirty so the next healer pass re-invites exactly the in-room set — cross-room invites are already skipped, so re-provisioning is "flush + let the healer converge" rather than a second membership engine.

## Tech Stack

- C++17, MSVC, CMake (existing build).
- Zoom Meeting SDK 7.1.5 vendored headers: `meeting_service_components/meeting_breakout_rooms_interface_v2.h` (`IMeetingBOController`, `IBOCreator`, `IBOAdmin`, `IBOAssistant`, `IBOAttendee`, `IBOData`, `BOOption`, `IBatchCreateBOHelper`).
- zctest harness, single `zcomms_audio_tests` exe.

## Spec

This document doubles as the spec.

### Requirements

1. An operator with the right role can, from the panel, define named rooms ("GREEN ROOM", "SEG 2 CREW"), assign people to them by display name, start and stop the breakout session, and move individuals between running rooms.
2. The desired layout is expressed once; the system converges: rooms missing are created, people misplaced are assigned or switched, and nothing is torn down that the layout still wants.
3. On any room transition — the station moving, or a channel member moving — talkback membership is re-provisioned within one healer pass, and no cross-room invite is ever attempted (it fails `SDKERR_WRONG_USAGE`, live 2026-08-29).
4. The panel always shows reach truth per channel: members reachable now, members present-but-in-another-room (dark, `in <room>`), and refuses keys on channels with zero reachable members the same way the existing keyed-but-empty warning works.
5. Role reality is honored and surfaced: creator/admin objects arrive only via `IMeetingBOControllerEvent` rights callbacks (host in main session = creator+admin+assistant+data; desktop co-host under a desktop host = the same; mobile-host co-host or plain attendee = not). When rights are absent, verbs fail with an ops line naming the missing right, never half-work.
6. All layout keys are display names (BO user ids are their own string GUIDs, unrelated to meeting user ids; the NAME is the only join — already established in `breakout.h`).
7. Editing constraints from the header are enforced client-side: room create/rename/delete only before the session starts (`BO_STATUS_EDIT`); after start, staffing changes go through `IBOAdmin::AssignNewUserToRunningBO` / `SwitchAssignedUserToRunningBO`.

### SDK reality (verified against vendored headers)

- Rights arrive via `IMeetingBOControllerEvent::onHasCreatorRightsNotification(IBOCreator*)` / `onHasAdminRightsNotification(IBOAdmin*)` etc., and depart via the matching `onLost*` callbacks — `BreakoutRooms` already caches these pointers.
- `IBOCreator::CreateBreakoutRoom(const zchar_t*)` is async → `IBOCreatorEvent::onCreateBOResponse(bool bSuccess, const zchar_t* strBOID)`. `AssignUserToBO(const zchar_t* strUserID, const zchar_t* strBOID)` returns bool. Batch creation exists: `IBOCreator::GetBatchCreateBOHelper()` → `CreateBOTransactionBegin()` / `AddNewBoToList(name)` / `CreateBoTransactionCommit()` (max 50 rooms, 32-char names) — use it for multi-room layouts, one transaction per convergence pass (the talkback rate-limit lesson, applied here preemptively).
- `IBOAdmin::StartBO()` / `StopBO()` are async → `IBOAdminEvent::onStartBOResponse(bool)` / `onStopBOResponse(bool)`; failures also surface via `onStartBOError(BOControllerError)`.
- `IBOData` supplies truth: `GetBOMeetingIDList()`, `GetBOMeetingByID()` → `IBOMeeting::GetBOName()` / `GetBOUserList()` / `GetBOUserStatus()`, `GetUnassignedUserList()`, `GetBOUserName(strUserID)`, `GetCurrentBoName()` — `BreakoutRooms::Snapshot()` already walks these.
- Status: `IMeetingBOController::GetBOStatus()` (`BO_STATUS_EDIT` / `BO_STATUS_STARTED` / ...), `IsBOStarted()`, `IsInBOMeeting()`.
- Change notifications for the stale-flush: `IBODataEvent::onBOInfoUpdated(const zchar_t* strBOID)` / `onUnAssignedUserUpdated()` / `OnBOListInfoUpdated()` (needs `IBOData::SetEvent`), plus `IMeetingBOControllerEvent::onBOStatusChanged(BO_STATUS)`.
- Broadcast extras that exist and are worth panel verbs later, but are OUT of this plan's scope: `IBOAdmin::BroadcastMessage(const zchar_t*)`, `IsBroadcastVoiceToBOSupport()` / `CanBroadcastVoiceToBO()` / `BroadcastVoiceToBo(bool)`.
- **Not found in the vendored SDK:** any way to deliver talkback across rooms (that is delivery law #2, an SDK law this plan works around, not through), and any synchronous room-creation API (everything is callback-confirmed; the planner must be re-entrant and idempotent).

## Global Constraints

- Build: `cmake -S . -B build`; `cmake --build build --config Release`. Tests: `build\Release\zcomms_audio_tests.exe` / `ctest --test-dir build -C Release`.
- SDK delivery laws in force here: talkback does not cross breakout rooms (`WRONG_USAGE` on cross-room invites; in-channel members in another room hear silence); mic must be OPEN or sends are accepted-but-silent; `SDKERR_TOO_FREQUENT_CALL` (18) on back-to-back calls — one batched exchange per pass, with backoff, exactly like the invite healer.
- Pure modules include no SDK headers (they must compile into `zcomms_audio_tests` without `sdk.lib`); SDK glue runs on the pump thread only.
- Zoom meeting user ids are meeting-scoped; BO user ids are separate GUID strings; durable keys are display names.
- Breakout behavior is NOT yet live-verified (CLAUDE.md: "needs a breakout production") — every task that touches the SDK ends in a live checklist, and nothing claims verified without it.
- Update `CLAUDE.md` in the same change as any substantive work.

## Tasks

### Task 1: `room_plan` — layout convergence planner (pure)

**Files**
- Create: `src/zoom/room_plan.h`
- Create: `src/zoom/room_plan.cpp`
- Test: `tests/zoom/test_room_plan.cpp`
- Modify: `CMakeLists.txt`, `tests/audio/test_util.h`, `tests/audio/test_main.cpp`

**Interfaces**
- Consumes: `BreakoutState` / `BreakoutRoomInfo` — **moved** into a new SDK-free header so the planner and tests can see them (today they live in `breakout.h`, which includes SDK headers).
- Produces:
  ```cpp
  // src/zoom/room_plan.h  (no SDK includes)
  namespace zc {
  struct BreakoutRoomInfo {           // moved verbatim from breakout.h
    std::string id;
    std::string name;
    std::vector<std::string> user_names;
  };
  struct BreakoutState {              // moved verbatim from breakout.h
    bool enabled = false;
    bool started = false;
    bool in_bo = false;
    std::string my_room;
    std::vector<BreakoutRoomInfo> rooms;
  };
  struct RoomLayout {                 // desired: room name -> member names
    std::vector<std::pair<std::string, std::vector<std::string>>> rooms;
  };
  enum class RoomActionKind {
    kCreateRoom,        // arg1 = room name             (pre-start only)
    kAssignUser,        // arg1 = person, arg2 = room   (pre-start)
    kAssignRunning,     // arg1 = person, arg2 = room   (session started, user unassigned)
    kSwitchRunning,     // arg1 = person, arg2 = room   (session started, user elsewhere)
    kNeedStart,         // layout wants rooms but session not started
  };
  struct RoomAction {
    RoomActionKind kind;
    std::string arg1;
    std::string arg2;
  };
  // Diff desired vs live. Idempotent: running it on a converged state yields {}.
  std::vector<RoomAction> PlanRooms(const RoomLayout& want, const BreakoutState& have);
  }
  ```

**Steps**

- [ ] Move `BreakoutRoomInfo`/`BreakoutState` into `room_plan.h`; in `breakout.h`, replace the struct definitions with `#include "room_plan.h"` (SDK-free headers may be included from SDK-including ones; never the reverse).
- [ ] Write the failing test `tests/zoom/test_room_plan.cpp`:
  ```cpp
  #include "room_plan.h"
  #include "test_util.h"

  void TestRoomPlan() {
    ZC_TEST("room_plan: empty state, two-room layout -> creates + assigns");
    zc::RoomLayout want;
    want.rooms = {{"GREEN ROOM", {"Pat"}}, {"SEG 2 CREW", {"Sam", "Alex"}}};
    zc::BreakoutState have;
    have.enabled = true;
    auto plan = zc::PlanRooms(want, have);
    int creates = 0, assigns = 0;
    for (const auto& a : plan) {
      if (a.kind == zc::RoomActionKind::kCreateRoom) ++creates;
      if (a.kind == zc::RoomActionKind::kAssignUser) ++assigns;
    }
    ZC_CHECK(creates == 2);
    ZC_CHECK(assigns == 3);

    ZC_TEST("room_plan: converged state plans nothing");
    zc::BreakoutState done;
    done.enabled = true;
    done.started = true;
    done.rooms = {{"id1", "GREEN ROOM", {"Pat"}}, {"id2", "SEG 2 CREW", {"Sam", "Alex"}}};
    ZC_CHECK(zc::PlanRooms(want, done).empty() ||
             (zc::PlanRooms(want, done).size() == 0));

    ZC_TEST("room_plan: started session moves people via running-BO verbs");
    zc::BreakoutState live;
    live.enabled = true;
    live.started = true;
    live.rooms = {{"id1", "GREEN ROOM", {"Pat", "Sam"}}, {"id2", "SEG 2 CREW", {"Alex"}}};
    auto plan2 = zc::PlanRooms(want, live);       // Sam belongs in SEG 2 CREW
    ZC_CHECK(plan2.size() == 1);
    ZC_CHECK(plan2[0].kind == zc::RoomActionKind::kSwitchRunning);
    ZC_CHECK(plan2[0].arg1 == "Sam");
    ZC_CHECK(plan2[0].arg2 == "SEG 2 CREW");

    ZC_TEST("room_plan: missing room after start is kNeedStart-free but no create");
    zc::RoomLayout want3;
    want3.rooms = {{"NEW ROOM", {"Pat"}}};
    auto plan3 = zc::PlanRooms(want3, live);
    for (const auto& a : plan3) {
      ZC_CHECK(a.kind != zc::RoomActionKind::kCreateRoom);  // header law: edit pre-start only
    }

    ZC_TEST("room_plan: rooms wanted but session not started emits kNeedStart");
    zc::BreakoutState idle;
    idle.enabled = true;
    idle.rooms = {{"id1", "GREEN ROOM", {"Pat"}}, {"id2", "SEG 2 CREW", {"Sam", "Alex"}}};
    auto plan4 = zc::PlanRooms(want, idle);
    bool need_start = false;
    for (const auto& a : plan4) need_start |= (a.kind == zc::RoomActionKind::kNeedStart);
    ZC_CHECK(need_start);
  }
  ```
- [ ] Register `TestRoomPlan` in `tests/audio/test_util.h` + `tests/audio/test_main.cpp`; add `tests/zoom/test_room_plan.cpp` and `src/zoom/room_plan.cpp` to the `zcomms_audio_tests` sources in `CMakeLists.txt` (and `target_include_directories(zcomms_audio_tests PRIVATE tests/audio src/zoom)` if the chat-signaling plan has not already added it).
- [ ] `cmake --build build --config Release --target zcomms_audio_tests` — failing (module absent).
- [ ] Implement `PlanRooms` in `src/zoom/room_plan.cpp`: index live rooms and person→room by name; for each wanted room missing live, emit `kCreateRoom` when `!have.started`, else skip (and rely on the ops line in Task 3); for each wanted (person, room): if the person is in the right room, nothing; if `have.started` and the person appears in another live room, `kSwitchRunning`; if `have.started` and in no room, `kAssignRunning`; if `!have.started`, `kAssignUser`. If `!have.started` and `want.rooms` is non-empty and every create/assign is already satisfied, emit one `kNeedStart`.
- [ ] Run `build\Release\zcomms_audio_tests.exe`; green, existing tests untouched.
- [ ] Commit: `git add src/zoom/room_plan.h src/zoom/room_plan.cpp src/zoom/breakout.h tests/zoom/test_room_plan.cpp tests/audio/test_util.h tests/audio/test_main.cpp CMakeLists.txt && git commit -m "feat(bo): room_plan convergence planner for sub-production layouts (pure)"`

### Task 2: reach truth — per-channel reachability (pure)

**Files**
- Create: `src/zoom/reach.h`
- Create: `src/zoom/reach.cpp`
- Test: `tests/zoom/test_reach.cpp`
- Modify: `CMakeLists.txt`, `tests/audio/test_util.h`, `tests/audio/test_main.cpp`

**Interfaces**
- Consumes: `BreakoutState` (from `room_plan.h`), member display names per channel slot.
- Produces:
  ```cpp
  // src/zoom/reach.h  (no SDK includes)
  namespace zc {
  struct ChannelReach {
    std::vector<std::string> reachable;                          // same room as station
    std::vector<std::pair<std::string, std::string>> elsewhere;  // (name, room name)
  };
  // Station room comes from s.my_room ("" = main floor). A name in no room
  // list is on the main floor (matches BreakoutRooms::RoomOf).
  ChannelReach ReachFor(const BreakoutState& s,
                        const std::vector<std::string>& member_names);
  }
  ```

**Steps**

- [ ] Write the failing test `tests/zoom/test_reach.cpp`:
  ```cpp
  #include "reach.h"
  #include "test_util.h"

  void TestReach() {
    zc::BreakoutState s;
    s.enabled = true;
    s.started = true;
    s.in_bo = true;
    s.my_room = "GREEN ROOM";
    s.rooms = {{"id1", "GREEN ROOM", {"Me", "Pat"}}, {"id2", "SEG 2 CREW", {"Sam"}}};

    ZC_TEST("reach: same-room member reachable, other-room member named dark");
    auto r = zc::ReachFor(s, {"Pat", "Sam", "Lee"});
    ZC_CHECK(r.reachable.size() == 1);
    ZC_CHECK(r.reachable[0] == "Pat");
    ZC_CHECK(r.elsewhere.size() == 2);
    ZC_CHECK(r.elsewhere[0].first == "Sam");
    ZC_CHECK(r.elsewhere[0].second == "SEG 2 CREW");
    ZC_CHECK(r.elsewhere[1].first == "Lee");     // main floor while station is in a BO
    ZC_CHECK(r.elsewhere[1].second == "");       // "" renders as "main"

    ZC_TEST("reach: station on main floor reaches main-floor members only");
    zc::BreakoutState m = s;
    m.in_bo = false;
    m.my_room = "";
    auto r2 = zc::ReachFor(m, {"Pat", "Lee"});
    ZC_CHECK(r2.reachable == std::vector<std::string>{"Lee"});
    ZC_CHECK(r2.elsewhere.size() == 1 && r2.elsewhere[0].second == "GREEN ROOM");

    ZC_TEST("reach: no breakout session = everyone reachable");
    zc::BreakoutState off;
    auto r3 = zc::ReachFor(off, {"Pat", "Sam"});
    ZC_CHECK(r3.reachable.size() == 2 && r3.elsewhere.empty());
  }
  ```
- [ ] Register/build-wire as in Task 1 (`TestReach`, `tests/zoom/test_reach.cpp`, `src/zoom/reach.cpp`); build to failure.
- [ ] Implement `ReachFor` using the same name-walk `BreakoutRooms::RoomOf` does (share it: reimplement `RoomOf` in `reach.cpp` as `RoomOfName(const BreakoutState&, const std::string&)` and change `BreakoutRooms::RoomOf` in `breakout.cpp` to delegate to it, so there is exactly one room-resolution rule).
- [ ] Run `build\Release\zcomms_audio_tests.exe`; green.
- [ ] Commit: `git add src/zoom/reach.h src/zoom/reach.cpp src/zoom/breakout.cpp tests/zoom/test_reach.cpp tests/audio/test_util.h tests/audio/test_main.cpp CMakeLists.txt && git commit -m "feat(bo): ReachFor per-channel cross-room reachability truth (pure)"`

### Task 3: `BreakoutRooms` creator/admin verbs (SDK glue)

**Files**
- Modify: `src/zoom/breakout.h`
- Modify: `src/zoom/breakout.cpp`

**Interfaces**
- Consumes: the cached `IBOCreator* creator_` (new — the existing class discards it in `onHasCreatorRightsNotification`), `IBOAdmin* admin_`, `IBOData* data_`, `IBatchCreateBOHelper`.
- Produces (added to `class BreakoutRooms`):
  ```cpp
  // Rights truth for the panel. Creator/admin arrive via callbacks only.
  bool can_edit() const { return creator_ != nullptr; }
  bool can_admin() const { return admin_ != nullptr; }

  // Pre-start room creation, one batch transaction per call.
  bool CreateRooms(const std::vector<std::string>& names, std::string* error);
  // Assign by display name (resolved to the BO GUID via IBOData).
  bool AssignByName(const std::string& person, const std::string& room,
                    bool session_started, std::string* error);
  bool StartSession(std::string* error);   // IBOAdmin::StartBO
  bool StopSession(std::string* error);    // IBOAdmin::StopBO

  // Set when any BO data/status changed since last call; reading clears it.
  // The healer uses this to flush channel-membership staleness (law #2).
  bool ConsumeRoomsDirty();
  ```
- New event plumbing: `BreakoutRooms` additionally implements `IBOCreatorEvent`, `IBOAdminEvent`, and `IBODataEvent` (multiple inheritance next to the existing `IMeetingBOControllerEvent`), and `Attach()` now calls `SetEvent(this)` on creator/admin/data helpers as they arrive in the rights callbacks.

**Steps**

- [ ] Extend `breakout.h`: cache `creator_` in `onHasCreatorRightsNotification` (currently `{}`), null it in `onLostCreatorRightsNotification`; add the interface above; implement `IBOCreatorEvent` (`onCreateBOResponse(bool, const zchar_t*)` sets `rooms_dirty_` and logs failures; `onBOCreateSuccess`, `OnWebPreAssignBODataDownloadStatusChanged`, `OnBOOptionChanged`, `onRemoveBOResponse`, `onUpdateBONameResponse` as logging/no-op overrides), `IBOAdminEvent` (`onStartBOResponse` / `onStopBOResponse` / `onStartBOError(BOControllerError)` → ops-visible log + `rooms_dirty_`; `onHelpRequestReceived` no-op for now; `onBOEndTimerUpdated` no-op), `IBODataEvent` (`onBOInfoUpdated` / `onUnAssignedUserUpdated` / `OnBOListInfoUpdated` → `rooms_dirty_ = true`).
- [ ] Implement `CreateRooms` via the batch helper:
  ```cpp
  bool BreakoutRooms::CreateRooms(const std::vector<std::string>& names,
                                  std::string* error) {
    if (creator_ == nullptr) { *error = "no creator rights (need host, or desktop co-host under a desktop host)"; return false; }
    ZOOM_SDK_NAMESPACE::IBatchCreateBOHelper* batch = creator_->GetBatchCreateBOHelper();
    if (batch == nullptr) { *error = "batch create helper unavailable"; return false; }
    if (batch->CreateBOTransactionBegin() != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
      *error = "CreateBOTransactionBegin failed"; return false;
    }
    for (const auto& n : names) {
      if (!batch->AddNewBoToList(Widen(n).c_str())) {   // 32-char SDK cap; surface it
        *error = "AddNewBoToList refused '" + n + "' (name too long or >50 rooms)";
        return false;
      }
    }
    if (batch->CreateBoTransactionCommit() != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
      *error = "CreateBoTransactionCommit failed"; return false;
    }
    return true;  // async; onCreateBOResponse confirms per room
  }
  ```
- [ ] Implement `AssignByName`: resolve the person's BO GUID by scanning `data_->GetBOMeetingIDList()` room user lists and `data_->GetUnassignedUserList()` with `GetBOUserName` comparison (names, never meeting ids); resolve the room GUID by `IBOMeeting::GetBOName()` match; then `session_started ? (already-in-a-room ? admin_->SwitchAssignedUserToRunningBO(uid, roomid) : admin_->AssignNewUserToRunningBO(uid, roomid)) : creator_->AssignUserToBO(uid, roomid)`; every null helper or failed lookup fills `*error` with which right or name was missing.
- [ ] Implement `StartSession`/`StopSession` on `admin_` (`CanStartBO()` gate first; error text names the gate that failed).
- [ ] Build the app target: `cmake --build build --config Release --target zcomms` — compile-clean gate.
- [ ] Commit: `git add src/zoom/breakout.h src/zoom/breakout.cpp && git commit -m "feat(bo): creator/admin verbs -- batch room create, assign/switch by name, start/stop"`

### Task 4: healer stale-flush + panel verbs + reach display

**Files**
- Modify: `src/app/main.cpp`
- Modify: `src/app/ui_html.h`
- Modify: `CLAUDE.md`

**Interfaces**
- Consumes: `PlanRooms`, `ReachFor`, `BreakoutRooms::{CreateRooms, AssignByName, StartSession, StopSession, ConsumeRoomsDirty, Snapshot}`, `Roster`, `TalkbackChannels`.
- Produces: `/act` verbs — `bo layout <name>:<person>,<person>;<name>:<person>` (replace desired layout), `bo apply` (run one `PlanRooms` pass and execute its actions through `BreakoutRooms`, one batched create transaction max per pass), `bo start`, `bo stop`; panel state JSON gains per-channel `reach` (from `ReachFor`) which the existing dark-cell rendering consumes.

**Steps**

- [ ] In the session loop: hold a session-scoped `RoomLayout desired;` (never static — per-meeting state rule). `bo layout` parses the verb argument into it; `bo apply` calls `PlanRooms(desired, breakout.Snapshot())` and executes: `kCreateRoom`s collected into ONE `CreateRooms` call, then `kAssignUser`/`kAssignRunning`/`kSwitchRunning` via `AssignByName`, `kNeedStart` → ops line `bo: layout staged -- run 'bo start'`. Every false return `log_op`s the error string.
- [ ] Stale-flush: each loop iteration, `if (breakout.ConsumeRoomsDirty()) roster_dirty = true;` (reuse the exact mechanism the roster's `ConsumeDirty` already feeds) so the next healer pass recomputes invite intent; the healer already skips cross-room invites, so convergence is automatic.
- [ ] Reach in state JSON: when building the published snapshot, for each channel slot compute `ReachFor(bo_state, member_names)` and emit `"reach":{"ok":[names],"dark":[[name,room],...]}`; extend the panel HTML's channel cell renderer to show dark members as the existing `in <room>` treatment (it already renders that for the single-station case — generalize to per-member).
- [ ] Key refusal: a keyed channel whose `reach.ok` is empty gets the amber ARMED treatment plus ops line `CH n keyed -- nobody reachable (all in other rooms)`; reuse the keyed-but-empty warning path.
- [ ] Live checklist (first breakout production — this also retires CLAUDE.md's "NOT live-verified yet" flag): (a) `bo layout` + `bo apply` + `bo start` creates and staffs two rooms from the panel; (b) move a CH 1 member to another room; confirm within one healer pass the cell goes dark with the room name, no `WRONG_USAGE` in the log; (c) bring them back; confirm re-invite and audible delivery (`--test-signal` + `zcomms-tap`); (d) `bo stop`; confirm reach returns to all-reachable.
- [ ] Update `CLAUDE.md` (sub-production section: verbs, the flush-and-converge design, live-verified status once (a)-(d) pass).
- [ ] Run `build\Release\zcomms_audio_tests.exe`; green.
- [ ] Commit: `git add src/app/main.cpp src/app/ui_html.h CLAUDE.md && git commit -m "feat(bo): sub-production verbs, healer stale-flush on room transitions, per-channel reach truth"`

## Self-review checklist (fix inline before PR)

- [ ] No `TBD`, every step has code or an exact command; layout/verb grammar stated exactly.
- [ ] `BreakoutState` moved, not duplicated — one definition, in the SDK-free header.
- [ ] All SDK calls appear only in `breakout.cpp` / `main.cpp`; test-linked files include no SDK headers.
- [ ] Async honesty: no step claims a room exists before `onCreateBOResponse`.
