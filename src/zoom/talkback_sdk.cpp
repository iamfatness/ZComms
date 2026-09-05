#include "talkback_sdk.h"

namespace zc {

const char* TalkbackCallName(TalkbackCall c) {
  switch (c) {
    case TalkbackCall::Ok: return "OK";
    case TalkbackCall::TooFrequent: return "TOO_FREQUENT (rate limited)";
    case TalkbackCall::NoController: return "NO_CONTROLLER";
    case TalkbackCall::WrongUsage: return "WRONG_USAGE";
    case TalkbackCall::Failed: return "FAILED";
  }
  return "FAILED";
}

const char* TalkbackEventName(TalkbackEvent e) {
  switch (e) {
    case TalkbackEvent::Ok: return "OK";
    case TalkbackEvent::NoPermission: return "NO_PERMISSION (need host/co-host)";
    case TalkbackEvent::AlreadyExists: return "ALREADY_EXIST";
    case TalkbackEvent::CountOverflow: return "COUNT_OVERFLOW (max 16)";
    case TalkbackEvent::NotExist: return "NOT_EXIST";
    case TalkbackEvent::Rejected: return "REJECTED";
    case TalkbackEvent::Timeout: return "TIMEOUT";
    case TalkbackEvent::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

}  // namespace zc
