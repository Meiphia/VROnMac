// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ProjectionRenderPose.h"
#include "StreamingFrameQueue.h"
using Catch::Approx;

TEST_CASE("Submitted stereo render pose stays attached to repeated pixels", "[streaming][render-pose]")
{
    XrPosef origin{{0,0,0,1},{0,0,0}};
    XrPosef left{{0,0,0,1},{-0.032f,1.6f,0.4f}};
    XrPosef right{{0,0,0,-1},{0.032f,1.6f,0.4f}};
    auto pose=ProjectionRenderPose(true,XR_REFERENCE_SPACE_TYPE_STAGE,origin,left,right);
    REQUIRE(pose.valid);
    REQUIRE(pose.position[0]==Approx(0));
    REQUIRE(pose.position[1]==Approx(1.6f));
    REQUIRE(pose.orientation[3]==Approx(1));
    StreamingFrame first; first.valid=true; first.source.renderPose=pose;
    StreamingFrameQueue queue; queue.Start(); queue.PushLatest(first);
    std::atomic<bool> running{true};
    StreamingFrame out; REQUIRE(queue.WaitPop(running, out));
    REQUIRE(out.source.renderPose.position[2]==Approx(0.4f));
    // Repeating this source must preserve its old pose, independent of tracking.
    auto repeated=ProjectionRenderPose(true,XR_REFERENCE_SPACE_TYPE_STAGE,origin,left,right);
    REQUIRE(repeated.position[2]==pose.position[2]);
    REQUIRE(repeated.orientation[3]==pose.orientation[3]);
    out.source.Reset(); REQUIRE_FALSE(out.source.renderPose.valid);
    queue.Stop();
}
TEST_CASE("Submitted pose includes reference-space offset and rotation", "[streaming][render-pose]")
{
    const float q=std::sqrt(0.5f);
    XrPosef origin{{0,q,0,q},{2,0,3}};
    XrPosef left{{0,0,0,1},{-0.03f,1.5f,-1}};
    XrPosef right{{0,0,0,1},{0.03f,1.5f,-1}};
    auto pose=ProjectionRenderPose(true,XR_REFERENCE_SPACE_TYPE_LOCAL,origin,left,right);
    REQUIRE(pose.valid);
    REQUIRE(pose.position[0]==Approx(1));
    REQUIRE(pose.position[1]==Approx(1.5f));
    REQUIRE(pose.position[2]==Approx(3));
    REQUIRE(pose.orientation[1]==Approx(q));
}
TEST_CASE("Dynamic or invalid render spaces retain the existing tracking fallback", "[streaming][render-pose]")
{
    XrPosef identity{{0,0,0,1},{0,0,0}};
    REQUIRE_FALSE(ProjectionRenderPose(true,XR_REFERENCE_SPACE_TYPE_VIEW,identity,identity,identity).valid);
    REQUIRE_FALSE(ProjectionRenderPose(false,XR_REFERENCE_SPACE_TYPE_STAGE,identity,identity,identity).valid);
    XrPosef invalid{};
    REQUIRE_FALSE(ProjectionRenderPose(true,XR_REFERENCE_SPACE_TYPE_STAGE,identity,invalid,identity).valid);
}
