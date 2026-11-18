// Blue Dragon VR: glue between re:Blue's Plume D3D12 renderer and the OpenXR module (bdvr.h, bdvr_xr.h).
//
// Stereo: the frame is recorded once into its D3D12 command list, through a forwarding list that remembers each
// resource's state before the frame. At the present the list runs twice on the queue: VrEye = 0, the left eye's
// copy to OpenXR, barriers back to the recorded states, VrEye = 1, then the renderer's own submit (right eye) and
// the right eye's copy.
#include "bdvr.h"

#ifdef _WIN32
#include "bdvr_xr.h"

#include <plume_d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdvr {

bool ClipActive();

using namespace plume;

namespace {

class StereoCommandList final : public RenderCommandList {
 public:
  void Open(RenderCommandList* target) {
    t_ = target;
    textures_.clear();
    buffers_.clear();
  }
  RenderCommandList* Target() const { return t_; }

  void RestoreBarriers(std::vector<D3D12_RESOURCE_BARRIER>& out) const {
    out.clear();
    for (const auto& [texture, before] : textures_) {
      if (texture->resourceStates == before) continue;
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition = {texture->d3d, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, texture->resourceStates, before};
      out.push_back(b);
    }
    for (const auto& [buffer, before] : buffers_) {
      if (buffer->resourceStates == before) continue;
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition = {buffer->d3d, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, buffer->resourceStates, before};
      out.push_back(b);
    }
  }

  void barriers(RenderBarrierStages stages, const RenderBufferBarrier* bufferBarriers, uint32_t bufferBarriersCount,
                const RenderTextureBarrier* textureBarriers, uint32_t textureBarriersCount) override {
    for (uint32_t i = 0; i < textureBarriersCount; ++i) {
      auto* t = static_cast<D3D12Texture*>(const_cast<RenderTexture*>(textureBarriers[i].texture));
      if (t != nullptr) textures_.try_emplace(t, t->resourceStates);
    }
    for (uint32_t i = 0; i < bufferBarriersCount; ++i) {
      auto* b = static_cast<D3D12Buffer*>(const_cast<RenderBuffer*>(bufferBarriers[i].buffer));
      if (b != nullptr) buffers_.try_emplace(b, b->resourceStates);
    }
    t_->barriers(stages, bufferBarriers, bufferBarriersCount, textureBarriers, textureBarriersCount);
  }
  void begin() override { t_->begin(); }
  void end() override { t_->end(); }
  void dispatch(uint32_t x, uint32_t y, uint32_t z) override { t_->dispatch(x, y, z); }
  void traceRays(uint32_t width, uint32_t height, uint32_t depth, RenderBufferReference shaderBindingTable, const RenderShaderBindingGroupsInfo &shaderBindingGroupsInfo) override { t_->traceRays(width, height, depth, shaderBindingTable, shaderBindingGroupsInfo); }
  void drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation) override { t_->drawInstanced(vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation); }
  void drawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation) override { t_->drawIndexedInstanced(indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation); }
  void setPipeline(const RenderPipeline *pipeline) override { t_->setPipeline(pipeline); }
  void setComputePipelineLayout(const RenderPipelineLayout *pipelineLayout) override { t_->setComputePipelineLayout(pipelineLayout); }
  void setComputePushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override { t_->setComputePushConstants(rangeIndex, data, offset, size); }
  void setComputeDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override { t_->setComputeDescriptorSet(descriptorSet, setIndex); }
  void setGraphicsPipelineLayout(const RenderPipelineLayout *pipelineLayout) override { t_->setGraphicsPipelineLayout(pipelineLayout); }
  void setGraphicsPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override { t_->setGraphicsPushConstants(rangeIndex, data, offset, size); }
  void setGraphicsDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override { t_->setGraphicsDescriptorSet(descriptorSet, setIndex); }
  void setGraphicsRootDescriptor(RenderBufferReference bufferReference, uint32_t rootDescriptorIndex) override { t_->setGraphicsRootDescriptor(bufferReference, rootDescriptorIndex); }
  void setRaytracingPipelineLayout(const RenderPipelineLayout *pipelineLayout) override { t_->setRaytracingPipelineLayout(pipelineLayout); }
  void setRaytracingPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override { t_->setRaytracingPushConstants(rangeIndex, data, offset, size); }
  void setRaytracingDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override { t_->setRaytracingDescriptorSet(descriptorSet, setIndex); }
  void setIndexBuffer(const RenderIndexBufferView *view) override { t_->setIndexBuffer(view); }
  void setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView *views, uint32_t viewCount, const RenderInputSlot *inputSlots) override { t_->setVertexBuffers(startSlot, views, viewCount, inputSlots); }
  void setViewports(const RenderViewport *viewports, uint32_t count) override { t_->setViewports(viewports, count); }
  void setScissors(const RenderRect *scissorRects, uint32_t count) override { t_->setScissors(scissorRects, count); }
  void setFramebuffer(const RenderFramebuffer *framebuffer) override { t_->setFramebuffer(framebuffer); }
  void setDepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias) override { t_->setDepthBias(depthBias, depthBiasClamp, slopeScaledDepthBias); }
  void clearColor(uint32_t attachmentIndex, RenderColor colorValue, const RenderRect *clearRects, uint32_t clearRectsCount) override { t_->clearColor(attachmentIndex, colorValue, clearRects, clearRectsCount); }
  void clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue, uint32_t stencilValue, const RenderRect *clearRects, uint32_t clearRectsCount) override { t_->clearDepthStencil(clearDepth, clearStencil, depthValue, stencilValue, clearRects, clearRectsCount); }
  void copyBufferRegion(RenderBufferReference dstBuffer, RenderBufferReference srcBuffer, uint64_t size) override { t_->copyBufferRegion(dstBuffer, srcBuffer, size); }
  void copyTextureRegion(const RenderTextureCopyLocation &dstLocation, const RenderTextureCopyLocation &srcLocation, uint32_t dstX, uint32_t dstY, uint32_t dstZ, const RenderBox *srcBox) override { t_->copyTextureRegion(dstLocation, srcLocation, dstX, dstY, dstZ, srcBox); }
  void copyBuffer(const RenderBuffer *dstBuffer, const RenderBuffer *srcBuffer) override { t_->copyBuffer(dstBuffer, srcBuffer); }
  void copyTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) override { t_->copyTexture(dstTexture, srcTexture); }
  void resolveTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) override { t_->resolveTexture(dstTexture, srcTexture); }
  void resolveTextureRegion(const RenderTexture *dstTexture, uint32_t dstX, uint32_t dstY, const RenderTexture *srcTexture, const RenderRect *srcRect, RenderResolveMode resolveMode) override { t_->resolveTextureRegion(dstTexture, dstX, dstY, srcTexture, srcRect, resolveMode); }
  void buildBottomLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, const RenderBottomLevelASBuildInfo &buildInfo) override { t_->buildBottomLevelAS(dstAccelerationStructure, scratchBuffer, buildInfo); }
  void buildTopLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, RenderBufferReference instancesBuffer, const RenderTopLevelASBuildInfo &buildInfo) override { t_->buildTopLevelAS(dstAccelerationStructure, scratchBuffer, instancesBuffer, buildInfo); }
  void discardTexture(const RenderTexture* texture) override { t_->discardTexture(texture); }
  void resetQueryPool(const RenderQueryPool *queryPool, uint32_t queryFirstIndex, uint32_t queryCount) override { t_->resetQueryPool(queryPool, queryFirstIndex, queryCount); }
  void writeTimestamp(const RenderQueryPool *queryPool, uint32_t queryIndex) override { t_->writeTimestamp(queryPool, queryIndex); }

 private:
  RenderCommandList* t_ = nullptr;
  std::unordered_map<D3D12Texture*, D3D12_RESOURCE_STATES> textures_;
  std::unordered_map<D3D12Buffer*, D3D12_RESOURCE_STATES> buffers_;
};

struct GlueState {
  RenderDevice* device = nullptr;
  std::unique_ptr<RenderBuffer> eye_buffer;  // VrEye (b4): default heap, flipped by a GPU copy between eye passes
  std::unique_ptr<RenderBuffer> zero_buffer; // VrEye when VR is off: an upload buffer of zeros
  StereoCommandList proxy;
  RenderCommandList* current = nullptr;  // the frame list being recorded (for GPU markers)
  bool stereo_frame = false;
  int lists_this_frame = 0;
  std::vector<D3D12_RESOURCE_BARRIER> restore;
  uint64_t stereo_frames = 0, skipped = 0;
  std::atomic<uint64_t> frame_index{0};
} g;

// GPU stage markers. Each marker writes (frame, sequence, name id) into a CPU-readable buffer twice: MARKER_IN (all
// earlier work has started) and MARKER_OUT (all earlier work has finished). After a device loss the two values say
// which stage was running when the GPU hung; GpuMarkerReport decodes them with the names recorded on the CPU.
namespace {
struct Markers {
  ID3D12Resource* buffer = nullptr;
  const volatile uint32_t* mapped = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS va = 0;
  std::vector<std::string> names;                // id -> name
  std::unordered_map<std::string, uint8_t> ids;  // name -> id
  static constexpr uint32_t kFrames = 8, kPerFrame = 256;
  uint8_t seq_ids[kFrames][kPerFrame] = {};      // per recorded frame: sequence -> name id
  uint32_t seq_count[kFrames] = {};
  uint32_t frame = 0, seq = 0;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12GraphicsCommandList2* list2 = nullptr;
} m;

void CreateMarkerBuffer(RenderDevice* device) {
  auto* dev = static_cast<D3D12Device*>(device)->d3d;
  if (!dev) return;
  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = 256;
  rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&m.buffer))))
    return;
  void* p = nullptr;
  D3D12_RANGE r{0, 256};
  if (FAILED(m.buffer->Map(0, &r, &p))) return;
  std::memset(p, 0, 256);
  m.mapped = static_cast<const volatile uint32_t*>(p);
  m.va = m.buffer->GetGPUVirtualAddress();
  m.names.push_back("(none)");
}
}  // namespace


bool StereoActive() { return RequestedMode() == Mode::Stereo && ClipActive(); }

std::atomic<uint64_t> g_trace_frame{~0ull};

}  // namespace

void RequestTrace() { g_trace_frame = g.frame_index.load() + 1; }
bool TraceFrame() { return g.frame_index.load(std::memory_order_relaxed) == g_trace_frame.load(std::memory_order_relaxed); }
uint64_t FrameIndex() { return g.frame_index.load(std::memory_order_relaxed); }

bool VrRequested() { return Requested(); }

bool Active() { return Xr::Get().Active(); }

double SettingGet(Setting id) {
  Settings& t = Cfg();
  switch (id) {
    case Setting::Mode: {
      const Mode m = RequestedMode();
      return m == Mode::Mono ? 1.0 : m == Mode::Theater ? 2.0 : 0.0;
    }
    case Setting::Resolution: return t.resolution;
    case Setting::WorldScale: return t.world_scale.load();
    case Setting::KeepPitch: return t.keep_pitch ? 1.0 : 0.0;
    case Setting::Hud: return t.hud ? 1.0 : 0.0;
    case Setting::HudFollow: return t.hud_follow ? 1.0 : 0.0;
    case Setting::HudDistance: return t.hud_distance.load();
    case Setting::HudWidth: return t.hud_width.load();
    case Setting::HudHeight: return t.hud_height.load();
    case Setting::ScreenDistance: return t.screen_distance.load();
    case Setting::ScreenWidth: return t.screen_width.load();
    case Setting::CullWiden: return t.cull_widen.load();
    case Setting::Supersampling: return t.supersampling;
    case Setting::Recenter: return 0.0;
  }
  return 0.0;
}

bool SettingSet(Setting id, double v) {
  Settings& t = Cfg();
  switch (id) {
    case Setting::Mode: SetMode(v >= 1.5 ? Mode::Theater : v >= 0.5 ? Mode::Mono : Mode::Stereo); break;
    case Setting::Resolution: t.resolution = int(v + 0.5); break;  // applies at the next start
    case Setting::WorldScale: t.world_scale = float(v); break;
    case Setting::KeepPitch: t.keep_pitch = v != 0.0; break;
    case Setting::Hud: t.hud = v != 0.0; break;
    case Setting::HudFollow: t.hud_follow = v != 0.0; break;
    case Setting::HudDistance: t.hud_distance = float(v); break;
    case Setting::HudWidth: t.hud_width = float(v); break;
    case Setting::HudHeight: t.hud_height = float(v); break;
    case Setting::ScreenDistance: t.screen_distance = float(v); break;
    case Setting::ScreenWidth: t.screen_width = float(v); break;
    case Setting::CullWiden: t.cull_widen = float(v); break;
    case Setting::Supersampling: t.supersampling = int(v + 0.5); break;
    case Setting::Recenter: Xr::Get().RequestRecenter(); return true;
  }
  SaveConfig();
  return true;
}

int SuperSampling() {
  if (!Xr::Get().Active()) return 0;
  return std::clamp(Cfg().supersampling, 1, 4);
}

int PacingFps() {
  if (!Xr::Get().Active()) return 0;
  const double hz = Xr::Get().DisplayHz();
  return hz > 0.0 ? int(hz + 0.5) : 90;
}

void InitFromPlume(RenderDevice* device, RenderCommandQueue* queue) {
  g.device = device;
  CreateMarkerBuffer(device);
  // VrEye must always be bound (every camera shader reads it): zeros unless stereo flips it.
  g.zero_buffer = device->createBuffer(RenderBufferDesc::UploadBuffer(256, RenderBufferFlag::CONSTANT));
  if (g.zero_buffer) {
    if (void* p = g.zero_buffer->map()) {
      std::memset(p, 0, 256);
      g.zero_buffer->unmap();
    }
  }
  if (!Requested()) return;
  // First start: write vr_config.txt with the defaults, so the settings are there to find and edit.
  if (!Cfg().from_file && !std::getenv("BD_VR")) {
    SaveConfig();
    Log("vr_config.txt written with the defaults");
  }
  auto* d = static_cast<D3D12Device*>(device);
  auto* q = static_cast<D3D12CommandQueue*>(queue);
  if (!Xr::Get().Initialize(d->d3d, q->d3d)) {
    Log("VR did not start (see above)");
    return;
  }
  g.eye_buffer = device->createBuffer(RenderBufferDesc::DefaultBuffer(256, RenderBufferFlag::CONSTANT));
  if (!g.eye_buffer) Log("eye buffer could not be created: no stereo");
}

bool RenderSize(uint32_t* width, uint32_t* height) {
  if (!Xr::Get().Active()) return false;
  uint32_t w = 0, h = 0;
  Xr::Get().RecommendedEyeSize(w, h);
  if (w == 0 || h == 0) return false;
  const double pct = std::clamp(Cfg().resolution, 20, 300) / 100.0;
  *width = (uint32_t(w * pct + 0.5) + 7) & ~7u;
  *height = (uint32_t(h * pct + 0.5) + 7) & ~7u;
  return true;
}

RenderCommandList* WrapCommands(RenderCommandList* list) {
  ++g.lists_this_frame;
  g.current = list;
  ++m.frame;
  m.seq = 0;
  g.stereo_frame = StereoActive() && g.eye_buffer != nullptr;
  if (!g.stereo_frame) return list;
  g.proxy.Open(list);
  return &g.proxy;
}

void GpuMarker(const char* what) {
  auto* list = static_cast<D3D12CommandList*>(g.current);
  if (list == nullptr || list->d3d == nullptr || what == nullptr || !m.mapped) return;
  if (list->d3d != m.list) {
    if (m.list2) m.list2->Release();
    m.list2 = nullptr;
    m.list = list->d3d;
    m.list->QueryInterface(IID_PPV_ARGS(&m.list2));
  }
  if (!m.list2) return;
  uint8_t id = 0;
  auto it = m.ids.find(what);
  if (it != m.ids.end()) {
    id = it->second;
  } else if (m.names.size() < 255) {
    id = uint8_t(m.names.size());
    m.names.emplace_back(what);
    m.ids.emplace(what, id);
  }
  const uint32_t f = m.frame % Markers::kFrames;
  if (m.seq < Markers::kPerFrame) {
    m.seq_ids[f][m.seq] = id;
    m.seq_count[f] = m.seq + 1;
  }
  const uint32_t value = ((m.frame & 0xFFF) << 20) | ((m.seq & 0xFFF) << 8) | id;
  ++m.seq;
  D3D12_WRITEBUFFERIMMEDIATE_PARAMETER prm[2] = {{m.va + 0, value}, {m.va + 4, value}};
  D3D12_WRITEBUFFERIMMEDIATE_MODE modes[2] = {D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
                                              D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT};
  m.list2->WriteBufferImmediate(2, prm, modes);
}

std::string GpuMarkerReport() {
  if (!m.mapped) return "VR GPU markers: none";
  auto describe = [](uint32_t v) {
    const uint32_t frame = v >> 20, seq = (v >> 8) & 0xFFF, id = v & 0xFF;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "frame %u #%u '%s'", frame, seq,
                  id < m.names.size() ? m.names[id].c_str() : "?");
    return std::string(buf);
  };
  const uint32_t in = m.mapped[0], out = m.mapped[1];
  std::string r = "VR GPU markers: all work before " + describe(out) + " finished; all work before " +
                  describe(in) + " started.";
  // The stages after the last finished marker, from the CPU record of that frame.
  const uint32_t frame = out >> 20, seq = (out >> 8) & 0xFFF;
  uint32_t slot = Markers::kFrames;
  for (uint32_t f = 0; f < Markers::kFrames; ++f)
    if (((m.frame - f) & 0xFFF) == frame) slot = (m.frame - f) % Markers::kFrames;
  if (slot < Markers::kFrames) {
    r += " Stages from there:";
    for (uint32_t i = seq; i < m.seq_count[slot] && i < seq + 8; ++i) {
      const uint8_t id = m.seq_ids[slot][i];
      r += " [#" + std::to_string(i) + " " + (id < m.names.size() ? m.names[id] : "?") + "]";
    }
  }
  return r;
}

void BindEye(RenderCommandList* list) {
  RenderBuffer* b = g.stereo_frame ? g.eye_buffer.get() : g.zero_buffer.get();
  if (b) list->setGraphicsRootDescriptor(RenderBufferReference(b), 4);
}

void BeforeFrameSubmit(RenderCommandList* list, RenderTexture* image, uint32_t width, uint32_t height, float gamma) {
  g.current = nullptr;  // the frame list is closed now: no markers until the next one opens
  Xr::Get().SetSourceGamma(gamma);
  if (!g.stereo_frame) return;
  const bool whole_frame = g.lists_this_frame == 1;
  if (!whole_frame || image == nullptr || g.proxy.Target() != list) {
    ++g.skipped;
    return;
  }
  g.proxy.RestoreBarriers(g.restore);
  auto* img = static_cast<D3D12Texture*>(image);
  auto* frame_list = static_cast<D3D12CommandList*>(list);
  auto* eye_buffer = static_cast<D3D12Buffer*>(g.eye_buffer.get());
  Xr::Get().RunFirstEye(frame_list->d3d, g.restore.data(), uint32_t(g.restore.size()), eye_buffer->d3d, img->d3d,
                        width, height);
  if (++g.stereo_frames % 600 == 1)
    Log("stereo frames %llu (skipped %llu), restore barriers %zu", (unsigned long long)g.stereo_frames,
        (unsigned long long)g.skipped, g.restore.size());
}

void AfterFrameSubmit(RenderTexture* image, uint32_t width, uint32_t height, float gamma, RenderTexture* hud_black,
                      RenderTexture* hud_white) {
  const bool stereo = g.stereo_frame && g.lists_this_frame == 1;
  g.lists_this_frame = 0;
  g.stereo_frame = false;
  if (Xr::Get().Active() && image != nullptr) {
    Xr::Get().SetSourceGamma(gamma);
    // A frame the world camera was not re-aimed for (title, menus without the field, videos) goes to the flat
    // screen: no rendered views with it.
    if (!ClipActive()) Xr::Get().SetRenderedViews(EyeViews{});
    auto* t = static_cast<D3D12Texture*>(image);
    Xr::Get().SubmitFrame(t->d3d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, width, height, stereo ? 1 : -1);
    if (hud_black && hud_white && width && height) {
      // The 2D layer is laid out in the largest centred 16:9 rectangle of the frame.
      uint32_t w = width, h = width * 9 / 16;
      if (h > height) {
        h = height;
        w = height * 16 / 9;
      }
      Xr::Get().SubmitFrame(static_cast<D3D12Texture*>(hud_black)->d3d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                            w, h, 2, (width - w) / 2, (height - h) / 2,
                            static_cast<D3D12Texture*>(hud_white)->d3d);
    }
  }
  g.frame_index.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace bdvr

#else

namespace bdvr {
bool VrRequested() { return false; }
void InitFromPlume(plume::RenderDevice*, plume::RenderCommandQueue*) {}
bool RenderSize(uint32_t*, uint32_t*) { return false; }
void OnRenderFrameBegin() {}
void OnCamera(uint32_t, const void*, const void*) {}
void WidenProjection(double*, double*) {}
void ClipForDraw(const void*, float out[2][16]) {
  for (int e = 0; e < 2; ++e)
    for (int i = 0; i < 16; ++i) out[e][i] = (i % 5) == 0 ? 1.0f : 0.0f;
}
plume::RenderCommandList* WrapCommands(plume::RenderCommandList* list) { return list; }
void BindEye(plume::RenderCommandList*) {}
void GpuMarker(const char*) {}
std::string GpuMarkerReport() { return {}; }
void BeforeFrameSubmit(plume::RenderCommandList*, plume::RenderTexture*, uint32_t, uint32_t, float) {}
void AfterFrameSubmit(plume::RenderTexture*, uint32_t, uint32_t, float, plume::RenderTexture*,
                      plume::RenderTexture*) {}
uint64_t FrameIndex() { return 0; }
int PacingFps() { return 0; }
int SuperSampling() { return 0; }
bool Active() { return false; }
double SettingGet(Setting) { return 0.0; }
bool SettingSet(Setting, double) { return false; }
bool TraceFrame() { return false; }
void RequestTrace() {}
void Log(const char*, ...) {}
}  // namespace bdvr

#endif
