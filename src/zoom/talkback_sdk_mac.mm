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
  // Reserved once so id_cache_ never reallocates. The SDK caps a meeting at
  // 16 simultaneous channels; 64 is headroom against churn over the
  // adapter's lifetime without ever mattering in the steady state (same
  // rationale as WidenCached's reserve on Windows).
  id_cache_.reserve(64);
}

TalkbackSdkMac::~TalkbackSdkMac() {
  if (controller_ != nullptr) {
    ((__bridge ZoomSDKTalkbackController*)controller_).delegate = nil;
  }
  if (delegate_ != nullptr) {
    CFRelease(delegate_);
    delegate_ = nullptr;
  }
  for (auto& entry : id_cache_) {
    CFRelease(entry.second);
  }
}

void* TalkbackSdkMac::CachedId(const std::string& channel_id) {
  for (const auto& entry : id_cache_) {
    if (entry.first == channel_id) return entry.second;
  }
  // capacity is reserved (ctor) so this cannot reallocate and invalidate a
  // pointer already handed back to an earlier caller.
  void* retained = (__bridge_retained void*)Ns(channel_id);
  id_cache_.emplace_back(channel_id, retained);
  return retained;
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
      inviteUsersToChannel:(__bridge NSString*)CachedId(channel_id)
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
      removeUsersFromChannel:(__bridge NSString*)CachedId(channel_id)
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
      sendAudioDataToChannel:(__bridge NSString*)CachedId(channel_id)
                   audioData:(char*)pcm
                  dataLength:(unsigned int)(samples * (int)sizeof(int16_t))
                  sampleRate:kSampleRate
                     channel:ZoomSDKAudioChannel_Mono]);
}

TalkbackResult TalkbackSdkMac::SetChannelBackgroundVolume(
    const std::string& channel_id, float volume) {
  if (controller_ == nullptr) return TalkbackCall::NoController;
  return FromZoomError([((__bridge ZoomSDKTalkbackController*)controller_)
      setChannelBackgroundVolume:(__bridge NSString*)CachedId(channel_id)
                backgroundVolume:volume]);
}

}  // namespace zc
