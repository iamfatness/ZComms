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
