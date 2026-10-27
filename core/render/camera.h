#pragma once

#include <cstdint>

namespace splat {

// Capture-style pinhole intrinsics, in pixels of one eye image.
struct CameraIntrinsics {
    float fx = 0.0f;
    float fy = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;

    bool valid() const { return fx > 0.0f && fy > 0.0f; }
};

// Scene axes are OpenCV: x right, y down, z forward. A zero pose looks down +z
// from the origin, which is the capture camera.
//
// The orientation is yaw about scene y, then pitch about the yawed x, then roll
// about the pitched forward axis: R = Ry(yaw) * Rx(pitch) * Rz(roll), which is
// the decomposition HmdView produces from a tracked head pose. A PSVR2 head
// with roll drops out of world lock if the renderer cannot take it: the image
// then stays level with the head instead of with the world.
struct CameraPose {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;    // radians, positive turns right (+x)
    float pitch = 0.0f;  // radians, positive looks down (+y)
    float roll = 0.0f;   // radians, positive tilts the camera's right side down
};

// One frame's camera and comfort state. ViewerState (viewer_state.h) builds
// this from a head pose; a PS5 HMD layer can do the same without the desktop.
struct FrameView {
    CameraPose midpoint;
    float ipd = 0.063f;    // metres; eyes at midpoint +- ipd / 2
    float scene_scale = 1.0f;
    // 1 = fully visible, 0 = backdrop only. The renderer fades the splat
    // coverage with this factor; it never drops splats.
    float comfort_fade = 1.0f;
};

}  // namespace splat
