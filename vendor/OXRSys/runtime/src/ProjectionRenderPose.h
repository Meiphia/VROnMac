// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "GraphicsTypes.h"
#include <openxr/openxr.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>

// Projection views describe the image's original render pose, even when an
// application resubmits an old image. Fixed reference spaces can be resolved
// without looking up a new tracking pose and incorrectly relabeling that image.
inline FrameRenderPose ProjectionRenderPose(bool referenceSpace, XrReferenceSpaceType type,
                                            const XrPosef& origin, const XrPosef& left,
                                            const XrPosef& right)
{
    FrameRenderPose result;
    if (!referenceSpace || (type != XR_REFERENCE_SPACE_TYPE_LOCAL &&
        type != XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR && type != XR_REFERENCE_SPACE_TYPE_STAGE)) return result;
    auto rotation = [](const XrQuaternionf& q) { return glm::quat(q.w,q.x,q.y,q.z); };
    auto position = [](const XrVector3f& v) { return glm::vec3(v.x,v.y,v.z); };
    glm::quat a = rotation(left.orientation), b = rotation(right.orientation);
    glm::quat base = rotation(origin.orientation);
    if (!std::isfinite(glm::dot(a,a)) || !std::isfinite(glm::dot(b,b)) ||
        !std::isfinite(glm::dot(base,base)) || glm::dot(a,a) < 1e-8f ||
        glm::dot(b,b) < 1e-8f || glm::dot(base,base) < 1e-8f) return result;
    a = glm::normalize(a); b = glm::normalize(b); base = glm::normalize(base);
    if (glm::dot(a,b) < 0.0f) b = -b;
    glm::quat center = glm::normalize(base * glm::normalize(a+b));
    glm::vec3 point = position(origin.position) + base * ((position(left.position)+position(right.position))*0.5f);
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return result;
    result.position[0]=point.x; result.position[1]=point.y; result.position[2]=point.z;
    result.orientation[0]=center.x; result.orientation[1]=center.y;
    result.orientation[2]=center.z; result.orientation[3]=center.w;
    result.valid=true;
    return result;
}
