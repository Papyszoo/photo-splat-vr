// The PS5 pad button word, in one place.
//
// A DualSense's ScePadData and a Sense controller's
// scePadVrControllerReadState state both start with the same 32-bit button
// mask, so every consumer of a pad sample - the viewer's scene controls, the
// panel UI's action map and the scripted-input player - reads one table. The
// values are HLA-PSVR2's PAD_* enum (ref/HLA-PSVR2/ps5xr/runtime/src/psvr2.c:386-389).
//
// The d-pad bits are the ones the panel UI navigates with; a real pad and a
// script's `Up`/`Down`/`Left`/`Right` lines produce the same bits, so a script
// exercises the real action path.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace splat {
namespace pad {

constexpr uint32_t kCreate = 0x00000001;
constexpr uint32_t kL3 = 0x00000002;
constexpr uint32_t kR3 = 0x00000004;
constexpr uint32_t kOptions = 0x00000008;
constexpr uint32_t kUp = 0x00000010;
constexpr uint32_t kRight = 0x00000020;
constexpr uint32_t kDown = 0x00000040;
constexpr uint32_t kLeft = 0x00000080;
constexpr uint32_t kL2 = 0x00000100;
constexpr uint32_t kR2 = 0x00000200;
constexpr uint32_t kL1 = 0x00000400;
constexpr uint32_t kR1 = 0x00000800;
constexpr uint32_t kTriangle = 0x00001000;
constexpr uint32_t kCircle = 0x00002000;
constexpr uint32_t kCross = 0x00004000;
constexpr uint32_t kSquare = 0x00008000;

// The system owns the pad while its own menu is up; the word carries this bit
// and a sample with it is neutral (HLA-PSVR2, psvr2.c:545-550).
constexpr uint32_t kIntercepted = 0x80000000u;

}  // namespace pad
}  // namespace splat
