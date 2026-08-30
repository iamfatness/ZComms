#include <string>

#include "signal_outbox.h"
#include "test_util.h"

void TestSignalOutbox() {
  std::printf("TestSignalOutbox\n");

  ZC_TEST("outbox: paces to one send per 300 ms");
  zc::SignalOutbox ob;
  ob.Push({1, "a"});
  ob.Push({2, "b"});
  zc::OutboundChat m;
  ZC_CHECK(ob.PopReady(1000, &m));
  ZC_CHECK(m.content == "a");
  ZC_CHECK(!ob.PopReady(1200, &m));  // 200 ms later: gated
  ZC_CHECK(ob.PopReady(1300, &m));   // 300 ms later: released
  ZC_CHECK(m.content == "b");
  ZC_CHECK(!ob.PopReady(9999, &m));  // empty

  ZC_TEST("outbox: drop-oldest past 64, counted");
  zc::SignalOutbox full;
  for (int i = 0; i < 70; ++i) full.Push({0, std::to_string(i)});
  ZC_CHECK(full.pending() == zc::SignalOutbox::kMaxQueued);
  ZC_CHECK(full.dropped() == 6);
  zc::OutboundChat first;
  ZC_CHECK(full.PopReady(0, &first));
  ZC_CHECK(first.content == "6");  // 0..5 were dropped oldest-first
}
