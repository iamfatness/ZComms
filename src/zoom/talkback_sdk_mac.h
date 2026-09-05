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
  void* delegate_;     // ZCommsTalkbackDelegate*, retained
  TalkbackSdkEvents* events_ = nullptr;

  // Caches channel_id's NSString* so SendAudio -- called at 50 Hz on the TX
  // path whenever a channel is keyed -- does not heap-allocate a fresh
  // NSString on every send. The Windows adapter hit exactly this cost
  // widening wstrings per call and fixed it by caching (talkback_sdk_win.h's
  // WidenCached); this is its macOS counterpart. Deliberately unlocked: this
  // adapter makes no threading promise (see the file header), and a mutex
  // here would be one.
  void* CachedId(const std::string& channel_id);  // returns an NSString*, owned by the cache
  std::vector<std::pair<std::string, void*>> id_cache_;  // NSString*, retained
};

}  // namespace zc
