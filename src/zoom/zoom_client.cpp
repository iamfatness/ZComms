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
