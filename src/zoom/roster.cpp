#include "roster.h"

#include <cstdio>

using namespace ZOOM_SDK_NAMESPACE;

namespace zc {
namespace {

std::string Narrow(const zchar_t* s) {
  if (s == nullptr) return "";
  std::string out;
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr,
                                    nullptr);
  if (n <= 1) return "";
  out.resize(static_cast<size_t>(n - 1));
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace

void Roster::Attach(IMeetingParticipantsController* controller) {
  controller_ = controller;
  if (controller_ == nullptr) return;
  controller_->SetEvent(this);
  Refresh();
  dirty_ = true;
}

bool Roster::ConsumeDirty() {
  const bool d = dirty_;
  dirty_ = false;
  return d;
}

void Roster::Refresh() {
  // Rebuilt from scratch on every change rather than incrementally patched:
  // the list is small (a meeting), the source of truth is the controller, and
  // an incremental view is exactly where a missed event turns into a stale
  // roster that no later event corrects.
  others_.clear();
  if (controller_ == nullptr) return;

  IUserInfo* self = controller_->GetMySelfUser();
  self_id_ = self != nullptr ? self->GetUserID() : 0;

  IList<unsigned int>* list = controller_->GetParticipantsList();
  if (list == nullptr) return;
  for (int i = 0; i < list->GetCount(); ++i) {
    const unsigned int uid = list->GetItem(i);
    if (uid == self_id_) continue;
    RosterMember m;
    m.user_id = uid;
    IUserInfo* info = controller_->GetUserByUserID(uid);
    if (info != nullptr) {
      m.name = Narrow(info->GetUserName());
      m.is_host = info->IsHost();
    }
    if (m.name.empty()) m.name = "user " + std::to_string(uid);
    others_.push_back(std::move(m));
  }
}

void Roster::onUserJoin(IList<unsigned int>* ids, const zchar_t*) {
  Refresh();
  dirty_ = true;
  if (ids != nullptr) {
    for (int i = 0; i < ids->GetCount(); ++i) {
      IUserInfo* info = controller_ != nullptr
                            ? controller_->GetUserByUserID(ids->GetItem(i))
                            : nullptr;
      std::printf("[roster] joined: %s\n",
                  info != nullptr ? Narrow(info->GetUserName()).c_str()
                                  : "(unknown)");
    }
  }
}

void Roster::onUserLeft(IList<unsigned int>* ids, const zchar_t*) {
  Refresh();
  dirty_ = true;
  if (ids != nullptr && ids->GetCount() > 0) {
    std::printf("[roster] %d participant(s) left\n", ids->GetCount());
  }
}

void Roster::onUserNamesChanged(IList<unsigned int>*) {
  Refresh();
  // Names only; membership is unchanged, so no dirty -- nothing to heal.
}

void Roster::onHostChangeNotification(unsigned int) { Refresh(); }

void Roster::onCoHostChangeNotification(unsigned int user_id, bool is_cohost) {
  // Loud on purpose: losing our own co-host role kills the ability to manage
  // channels, and plan §10 lists mid-show demotion as an open risk.
  std::printf("[roster] co-host %s for user %u\n",
              is_cohost ? "granted" : "REMOVED", user_id);
  Refresh();
}

}  // namespace zc
