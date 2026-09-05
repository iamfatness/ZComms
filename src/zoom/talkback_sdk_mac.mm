#include "talkback_sdk_mac.h"

// This file is compiled with -fobjc-arc (CMakeLists.txt, scoped to this one
// source file). It is a hard requirement, not a preference: the adapter's
// __bridge_retained/CFRelease pairing (delegate_ and the id cache) only does
// what it reads as under ARC. Under plain MRC, __bridge_retained silently
// drops the retain (a warning, not an error), and the cached NSString*s'
// autorelease gets freed out from under the id cache the moment a pool
// drains -- silent memory corruption, not a crash at the point of the bug.
// If this ever compiles outside the CMake target that sets the flag (e.g.
// added directly to the zcomms target in a later phase), fail loudly here
// instead of miscompiling quietly.
#if !__has_feature(objc_arc)
#error "talkback_sdk_mac.mm requires ARC (-fobjc-arc); see CMakeLists.txt"
#endif

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
  // Headroom, not a correctness requirement: CachedId returns a copied
  // void*, not a reference into this vector, so a reallocation costs an
  // extra copy and nothing more. Reserved once anyway so the common case
  // (SDK caps a meeting at 16 simultaneous channels) never reallocates at
  // all; 64 covers churn over the adapter's lifetime.
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
  // The pacer thread (SendAudio, under TalkbackChannels' send_m_) and the
  // healer (InviteUsers/RemoveUsers, called after it has already released
  // its own lock) both reach this vector with no lock shared between them --
  // id_cache_m_ is the only thing serialising the scan against the
  // emplace_back below. The returned void* is a copied value, not a
  // reference into the vector, so it stays valid after the lock is dropped
  // even if a later call reallocates the backing storage.
  std::lock_guard<std::mutex> lock(id_cache_m_);
  for (const auto& entry : id_cache_) {
    if (entry.first == channel_id) return entry.second;
  }
  // reserve() in the ctor keeps this from reallocating in the steady state
  // (headroom, not correctness -- see above, a reallocation here would only
  // cost an extra copy, never invalidate anything a caller is holding).
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
