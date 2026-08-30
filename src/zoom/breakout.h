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

class BreakoutRooms : public ZOOM_SDK_NAMESPACE::IMeetingBOControllerEvent {
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

  // IMeetingBOControllerEvent -- rights arrive and depart via callbacks.
  void onHasCreatorRightsNotification(
      ZOOM_SDK_NAMESPACE::IBOCreator*) override {}
  void onHasAdminRightsNotification(
      ZOOM_SDK_NAMESPACE::IBOAdmin* admin) override { admin_ = admin; }
  void onHasAssistantRightsNotification(
      ZOOM_SDK_NAMESPACE::IBOAssistant* a) override { assistant_ = a; }
  void onHasAttendeeRightsNotification(
      ZOOM_SDK_NAMESPACE::IBOAttendee* a) override { attendee_ = a; }
  void onHasDataHelperRightsNotification(
      ZOOM_SDK_NAMESPACE::IBOData* d) override { data_ = d; }
  void onLostCreatorRightsNotification() override {}
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
  void onBOStatusChanged(ZOOM_SDK_NAMESPACE::BO_STATUS) override {}
  void onBOSwitchRequestReceived(const zchar_t*, const zchar_t*) override {}
  void onBroadcastBOVoiceStatus(bool) override {}
  void onShareFromMainSession(const unsigned int,
                              ZOOM_SDK_NAMESPACE::SharingStatus,
                              ZOOM_SDK_NAMESPACE::IShareAction*) override {}

 private:
  ZOOM_SDK_NAMESPACE::IMeetingBOController* controller_ = nullptr;
  ZOOM_SDK_NAMESPACE::IBOAdmin* admin_ = nullptr;
  ZOOM_SDK_NAMESPACE::IBOAssistant* assistant_ = nullptr;
  ZOOM_SDK_NAMESPACE::IBOAttendee* attendee_ = nullptr;
  ZOOM_SDK_NAMESPACE::IBOData* data_ = nullptr;
};

}  // namespace zc
