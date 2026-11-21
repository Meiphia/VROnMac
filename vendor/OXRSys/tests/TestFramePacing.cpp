// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>
#include "FramePacer.h"
using namespace std::chrono_literals;
using Pacer = oxrsys::FramePacer;
TEST_CASE("Timer oversleep does not shift the frame deadline grid", "[pacing]")
{
    Pacer p; Pacer::TimePoint start{};
    REQUIRE(p.Deadline(start,10ms)==start+10ms);
    p.Advance(start+12ms);
    REQUIRE(p.Deadline(start+13ms,10ms)==start+20ms);
    p.Advance(start+22ms);
    REQUIRE(p.Deadline(start+23ms,10ms)==start+30ms);
}
TEST_CASE("Late apps do not add another period or build a catch-up queue", "[pacing]")
{
    Pacer p; Pacer::TimePoint start{};
    p.Deadline(start,10ms); p.Advance(start+10ms);
    REQUIRE(p.Deadline(start+35ms,10ms)<start+35ms);
    p.Advance(start+35ms);
    REQUIRE(p.Deadline(start+36ms,10ms)==start+40ms);
    p.Advance(start+10000ms);
    REQUIRE(p.Deadline(start+10000ms,10ms)==start+10010ms);
}
TEST_CASE("Negotiated refresh and a new session reset the schedule", "[pacing]")
{
    Pacer p; Pacer::TimePoint start{};
    p.Deadline(start,10ms); p.Advance(start+10ms);
    REQUIRE(p.Deadline(start+15ms,20ms)==start+35ms);
    p.Reset();
    REQUIRE(p.Deadline(start+18ms,20ms)==start+38ms);
}
