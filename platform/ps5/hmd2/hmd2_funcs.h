// The libSceHmd2 / libSceVrTracker2 / libSceVrHand exports this app resolves,
// as X-macro lists: hmd2.cpp expands them into its function pointers and
// name/slot tables, and tests/hmd2_tables_test.cpp expands them into the names
// every export table must carry. A name added here therefore reaches both the
// runtime and the test.
//
// The lists are data only: X(SizeAlign, ...) entries name hmd2.cpp's SizeAlign
// at expansion, so a consumer that only needs names (the test) ignores the
// return type and the argument list.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// -- Hmd2 and VrTracker2, resolved at runtime -------------------------------
#define HMD2_FUNCS(X)                                                                           \
    X(int, sceHmd2Initialize, (const void *param))                                              \
    X(int, sceHmd2Terminate, (void))                                                            \
    X(int, sceHmd2Open, (int user_id, int type, int index, const void *param))                  \
    X(int, sceHmd2Close, (int handle))                                                          \
    X(int, sceHmd2GetDeviceInformation, (void *info))                                           \
    X(int, sceHmd2GetFieldOfViewWithoutHandle, (void *fov))                                     \
    X(SizeAlign, sceHmd2ReprojectionQueryBufferSizeAlign, (void))                                \
    X(SizeAlign, sceHmd2ReprojectionQueryDisplayBufferSizeAlign, (void))                         \
    X(int, sceHmd2ReprojectionInitialize, (const void *param, const void *reserved))            \
    X(int, sceHmd2ReprojectionEnableVrMode, (uint64_t mode))                                    \
    X(int, sceHmd2ReprojectionDisableVrMode, (void))                                            \
    X(int, sceHmd2ReprojectionBeginFrame, (uint64_t frame))                                     \
    X(int, sceHmd2ReprojectionGetPredictedDisplayTime, (uint64_t frame, uint64_t *time))        \
    X(int, sceHmd2ReprojectionSetParam, (uint32_t buffer, uint64_t frame, const void *param,     \
                                         const void *trace))                                   \
    X(int, sceHmd2ReprojectionSetRenderConfig, (uint64_t half_rate, uint64_t r0, uint64_t r1))  \
    X(int, sceHmd2ReprojectionTerminate, (void))
// The TV mirror's five exports are optional: a library without them costs the
// TV, not the VR session, so they are resolved (only when Config::mirror asks
// for them) apart from the required slots above.
#define HMD2_MIRROR_FUNCS(X)                                                                    \
    X(SizeAlign, sceHmd2ReprojectionGetMirroringWorkMemorySizeAlign, (void))                     \
    X(SizeAlign, sceHmd2ReprojectionGetMirroringDisplayBufferSizeAlign, (void))                  \
    X(int, sceHmd2ReprojectionSetMirroringOption, (const void *option))                          \
    X(int, sceHmd2ReprojectionEnableMirroring, (const void *param))                              \
    X(int, sceHmd2ReprojectionDisableMirroring, (void))
#define VRTRACKER2_FUNCS(X)                                                                     \
    X(int, sceVrTracker2QueryMemory, (const void *param, void *out))                            \
    X(int, sceVrTracker2Initialize, (const void *param))                                        \
    X(int, sceVrTracker2Finalize, (void))                                                       \
    X(int, sceVrTracker2RegisterDevice, (int type, int handle))                                 \
    X(int, sceVrTracker2UnregisterDevice, (int handle))                                         \
    X(int, sceVrTracker2GetResult, (const void *param, void *result))
#define VRHAND_FUNCS(X)                                                                         \
    X(int, sceVrHandInitialize, (const void *param))                                            \
    X(int, sceVrHandFinalize, (void))                                                           \
    X(int, sceVrHandStart, (void))                                                              \
    X(int, sceVrHandStop, (void))                                                               \
    X(int, sceVrHandGetResult, (const void *param, void *result))
