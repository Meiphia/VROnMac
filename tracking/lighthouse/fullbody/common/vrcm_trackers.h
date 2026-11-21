// Local experimental tracker feed. Little endian, fixed-size UDP on loopback.
#pragma once
#include "vrcm_protocol.h"
#define VRCM_TRACKER_MAGIC 0x31544b56u
#define VRCM_TRACKER_VERSION 1u
#define VRCM_TRACKER_PORT 47920
#define VRCM_MAX_TRACKERS 16u
#define VRCM_TRACKER_POSITION_VALID 1u
#define VRCM_TRACKER_ORIENTATION_VALID 2u
#define VRCM_TRACKER_CALIBRATED 1u
#pragma pack(push,1)
typedef struct VrcmTracker {
    uint64_t id;
    char name[128];
    char serial[128];
    uint32_t flags;
    VrcmPose pose;
} VrcmTracker;
typedef struct VrcmTrackerPacket {
    uint32_t magic, version, count, flags;
    int64_t sampleTimeNs;
    VrcmTracker trackers[VRCM_MAX_TRACKERS];
} VrcmTrackerPacket;
#define VRCM_HEAD_MAGIC 0x31444856u
#define VRCM_HEAD_PORT 47921
// Head pose as used by the native bridge, stamped on the host steady clock.
// Runtime prediction/network latency must be accounted for by calibration.
typedef struct VrcmHeadPacket {
    uint32_t magic, version, valid;
    int64_t sampleTimeNs;
    VrcmPose pose;
} VrcmHeadPacket;
#pragma pack(pop)
