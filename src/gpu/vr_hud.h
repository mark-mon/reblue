/**
 * @file    gpu/vr_hud.h
 * @brief   VR: the HUD (the 2D list drain) captured on its own (vr_hud.cpp).
 */
#pragma once

#include <rex/types.h>

namespace plume {
struct RenderTexture;
}

namespace bd::gpu::vrhud {

// Render thread, around the 2D list drain while VR keeps the HUD apart:
// Begin (world copied out, target cleared black) -> drain -> AfterBlack (copy,
// target cleared white) -> drain -> AfterWhite (copy, world put back) -> the
// normal drain. Begin false: nothing done, drain normally.
bool Begin();
void AfterBlack();
void AfterWhite();

// This frame's world (no HUD) and the HUD over black / white, all in
// SHADER_READ. False when the frame did not capture them.
bool Images(plume::RenderTexture **world, plume::RenderTexture **black,
            plume::RenderTexture **white, u32 *w, u32 *h);

} // namespace bd::gpu::vrhud
