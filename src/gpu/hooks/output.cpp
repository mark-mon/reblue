/**
 * @file    gpu/hooks/output.cpp
 * @brief   Retarget BD's render chain from its fixed design canvas to the
 *          window's aspect-fit rectangle, and keep the 2D layer authored
 *          against that canvas.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "vr/bdvr.h"
#include "gpu/vr_hud.h"
#include "gpu/output.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "engine/engine.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/host_resource_heap.h"
#include "gpu/settings.h"
#include "gpu/hooks/tweaks.h"

// screenW/H on VisualRender is BD's master dim: the scene RT base, the
// post/bloom pyramid, the composite and the 2D basis all derive from it. The
// device backbuffer dims size only the front buffer and the post ping-pong.
// Output res sets all of them to one logical size, then neutralizes
// bdRenderStep's per-frame force back to 1280.

// The hook bodies below are global symbols the recompiler dispatch externs by
// name, so they use these names unqualified.
using bd::gpu::kDesignCanvasAspect;
using bd::gpu::kDesignCanvasAspectEpsilon;
using bd::gpu::kDesignCanvasHeight;
using bd::gpu::kDesignCanvasWidth;
using bd::gpu::Output;

namespace {

// The close-up view's own W/H, both f32 on its 'this'.
constexpr u32 kCloseUpViewWidthOff = 0x1D0;
constexpr u32 kViewTextureWOff = 0x38;
constexpr u32 kViewTextureHOff = 0x3C;

constexpr u32 kViewRateOff = 0x348;
constexpr f32 kViewFitSlack = 8.0f;

// SAFE/RATE under the Mindows RENDER>DEBUG tree, the f32 scale of the guide box
// the renderer draws when SAFE/DISP is on. Stock 0.9 is the CRT overscan margin,
// which hides nothing on a display that shows the whole frame.
constexpr f32 kSafeAreaRate = 0.99f;

// Guest globals the output res hooks rewrite.
constexpr u32 kDeviceBackBufferWEA = 0x82DDA670;
constexpr u32 kDeviceBackBufferHEA = 0x82DDA674;
constexpr u32 kDisplayFloatDimsEA = 0x82DDA5E8; // {width, height} f32 pair
constexpr u32 kViewportWidthEA = 0x82DE8918;
constexpr u32 kViewportHeightEA = 0x82DE891C;

constexpr u32 kVisualRenderScreens[] = {0x1A28, 0x1B14, 0x1B1C};

constexpr u32 kCompositeScreenEA = 0x82DC98D0;

constexpr u32 kScreenFormat = 0x28280106;
constexpr u32 kCompositeFormat = 0x182801B6;

constexpr u32 kRenderTargetNextOff = 0x08;
constexpr u32 kRenderTargetOwnerOff = 0x0C;
constexpr u32 kRenderTargetTextureOff = 0x04;

// An authored sequence sizes its screen-covering effect quads to just span the
// fov the game frames itself at, so a wider frame leaves them short of the
// edges. Battle carries the summon and corporeal sequences, which run off the
// battle action steps rather than an .evt scene.
bool AuthoredFraming() {
  return bd::engine::IssEvent::LiveCount() > 0 ||
         static_cast<bool>(bd::engine::Game::Get().BattleCameraTask());
}

void WriteGuestOutputDims(u32 w, u32 h) {
  bd::mem::store<u32>(kDeviceBackBufferWEA, w);
  bd::mem::store<u32>(kDeviceBackBufferHEA, h);
  bd::mem::store<float>(kDisplayFloatDimsEA, static_cast<float>(w));
  bd::mem::store<float>(kDisplayFloatDimsEA + 4, static_cast<float>(h));
}

bool ScaleDesignDims(f64 &w, f64 &h) {
  if (w > kDesignCanvasWidth || h > kDesignCanvasHeight)
    return false;
  const f64 s = Output::RenderDensity();
  if (s <= 1.0)
    return false;
  w *= s;
  h *= s;
  // VR: the eye surface is narrower than 16:9, so a design-ratio view scaled by height (the event / cutscene views)
  // comes out wider than every screen surface and its picture never reaches the composite (the window and the
  // headset kept the last field frame). Give such a view the render surface itself, like the field's scene.
  u32 vr_w = 0, vr_h = 0;
  if (bdvr::RenderSize(&vr_w, &vr_h)) {
    u32 fit_w = 0, fit_h = 0;
    if (Output::RenderSize(fit_w, fit_h) && w > fit_w + 8.0) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        bdvr::Log("design-ratio view %.0fx%.0f wider than the render surface: %ux%u", w, h, fit_w, fit_h);
      }
      w = fit_w;
      h = fit_h;
    }
  }
  return true;
}

} // namespace

// Right after the ctor seeds screenW/H, so every buffer derived from them is
// created after this point.
void bdOutputResScreenDimsHook(PPCRegister &r31) {
  u32 w, h;
  if (!Output::RenderSize(w, h))
    return;
  bd::engine::VisualRender render(r31.u32);
  render.SetScreenW(static_cast<f32>(w));
  render.SetScreenH(static_cast<f32>(h));
}

// The bdInitGpuMemory tail, after the device dims and the float pair are
// written. Overwriting all three sizes the front buffer, the post ping-pong and
// the resolve source rect together.
void bdOutputResDeviceDimsHook() {
  u32 w, h;
  if (!Output::RenderSize(w, h))
    return;
  WriteGuestOutputDims(w, h);
  BD_INFO("[output-res] BD render dims -> {}x{} (swapchain {}x{})", w, h,
          bd::gpu::Video::OutputWidth(), bd::gpu::Video::OutputHeight());
}

bool bdOutputResRenderStepNeutralizeHook() {
  u32 w, h;
  return Output::RenderSize(w, h);
}

// r3/r4 are the hardcoded 1280x720 the ctor creates its composite/history
// texture at. It is the scene composite resolve target, so scaling it to the
// output dims keeps that resolve 1:1 against the source rect.
void bdOutputResCompositeTexScaleHook(PPCRegister &r3, PPCRegister &r4) {
  u32 w, h;
  if (!Output::RenderSize(w, h))
    return;
  r3.u32 = w;
  r4.u32 = h;
}

REX_IMPORT(__imp__bdCreateDynamicTexture, CreateDynamicTexture,
           u32(u32, u32, u32, u32, u32));
REX_IMPORT(__imp__bdCameraViewInit, CameraViewInit, void(u32, u32));

namespace {

void ResizeStaleRenderTargets(rex::CallFrame &frame, u8 *base, u32 node, u32 w,
                              u32 h) {
  if (!w || !h)
    return;
  for (u32 guard = 0; node && guard < 256; ++guard) {
    const u32 owner = bd::mem::load<u32>(node + kRenderTargetOwnerOff);
    if (owner) {
      const auto *texture =
          bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(
              bd::mem::load<u32>(owner + kRenderTargetTextureOff));
      if (texture && (texture->width != w || texture->height != h))
        CreateDynamicTexture(frame, base, owner, w, h, 1, kScreenFormat);
    }
    node = bd::mem::load<u32>(node + kRenderTargetNextOff);
  }
}

void RebuildForOutputSize(rex::CallFrame &frame, u8 *base, u32 w, u32 h) {
  const u32 render = bd::engine::VisualRender::Get().Address();
  if (!render)
    return;

  WriteGuestOutputDims(w, h);
  for (const u32 screen : kVisualRenderScreens)
    CreateDynamicTexture(frame, base, render + screen, w, h, 1, kScreenFormat);
  CreateDynamicTexture(frame, base, kCompositeScreenEA, w, h, 1,
                       kCompositeFormat);

  CameraViewInit(frame, base, render, 0);
  BD_INFO("[output-res] engine surfaces rebuilt at {}x{}", w, h);
}

} // namespace

REX_EXTERN(__imp__bdCreateRenderTargetTextures);
REX_HOOK_RAW(bdCreateRenderTargetTextures) {
  u32 w = 0;
  u32 h = 0;
  if (Output::RenderSize(w, h)) {
    const u32 head = ctx.r3.u32;
    const u32 want_w = (static_cast<u32>(ctx.f1.f64) + 7) & ~7u;
    const u32 want_h = (static_cast<u32>(ctx.f2.f64) + 7) & ~7u;

    rex::CallFrame frame(ctx);
    static u32 applied = Output::Generation();
    const u32 generation = Output::Generation();
    if (generation != applied) {
      applied = generation;
      RebuildForOutputSize(frame, base, w, h);
    }
    ResizeStaleRenderTargets(frame, base, head, want_w, want_h);
  }
  __imp__bdCreateRenderTargetTextures(ctx, base);
}

// The projection aspect is a camera field seeded with a literal 16:9, never
// derived from the screen, so widening the render target alone stretches the
// image. The composed ratio holds the vertical view and opens the horizontal,
// and going narrower holds the horizontal instead rather than crop what the
// game was framed for. Cameras carrying an aspect keep it. bd_fov_offset
// applies here too, since scaling the half-angle's tangent changes the term,
// and drops out under AuthoredFraming.
//
// BD open-codes this matrix in three places, all hooked here. Patching the
// register rather than the camera leaves the globals BD reads back for its
// own fov conversions at the ratio those were written for.
void bdProjectionAspectHook(PPCRegister &fov_half, PPCRegister &aspect) {
  if (std::fabs(aspect.f64 - kDesignCanvasAspect) > kDesignCanvasAspectEpsilon)
    return;

  double tan_scale =
      AuthoredFraming() ? 1.0 : bd::gpu::Settings::Get().FOVTanScale();
  const double target = bd::gpu::Output::ProjectionAspect();
  if (std::fabs(target - kDesignCanvasAspect) > kDesignCanvasAspectEpsilon) {
    aspect.f64 = target;
    if (target < kDesignCanvasAspect)
      tan_scale *= kDesignCanvasAspect / target;
  }
  // Left exactly alone when nothing asked for a change, rather than rebuilt
  // through atan(tan(x)) and moved by the float residue.
  if (tan_scale != 1.0)
    fov_half.f64 = std::atan(std::tan(fov_half.f64) * tan_scale);
  // VR: the game culls with this frustum; widen it to what the headset sees.
  bdvr::WidenProjection(&fov_half.f64, &aspect.f64);
}

void bdOutputResViewScaleHook(PPCRegister &w, PPCRegister &h) {
  ScaleDesignDims(w.f64, h.f64);
}

void bdSubViewRenderScaleHook(PPCRegister &r31) {
  u32 fit_w = 0;
  u32 fit_h = 0;
  if (!Output::RenderSize(fit_w, fit_h))
    return;
  const f64 full = std::min<f64>(kDesignCanvasWidth * Output::RenderDensity(),
                                 fit_w);
  const f32 width = bd::mem::load<float>(r31.u32 + kCloseUpViewWidthOff);
  if (width + kViewFitSlack >= full)
    return;
  bd::mem::store<float>(r31.u32 + kViewRateOff, 1.0f);
}

void bdFreeDfsViewTextureSizeHook(PPCRegister &r11) {
  f64 w = bd::mem::load<float>(r11.u32 + kViewTextureWOff);
  f64 h = bd::mem::load<float>(r11.u32 + kViewTextureHOff);
  if (!ScaleDesignDims(w, h))
    return;
  bd::mem::store<float>(r11.u32 + kViewTextureWOff, static_cast<float>(w));
  bd::mem::store<float>(r11.u32 + kViewTextureHOff, static_cast<float>(h));
}

void bdIssEventDimHook(PPCRegister &r10, PPCRegister &r11) {
  f64 w = r11.u32;
  f64 h = r10.u32;
  if (!ScaleDesignDims(w, h))
    return;
  r11.u32 = static_cast<u32>(w);
  r10.u32 = static_cast<u32>(h);
}

// This site takes its aspect from the view's own width over height, so the
// full-frame view renders at the design ratio and the composite stretches it
// over the whole surface. Nothing here can see that stretch, so the full-frame
// view is identified by size: anything narrower than the design canvas is a
// sub-view drawn into a rect of its own ratio, where a widened projection would
// be the distortion rather than the cure.
void bdViewProjectionAspectHook(PPCRegister &r31, PPCRegister &fov_half,
                                PPCRegister &aspect) {
  if (bd::mem::load<float>(r31.u32 + kCloseUpViewWidthOff) <
      kDesignCanvasWidth * bd::gpu::SceneRenderScale())
    return;
  bdProjectionAspectHook(fov_half, aspect);
}

// The seed hook above let the ctor build its RTs at the output dims, so restore
// the design canvas on exit. screenW/H stays there from now on, and the
// renderer takes the output dims from the per-site register patches below.
//
// Raw, on the inherited context: a typed REX_IMPORT re-roots the guest stack
// at ThreadState's r1 and overwrites the frames live underneath it.
REX_EXTERN(__imp__VisualRender__ctor);
REX_HOOK_RAW(VisualRender__ctor) {
  const u32 self = ctx.r3.u32;
  __imp__VisualRender__ctor(ctx, base);
  if (!self)
    return;
  bd::engine::VisualRender render(self);
  render.SetSafeRate(kSafeAreaRate);
  u32 w, h;
  if (!Output::RenderSize(w, h))
    return;
  render.SetScreenW(kDesignCanvasWidth);
  render.SetScreenH(kDesignCanvasHeight);
}

// The patch sites cover every read that wants the output dims, so the struct
// itself stays at the design canvas for the main thread UI layout reads. One
// wrapper per (axis, register) pair the sites load into.
namespace {
void OutputResPatchDim(PPCRegister &fr, bool height) {
  u32 w, h;
  if (!Output::RenderSize(w, h))
    return;
  fr.f64 = static_cast<double>(height ? h : w);
}
} // namespace
void bdOutputResScreenWf0Hook(PPCRegister &f0) { OutputResPatchDim(f0, false); }
void bdOutputResScreenHf0Hook(PPCRegister &f0) { OutputResPatchDim(f0, true); }
void bdOutputResScreenWf1Hook(PPCRegister &f1) { OutputResPatchDim(f1, false); }
void bdOutputResScreenHf2Hook(PPCRegister &f2) { OutputResPatchDim(f2, true); }
void bdOutputResScreenHf8Hook(PPCRegister &f8) { OutputResPatchDim(f8, true); }
void bdOutputResScreenWf9Hook(PPCRegister &f9) { OutputResPatchDim(f9, false); }
void bdOutputResScreenWf11Hook(PPCRegister &f11) {
  OutputResPatchDim(f11, false);
}
void bdOutputResScreenHf12Hook(PPCRegister &f12) {
  OutputResPatchDim(f12, true);
}

// These map NDC to pixels using the view's screen dims, which the patches above
// filled with the output dims, but every caller reads the result as
// design canvas coordinates. Pin the dim loads back and leave the struct alone.
void bdWorldToScreenDesignWf3Hook(PPCRegister &f3) {
  u32 w, h;
  if (Output::RenderSize(w, h))
    f3.f64 = kDesignCanvasWidth;
}
void bdWorldToScreenDesignHf4Hook(PPCRegister &f4) {
  u32 w, h;
  if (Output::RenderSize(w, h))
    f4.f64 = kDesignCanvasHeight;
}

// The pinned design canvas basis suits every quad authored against it, but the
// camera composite background quad is authored at the camera target texture
// size, which output res enlarged: under the pinned viewport constant only its
// top-left quarter shows. Walk the command nodes before the drain and rescale
// any quad whose extent matches the output dims.
struct Bd2DCommandNode {
  be_u32 pad_00[2]; // +0x00
  be_u32 next;      // +0x08
  be_u32 pad_0C[2]; // +0x0C
  be_u32 vertCount; // +0x14
  be_u32 vertData;  // +0x18  inline vertex payload VA (0 for text nodes)
  be_u32 stride;    // +0x1C
};
static_assert(offsetof(Bd2DCommandNode, next) == 0x08);
static_assert(offsetof(Bd2DCommandNode, vertCount) == 0x14);
static_assert(offsetof(Bd2DCommandNode, vertData) == 0x18);
static_assert(offsetof(Bd2DCommandNode, stride) == 0x1C);
struct Bd2DVertex {
  be_f32 x;
  be_f32 y;
}; // first 8 of the 0x14 stride

namespace {
// Authored extents this far apart still count as the same edge.
constexpr float kEdgeTolerance = 8.0f;

bool MaxMatches(float max_x, float max_y, double w, double h) {
  return std::fabs(max_x - static_cast<float>(w)) <= kEdgeTolerance &&
         std::fabs(max_y - static_cast<float>(h)) <= kEdgeTolerance;
}

// A quad node covering the whole surface (the scene composite, menu backdrops).
bool SpansSurface(const Bd2DCommandNode *n, u32 out_w, u32 out_h, float *max_out = nullptr) {
  const double density = Output::RenderDensity();
  const u32 count = n->vertCount;
  const u32 data = n->vertData;
  const u32 stride = n->stride;
  if (!data || stride != 0x14 || count < 3 || count > 64)
    return false;
  float max_x = -999999.0f, max_y = -999999.0f;
  float min_x = 999999.0f, min_y = 999999.0f;
  for (u32 i = 0; i < count; ++i) {
    const auto *v = bd::mem::at<const Bd2DVertex>(data + i * stride);
    if (!v)
      return false;
    min_x = std::min(min_x, static_cast<float>(v->x));
    min_y = std::min(min_y, static_cast<float>(v->y));
    max_x = std::max(max_x, static_cast<float>(v->x));
    max_y = std::max(max_y, static_cast<float>(v->y));
  }
  if (max_out) {
    max_out[0] = max_x;
    max_out[1] = max_y;
  }
  return std::fabs(min_x) <= kEdgeTolerance && std::fabs(min_y) <= kEdgeTolerance &&
         (MaxMatches(max_x, max_y, out_w, out_h) ||
          MaxMatches(max_x, max_y, kDesignCanvasWidth * density, kDesignCanvasHeight * density) ||
          MaxMatches(max_x, max_y, kDesignCanvasWidth, kDesignCanvasHeight));
}

void RenormalizeSizedQuads(u32 node, u32 out_w, u32 out_h) {
  for (int guard = 0; node && guard < 4096; ++guard) {
    const auto *n = bd::mem::at<const Bd2DCommandNode>(node);
    if (!n) {
      BD_WARN("RenormalizeSizedQuads: unmapped 2D node VA 0x{:08X}, "
              "stopping "
              "walk",
              node);
      break;
    }
    float mx[2];
    if (SpansSurface(n, out_w, out_h, mx)) {
      // Onto the canvas the pinned basis expects, flush to its edges, so
      // the drain's per-draw fit reads it as a backdrop.
      const u32 data = n->vertData, stride = n->stride;
      for (u32 i = 0; i < n->vertCount; ++i) {
        auto *v = bd::mem::at<Bd2DVertex>(data + i * stride);
        v->x = kDesignCanvasWidth * (static_cast<float>(v->x) / mx[0]);
        v->y = kDesignCanvasHeight * (static_cast<float>(v->y) / mx[1]);
      }
    }
    node = n->next;
  }
}

// VR dev: the scene composite list's nodes (count, stride, vertex bounds) logged when they change.
void LogCompositeNodes(u32 head, u32 out_w, u32 out_h) {
  std::string desc;
  int n_nodes = 0;
  for (u32 node = head; node && n_nodes < 16; ++n_nodes) {
    const auto *n = bd::mem::at<const Bd2DCommandNode>(node);
    if (!n)
      break;
    char buf[160];
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const u32 count = n->vertCount, data = n->vertData, stride = n->stride;
    if (data && stride >= 8 && count && count <= 64) {
      x0 = y0 = 1e9f;
      x1 = y1 = -1e9f;
      for (u32 i = 0; i < count; ++i) {
        const auto *v = bd::mem::at<const Bd2DVertex>(data + i * stride);
        if (!v)
          break;
        x0 = std::min(x0, float(v->x)), x1 = std::max(x1, float(v->x));
        y0 = std::min(y0, float(v->y)), y1 = std::max(y1, float(v->y));
      }
    }
    std::snprintf(buf, sizeof(buf), " [%u v, stride 0x%X, x %.0f..%.0f y %.0f..%.0f%s]", count, stride, x0, x1, y0,
                  y1, SpansSurface(n, out_w, out_h) ? " full" : "");
    desc += buf;
    node = n->next;
  }
  static std::string last;
  if (desc != last) {
    last = desc;
    bdvr::Log("scene list (%ux%u): %d node(s)%s", out_w, out_h, n_nodes, desc.c_str());
  }
}

// VR: the scene composite list (+0x1A54) also carries the talk portraits (a second camera's image drawn as a
// quad over the scene). Split it: the full-surface nodes stay in the eyes, the rest go with the HUD layer.
// Relinks the guest list into two chains; Restore puts the links back.
struct ListSplit {
  u32 surface = 0, rest = 0;
  std::vector<std::pair<u32, u32>> links; // node VA, original next
  bool Build(u32 head, u32 out_w, u32 out_h) {
    std::vector<u32> a, b;
    for (u32 node = head; node && a.size() + b.size() < 4096;) {
      auto *n = bd::mem::at<Bd2DCommandNode>(node);
      if (!n)
        return false;
      (SpansSurface(n, out_w, out_h) ? a : b).push_back(node);
      links.emplace_back(node, static_cast<u32>(n->next));
      node = n->next;
    }
    if (a.empty() || b.empty())
      return false;
    auto chain = [](const std::vector<u32> &v) {
      for (size_t i = 0; i < v.size(); ++i)
        bd::mem::at<Bd2DCommandNode>(v[i])->next = i + 1 < v.size() ? v[i + 1] : 0u;
      return v.front();
    };
    surface = chain(a);
    rest = chain(b);
    static size_t logged_a = 0, logged_b = 0;
    if (a.size() != logged_a || b.size() != logged_b) {
      logged_a = a.size();
      logged_b = b.size();
      bdvr::Log("scene list split: %zu full-surface node(s) in the eyes, %zu node(s) to the HUD layer", a.size(),
                b.size());
    }
    return true;
  }
  void Restore() {
    for (auto &[node, next] : links)
      if (auto *n = bd::mem::at<Bd2DCommandNode>(node))
        n->next = next;
  }
};
} // namespace

// The 2D overlay list is baked at the design canvas but drained after
// bdRenderFrame has set the viewport globals to the output res surface size.
// Pin the canvas basis for the drain, so text, HUD graphics and the background
// quad fill the surface as one unit, and restore after. Marking the drain rides
// along: under the pinned basis every draw inside it is in canvas coordinates,
// so the upload path can fit each one.
//
// Raw, on the inherited context: a typed REX_IMPORT re-roots the guest stack
// at ThreadState's r1 and overwrites the frames live underneath it.
// VisualRender (engine/state_layout.h kVisualRender) and its 2D list (the HUD
// and menus), drained by bdRenderFrame after the 3D composite.
constexpr u32 kVisualRenderVA = 0x82DC9848;
constexpr u32 kHudListOffsets[3] = {0x1A54, 0x1A64, 0x1A74};

REX_EXTERN(__imp__bdRenderSubmitList);
REX_HOOK_RAW(bdRenderSubmitList) {
  bdvr::TraceMark("RenderSubmitList", ctx.r3.u32);
  u32 w, h;
  auto *vw = bd::mem::at<be_f32>(kViewportWidthEA);
  auto *vh = bd::mem::at<be_f32>(kViewportHeightEA);
  const bool pin = Output::RenderSize(w, h) && vw && vh;

  float saved_w = 0.0f, saved_h = 0.0f;
  if (pin) {
    saved_w = *vw;
    saved_h = *vh;
    RenormalizeSizedQuads(ctx.r3.u32, w, h);
    *vw = kDesignCanvasWidth;
    *vh = kDesignCanvasHeight;
  }
  if (pin)
    bd::gpu::Video::SetDesignCanvasDrain(true);
  // VR: the 2D lists (VisualRender+0x1A64 / 0x1A74: HUD, menus) drawn over black and white first for their own layer
  // (gpu/vr_hud.cpp); the normal drain below is for the window.
  const u32 visual_render = bd::mem::try_load<u32>(kVisualRenderVA);
  int list_index = -1;
  for (int i = 0; i < 3 && visual_render && ctx.r3.u32; ++i)
    if (bd::mem::try_load<u32>(visual_render + kHudListOffsets[i]) == ctx.r3.u32)
      list_index = i;
  const u32 draws_before = bdvr::DrawCount();
  // +0x1A54 is the scene composite (a full-screen draw) plus, in talks, the speaker portraits: those go to the HUD
  // layer with the 2D lists (+0x1A64 / 0x1A74: HUD, menus), drawn over black and white first for their own layer
  // (gpu/vr_hud.cpp); the normal drain is for the window.
  auto hud_pass = [&](PPCContext &c) {
    if (!bd::gpu::vrhud::Begin())
      return;
    const PPCContext saved = c;
    __imp__bdRenderSubmitList(c, base);
    bd::gpu::vrhud::AfterBlack();
    c = saved;
    __imp__bdRenderSubmitList(c, base);
    bd::gpu::vrhud::AfterWhite();
    c = saved;
  };
  // Nothing in the overlay lists is re-aimed: some of their draws (talk name tags, the composite quad) use shaders
  // that read c24.. with the scene camera's constants still loaded and would otherwise match the world camera.
  bdvr::InOverlayList(list_index >= 0);
  static const char *const kListMarker[] = {"list 1A54", "list 1A64", "list 1A74"};
  if (list_index >= 0 && list_index < 3)
    bdvr::GpuMarker(kListMarker[list_index]);
  if (list_index == 0 && pin)
    LogCompositeNodes(ctx.r3.u32, w, h);
  ListSplit split;
  if (list_index == 0 && bdvr::HudSeparate() && split.Build(ctx.r3.u32, w, h)) {
    const PPCContext saved = ctx;
    ctx.r3.u32 = split.surface;
    __imp__bdRenderSubmitList(ctx, base);
    ctx = saved;
    ctx.r3.u32 = split.rest;
    bdvr::GpuMarker("list 1A54 portraits");
    hud_pass(ctx);
    __imp__bdRenderSubmitList(ctx, base);
    ctx = saved;
    split.Restore();
  } else {
    if (list_index >= 1 && bdvr::HudSeparate())
      hud_pass(ctx);
    __imp__bdRenderSubmitList(ctx, base);
  }
  bdvr::InOverlayList(false);
  bdvr::NoteList(list_index, ctx.r3.u32, bdvr::DrawCount() - draws_before);
  if (pin)
    bd::gpu::Video::SetDesignCanvasDrain(false);
  if (pin) {
    *vw = saved_w;
    *vh = saved_h;
  }
}
