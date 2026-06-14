#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_session_energy.h"

TEST_CASE("accumulates watt-seconds and Wh over a charging interval") {
  LiteSessionEnergy e;
  e.tick(0, true, 0);              // rising edge into charging -> session starts at t=0
  e.tick(3680, true, 3600000);     // 3680 W held for 1 h (3,600,000 ms)
  CHECK(e.wattSeconds() == 13248000u);  // 3680 * 3600 s
  CHECK(e.wattHours()   == 3680u);      // 13,248,000 / 3600
}

TEST_CASE("elapsed tracks wall time from session start") {
  LiteSessionEnergy e;
  e.tick(1000, true, 1000);        // rising edge: session start = 1000 ms
  e.tick(1000, true, 31000);       // +30 s
  CHECK(e.elapsedSecs() == 30u);
}

TEST_CASE("rising edge into charging resets the session") {
  LiteSessionEnergy e;
  e.tick(0, true, 0);
  e.tick(3600, true, 3600000);     // accrue some energy
  CHECK(e.wattHours() == 3600u);
  e.tick(3600, false, 3700000);    // stop charging
  e.tick(0, true, 4000000);        // new rising edge -> fresh session
  e.tick(1000, true, 4000000 + 3600000);
  CHECK(e.wattHours() == 1000u);   // only the new session counts
  CHECK(e.elapsedSecs() == 3600u);
}

TEST_CASE("no accrual when idle or not charging") {
  LiteSessionEnergy e;
  e.tick(0, true, 0);
  e.tick(0, true, 3600000);        // charging but zero power
  CHECK(e.wattSeconds() == 0u);
  CHECK(e.elapsedSecs() == 3600u); // elapsed still advances while charging
  LiteSessionEnergy e2;
  e2.tick(3000, false, 0);
  e2.tick(3000, false, 3600000);   // never charging
  CHECK(e2.wattSeconds() == 0u);
  CHECK(e2.elapsedSecs() == 0u);
}

TEST_CASE("stop freezes the session total (does not zero)") {
  LiteSessionEnergy e;
  e.tick(0, true, 0);
  e.tick(3600, true, 3600000);
  e.tick(3600, false, 3700000);    // stop
  CHECK(e.wattHours() == 3600u);   // frozen, not reset
}

TEST_CASE("long high-power session does not overflow") {
  LiteSessionEnergy e;
  e.tick(0, true, 0);
  // 19.2 kW for 24 h = 460,800 Wh -> 1,658,880,000 Ws (exceeds uint32 if mishandled)
  e.tick(19200, true, 86400000u);
  CHECK(e.wattHours() == 460800u);
}

TEST_CASE("rising-edge tick does not fold the pre-session idle gap into the new session") {
  LiteSessionEnergy e;
  e.tick(0, true, 0);
  e.tick(7000, true, 3600000);        // session A: 1 h @ 7000 W
  e.tick(7000, false, 3700000);       // stop
  // New session whose FIRST (rising-edge) sample is non-zero, after a long idle gap:
  e.tick(7000, true, 100000000u);     // rising edge with power -> must NOT integrate the gap
  e.tick(7000, true, 100000000u + 3600000);  // 1 h of real charging
  CHECK(e.wattHours() == 7000u);      // only the new session's 1 h, no gap leak
}

TEST_CASE("millis wraparound integrates without a spike") {
  LiteSessionEnergy e;
  e.tick(0, true, 0xFFFFF000u);        // rising edge near uint32 max
  e.tick(3600, true, 0x00001000u);     // wrapped; dt = 0x2000 = 8192 ms
  CHECK(e.wattSeconds() == 29491u);    // 3600 * 8192 / 1000 = 29491.2 -> 29491
}
