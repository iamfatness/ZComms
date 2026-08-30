#include "breakout.h"

#include <cstdio>

#include "reach.h"

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
    // answer the same question on demand. Each helper also gets its OWN
    // event stream pointed at us (confirmations + the stale-flush).
    creator_ = controller_->GetBOCreatorHelper();
    if (creator_ != nullptr) creator_->SetEvent(this);
    admin_ = controller_->GetBOAdminHelper();
    if (admin_ != nullptr) admin_->SetEvent(this);
    assistant_ = controller_->GetBOAssistantHelper();
    attendee_ = controller_->GetBOAttedeeHelper();
    data_ = controller_->GetBODataHelper();
    if (data_ != nullptr) data_->SetEvent(this);
  }
}

bool BreakoutRooms::ConsumeRoomsDirty() {
  const bool was = rooms_dirty_;
  rooms_dirty_ = false;
  return was;
}

// --- IBOCreatorEvent / IBOAdminEvent (async confirmations) ------------------

void BreakoutRooms::onBOCreateSuccess(const zchar_t* strBOID) {
  std::printf("[bo] room created: %s\n", Narrow(strBOID).c_str());
  rooms_dirty_ = true;
}

void BreakoutRooms::onCreateBOResponse(bool bSuccess, const zchar_t* strBOID) {
  // No step anywhere may claim a room exists before this fires.
  std::printf("[bo] create response: %s (%s)\n", Narrow(strBOID).c_str(),
              bSuccess ? "ok" : "FAILED");
  rooms_dirty_ = true;
}

void BreakoutRooms::onStartBOError(BOControllerError errCode) {
  std::printf("[bo] start error: %d\n", static_cast<int>(errCode));
  rooms_dirty_ = true;
}

void BreakoutRooms::onStartBOResponse(bool bSuccess) {
  std::printf("[bo] start: %s\n", bSuccess ? "ok" : "FAILED");
  rooms_dirty_ = true;
}

void BreakoutRooms::onStopBOResponse(bool bSuccess) {
  std::printf("[bo] stop: %s\n", bSuccess ? "ok" : "FAILED");
  rooms_dirty_ = true;
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
  // One room-resolution rule for the whole product; reach.cpp owns it.
  return RoomOfName(s, name);
}

bool BreakoutRooms::CreateRooms(const std::vector<std::string>& names,
                                std::string* error) {
  if (names.empty()) return true;
  if (creator_ == nullptr) {
    *error = "no creator rights (need host, or desktop co-host under a "
             "desktop host)";
    return false;
  }
  // One batch transaction for the lot -- the talkback rate-limit lesson,
  // applied preemptively. SDK caps: 50 rooms, 32-char names.
  IBatchCreateBOHelper* batch = creator_->GetBatchCreateBOHelper();
  if (batch == nullptr) {
    *error = "batch create helper unavailable";
    return false;
  }
  if (batch->CreateBOTransactionBegin() != SDKERR_SUCCESS) {
    *error = "CreateBOTransactionBegin failed";
    return false;
  }
  for (const std::string& n : names) {
    if (!batch->AddNewBoToList(Widen(n).c_str())) {
      *error = "AddNewBoToList refused '" + n +
               "' (name too long or >50 rooms)";
      return false;
    }
  }
  if (batch->CreateBoTransactionCommit() != SDKERR_SUCCESS) {
    *error = "CreateBoTransactionCommit failed";
    return false;
  }
  return true;  // async; onCreateBOResponse confirms per room
}

bool BreakoutRooms::AssignByName(const std::string& person,
                                 const std::string& room,
                                 bool session_started, std::string* error) {
  if (data_ == nullptr) data_ = controller_ ? controller_->GetBODataHelper()
                                            : nullptr;
  if (data_ == nullptr) {
    *error = "no BO data rights";
    return false;
  }
  // Resolve the person's BO GUID and current placement by NAME -- meeting
  // user ids never appear in this world.
  std::string person_guid, person_current_room_id, room_guid;
  IList<const zchar_t*>* ids = data_->GetBOMeetingIDList();
  if (ids != nullptr) {
    for (int i = 0; i < ids->GetCount() && room_guid.empty(); ++i) {
      IBOMeeting* bo = data_->GetBOMeetingByID(ids->GetItem(i));
      if (bo == nullptr) continue;
      if (Narrow(bo->GetBOName()) == room) room_guid = Narrow(bo->GetBOID());
    }
    for (int i = 0; i < ids->GetCount() && person_guid.empty(); ++i) {
      IBOMeeting* bo = data_->GetBOMeetingByID(ids->GetItem(i));
      if (bo == nullptr) continue;
      IList<const zchar_t*>* users = bo->GetBOUserList();
      if (users == nullptr) continue;
      for (int u = 0; u < users->GetCount(); ++u) {
        if (Narrow(data_->GetBOUserName(users->GetItem(u))) == person) {
          person_guid = Narrow(users->GetItem(u));
          person_current_room_id = Narrow(bo->GetBOID());
          break;
        }
      }
    }
  }
  if (person_guid.empty()) {
    IList<const zchar_t*>* unassigned = data_->GetUnassignedUserList();
    if (unassigned != nullptr) {
      for (int i = 0; i < unassigned->GetCount(); ++i) {
        if (Narrow(data_->GetBOUserName(unassigned->GetItem(i))) == person) {
          person_guid = Narrow(unassigned->GetItem(i));
          break;
        }
      }
    }
  }
  if (room_guid.empty()) {
    *error = "room '" + room + "' not found (created yet?)";
    return false;
  }
  if (person_guid.empty()) {
    *error = "'" + person + "' not found in the breakout roster";
    return false;
  }
  const std::wstring pw = Widen(person_guid);
  const std::wstring rw = Widen(room_guid);
  if (session_started) {
    if (admin_ == nullptr) {
      *error = "no admin rights (need host, or desktop co-host under a "
               "desktop host)";
      return false;
    }
    const bool in_a_room = !person_current_room_id.empty();
    const bool ok = in_a_room
                        ? admin_->SwitchAssignedUserToRunningBO(pw.c_str(),
                                                                rw.c_str())
                        : admin_->AssignNewUserToRunningBO(pw.c_str(),
                                                           rw.c_str());
    if (!ok) {
      *error = std::string(in_a_room ? "SwitchAssignedUserToRunningBO"
                                     : "AssignNewUserToRunningBO") +
               " refused for '" + person + "'";
      return false;
    }
    return true;
  }
  if (creator_ == nullptr) {
    *error = "no creator rights (need host, or desktop co-host under a "
             "desktop host)";
    return false;
  }
  if (!creator_->AssignUserToBO(pw.c_str(), rw.c_str())) {
    *error = "AssignUserToBO refused for '" + person + "'";
    return false;
  }
  return true;
}

bool BreakoutRooms::StartSession(std::string* error) {
  if (admin_ == nullptr) {
    *error = "no admin rights (need host, or desktop co-host under a "
             "desktop host)";
    return false;
  }
  if (!admin_->CanStartBO()) {
    *error = "CanStartBO says no (no rooms staged, or already running?)";
    return false;
  }
  if (!admin_->StartBO()) {
    *error = "StartBO refused";
    return false;
  }
  return true;  // async; onStartBOResponse confirms
}

bool BreakoutRooms::StopSession(std::string* error) {
  if (admin_ == nullptr) {
    *error = "no admin rights (need host, or desktop co-host under a "
             "desktop host)";
    return false;
  }
  if (!admin_->StopBO()) {
    *error = "StopBO refused";
    return false;
  }
  return true;  // async; onStopBOResponse confirms
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
