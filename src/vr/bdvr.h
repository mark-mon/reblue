// Blue Dragon VR: what re:Blue's renderer and engine hooks call. OpenXR itself is in bdvr_xr.h.
//
// Head tracking and stereo are applied on the GPU only, as a clip-space transform after the game's own projection:
// every vertex shader that reads the camera constants (VS c24..c35: view, projection, view-projection, set by
// bdBuildViewMatrix) ends with  oPos = C[eye] * oPos  (XenosRecomp VR patch). C = P_eye * View(eye pose) * Level *
// P_game^-1 re-aims the game camera's image to the headset view; for draws whose camera is not a world camera
// (shadow maps, 2D) C is the identity. The guest's matrices are never changed, so frame interpolation, mouse look
// and the game's own camera logic are untouched.
//
// Stereo: the frame is recorded once (both eyes' C in every draw's SharedConstants) and its command list runs twice
// at the present: VrEye (b4, space4) = 0 for the left eye, then 1 for the right one (bdvr_plume.cpp).
#pragma once

#include <cstdint>

namespace plume {
struct RenderDevice;
struct RenderCommandQueue;
struct RenderCommandList;
struct RenderTexture;
}  // namespace plume

namespace bdvr {

// VR asked for at start (vr_config.txt next to the exe / BD_VR environment). Windows only.
bool VrRequested();
// D3D12 backend, once the device and the direct queue exist: start OpenXR (no-op when VR is off).
void InitFromPlume(plume::RenderDevice* device, plume::RenderCommandQueue* queue);
// The render size while VR runs (headset eye size x resolution %). False: the normal size rules apply.
bool RenderSize(uint32_t* width, uint32_t* height);

// ---- render thread, guest side
// bdRenderStep starts a frame: one head pose is latched for everything the frame draws.
void OnRenderFrameBegin();
// bdBuildViewMatrix got a camera: `view` / `proj` are the guest's row-vector matrices (big-endian floats, the
// values after frame interpolation). `view_va` tells the special views apart (shadow lights, projectors).
void OnCamera(uint32_t view_va, const void* view_be, const void* proj_be);
// Dev: the field camera's eye and look target (game units), logged every 10 s.
// Render thread: bdCameraRenderSetup entered / left (a frame can render several cameras' views).
void CameraSetup(bool begin);
// Render thread: an overlay list (+0x1A54 / 0x1A64 / 0x1A74) is being drained: its draws are never re-aimed.
void InOverlayList(bool in);
void NoteFieldCamera(const float eye[3], const float target[3]);
// Projection build (bdBuildProjectionMatrix and its inlined copies): widen the game's frustum to the headset's
// (times cull_widen) so the game culls for what the headset can see. `half_fov` (vertical, radians) and `aspect`
// are changed in place.
void WidenProjection(double* half_fov, double* aspect);

// ---- render thread, host side
// The per-draw clip matrices (2 eyes x 16 floats, rows of the column-form matrix) for SharedConstants, from the
// draw's VS float constants (big-endian, guest device+vsFloatConstants). Identity unless the draw's camera is a
// world camera this frame.
void ClipForDraw(const void* vs_constants_be, float out[2][16]);
// The frame's command list was just begun: the list the renderer records into (stereo: a forwarding wrapper).
plume::RenderCommandList* WrapCommands(plume::RenderCommandList* list);
// After the main pipeline layout is set on a new list: bind VrEye (root descriptor 4).
void BindEye(plume::RenderCommandList* list);
// Present, the frame's list closed, just before it is submitted: stereo runs it once for the left eye first.
// `image` is the guest's final picture (in SHADER_READ), `width` x `height` its size.
void BeforeFrameSubmit(plume::RenderCommandList* list, plume::RenderTexture* image, uint32_t width, uint32_t height,
                       float gamma);
// Present, after the frame's list was submitted: the picture to OpenXR (stereo: the right eye), and the HUD (drawn
// over black / white, same size as the picture; null: no HUD layer this frame).
void AfterFrameSubmit(plume::RenderTexture* image, uint32_t width, uint32_t height, float gamma,
                      plume::RenderTexture* hud_black = nullptr, plume::RenderTexture* hud_white = nullptr);
// While VR runs: the scene supersampling factor (vr_config supersampling, 1..4). 0 = not VR.
int SuperSampling();
// While VR runs: the frame rate to pace the game to (the headset's refresh), and no desktop vsync. 0 = not VR.
int PacingFps();
// The VR rows of re:Blue's settings menu (Display page): get / set by id, saved to vr_config.txt on every change.
enum class Setting : int {
  Mode, Resolution, WorldScale, KeepPitch, Hud, HudFollow, HudDistance, HudWidth, HudHeight, ScreenDistance,
  ScreenWidth, CullWiden, Supersampling, Recenter
};
bool Active();
double SettingGet(Setting id);
bool SettingSet(Setting id, double value);
// Frames presented so far (present thread).
uint64_t FrameIndex();

// Draws so far (all of them, render thread) and a 2D list drain (index 0..2 = VisualRender+0x1A54 / 0x1A64 /
// 0x1A74, -1 another list; `draws` = all its draws incl. the HUD capture passes): logged when the set changes.
uint32_t DrawCount();
void NoteList(int index, uint32_t list, uint32_t draws);
// Dev trace (frame after bdvr_dump.now): a structure mark with the draws since the previous mark.
void TraceMark(const char* what, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);

// VR is re-aiming the world camera this frame (mono / stereo, a 3D scene).
bool ClipActive();
// ... and the HUD goes on its own panel (vr_config hud = 1).
bool HudSeparate();

// Dev trace: true while recording the frame after a dump request (bdvr_dump.now).
bool TraceFrame();
void RequestTrace();
void Log(const char* format, ...);

}  // namespace bdvr
