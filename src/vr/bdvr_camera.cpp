// Blue Dragon VR: world cameras of the frame and their clip-space transforms (see bdvr.h).
#include "bdvr.h"
#include "bdvr_xr.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace bdvr {

// re:Blue's fixed addresses (engine/state_layout.h): views that are not world cameras.
namespace {
constexpr uint32_t kShadowLightView = 0x82DD6144;
constexpr uint32_t kCubeShadowLightView = 0x82776DA8;
constexpr uint32_t kWaterBottomLightView = 0x82776F34;
constexpr uint32_t kProjectorMapInfos = 0x82DD6100;
constexpr uint32_t kProjectorMapInfosEnd = 0x82DD7170;

// Column-vector 4x4 (m[row][col], v' = m v).
struct M4 {
  float m[4][4] = {};
  static M4 Identity() {
    M4 r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
    return r;
  }
};
M4 Mul(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double v = 0;
      for (int k = 0; k < 4; ++k) v += double(a.m[i][k]) * b.m[k][j];
      r.m[i][j] = float(v);
    }
  return r;
}
bool Invert(const M4& in, M4& out) {
  double a[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? in.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    if (std::fabs(a[p][c]) < 1e-12) return false;
    if (p != c)
      for (int j = 0; j < 8; ++j) std::swap(a[p][j], a[c][j]);
    const double d = a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      if (f != 0)
        for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
    }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out.m[i][j] = float(a[i][j + 4]);
  return true;
}
M4 FromQuat(float x, float y, float z, float w) {
  M4 r = M4::Identity();
  r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - z * w);     r.m[0][2] = 2 * (x * z + y * w);
  r.m[1][0] = 2 * (x * y + z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - x * w);
  r.m[2][0] = 2 * (x * z - y * w);     r.m[2][1] = 2 * (y * z + x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y);
  return r;
}
// World -> view for a pose given in OpenXR's right-handed space (-z forward). The game's main camera is right
// handed too (clip.w = -z); a left-handed view space (+z forward, the mirrored reflection camera) gets the pose
// mirrored in z (S R S, S p). Position scaled to game units.
M4 ViewOf(const Pose& p, float scale, bool left_handed) {
  const M4 r = left_handed ? FromQuat(-p.qx, -p.qy, p.qz, p.qw) : FromQuat(p.qx, p.qy, p.qz, p.qw);
  M4 v = M4::Identity();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) v.m[i][j] = r.m[j][i];
  const float t[3] = {p.px * scale, p.py * scale, (left_handed ? -p.pz : p.pz) * scale};
  for (int i = 0; i < 3; ++i) v.m[i][3] = -(v.m[i][0] * t[0] + v.m[i][1] * t[1] + v.m[i][2] * t[2]);
  return v;
}
// Projection for tangent extents (w = hand * z: hand -1 right handed, +1 left handed), keeping the game projection's
// depth rows (2, 3).
M4 ProjectionFor(const Fov& f, const M4& game, float hand) {
  const float l = std::tan(f.left), r = std::tan(f.right), u = std::tan(f.up), d = std::tan(f.down);
  M4 p;
  p.m[0][0] = 2.0f / (r - l);
  p.m[0][2] = -hand * (r + l) / (r - l);
  p.m[1][1] = 2.0f / (u - d);
  p.m[1][2] = -hand * (u + d) / (u - d);
  for (int j = 0; j < 4; ++j) {
    p.m[2][j] = game.m[2][j];
    p.m[3][j] = game.m[3][j];
  }
  return p;
}
Fov UnionFov(const EyeViews& v) {
  Fov f;
  f.left = std::min(v.fov[0].left, v.fov[1].left);
  f.right = std::max(v.fov[0].right, v.fov[1].right);
  f.up = std::max(v.fov[0].up, v.fov[1].up);
  f.down = std::min(v.fov[0].down, v.fov[1].down);
  return f;
}
float EnvFloat(const char* name, float def) {
  const char* e = std::getenv(name);
  return e && *e ? float(std::atof(e)) : def;
}
void LoadBE(const void* src, float out[16]) {
  const uint8_t* b = static_cast<const uint8_t*>(src);
  for (int i = 0; i < 16; ++i) {
    uint32_t u = (uint32_t(b[i * 4]) << 24) | (uint32_t(b[i * 4 + 1]) << 16) | (uint32_t(b[i * 4 + 2]) << 8) |
                 uint32_t(b[i * 4 + 3]);
    out[i] = std::bit_cast<float>(u);
  }
}

// The world cameras of the frame being recorded (render thread only).
struct Camera {
  float key[32];           // view then projection as the VS constants hold them (c24..c31, transposed rows)
  float key_raw[32];       // the same, untransposed (whichever the game uses matches)
  float clip[2][16];       // per eye, rows of the column-form matrix
};
constexpr int kMaxCameras = 16;
struct Frame {
  Camera cams[kMaxCameras];
  int count = 0;
  int next = 0;             // ring once full
  bool latched = false;
  EyeViews views;           // the pose of this frame
  uint64_t serial = 0;
  // stats
  uint64_t world_draws = 0, other_draws = 0, cameras = 0, rejected = 0;
} f;
int g_setup_count = 0, g_setup_depth = 0;  // CameraRenderSetup calls this frame / open now
bool g_in_overlay = false;
int g_cur_setup = -1, g_main_setup = -1;    // the open one / the first that built a perspective camera
std::atomic<uint64_t> g_world_draws_frame{0};  // world draws of the last finished frame (scene kind)
std::atomic<bool> g_scene3d{false};

const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

bool ClipWanted() {
  const Mode m = RequestedMode();
  return (m == Mode::Mono || m == Mode::Stereo) && Xr::Get().Active();
}

}  // namespace

bool SceneIs3d() { return g_scene3d.load(std::memory_order_relaxed); }
bool ClipActive() { return ClipWanted() && SceneIs3d(); }
bool HudSeparate() { return ClipActive() && Cfg().hud; }

// Present thread, once per frame: 3D (re-aimed) while the frames draw through a world camera, else flat.
// The camp menu / after-battle screens keep the world camera but draw only a few dozen of its draws (the scene is a
// frozen blurred picture of the last frame): those count as flat too, so the picture goes on the flat screen
// instead of being shown head-locked, with one eye's view, in both eyes.
constexpr uint64_t kMinWorldDraws = 64;
void UpdateSceneKind(uint64_t world_draws) {
  static uint32_t without = 0, with = 0;
  if (world_draws < kMinWorldDraws) {
    ++without;
    with = 0;
  } else {
    ++with;
    without = 0;
  }
  bool now = g_scene3d.load();
  if (now && without >= 30) now = false;
  if (!now && with >= 3) now = true;
  if (now != g_scene3d.exchange(now)) Log("scene: %s (world draws %llu)", now ? "3D (world camera)" : "flat (no or few world camera draws)", (unsigned long long)world_draws);
}

void FlushListStats();

void OnRenderFrameBegin() {
  FlushListStats();
  // The world draws of the frame that just ended decide the next frame's kind.
  UpdateSceneKind(f.world_draws);
  static auto last = std::chrono::steady_clock::now();
  static uint64_t frames = 0, world = 0, other = 0, cams = 0;
  ++frames;
  world += f.world_draws;
  other += f.other_draws;
  cams += f.cameras;
  const auto now = std::chrono::steady_clock::now();
  if (now - last >= std::chrono::seconds(10)) {
    const double secs = std::chrono::duration<double>(now - last).count();
    Log("camera: %.1f frames/s, world draws %.0f/frame, other camera-shader draws %.0f/frame, world cameras "
        "%.1f/frame, rejected views %llu",
        frames / secs, frames ? double(world) / frames : 0.0, frames ? double(other) / frames : 0.0,
        frames ? double(cams) / frames : 0.0, (unsigned long long)f.rejected);
    frames = world = other = cams = 0;
    last = now;
  }
  f.count = f.next = 0;
  g_setup_count = g_setup_depth = 0;
  g_cur_setup = g_main_setup = -1;
  f.world_draws = f.other_draws = f.cameras = 0;
  f.latched = false;
}

void CameraSetup(bool begin) {
  if (begin) {
    g_cur_setup = g_setup_count++;
    ++g_setup_depth;
  } else if (g_setup_depth > 0) {
    --g_setup_depth;
  }
}

void InOverlayList(bool in) { g_in_overlay = in; }

void NoteFieldCamera(const float eye[3], const float target[3]) {
  static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(9);
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(10)) return;
  last = now;
  const float d[3] = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
  Log("field camera: eye %.1f %.1f %.1f target %.1f %.1f %.1f distance %.1f (height above target %.1f)", eye[0], eye[1],
      eye[2], target[0], target[1], target[2], std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), d[1]);
}

void WidenProjection(double* half_fov, double* aspect) {
  {
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(9);
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(10)) {
      last = now;
      Log("projection build: game vertical fov %.2f deg, aspect %.3f", *half_fov * 2.0 * 57.29578, *aspect);
    }
  }
  if (!ClipActive() || *aspect <= 0.0) return;
  const EyeViews v = Xr::Get().LatestViews();
  if (!v.valid) return;
  const Fov u = UnionFov(v);
  const double k = std::clamp(double(Cfg().cull_widen.load()), 1.0, 4.0);
  const double tv = std::max(std::tan(double(u.up)), -std::tan(double(u.down))) * k;
  const double th = std::max(-std::tan(double(u.left)), std::tan(double(u.right))) * k;
  const double t = std::max({std::tan(*half_fov), tv, th / *aspect});
  *half_fov = std::atan(t);
}

void OnCamera(uint32_t view_va, const void* view_be, const void* proj_be) {
  if (!ClipWanted() || !view_be || !proj_be) return;
  if (view_va == kShadowLightView || view_va == kCubeShadowLightView || view_va == kWaterBottomLightView ||
      (view_va >= kProjectorMapInfos && view_va < kProjectorMapInfosEnd))
    return;
  float vr[16], pr[16];  // row-vector (D3D) layout, row-major
  LoadBE(view_be, vr);
  LoadBE(proj_be, pr);
  if (TraceFrame()) {
    Log("trace OnCamera va %08X P row0 %.4f %.4f %.4f %.4f row1 %.4f %.4f %.4f %.4f row2 %.4f %.4f %.4f %.4f "
        "row3 %.4f %.4f %.4f %.4f | V row3 %.1f %.1f %.1f",
        view_va, pr[0], pr[1], pr[2], pr[3], pr[4], pr[5], pr[6], pr[7], pr[8], pr[9], pr[10], pr[11], pr[12],
        pr[13], pr[14], pr[15], vr[12], vr[13], vr[14]);
  }
  // Perspective: clip.w = +-view z (column 3 of P = 0 0 +-1 0). The main camera is right handed (-1), the
  // mirrored reflection camera left handed (+1).
  if (std::fabs(pr[3]) > 1e-6f || std::fabs(pr[7]) > 1e-6f || std::fabs(std::fabs(pr[11]) - 1.0f) > 1e-4f ||
      std::fabs(pr[15]) > 1e-6f) {
    ++f.rejected;
    return;
  }
  // The VS constants hold each matrix transposed (rows of the column form): key = V^T, P^T.
  Camera c;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      c.key[i * 4 + j] = vr[j * 4 + i];
      c.key[16 + i * 4 + j] = pr[j * 4 + i];
    }
  std::memcpy(c.key_raw, vr, sizeof(vr));
  std::memcpy(c.key_raw + 16, pr, sizeof(pr));
  for (int i = 0; i < f.count; ++i)
    if (std::memcmp(f.cams[i].key, c.key, sizeof(c.key)) == 0) return;  // seen this frame
  // One head pose per frame (jitter.txt fix 3).
  if (!f.latched) {
    f.views = Xr::Get().LatestViews();
    f.latched = true;
  }
  const EyeViews& views = f.views;
  if (!views.valid) return;
  // A later camera setup of the frame (the talk portraits' close-up camera) renders into its own small target
  // that is then drawn as a picture: drawn as the game does, not re-aimed by the head.
  if (g_setup_depth > 0 && g_main_setup < 0) g_main_setup = g_cur_setup;
  if (g_setup_depth > 0 && g_cur_setup != g_main_setup) {
    static float logged[8];
    static int nlogged = 0;
    bool seen = false;
    for (int i = 0; i < nlogged; ++i) seen = seen || std::fabs(logged[i] - pr[0]) < 1e-3f;
    if (!seen && nlogged < 8) {
      logged[nlogged++] = pr[0];
      Log("secondary camera setup %d (P00 %.3f P11 %.3f): drawn as the game does, not re-aimed", g_cur_setup, pr[0],
          pr[5]);
    }
    for (int e = 0; e < 2; ++e) std::memcpy(c.clip[e], kIdentity, sizeof(kIdentity));
    if (f.count < kMaxCameras) {
      f.cams[f.count++] = c;
    } else {
      f.cams[f.next] = c;
      f.next = (f.next + 1) % kMaxCameras;
    }
    return;
  }
  M4 pg;  // column form
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) pg.m[i][j] = pr[j * 4 + i];
  M4 pg_inv;
  if (!Invert(pg, pg_inv)) return;
  // Level: the game camera's yaw only (no pitch / roll) unless keep_pitch. Game view axes in world: columns of
  // the row-vector view matrix.
  M4 level = M4::Identity();
  if (!Cfg().keep_pitch) {
    float ax[3][3];
    for (int a = 0; a < 3; ++a)
      for (int w = 0; w < 3; ++w) ax[a][w] = vr[w * 4 + a];
    float fwd[3] = {ax[2][0], 0.0f, ax[2][2]};
    const float n = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    if (n > 1e-4f) {
      fwd[0] /= n;
      fwd[2] /= n;
      const float up[3] = {0, 1, 0};
      const float right[3] = {fwd[2], 0.0f, -fwd[0]};  // up x fwd
      const float* lv[3] = {right, up, fwd};
      for (int i = 0; i < 3; ++i)
        for (int a = 0; a < 3; ++a)
          level.m[i][a] = lv[i][0] * ax[a][0] + lv[i][1] * ax[a][1] + lv[i][2] * ax[a][2];
    }
  }
  const float scale = Cfg().world_scale;
  const M4 base = Mul(level, pg_inv);
  M4 m[2];
  const float hand = pr[11] > 0.0f ? 1.0f : -1.0f;
  const bool lh = hand > 0.0f;
  if (RequestedMode() == Mode::Stereo) {
    for (int e = 0; e < 2; ++e)
      m[e] = Mul(ProjectionFor(views.fov[e], pg, hand), Mul(ViewOf(views.eye[e], scale, lh), base));
  } else {
    m[0] = m[1] = Mul(ProjectionFor(UnionFov(views), pg, hand), Mul(ViewOf(views.head, scale, lh), base));
  }
  for (int e = 0; e < 2; ++e) std::memcpy(c.clip[e], m[e].m, sizeof(c.clip[e]));
  if (f.count < kMaxCameras) {
    f.cams[f.count++] = c;
  } else {
    f.cams[f.next] = c;
    f.next = (f.next + 1) % kMaxCameras;
  }
  ++f.cameras;
  // The views the frame is rendered with go with its image (mono: the union FOV for both eyes).
  EyeViews rendered = views;
  if (RequestedMode() != Mode::Stereo) rendered.fov[0] = rendered.fov[1] = UnionFov(views);
  Xr::Get().SetRenderedViews(rendered);
  if (TraceFrame()) {
    Log("trace camera va %08X P00 %.4f P11 %.4f P22 %.6f P32 %.4f  view row0 %.3f %.3f %.3f  row3 %.1f %.1f %.1f",
        view_va, pr[0], pr[5], pr[10], pr[14], vr[0], vr[1], vr[2], vr[12], vr[13], vr[14]);
  }
}

namespace {
uint32_t g_mark_world = 0, g_mark_other = 0;
uint32_t g_draws = 0;
uint32_t g_list_draws[4] = {}, g_list_calls[4] = {};
uint32_t g_list_sig = ~0u;
}

uint32_t DrawCount() { return g_draws; }

void NoteList(int index, uint32_t list, uint32_t draws) {
  const int i = index < 0 ? 3 : index;
  g_list_draws[i] += draws;
  ++g_list_calls[i];
  (void)list;
}

// Render frame start: the 2D lists of the frame that ended, logged when which of them drew changes.
void FlushListStats() {
  uint32_t sig = 0;
  for (int i = 0; i < 4; ++i)
    if (g_list_draws[i]) sig |= 1u << i;
  if (sig != g_list_sig) {
    g_list_sig = sig;
    Log("2D lists now: +1A54 %u draws, +1A64 %u, +1A74 %u, other %u (%u calls); scene %s, hud %s", g_list_draws[0],
        g_list_draws[1], g_list_draws[2], g_list_draws[3], g_list_calls[3], SceneIs3d() ? "3D" : "flat",
        HudSeparate() ? "apart" : "in frame");
  }
  for (int i = 0; i < 4; ++i) g_list_draws[i] = g_list_calls[i] = 0;
}

void TraceMark(const char* what, uint32_t a, uint32_t b, uint32_t c) {
  GpuMarker(what);
  if (!TraceFrame()) return;
  Log("mark %-22s %08X %08X %08X | draws since: world %u other %u", what, a, b, c, g_mark_world, g_mark_other);
  g_mark_world = g_mark_other = 0;
}

void ClipForDraw(const void* vs_constants_be, float out[2][16]) {
  ++g_draws;
  std::memcpy(out[0], kIdentity, sizeof(kIdentity));
  std::memcpy(out[1], kIdentity, sizeof(kIdentity));
  if (!vs_constants_be || g_in_overlay) {
    ++f.other_draws;
    ++g_mark_other;
    return;
  }
  float key[32];
  LoadBE(static_cast<const uint8_t*>(vs_constants_be) + 24 * 16, key);
  LoadBE(static_cast<const uint8_t*>(vs_constants_be) + 28 * 16, key + 16);
  for (int i = 0; i < f.count; ++i) {
    const bool t = std::memcmp(f.cams[i].key, key, sizeof(key)) == 0;
    if (!t && std::memcmp(f.cams[i].key_raw, key, sizeof(key)) != 0) continue;
    static bool logged = false;
    if (!logged) {
      logged = true;
      Log("camera constants found in the draws (%s)", t ? "transposed" : "as stored");
    }
    ++f.world_draws;
    ++g_mark_world;
    if (!ClipActive()) return;  // counted for the scene kind, drawn as the game does
    std::memcpy(out, f.cams[i].clip, sizeof(f.cams[i].clip));
    return;
  }
  ++f.other_draws;
  ++g_mark_other;
  if (TraceFrame()) {
    static float seen[8][32];
    static int nseen = 0;
    static uint64_t frame = ~0ull;
    if (frame != FrameIndex()) {
      frame = FrameIndex();
      nseen = 0;
    }
    bool dup = false;
    for (int i = 0; i < nseen && !dup; ++i) dup = std::memcmp(seen[i], key, sizeof(key)) == 0;
    if (!dup && nseen < 8) {
      std::memcpy(seen[nseen++], key, sizeof(key));
      Log("trace non-world draw: c24 %.3f %.3f %.3f %.3f | c27 %.3f %.3f %.3f %.3f | c28 %.4f %.4f %.4f %.4f | "
          "c29 %.4f %.4f %.4f %.4f | c30 %.4f %.4f %.4f %.4f | c31 %.4f %.4f %.4f %.4f",
          key[0], key[1], key[2], key[3], key[12], key[13], key[14], key[15], key[16], key[17], key[18], key[19],
          key[20], key[21], key[22], key[23], key[24], key[25], key[26], key[27], key[28], key[29], key[30], key[31]);
    }
  }
}

}  // namespace bdvr
