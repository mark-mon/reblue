// Blue Dragon VR: OpenXR output (opt-in: BD_VR=theater|mono|stereo or vr_config.txt next to the exe).
//
// D3D12 backend only. The finished guest frame is drawn into OpenXR swapchain images by a small command list of
// our own, submitted on the game's direct queue right after the frame's own work. The OpenXR frame loop
// (xrWaitFrame / Begin / End) runs on its own thread at the headset rate and re-submits the most recently
// released images, so the headset never waits for the game.
//
// Phases: theater (the frame on a screen in front of you), mono (head-tracked camera, one image for both eyes as
// a projection layer), stereo (one image per eye).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
struct ID3D12CommandList;
struct D3D12_RESOURCE_BARRIER;

namespace bdvr {

enum class Mode : uint32_t { Off = 0, Theater = 1, Mono = 2, Stereo = 3 };
// Current mode (start: vr_config.txt / BD_VR; F8 cycles stereo / mono / theater while VR runs).
Mode RequestedMode();
// VR asked for at start (any mode but off). The renderer then picks D3D12.
bool Requested();
void CycleMode();
// Settings menu: switch between stereo / mono / theater (not off) and remember it for SaveConfig.
void SetMode(Mode mode);

struct Settings {
  std::string mode = "stereo";  // the VR build starts in stereo without a vr_config.txt (no headset: plays flat)
  int resolution = 100;                                          // percent of the recommended eye size
  int supersampling = 1;                                         // scene render scale on top (re:Blue's own setting)
  std::atomic<float> world_scale{10.0f};                         // game units per metre (1 unit ~ 10 cm)
  std::atomic<bool> keep_pitch{false};
  std::atomic<float> screen_width{3.2f}, screen_distance{2.5f};  // theater screen / menus, metres
  std::atomic<bool> hud{true};                                   // HUD of 3D frames on its own panel
  std::atomic<float> hud_distance{1.4f}, hud_width{1.4f}, hud_height{-0.1f};  // metres
  std::atomic<bool> hud_follow{false};                           // head-locked (else fixed in the room)
  std::atomic<float> cull_widen{1.6f};                           // game frustum (culling) tangent scale over the headset's
  std::atomic<float> menu_distance{1.0f}, menu_size{1.0f};      // settings menu panel: metres; 1 = 1 mm per pixel at 1 m
  bool from_file = false;
};
Settings& Cfg();
void SaveConfig();

struct Pose {
  float qx = 0, qy = 0, qz = 0, qw = 1;  // orientation
  float px = 0, py = 0, pz = 0;          // position, metres
};
struct Fov {
  float left = -0.8f, right = 0.8f, up = 0.8f, down = -0.8f;  // tangent angles, radians
};
struct EyeViews {
  bool valid = false;
  Pose head;    // view space in the (recentred) local space
  Pose eye[2];  // eye poses in the same space
  Fov fov[2];   // per-eye field of view
  uint64_t serial = 0;
};

class Xr {
 public:
  static Xr& Get();

  // Render thread, once the D3D12 device and direct queue exist. Starts OpenXR when VR is requested.
  bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
  void Shutdown();
  bool Active() const;

  // Present thread, after the frame's own command lists that write `source` were submitted to the queue.
  // `source` is in `source_state` (a D3D12_RESOURCE_STATES value) and is left in it. `width` x `height` is the
  // part of it that holds the image, at (`x0`, `y0`). `eye`: -1 = the whole frame (theater / mono), 0 / 1 = that
  // eye, 2 = the HUD, 3 = the settings menu (both premultiplied alpha kept).
  // HUD (eye 2) with `white`: `source` is the HUD drawn over black and `white` over white (same size and state);
  // alpha = 1 - (white - black), colour = black (premultiplied).
  void SubmitFrame(ID3D12Resource* source, uint32_t source_state, uint32_t width, uint32_t height, int eye = -1,
                   uint32_t x0 = 0, uint32_t y0 = 0, ID3D12Resource* white = nullptr);
  // The guest output's scanout gamma (re:Blue applies x^gamma at present): the copy shader does the same before
  // decoding sRGB.
  void SetSourceGamma(float gamma);

  // Present thread, stereo: run the frame's closed command list once for the left eye before the renderer submits
  // it again for the right one: XeEye = 0, the list, the left eye's copy (`image`, left in PIXEL_SHADER_RESOURCE by
  // the frame), the `restore` barriers (frame-end states back to frame-start ones), XeEye = 1.
  void RunFirstEye(ID3D12CommandList* frame_list, const D3D12_RESOURCE_BARRIER* restore, uint32_t restore_count,
                   ID3D12Resource* eye_buffer, ID3D12Resource* image, uint32_t width, uint32_t height);

  // Any thread: the newest head / eye poses and FOVs.
  EyeViews LatestViews() const;
  // The views the next submitted image was rendered with (mono / stereo frame matching).
  void SetRenderedViews(const EyeViews& views);
  void RecommendedEyeSize(uint32_t& width, uint32_t& height) const;
  // The headset's refresh rate (the highest seen: SteamVR's throttling halves the period at times), 0 = unknown.
  double DisplayHz() const;
  void RequestRecenter();
  // L3 + R3 held (300 ms) since the last call.
  bool TakeMenuToggle();

 private:
  Xr() = default;
};

// bdvr.log next to the exe (BD_VR_LOG overrides).
void Log(const char* format, ...);

}  // namespace bdvr
