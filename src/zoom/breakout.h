// Breakout-room awareness (delivery law #2, live 2026-08-29): Zoom talkback
// does not cross breakout rooms -- a cross-room invite fails WRONG_USAGE,
// and a channel member who moves to another room hears nothing while every
// counter looks healthy. The desk must therefore always know which room it
// is in and which room every person is in, and must never offer a key that
// cannot deliver.
//
// Identity note: breakout user ids are their own string GUIDs, unrelated to
// meeting user ids -- the only join between the BO world and the roster is
// the display NAME, same as CoreVideo's talkback (names, never ids).
//
// Threading: everything here runs on the SDK pump thread. Snapshot() walks
// the SDK's own lists on demand; nothing is cached beyond the helper
// pointers the rights callbacks deliver.
#pragma once

// clang-format off
#include <windows.h>
// clang-format on

#include <string>
#include <vector>

#include "meeting_service_components/meeting_breakout_rooms_interface_v2.h"
// BreakoutRoomInfo/BreakoutState live in the SDK-free room_plan.h so the
// planner and its tests compile without sdk.lib; SDK-free headers may be
// included from SDK-including ones, never the reverse.
#include "room_plan.h"

namespace zc {

class BreakoutRooms : public ZOOM_SDK_NAMESPACE::IMeetingBOControllerEvent,
                      public ZOOM_SDK_NAMESPACE::IBOCreatorEvent,
                      public ZOOM_SDK_NAMESPACE::IBOAdminEvent,
                      public ZOOM_SDK_NAMESPACE::IBODataEvent {
 public:
  void Attach(ZOOM_SDK_NAMESPACE::IMeetingBOController* controller);

  BreakoutState Snapshot();

  // Where a display name currently lives: the room's name, or "" for the
  // main session / unknown. Someone in no room list is on the main floor.
  static std::string RoomOf(const BreakoutState& s, const std::string& name);

  // Moving the station. Switching to an arbitrary room needs assistant
  // rights (granted with co-host); attendees can only join their ASSIGNED
  // room. Both fail with a reason rather than half-working.
  bool SwitchToRoom(const std::string& bo_id, std::string* error);
  bool ReturnToMain(std::string* error);

  // Rights truth for the panel. Creator/admin objects arrive only via the
  // rights callbacks (host in main session = all four; a mobile-host
  // co-host or plain attendee = not) -- verbs below fail with the missing
  // right named, never half-work.
  bool can_edit() const { return creator_ != nullptr; }
  bool can_admin() const { return admin_ != nullptr; }

  // Pre-start room creation, ONE batch transaction per call (the talkback
  // rate-limit lesson applied preemptively: never N back-to-back calls).
  // Async: true means the transaction was accepted; rooms confirm one by
  // one via onCreateBOResponse.
  bool CreateRooms(const std::vector<std::string>& names, std::string* error);
  // Assign by display name (resolved to BO GUIDs via IBOData -- names are
  // the only join between the BO world and the roster).
  bool AssignByName(const std::string& person, const std::string& room,
                    bool session_started, std::string* error);
  bool StartSession(std::string* error);  // IBOAdmin::StartBO
  bool StopSession(std::string* error);   // IBOAdmin::StopBO

  // Set when any BO data/status changed since the last call; reading
  // clears it. The membership healer uses this to flush staleness (law #2:
  // any room transition re-provisions channel membership).
  bool ConsumeRoomsDirty();

  // IMeetingBOControllerEvent -- rights arrive and depart via callbacks;
  // each acquisition also hooks that helper's own event stream to us.
  void onHasCreatorRightsNotification(
      ZOOM_SDK_NAMESPACE::IBOCreator* c) override {
    creator_ = c;
    if (creator_ != nullptr) creator_->SetEvent(this);
  }
  void onHasAdminRightsNotification(
      ZOOM_SDK_NAMESPACE::IBOAdmin* admin) override {
    admin_ = admin;
    if (admin_ != nullptr) admin_->SetEvent(this);
  }
  void onHasAssistantRightsNotification(
      ZOOM_SDK_NAMESPACE::IBOAssistant* a) override { assistant_ = a; }
  void onHasAttendeeRightsNotification(
      ZOOM_SDK_NAMESPACE::IBOAttendee* a) override { attendee_ = a; }
  void onHasDataHelperRightsNotification(
      ZOOM_SDK_NAMESPACE::IBOData* d) override {
    data_ = d;
    if (data_ != nullptr) data_->SetEvent(this);
  }
  void onLostCreatorRightsNotification() override { creator_ = nullptr; }
  void onLostAdminRightsNotification() override { admin_ = nullptr; }
  void onLostAssistantRightsNotification() override { assistant_ = nullptr; }
  void onLostAttendeeRightsNotification() override { attendee_ = nullptr; }
  void onLostDataHelperRightsNotification() override { data_ = nullptr; }
  void onNewBroadcastMessageReceived(const zchar_t*, unsigned int,
                                     const zchar_t*) override {}
  void onBOStopCountDown(unsigned int) override {}
  void onHostInviteReturnToMainSession(
      const zchar_t*,
      ZOOM_SDK_NAMESPACE::IReturnToMainSessionHandler* handler) override {
    // The host pulling everyone home is an instruction, not a question.
    if (handler != nullptr) handler->ReturnToMainSession();
  }
  void onBOStatusChanged(ZOOM_SDK_NAMESPACE::BO_STATUS) override {
    rooms_dirty_ = true;
  }
  void onBOSwitchRequestReceived(const zchar_t*, const zchar_t*) override {}
  void onBroadcastBOVoiceStatus(bool) override {}
  void onShareFromMainSession(const unsigned int,
                              ZOOM_SDK_NAMESPACE::SharingStatus,
                              ZOOM_SDK_NAMESPACE::IShareAction*) override {}

  // IBOCreatorEvent -- room lifecycle confirmations (async).
  void onBOCreateSuccess(const zchar_t* strBOID) override;
  void OnWebPreAssignBODataDownloadStatusChanged(
      ZOOM_SDK_NAMESPACE::PreAssignBODataStatus) override {}
  void OnBOOptionChanged(const ZOOM_SDK_NAMESPACE::BOOption&) override {}
  void onCreateBOResponse(bool bSuccess, const zchar_t* strBOID) override;
  void onRemoveBOResponse(bool, const zchar_t*) override { rooms_dirty_ = true; }
  void onUpdateBONameResponse(bool, const zchar_t*) override {
    rooms_dirty_ = true;
  }

  // IBOAdminEvent -- session lifecycle confirmations (async).
  void onHelpRequestReceived(const zchar_t*) override {}
  void onStartBOError(ZOOM_SDK_NAMESPACE::BOControllerError errCode) override;
  void onBOEndTimerUpdated(int, bool) override {}
  void onStartBOResponse(bool bSuccess) override;
  void onStopBOResponse(bool bSuccess) override;

  // IBODataEvent -- the stale-flush triggers: any membership/list movement
  // anywhere marks the room world dirty.
  void onBOInfoUpdated(const zchar_t*) override { rooms_dirty_ = true; }
  void onUnAssignedUserUpdated() override { rooms_dirty_ = true; }
  void OnBOListInfoUpdated() override { rooms_dirty_ = true; }

 private:
  ZOOM_SDK_NAMESPACE::IMeetingBOController* controller_ = nullptr;
  ZOOM_SDK_NAMESPACE::IBOCreator* creator_ = nullptr;
  ZOOM_SDK_NAMESPACE::IBOAdmin* admin_ = nullptr;
  ZOOM_SDK_NAMESPACE::IBOAssistant* assistant_ = nullptr;
  ZOOM_SDK_NAMESPACE::IBOAttendee* attendee_ = nullptr;
  ZOOM_SDK_NAMESPACE::IBOData* data_ = nullptr;
  bool rooms_dirty_ = false;  // pump thread only, like everything here
};

}  // namespace zc
