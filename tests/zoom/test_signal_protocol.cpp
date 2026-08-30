#include <string>

#include "signal_protocol.h"
#include "test_util.h"

void TestSignalProtocol() {
  std::printf("TestSignalProtocol\n");

  ZC_TEST("signal: cue round-trips");
  zc::SignalMsg m;
  m.kind = zc::SignalKind::kCue;
  m.slot = 3;
  m.on = true;
  const std::string wire = zc::EncodeSignal(m);
  ZC_CHECK(wire.rfind("~ZC1~", 0) == 0);
  zc::SignalMsg back;
  ZC_CHECK(zc::DecodeSignal(wire, &back));
  ZC_CHECK(back.kind == zc::SignalKind::kCue);
  ZC_CHECK(back.slot == 3);
  ZC_CHECK(back.on == true);

  ZC_TEST("signal: assign carries channel name and sender");
  zc::SignalMsg a;
  a.kind = zc::SignalKind::kAssign;
  a.slot = 2;
  a.channel_name = "CH 3";
  a.from = "Desk A";
  zc::SignalMsg aback;
  ZC_CHECK(zc::DecodeSignal(zc::EncodeSignal(a), &aback));
  ZC_CHECK(aback.channel_name == "CH 3");
  ZC_CHECK(aback.from == "Desk A");

  ZC_TEST("signal: non-signal text is rejected, not an error");
  zc::SignalMsg junk;
  ZC_CHECK(!zc::DecodeSignal("hello everyone", &junk));
  ZC_CHECK(!zc::DecodeSignal("~ZC2~{\"t\":\"cue\"}", &junk));  // future version
  ZC_CHECK(!zc::IsSignal("~zc1~{}"));  // case-sensitive prefix

  ZC_TEST("signal: quotes and backslashes in names survive");
  zc::SignalMsg q;
  q.kind = zc::SignalKind::kHello;
  q.from = "A \"B\" \\ C";
  zc::SignalMsg qback;
  ZC_CHECK(zc::DecodeSignal(zc::EncodeSignal(q), &qback));
  ZC_CHECK(qback.from == "A \"B\" \\ C");

  ZC_TEST("signal: assign notice is plain text, not protocol");
  const std::string notice = zc::AssignNoticeText("Pat", "CH 3");
  ZC_CHECK(!zc::IsSignal(notice));
  ZC_CHECK(notice.find("CH 3") != std::string::npos);
}
