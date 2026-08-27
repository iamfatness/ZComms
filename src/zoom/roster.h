// The meeting roster: who is here, by stable-enough handle, kept current.
//
// Plan §5's rule is that Zoom user ids are meeting-scoped and recycled, so
// nothing durable may be keyed by them. The roster is the one place that
// touches raw ids: it listens to the participant events, keeps the id→name
// view current, and raises a dirty flag the app uses to heal talkback channel
// membership after joins, leaves and rejoins. Everything above it deals in
// names and in "everyone but me".
//
// Threading: all methods and callbacks run on the SDK pump thread, which is
// also the app's main loop thread. No locks by design; do not call from the
// audio path.
#pragma once

// clang-format off
#include <windows.h>
// clang-format on

#include <functional>
#include <string>
#include <vector>

// The participants header uses AudioType without including the header that
// defines it, so the audio interface must come first.
#include "meeting_service_components/meeting_audio_interface.h"
#include "meeting_service_components/meeting_participants_ctrl_interface.h"

namespace zc {

struct RosterMember {
  unsigned int user_id = 0;
  std::string name;
  bool is_host = false;
};

class Roster : public ZOOM_SDK_NAMESPACE::IMeetingParticipantsCtrlEvent {
 public:
  // Registers for events and takes the initial snapshot. The controller must
  // outlive the roster.
  void Attach(ZOOM_SDK_NAMESPACE::IMeetingParticipantsController* controller);

  // Everyone except this client.
  const std::vector<RosterMember>& others() const { return others_; }

  // Set when membership changed since the last call; reading clears it. The
  // app polls this once per loop and re-heals channel membership when true.
  bool ConsumeDirty();

  // IMeetingParticipantsCtrlEvent
  void onUserJoin(ZOOM_SDK_NAMESPACE::IList<unsigned int>* ids,
                  const zchar_t* info) override;
  void onUserLeft(ZOOM_SDK_NAMESPACE::IList<unsigned int>* ids,
                  const zchar_t* info) override;
  void onUserNamesChanged(ZOOM_SDK_NAMESPACE::IList<unsigned int>* ids) override;
  void onHostChangeNotification(unsigned int user_id) override;
  void onCoHostChangeNotification(unsigned int user_id, bool is_cohost) override;

  // The rest of the interface, unused but pure virtual.
  void onLowOrRaiseHandStatusChanged(bool, unsigned int) override {}
  void onInvalidReclaimHostkey() override {}
  void onAllHandsLowered() override {}
  void onLocalRecordingStatusChanged(
      unsigned int, ZOOM_SDK_NAMESPACE::RecordingStatus) override {}
  void onAllowParticipantsRenameNotification(bool) override {}
  void onAllowParticipantsUnmuteSelfNotification(bool) override {}
  void onAllowParticipantsStartVideoNotification(bool) override {}
  void onAllowParticipantsShareWhiteBoardNotification(bool) override {}
  void onRequestLocalRecordingPrivilegeChanged(
      ZOOM_SDK_NAMESPACE::LocalRecordingRequestPrivilegeStatus) override {}
  void onAllowParticipantsRequestCloudRecording(bool) override {}
  void onInMeetingUserAvatarPathUpdated(unsigned int) override {}
  void onParticipantProfilePictureStatusChange(bool) override {}
  void onFocusModeStateChanged(bool) override {}
  void onFocusModeShareTypeChanged(
      ZOOM_SDK_NAMESPACE::FocusModeShareType) override {}
  void onBotAuthorizerRelationChanged(unsigned int) override {}
  void onVirtualNameTagStatusChanged(bool, unsigned int) override {}
  void onVirtualNameTagRosterInfoUpdated(unsigned int) override {}
  void onCreateCompanionRelation(unsigned int, unsigned int) override {}
  void onRemoveCompanionRelation(unsigned int) override {}
  void onGrantCoOwnerPrivilegeChanged(bool) override {}

 private:
  void Refresh();

  ZOOM_SDK_NAMESPACE::IMeetingParticipantsController* controller_ = nullptr;
  std::vector<RosterMember> others_;
  unsigned int self_id_ = 0;
  bool dirty_ = false;
};

}  // namespace zc
