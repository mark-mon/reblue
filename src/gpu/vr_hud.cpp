/**
 * @file    gpu/vr_hud.cpp
 * @brief   VR: the HUD on its own layer. Around each 2D list drain
 *          (VisualRender+0x1A64 / +0x1A74) the target is copied out
 *          (before the first: the world the eyes get), the list is drawn over
 *          the HUD so far on black and on white (alpha = 1 - (white - black),
 *          colour = black, premultiplied), and the target is put back for the
 *          normal drain the window shows.
 */
#include "gpu/frame.h"
#include "gpu/vr_hud.h"

#include <memory>
#include <mutex>
#include <vector>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "vr/bdvr.h"

namespace bd::gpu::vrhud {
namespace {

struct Tex {
  std::unique_ptr<plume::RenderTexture> tex;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  u32 w = 0, h = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
};
// world (before the first 2D list), cur (before this list), black / white (the 2D lists so far over black /
// white)
enum { kWorld, kCur, kBlack, kWhite, kCount };
Tex g_tex[kCount];
std::vector<std::unique_ptr<plume::RenderTexture>> g_retired;  // old sizes, kept alive (rare)
GuestTexture *g_rt = nullptr;      // the target of the list being captured
GuestTexture *g_frame_rt = nullptr; // the target of the frame's first captured list
bool g_first = false;              // the list being captured is the frame's first
u64 g_frame = ~0ull;               // frame of the captures
bool g_valid = false;

bool Ensure(VideoState &s, Tex &t, const GuestTexture *rt) {
  if (t.tex && t.w == rt->width && t.h == rt->height && t.format == rt->format)
    return true;
  if (t.tex)
    g_retired.push_back(std::move(t.tex));
  t.tex = s.device->createTexture(plume::RenderTextureDesc::Texture2D(
      rt->width, rt->height, 1, rt->format));
  t.layout = plume::RenderTextureLayout::UNKNOWN;
  t.w = rt->width;
  t.h = rt->height;
  t.format = rt->format;
  if (!t.tex) {
    BD_ERROR("[vr-hud] texture {}x{} could not be created", rt->width, rt->height);
    return false;
  }
  bdvr::Log("hud: textures %ux%u format %d", rt->width, rt->height, int(rt->format));
  return true;
}

void Copy(VideoState &s, plume::RenderTexture *dst, plume::RenderTextureLayout &dst_layout,
          plume::RenderTexture *src, plume::RenderTextureLayout &src_layout) {
  plume::RenderTextureBarrier pre[2] = {
      plume::RenderTextureBarrier(src, plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(dst, plume::RenderTextureLayout::COPY_DEST)};
  s.command_list->barriers(plume::RenderBarrierStage::COPY, pre, 2);
  s.command_list->copyTexture(dst, src);
  src_layout = plume::RenderTextureLayout::COPY_SOURCE;
  dst_layout = plume::RenderTextureLayout::COPY_DEST;
}
void CopyOut(VideoState &s, GuestTexture *rt, Tex &t) { Copy(s, t.tex.get(), t.layout, rt->texture, rt->layout); }
void CopyIn(VideoState &s, GuestTexture *rt, Tex &t) { Copy(s, rt->texture, rt->layout, t.tex.get(), t.layout); }

// The target the 2D list draws into (single sample, a real texture).
GuestTexture *Target(VideoState &s) {
  GuestTexture *rt = s.render_target;
  if (!rt || !rt->texture || rt->sampleCount != plume::RenderSampleCount::COUNT_1)
    return nullptr;
  return rt;
}

void ClearTarget(VideoState &s, float v) {
  if (!Video::BindDrawFramebufferLocked(true))
    return;
  s.command_list->clearColor(0, plume::RenderColor(v, v, v, 0.0f));
}

void ToShaderRead(VideoState &s, Tex &t) {
  if (t.layout == plume::RenderTextureLayout::SHADER_READ)
    return;
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(t.tex.get(), plume::RenderTextureLayout::SHADER_READ));
  t.layout = plume::RenderTextureLayout::SHADER_READ;
}

}  // namespace

bool Begin() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  g_rt = nullptr;
  if (!s.ready || !s.command_list_open)
    return false;
  GuestTexture *rt = Target(s);
  if (!rt)
    return false;
  const u64 frame = bdvr::FrameIndex();
  g_first = g_frame != frame;
  if (!g_first && rt != g_frame_rt) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      bdvr::Log("hud: a later 2D list draws into another target (%ux%u): left in the eyes", rt->width, rt->height);
    }
    return false;
  }
  if (g_first) {
    for (Tex &t : g_tex)
      if (!Ensure(s, t, rt))
        return false;
    g_frame = frame;
    g_frame_rt = rt;
    g_valid = false;
  }
  s.draw_framebuffer_bound = false;
  if (g_first)
    CopyOut(s, rt, g_tex[kWorld]);
  CopyOut(s, rt, g_tex[kCur]);
  if (g_first)
    ClearTarget(s, 0.0f);
  else
    CopyIn(s, rt, g_tex[kBlack]);
  g_rt = rt;
  return true;
}

void AfterBlack() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  GuestTexture *rt = g_rt;
  if (!rt || Target(s) != rt)
    return;
  s.draw_framebuffer_bound = false;
  CopyOut(s, rt, g_tex[kBlack]);
  if (g_first)
    ClearTarget(s, 1.0f);
  else
    CopyIn(s, rt, g_tex[kWhite]);
}

void AfterWhite() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  GuestTexture *rt = g_rt;
  g_rt = nullptr;
  if (!rt || Target(s) != rt)
    return;
  s.draw_framebuffer_bound = false;
  CopyOut(s, rt, g_tex[kWhite]);
  // What the target held before this list: the normal drain draws it again for the window.
  CopyIn(s, rt, g_tex[kCur]);
  for (Tex &t : g_tex)
    ToShaderRead(s, t);
  g_valid = true;
}

bool Images(plume::RenderTexture **world, plume::RenderTexture **black, plume::RenderTexture **white, u32 *w,
            u32 *h) {
  if (!g_valid || g_frame != bdvr::FrameIndex())
    return false;
  *world = g_tex[kWorld].tex.get();
  *black = g_tex[kBlack].tex.get();
  *white = g_tex[kWhite].tex.get();
  *w = g_tex[kWorld].w;
  *h = g_tex[kWorld].h;
  return true;
}

}  // namespace bd::gpu::vrhud
