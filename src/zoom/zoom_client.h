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
