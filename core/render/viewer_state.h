#pragma once

#include "render/camera.h"

namespace splat {

// Comfort defaults from the C7 spec.
inline constexpr float kDefaultComfortRadius = 0.25f;  // metres
// The VR path is world-locked: the user stands at the capture point and steps
// around it. The desktop's 0.25 m radius fades the whole scene after a step
// (the VR2 headset run's grey wash); 2 m keeps a 1 m free zone and still fades
// before the capture volume is far behind. COMFORT_RADIUS overrides it.
inline constexpr float kDefaultVrComfortRadius = 2.0f;
// Fade begins at this fraction of the comfort radius, reaching the backdrop at
// the radius. 0.5 gives 12.5 cm of head travel to fade out at the default
// radius, instead of the 5 cm a 0.8 start left; the smoothstep curve is a
// function of distance only, so it is frame-rate independent.
inline constexpr float kDefaultFadeStart = 0.5f;

// The VR scene controls (L2/R2) go from a quarter size to four times in the
// viewer. The desktop's WORLD_SCALE key is clamped to the same range.
inline constexpr float kMinWorldScale = 0.25f;
inline constexpr float kMaxWorldScale = 4.0f;
inline constexpr float kMinComfortRadius = 0.05f;
inline constexpr float kMaxComfortRadius = 5.0f;

// Platform-neutral seated-VR viewer state shared by the desktop viewer and the
// PS5 HMD layer. It maps a tracked head pose to a scene pose and computes the
// comfort fade. It has no Vulkan or console dependency, so the maths is unit
// tested on any host.
//
// The capture point (scene origin) is pinned to `anchor()`, a position in the
// tracking space. Recentre moves the anchor to the current head position and
// rotates the scene so the current head yaw becomes scene yaw zero. World
// scale changes the rendered size of the scene: the renderer scales every splat
// position (and Gaussian) by it about the scene origin, and a scale press also
// moves the scene so that one chosen world point - the pivot, a cone
// percentile ahead of the eyes - keeps its place in the picture
// (scale_about_pivot()). The viewer
// never moves: the camera is the head, and the scene is what changes around it.
// `scene_offset()` translates the whole scene. Both are scene adjustments the
// user makes with the sticks and triggers, applied in the placement frame (x
// right, y down, z forward) - moving the scene by `d` moves the camera by `-d`,
// which is why the frame's position subtracts the offset. The comfort radius is
// measured as physical head travel from the anchor, in metres.
class ViewerState {
public:
    ViewerState();

    const float* anchor() const { return anchor_; }
    void set_anchor(float x, float y, float z);

    // Clamped to [kMinWorldScale, kMaxWorldScale].
    void set_world_scale(float scale);
    // Same, by a factor, about the scene's origin (the capture point): a pure
    // zoom when the viewer stands at the capture point, which is why the VR
    // viewer uses scale_about_pivot() instead.
    void scale_world(float factor) { set_world_scale(world_scale_ * factor); }
    float world_scale() const { return world_scale_; }
    // Scales the scene by `factor` about a point fixed in the world (the
    // placement frame), keeping that point where it is in the picture: the
    // displayed point p = scale * x + offset becomes pivot + f * (p - pivot),
    // i.e. `scale' = f * scale` and `offset' = pivot + f * (offset - pivot)`.
    // The camera (= the viewer) does not move. `f` is the factor the scale
    // range actually applied, so a press at 4x does not drift the offset.
    void scale_about_pivot(float factor, const float pivot[3]);

    // Translation of the scene in the placement frame, metres.
    void set_scene_offset(float x, float y, float z);
    void move_scene(float dx, float dy, float dz) {
        set_scene_offset(offset_[0] + dx, offset_[1] + dy, offset_[2] + dz);
    }
    const float* scene_offset() const { return offset_; }

    // Clamped to [kMinComfortRadius, kMaxComfortRadius].
    void set_comfort_radius(float radius);
    float comfort_radius() const { return comfort_radius_; }

    // Fraction of the radius where the fade starts, clamped to [0, 1].
    void set_fade_start(float fraction);
    float fade_start() const { return fade_start_; }

    // Auto-pause (headset removed, window unfocused) is decided by the
    // platform layer; the state only carries the flag for the UI.
    void set_paused(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }

    // Re-anchors at the current head pose: the anchor position becomes the
    // head position and the current head yaw becomes scene yaw zero. Pitch is
    // left alone.
    void recentre(const float head_position[3], float head_yaw);
    // The yaw that recentre() made scene yaw zero; scene_pose subtracts it.
    float yaw_offset() const { return yaw_offset_; }

    CameraPose scene_pose(const float head_position[3], float head_yaw,
                          float head_pitch) const;

    float head_distance(const float head_position[3]) const;
    // 1 inside fade_start * radius, 0 at or beyond radius, smoothstep between.
    float fade_for_distance(float distance) const;
    float comfort_fade(const float head_position[3]) const;

    // Full frame state for the renderer. `head_position` is tracking space.
    FrameView frame(const float head_position[3], float head_yaw, float head_pitch,
                    float ipd = 0.063f) const;

private:
    float anchor_[3] = {0.0f, 0.0f, 0.0f};
    float offset_[3] = {0.0f, 0.0f, 0.0f};
    float yaw_offset_ = 0.0f;
    float world_scale_ = 1.0f;
    float comfort_radius_ = kDefaultComfortRadius;
    float fade_start_ = kDefaultFadeStart;
    bool paused_ = false;
};

}  // namespace splat
