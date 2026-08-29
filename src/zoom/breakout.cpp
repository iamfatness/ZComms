#include "breakout.h"

#include <cstdio>

using namespace ZOOM_SDK_NAMESPACE;

namespace zc {
namespace {

std::string Narrow(const zchar_t* s) {
  if (s == nullptr) return "";
  std::string out;
  for (const zchar_t* p = s; *p != 0; ++p) {
    out.push_back(*p < 128 ? static_cast<char>(*p) : '?');
  }
  return out;
}

std::wstring Widen(const std::string& s) {
  return std::wstring(s.begin(), s.end());  // BO ids are ASCII GUIDs
}

}  // namespace

void BreakoutRooms::Attach(IMeetingBOController* controller) {
  controller_ = controller;
  if (controller_ != nullptr) {
    controller_->SetEvent(this);
    // The rights callbacks may have fired before we listened; the getters
    // answer the same question on demand.
    admin_ = controller_->GetBOAdminHelper();
    assistant_ = controller_->GetBOAssistantHelper();
    attendee_ = controller_->GetBOAttedeeHelper();
    data_ = controller_->GetBODataHelper();
  }
}

BreakoutState BreakoutRooms::Snapshot() {
  BreakoutState s;
  if (controller_ == nullptr) return s;
  s.enabled = controller_->IsBOEnabled();
  s.started = controller_->IsBOStarted();
  s.in_bo = controller_->IsInBOMeeting();
  if (!s.started) return s;

  // Helper pointers can arrive lazily; refresh on demand.
  if (data_ == nullptr) data_ = controller_->GetBODataHelper();
  if (data_ == nullptr) return s;

  s.my_room = Narrow(data_->GetCurrentBoName());
  IList<const zchar_t*>* ids = data_->GetBOMeetingIDList();
  if (ids == nullptr) return s;
  for (int i = 0; i < ids->GetCount(); ++i) {
    IBOMeeting* bo = data_->GetBOMeetingByID(ids->GetItem(i));
    if (bo == nullptr) continue;
    BreakoutRoomInfo room;
    room.id = Narrow(bo->GetBOID());
    room.name = Narrow(bo->GetBOName());
    IList<const zchar_t*>* users = bo->GetBOUserList();
    if (users != nullptr) {
      for (int u = 0; u < users->GetCount(); ++u) {
        // BO user ids are their own GUIDs; only the display name joins
        // this world to the roster.
        room.user_names.push_back(Narrow(data_->GetBOUserName(users->GetItem(u))));
      }
    }
    s.rooms.push_back(std::move(room));
  }
  return s;
}

std::string BreakoutRooms::RoomOf(const BreakoutState& s,
                                  const std::string& name) {
  for (const BreakoutRoomInfo& r : s.rooms) {
    for (const std::string& u : r.user_names) {
      if (u == name) return r.name;
    }
  }
  return "";  // main floor (or not visible to us)
}

bool BreakoutRooms::SwitchToRoom(const std::string& bo_id, std::string* error) {
  if (assistant_ == nullptr && controller_ != nullptr) {
    assistant_ = controller_->GetBOAssistantHelper();
  }
  if (assistant_ != nullptr) {
    const std::wstring idw = Widen(bo_id);
    if (assistant_->JoinBO(idw.c_str())) return true;
    *error = "JoinBO refused";
    return false;
  }
  // An attendee can only join the room they were assigned to.
  if (attendee_ == nullptr && controller_ != nullptr) {
    attendee_ = controller_->GetBOAttedeeHelper();
  }
  if (attendee_ != nullptr) {
    if (attendee_->JoinBo()) return true;
    *error = "JoinBo refused (attendees can only join their assigned room)";
    return false;
  }
  *error = "no breakout rights (need co-host for free room movement)";
  return false;
}

bool BreakoutRooms::ReturnToMain(std::string* error) {
  if (assistant_ == nullptr && controller_ != nullptr) {
    assistant_ = controller_->GetBOAssistantHelper();
  }
  if (assistant_ != nullptr && assistant_->LeaveBO()) return true;
  if (attendee_ == nullptr && controller_ != nullptr) {
    attendee_ = controller_->GetBOAttedeeHelper();
  }
  if (attendee_ != nullptr && attendee_->LeaveBo()) return true;
  *error = "could not leave the breakout room";
  return false;
}

}  // namespace zc
