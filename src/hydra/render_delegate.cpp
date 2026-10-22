/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_delegate.h"
#include "graph/node_enum.h"
#include "hydra/camera.h"
#include "hydra/curves.h"
#include "hydra/diagnostic.h"
#include "hydra/field.h"
#include "hydra/instancer.h"
#include "hydra/light.h"
#include "hydra/material.h"
#include "hydra/mesh.h"
#include "hydra/node_util.h"
#include "hydra/pointcloud.h"
#include "hydra/render_buffer.h"
#include "hydra/render_pass.h"
#include "hydra/session.h"
#include "hydra/volume.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/light.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "session/session.h"
#include "util/colorspace.h"

#include <pxr/base/tf/getenv.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hgi/tokens.h>

#include <algorithm>
#include <mutex>

HDCYCLES_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdCyclesRenderSettingsTokens, HD_CYCLES_RENDER_SETTINGS_TOKENS);

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (cycles)
    (mtlx)
    (openvdbAsset)
);
// clang-format on

namespace {

const TfTokenVector kSupportedRPrimTypes = {
    HdPrimTypeTokens->basisCurves,
    HdPrimTypeTokens->mesh,
    HdPrimTypeTokens->points,
#ifdef WITH_OPENVDB
    HdPrimTypeTokens->volume,
#endif
};

const TfTokenVector kSupportedSPrimTypes = {
    HdPrimTypeTokens->camera,
    HdPrimTypeTokens->material,
    HdPrimTypeTokens->diskLight,
    HdPrimTypeTokens->distantLight,
    HdPrimTypeTokens->domeLight,
    HdPrimTypeTokens->rectLight,
    HdPrimTypeTokens->sphereLight,
};

const TfTokenVector kSupportedBPrimTypes = {
    HdPrimTypeTokens->renderBuffer,
#ifdef WITH_OPENVDB
    _tokens->openvdbAsset,
#endif
};

// Integrator socket names that the delegate manages itself and must not be
// exposed or overwritten via SetRenderSetting.
const std::unordered_set<std::string> kIntegratorExcluded = {
    "aa_samples",
    "use_sample_subset",
    "sample_subset_offset",
    "sample_subset_length",
    "motion_blur",
};

// Film socket names exposed through cycles:film:<name>
const std::unordered_set<std::string> kFilmSocketsExposed = {
    "exposure",
    "pass_alpha_threshold",
    "filter_type",
    "filter_width",
    "use_approximate_shadow_catcher",
};

// Map SocketType to the USD type name string used in the manifest.
const char *SocketTypeToUsdTypeName(SocketType::Type type)
{
  switch (type) {
    case SocketType::BOOLEAN:
      return "bool";
    case SocketType::INT:
    case SocketType::UINT:
      return "int";
    case SocketType::FLOAT:
      return "float";
    case SocketType::ENUM:
      return "token";
    case SocketType::STRING:
      return "string";
    default:
      return nullptr;  // Not exposed in the manifest
  }
}

// Returns the default VtValue for a socket, converting enum int → TfToken.
VtValue GetSocketDefaultForManifest(const Node *node, const SocketType &socket)
{
  if (socket.type == SocketType::ENUM) {
    const int defaultInt = node->get_int(socket);
    if (socket.enum_values && socket.enum_values->exists(defaultInt)) {
      return VtValue(TfToken((*socket.enum_values)[defaultInt].c_str()));
    }
    return VtValue(TfToken(""));
  }
  return GetNodeValue(node, socket);
}

// Returns a VtStringArray of all enum value names.
VtStringArray GetSocketEnumTokens(const SocketType &socket)
{
  VtStringArray tokens;
  if (socket.enum_values) {
    for (auto it = socket.enum_values->begin(); it != socket.enum_values->end(); ++it) {
      tokens.push_back(it->first.c_str());
    }
    // Sort for deterministic order
    std::sort(tokens.begin(), tokens.end());
  }
  return tokens;
}

// Cycles' working colorspace is process-wide: every open delegate renders in the one set
// last. Tracked so that a delegate switching to a different one can be reported.
std::mutex g_colorSpaceMutex;
int g_liveDelegates = 0;
std::string g_activeColorSpace;

// renderingColorSpace is a token in UsdRenderSettings; accept a string as well.
std::string GetColorSpaceName(const VtValue &value)
{
  if (value.IsHolding<TfToken>()) {
    return value.UncheckedGet<TfToken>().GetString();
  }
  if (value.IsHolding<std::string>()) {
    return value.UncheckedGet<std::string>();
  }
  return std::string();
}

// One canonical descriptor for every non-integrator render setting.
// Integrator sockets are dynamic (from the node type) and handled separately.
struct HdCyclesSettingDesc {
  TfToken key;
  const char *label;
  const char *typeName;  // "bool"|"int"|"float"|"double"|"token"|"string"
  VtValue defaultVal;
  VtStringArray tokens;  // populated only for token-typed settings
  bool restart;          // true: requires a new Session to take effect
};

const std::vector<HdCyclesSettingDesc> &GetSettingsTable()
{
  // Static so we pay the construction cost only once; the token values are runtime
  // constants initialised before any delegate is created.
  static const std::vector<HdCyclesSettingDesc> kTable = {
      // Session scalars
      {HdCyclesRenderSettingsTokens->samples, "Sample Count", "int", VtValue(1024), {}, false},
      {HdCyclesRenderSettingsTokens->timeLimit, "Time Limit", "double", VtValue(0.0), {}, false},
      {HdCyclesRenderSettingsTokens->sampleOffset, "Sample Offset", "int", VtValue(0), {}, false},
      {HdCyclesRenderSettingsTokens->threads, "Threads", "int", VtValue(0), {}, true},
      {HdCyclesRenderSettingsTokens->device,
       "Device",
       "token",
       VtValue(TfToken("AUTO")),
       VtStringArray{"AUTO", "CPU", "METAL", "CUDA", "OPTIX", "HIP", "ONEAPI"},
       true},
      // Film sockets (hardcoded defaults match Cycles Film node defaults)
      {HdCyclesRenderSettingsTokens->filmExposure, "Exposure", "float", VtValue(1.0f), {}, false},
      {HdCyclesRenderSettingsTokens->filmPassAlphaThreshold,
       "Pass Alpha Threshold",
       "float",
       VtValue(0.5f),
       {},
       false},
      {HdCyclesRenderSettingsTokens->filmFilterType,
       "Filter Type",
       "token",
       VtValue(TfToken("box")),
       VtStringArray{"blackman_harris", "box", "gaussian"},
       false},
      {HdCyclesRenderSettingsTokens->filmFilterWidth,
       "Filter Width",
       "float",
       VtValue(1.0f),
       {},
       false},
      {HdCyclesRenderSettingsTokens->filmUseApproxShadowCatcher,
       "Use Approximate Shadow Catcher",
       "bool",
       VtValue(false),
       {},
       false},
      // Film transparent (custom, not a Film node socket)
      {HdCyclesRenderSettingsTokens->filmTransparent,
       "Film Transparent",
       "bool",
       VtValue(false),
       {},
       false},
      // Session restart params
      {HdCyclesRenderSettingsTokens->sessionUseAutoTile,
       "Use Auto Tile",
       "bool",
       VtValue(true),
       {},
       true},
      {HdCyclesRenderSettingsTokens->sessionTileSize, "Tile Size", "int", VtValue(2048), {}, true},
      // Scene restart params
      {HdCyclesRenderSettingsTokens->sceneUseBvhSpatialSplit,
       "Use BVH Spatial Split",
       "bool",
       VtValue(false),
       {},
       true},
      {HdCyclesRenderSettingsTokens->sceneUseTextureCache,
       "Use Texture Cache",
       "bool",
       VtValue(true),
       {},
       true},
      {HdCyclesRenderSettingsTokens->sceneAutoTextureCache,
       "Auto Texture Cache",
       "bool",
       VtValue(false),
       {},
       true},
      {HdCyclesRenderSettingsTokens->sceneTextureCachePath,
       "Texture Cache Path",
       "string",
       VtValue(std::string("")),
       {},
       true},
      {HdCyclesRenderSettingsTokens->sceneTextureLimit,
       "Texture Limit",
       "int",
       VtValue(0),
       {},
       true},
  };
  return kTable;
}
}  // namespace

// Construction helpers

std::pair<SessionParams, std::string> HdCyclesDelegate::BuildSessionParams() const
{
  SessionParams params;
  params.threads = 0;
  params.background = false;
  params.use_resolution_divider = false;
  std::string effectiveDeviceType;

  // Threads
  {
    auto it = _restartSettings.find(HdCyclesRenderSettingsTokens->threads);
    if (it != _restartSettings.end()) {
      params.threads = VtValue::Cast<int>(it->second).GetWithDefault(params.threads);
    }
  }

  // Tile settings
  params.use_auto_tile = true;
  params.tile_size = 2048;
  {
    auto it = _restartSettings.find(HdCyclesRenderSettingsTokens->sessionUseAutoTile);
    if (it != _restartSettings.end()) {
      params.use_auto_tile = VtValue::Cast<bool>(it->second).GetWithDefault(params.use_auto_tile);
    }
  }
  {
    auto it = _restartSettings.find(HdCyclesRenderSettingsTokens->sessionTileSize);
    if (it != _restartSettings.end()) {
      params.tile_size = VtValue::Cast<int>(it->second).GetWithDefault(params.tile_size);
    }
  }

  // Device selection:
  //   Priority: CYCLES_DEVICE env var > cycles:device setting > AUTO
  std::string deviceType;

  const std::string envDevice = TfGetenv("CYCLES_DEVICE");
  if (!envDevice.empty()) {
    deviceType = envDevice;
  }
  else {
    auto it = _restartSettings.find(HdCyclesRenderSettingsTokens->device);
    if (it != _restartSettings.end()) {
      const VtValue &v = it->second;
      if (v.IsHolding<TfToken>()) {
        deviceType = v.UncheckedGet<TfToken>().GetString();
      }
      else {
        deviceType = VtValue::Cast<std::string>(v).GetWithDefault(std::string());
      }
    }
  }

  // Normalise to upper-case for Device::type_from_string
  std::transform(deviceType.begin(), deviceType.end(), deviceType.begin(), ::toupper);

  if (deviceType.empty() || deviceType == "AUTO") {
    // AUTO: pick first type with available devices in priority order
    static const DeviceType kAutoOrder[] = {
        DEVICE_OPTIX, DEVICE_CUDA, DEVICE_HIP, DEVICE_METAL, DEVICE_ONEAPI, DEVICE_CPU};
    for (DeviceType type : kAutoOrder) {
      vector<DeviceInfo> devs = Device::available_devices(DEVICE_MASK(type));
      if (!devs.empty()) {
        params.device = Device::get_multi_device(devs, params.threads, params.background);
        effectiveDeviceType = Device::string_from_type(type);
        break;
      }
    }
    if (effectiveDeviceType.empty()) {
      effectiveDeviceType = Device::string_from_type(DEVICE_CPU);
    }
  }
  else {
    const DeviceType type = Device::type_from_string(deviceType.c_str());
    vector<DeviceInfo> devs = Device::available_devices(DEVICE_MASK(type));
    if (devs.empty()) {
      HDCYCLES_WARN("Cycles: device type '%s' has no available devices; falling back to CPU",
                    deviceType.c_str());
      devs = Device::available_devices(DEVICE_MASK_CPU);
      effectiveDeviceType = Device::string_from_type(DEVICE_CPU);
    }
    else {
      effectiveDeviceType = Device::string_from_type(type);
    }
    if (!devs.empty()) {
      params.device = Device::get_multi_device(devs, params.threads, params.background);
    }
  }

  params.denoise_device = params.device;
  return {params, effectiveDeviceType};
}

SceneParams HdCyclesDelegate::BuildSceneParams() const
{
  SceneParams params;

  {
    auto it = _restartSettings.find(HdCyclesRenderSettingsTokens->sceneUseBvhSpatialSplit);
    if (it != _restartSettings.end()) {
      params.use_bvh_spatial_split =
          VtValue::Cast<bool>(it->second).GetWithDefault(params.use_bvh_spatial_split);
    }
  }
  {
    auto it = _restartSettings.find(HdCyclesRenderSettingsTokens->sceneUseTextureCache);
    if (it != _restartSettings.end()) {
      params.use_texture_cache =
          VtValue::Cast<bool>(it->second).GetWithDefault(params.use_texture_cache);
    }
  }
  {
    auto it = _restartSettings.find(HdCyclesRenderSettingsTokens->sceneAutoTextureCache);
    if (it != _restartSettings.end()) {
      params.auto_texture_cache =
          VtValue::Cast<bool>(it->second).GetWithDefault(params.auto_texture_cache);
    }
  }
  {
    auto it = _restartSettings.find(HdCyclesRenderSettingsTokens->sceneTextureCachePath);
    if (it != _restartSettings.end()) {
      params.texture_cache_path =
          VtValue::Cast<std::string>(it->second).GetWithDefault(params.texture_cache_path);
    }
  }
  {
    auto it = _restartSettings.find(HdCyclesRenderSettingsTokens->sceneTextureLimit);
    if (it != _restartSettings.end()) {
      params.texture_limit = VtValue::Cast<int>(it->second).GetWithDefault(params.texture_limit);
    }
  }

  return params;
}

bool HdCyclesDelegate::CanRebuildSession() const
{
  return _renderParam->OwnsSession() && _realPrimCount == 0 && *_renderPassCount == 0;
}

static bool IsRestartOnlyKey(const TfToken &key)
{
  for (const HdCyclesSettingDesc &desc : GetSettingsTable()) {
    if (desc.key == key) {
      return desc.restart;
    }
  }
  return false;
}

// Constructor / destructor

HdCyclesDelegate::HdCyclesDelegate(const HdRenderSettingsMap &settingsMap,
                                   Session *session_,
                                   const bool keep_nodes)
    : HdRenderDelegate(), _renderPassCount(std::make_shared<int>(0))
{
  {
    const std::lock_guard<std::mutex> lock(g_colorSpaceMutex);
    ++g_liveDelegates;
  }

  // Cycles' working colorspace is process-wide, and the scene reads it when it is created, so
  // set it before the session. Without the setting, use the OCIO config's scene_linear role,
  // rather than whatever an earlier delegate left behind.
  const auto colorSpaceSetting = settingsMap.find(
      HdCyclesRenderSettingsTokens->renderingColorSpace);
  ApplyRenderingColorSpace(colorSpaceSetting != settingsMap.end() ?
                               GetColorSpaceName(colorSpaceSetting->second) :
                               std::string());

  if (session_) {
    // Wrapping an external session: the caller owns it and controls its params.
    // Never collect restart settings or trigger a rebuild.
    _renderParam = std::make_unique<HdCyclesSession>(session_, keep_nodes);
    _effectiveDeviceType = Device::string_from_type(session_->params.device.type);
  }
  else {
    // Collect restart-only settings before building the session so BuildSessionParams/
    // BuildSceneParams can use them.
    for (const auto &setting : settingsMap) {
      if (IsRestartOnlyKey(setting.first)) {
        _restartSettings[setting.first] = setting.second;
      }
    }
    const auto br = BuildSessionParams();
    _effectiveDeviceType = br.second;
    _renderParam = std::make_unique<HdCyclesSession>(br.first, BuildSceneParams());
  }

  for (const auto &setting : settingsMap) {
    // These have already been handled above (colorspace before session; restart settings
    // stored before session creation); skip them here.
    if (setting.first == HdCyclesRenderSettingsTokens->renderingColorSpace ||
        IsRestartOnlyKey(setting.first))
    {
      continue;
    }

    SetRenderSetting(setting.first, setting.second);
  }
}

void HdCyclesDelegate::ApplyRenderingColorSpace(std::string colorSpace)
{
  _requestedColorSpace = colorSpace;
  if (!CCL_NS::ColorSpaceManager::set_scene_linear_colorspace(colorSpace)) {
    HDCYCLES_WARN(
        "Rendering color space '%s' is not in the OCIO config, using its scene_linear role",
        colorSpace.c_str());
    colorSpace.clear();
    CCL_NS::ColorSpaceManager::set_scene_linear_colorspace(colorSpace);
  }
  _renderingColorSpace = std::move(colorSpace);

  const std::lock_guard<std::mutex> lock(g_colorSpaceMutex);
  if (g_liveDelegates > 1 && g_activeColorSpace != _renderingColorSpace) {
    HDCYCLES_WARN(
        "Cycles renderers with different rendering color spaces are open ('%s' and '%s'); "
        "all of them now render in '%s'",
        g_activeColorSpace.c_str(),
        _renderingColorSpace.c_str(),
        _renderingColorSpace.c_str());
  }
  g_activeColorSpace = _renderingColorSpace;
}

void HdCyclesDelegate::RebuildSession()
{
  const double stageMetersPerUnit = _renderParam->GetStageMetersPerUnit();

  // Colorspace must be set process-wide before the Session is created.
  ApplyRenderingColorSpace(_requestedColorSpace);

  const auto br = BuildSessionParams();
  _effectiveDeviceType = br.second;
  const SceneParams sceneParams = BuildSceneParams();

  auto newSession = std::make_unique<HdCyclesSession>(br.first, sceneParams);
  newSession->SetStageMetersPerUnit(stageMetersPerUnit);
  newSession->filmTransparent = _filmTransparent;

  _renderParam = std::move(newSession);

  // Replay all previously applied non-restart settings onto the fresh scene.
  for (const auto &[key, value] : _appliedSettings) {
    // Avoid infinite recursion: call internal apply directly.
    SetRenderSetting(key, value);
  }

  ++_settingsVersion;
}

HdCyclesDelegate::~HdCyclesDelegate()
{
  const std::lock_guard<std::mutex> lock(g_colorSpaceMutex);
  --g_liveDelegates;
}

// Driver / display

void HdCyclesDelegate::SetDrivers(const HdDriverVector &drivers)
{
  for (HdDriver *hdDriver : drivers) {
    if (hdDriver->name == HgiTokens->renderDriver && hdDriver->driver.IsHolding<Hgi *>()) {
      _hgi = hdDriver->driver.UncheckedGet<Hgi *>();
      break;
    }
  }
}

bool HdCyclesDelegate::IsDisplaySupported() const
{
#if defined(_WIN32) && defined(WITH_HYDRA_DISPLAY_DRIVER)
  return _hgi && _hgi->GetAPIName() == HgiTokens->OpenGL;
#else
  return false;
#endif
}

// Prim type lists

const TfTokenVector &HdCyclesDelegate::GetSupportedRprimTypes() const
{
  return kSupportedRPrimTypes;
}

const TfTokenVector &HdCyclesDelegate::GetSupportedSprimTypes() const
{
  return kSupportedSPrimTypes;
}

const TfTokenVector &HdCyclesDelegate::GetSupportedBprimTypes() const
{
  return kSupportedBPrimTypes;
}

// Render param

HdRenderParam *HdCyclesDelegate::GetRenderParam() const
{
  return _renderParam.get();
}

HdResourceRegistrySharedPtr HdCyclesDelegate::GetResourceRegistry() const
{
  return HdResourceRegistrySharedPtr();
}

// Pause / Resume

bool HdCyclesDelegate::IsPauseSupported() const
{
  return true;
}

bool HdCyclesDelegate::Pause()
{
  _renderParam->session->set_pause(true);
  return true;
}

bool HdCyclesDelegate::Resume()
{
  _renderParam->session->set_pause(false);
  return true;
}

// Prim creation / destruction  (tracking for rebuild window)

HdRenderPassSharedPtr HdCyclesDelegate::CreateRenderPass(HdRenderIndex *index,
                                                         const HdRprimCollection &collection)
{
  // The pass constructor increments *_renderPassCount; destructor decrements it.
  return HdRenderPassSharedPtr(
      new HdCyclesRenderPass(index, collection, _renderParam.get(), _renderPassCount));
}

HdInstancer *HdCyclesDelegate::CreateInstancer(HdSceneDelegate *delegate,
                                               const SdfPath &instancerId)
{
  return new HdCyclesInstancer(delegate, instancerId);
}

void HdCyclesDelegate::DestroyInstancer(HdInstancer *instancer)
{
  delete instancer;
}

HdRprim *HdCyclesDelegate::CreateRprim(const TfToken &typeId, const SdfPath &rprimId)
{
  HdRprim *prim = nullptr;
  if (typeId == HdPrimTypeTokens->mesh) {
    prim = new HdCyclesMesh(rprimId);
  }
  else if (typeId == HdPrimTypeTokens->basisCurves) {
    prim = new HdCyclesCurves(rprimId);
  }
  else if (typeId == HdPrimTypeTokens->points) {
    prim = new HdCyclesPoints(rprimId);
  }
#ifdef WITH_OPENVDB
  else if (typeId == HdPrimTypeTokens->volume) {
    prim = new HdCyclesVolume(rprimId);
  }
#endif
  else {
    TF_CODING_ERROR("Unknown Rprim type %s", typeId.GetText());
  }
  // Only count prims that were actually created; unknown type -> nullptr must not leak.
  if (prim) {
    ++_realPrimCount;
  }
  return prim;
}

void HdCyclesDelegate::DestroyRprim(HdRprim *rPrim)
{
  if (rPrim && _realPrimCount > 0) {
    --_realPrimCount;
  }
  delete rPrim;
}

HdSprim *HdCyclesDelegate::CreateSprim(const TfToken &typeId, const SdfPath &sprimId)
{
  // Cameras hold no Cycles scene data (the render pass applies them to the scene camera), and
  // Houdini's viewport creates one before it sends render settings, so they must not block a
  // session rebuild. Other non-fallback Sprims mark the end of the pre-prim window.
  if (typeId == HdPrimTypeTokens->camera) {
    return new HdCyclesCamera(sprimId);
  }
  if (!sprimId.IsEmpty()) {
    ++_realPrimCount;
  }

  if (typeId == HdPrimTypeTokens->material) {
    return new HdCyclesMaterial(sprimId);
  }
  if (typeId == HdPrimTypeTokens->diskLight || typeId == HdPrimTypeTokens->distantLight ||
      typeId == HdPrimTypeTokens->domeLight || typeId == HdPrimTypeTokens->rectLight ||
      typeId == HdPrimTypeTokens->sphereLight)
  {
    return new HdCyclesLight(sprimId, typeId);
  }

  TF_CODING_ERROR("Unknown Sprim type %s", typeId.GetText());
  return nullptr;
}

HdSprim *HdCyclesDelegate::CreateFallbackSprim(const TfToken &typeId)
{
  return CreateSprim(typeId, SdfPath::EmptyPath());
}

void HdCyclesDelegate::DestroySprim(HdSprim *sPrim)
{
  // Decrement only for the Sprims CreateSprim counted: non-fallback, non-camera.
  if (sPrim && !sPrim->GetId().IsEmpty() && !dynamic_cast<HdCyclesCamera *>(sPrim) &&
      _realPrimCount > 0)
  {
    --_realPrimCount;
  }
  delete sPrim;
}

HdBprim *HdCyclesDelegate::CreateBprim(const TfToken &typeId, const SdfPath &bprimId)
{
  // Render buffers hold no Cycles scene data: their AOV bindings live in the session and are
  // synced on the first render-pass execute. Houdini's viewport creates its render buffers
  // before it sends render settings, so they must not block a session rebuild.
  if (typeId == HdPrimTypeTokens->renderBuffer) {
    return new HdCyclesRenderBuffer(bprimId);
  }

  if (!bprimId.IsEmpty()) {
    ++_realPrimCount;
  }
#ifdef WITH_OPENVDB
  if (typeId == _tokens->openvdbAsset) {
    return new HdCyclesField(bprimId, typeId);
  }
#endif

  TF_CODING_ERROR("Unknown Bprim type %s", typeId.GetText());
  return nullptr;
}

HdBprim *HdCyclesDelegate::CreateFallbackBprim(const TfToken &typeId)
{
  return CreateBprim(typeId, SdfPath::EmptyPath());
}

void HdCyclesDelegate::DestroyBprim(HdBprim *bPrim)
{
  if (bPrim && !bPrim->GetId().IsEmpty() && !dynamic_cast<HdCyclesRenderBuffer *>(bPrim) &&
      _realPrimCount > 0)
  {
    --_realPrimCount;
  }
  delete bPrim;
}

// Scene commit

void HdCyclesDelegate::CommitResources(HdChangeTracker *tracker)
{
  _sceneStateVersion = tracker->GetSceneStateVersion();

  const SceneLock lock(_renderParam.get());

  _renderParam->UpdateScene();
}

// Material / context helpers

TfToken HdCyclesDelegate::GetMaterialBindingPurpose() const
{
  return HdTokens->full;
}

TfTokenVector HdCyclesDelegate::GetMaterialRenderContexts() const
{
  /* MaterialX networks are translated to Cycles nodes (hydra/materialx.cpp). */
  return {_tokens->cycles, _tokens->mtlx};
}

// Render stats / AOV

VtDictionary HdCyclesDelegate::GetRenderStats() const
{
  const Stats &stats = _renderParam->session->stats;
  const Progress &progress = _renderParam->session->progress;

  double totalTime;
  double renderTime;
  progress.get_time(totalTime, renderTime);
  const double fractionDone = progress.get_progress();

  std::string status;
  std::string substatus;
  progress.get_status(status, substatus);
  if (!substatus.empty()) {
    status += " | " + substatus;
  }

  // husk reads huskErrorStatus and exits non-zero when it is set.
  VtDictionary renderStats = {{"rendererName", VtValue("Cycles")},
                              {"rendererVersion", VtValue(GfVec3i(0, 0, 0))},
                              {"percentDone", VtValue(floor_to_int(fractionDone * 100))},
                              {"fractionDone", VtValue(fractionDone)},
                              {"loadClockTime", VtValue(totalTime - renderTime)},
                              {"peakMemory", VtValue(stats.mem_peak)},
                              {"totalClockTime", VtValue(totalTime)},
                              {"totalMemory", VtValue(stats.mem_used)},
                              {"renderProgressAnnotation", VtValue(status)},
                              {"huskErrorStatus", VtValue(progress.get_error() ? 1 : 0)}};

  // husk reads the stats for the image once the render is complete, when the scene is not
  // being updated: wait for it then, so the image never gets a missing or stale manifest.
  for (const auto &[key, value] : GetCryptomatteStats(fractionDone >= 1.0)) {
    renderStats[key] = value;
  }
  return renderStats;
}

VtDictionary HdCyclesDelegate::GetCryptomatteStats(const bool waitForScene) const
{
  // Cryptomatte metadata for each type being rendered, which UsdRenderers.json's
  // husk.metadata writes into the image as the cryptomatte specification names it. Types not
  // rendered publish nothing, so husk leaves their metadata out.
  const std::lock_guard cacheLock(_cryptomatteStatsMutex);
  Scene *const scene = _renderParam->session->scene.get();
  // The session holds the scene through whole updates, BVH builds among them, while hosts
  // poll the stats for progress.
  thread_scoped_lock sceneLock(scene->mutex, std::defer_lock);
  if (waitForScene) {
    sceneLock.lock();
  }
  else if (!sceneLock.try_lock()) {
    return _cryptomatteStats;
  }
  const int types = scene->film->get_cryptomatte_passes();
  const unsigned version = _sceneStateVersion;
  if (types == _cryptomatteStatsTypes && version == _cryptomatteStatsVersion) {
    return _cryptomatteStats;
  }

  _cryptomatteStats.clear();
  const auto publish = [this](const char *type, const char *layer, string manifest) {
    const std::string prefix = std::string("cryptomatte_") + type + "_";
    _cryptomatteStats[prefix + "name"] = VtValue(std::string(layer));
    _cryptomatteStats[prefix + "hash"] = VtValue(std::string("MurmurHash3_32"));
    _cryptomatteStats[prefix + "conversion"] = VtValue(std::string("uint32_to_float32"));
    _cryptomatteStats[prefix + "manifest"] = VtValue(std::move(manifest));
  };
  if (types & CRYPT_OBJECT) {
    publish("object", "CryptoObject", scene->object_manager->get_cryptomatte_objects(scene));
  }
  if (types & CRYPT_MATERIAL) {
    publish("material", "CryptoMaterial", scene->shader_manager->get_cryptomatte_materials(scene));
  }
  if (types & CRYPT_ASSET) {
    publish("asset", "CryptoAsset", scene->object_manager->get_cryptomatte_assets(scene));
  }
  _cryptomatteStatsTypes = types;
  _cryptomatteStatsVersion = version;
  return _cryptomatteStats;
}

HdAovDescriptor HdCyclesDelegate::GetDefaultAovDescriptor(const TfToken &name) const
{
  if (name == HdAovTokens->color) {
    HdFormat colorFormat = HdFormatFloat32Vec4;
    if (IsDisplaySupported()) {
      // Can use Cycles 'DisplayDriver' in OpenGL, but it only supports 'half4' format
      colorFormat = HdFormatFloat16Vec4;
    }

    return HdAovDescriptor(colorFormat, false, VtValue(GfVec4f(0.0f)));
  }
  if (name == HdAovTokens->depth) {
    return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.0f));
  }
  if (name == HdAovTokens->normal) {
    return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
  }
  if (name == HdAovTokens->primId || name == HdAovTokens->instanceId ||
      name == HdAovTokens->elementId)
  {
    return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
  }

  // Any other pass in the AOV table: float channels, as many as the Cycles pass has.
  const PassType type = HdCyclesPassTypeForAov(name);
  if (type == PASS_NONE) {
    return HdAovDescriptor();
  }
  switch (Pass::get_info(type).num_components) {
    case 1:
      return HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f));
    case 3:
      return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
    default:
      return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0f)));
  }
}

// Render settings descriptors

HdRenderSettingDescriptorList HdCyclesDelegate::GetRenderSettingDescriptors() const
{
  Scene *const scene = _renderParam->session->scene.get();
  HdRenderSettingDescriptorList descriptors;

  // Non-integrator settings from the canonical table.
  for (const HdCyclesSettingDesc &desc : GetSettingsTable()) {
    descriptors.push_back({desc.label, desc.key, desc.defaultVal});
  }

  // Integrator sockets are dynamic (from the node type).
  for (const SocketType &socket : scene->integrator->type->inputs) {
    if (kIntegratorExcluded.count(socket.name.string())) {
      continue;
    }
    if (!SocketTypeToUsdTypeName(socket.type)) {
      continue;
    }
    descriptors.push_back({socket.ui_name.string(),
                           TfToken("cycles:integrator:" + socket.name.string()),
                           GetSocketDefaultForManifest(scene->integrator, socket)});
  }

  return descriptors;
}

// SetRenderSetting

void HdCyclesDelegate::SetRenderSetting(const PXR_NS::TfToken &key, const PXR_NS::VtValue &value)
{
  if (IsRestartOnlyKey(key)) {
    _restartSettings[key] = value;
    if (CanRebuildSession()) {
      // A stage usually authors every restart-only setting, most of them at values the
      // current session already has; only rebuild (and recreate the device) when the Session
      // or Scene parameters actually change.
      const Session *session = _renderParam->session;
      if (BuildSessionParams().first.modified(session->params) ||
          BuildSceneParams().modified(session->scene->params))
      {
        RebuildSession();
      }
    }
    else {
      // Emit a warning once per key, then ignore.
      if (_warnedRestartOnly.insert(key).second) {
        HDCYCLES_WARN(
            "Cycles: setting '%s' requires a renderer restart to take effect; "
            "ignoring until the renderer is restarted (restartrendersettings).",
            key.GetText());
      }
    }
    return;
  }

  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (key == HdCyclesRenderSettingsTokens->stageMetersPerUnit) {
    _renderParam->SetStageMetersPerUnit(
        VtValue::Cast<double>(value).GetWithDefault(_renderParam->GetStageMetersPerUnit()));
    // No _settingsVersion bump; this is applied via scene units, not a render restart.
    return;
  }

  if (key == HdCyclesRenderSettingsTokens->timeLimit) {
    session->set_time_limit(
        VtValue::Cast<double>(value).GetWithDefault(session->params.time_limit));
    _appliedSettings[key] = value;
    return;
  }

  if (key == HdCyclesRenderSettingsTokens->samples) {
    static const int max_samples = Integrator::MAX_SAMPLES;
    int samples = VtValue::Cast<int>(value).GetWithDefault(session->params.samples);
    samples = std::min(std::max(1, samples), max_samples);
    session->set_samples(samples);
    _appliedSettings[key] = value;
    return;
  }

  if (key == HdCyclesRenderSettingsTokens->sampleOffset) {
    session->params.sample_subset_offset = VtValue::Cast<int>(value).GetWithDefault(
        session->params.sample_subset_offset);
    session->params.sample_subset_length = Integrator::MAX_SAMPLES;
    session->params.use_sample_subset = session->params.sample_subset_offset > 0;
    _appliedSettings[key] = value;
    ++_settingsVersion;
    return;
  }

  if (key == HdCyclesRenderSettingsTokens->renderingColorSpace) {
    // Houdini's viewport creates the delegate without settings and sets them before the first
    // sync, and restarts the renderer when this one changes later (restartrendersettings).
    // Images already converted to the old space would not be reloaded.
    std::string colorSpace = GetColorSpaceName(value);
    if (colorSpace != _requestedColorSpace) {
      // The render thread reads the colour matrices while it updates the scene.
      const thread_scoped_lock sceneLock(scene->mutex);
      ApplyRenderingColorSpace(std::move(colorSpace));
      scene->shader_manager->init_xyz_transforms();
      scene->shader_manager->tag_update(scene, ShaderManager::UPDATE_ALL);
      ++_settingsVersion;
    }
    // Note: not stored in _appliedSettings because colorspace is handled separately
    // in RebuildSession via ApplyRenderingColorSpace(_requestedColorSpace).
    return;
  }

  if (key == HdCyclesRenderSettingsTokens->filmTransparent) {
    const bool newVal = VtValue::Cast<bool>(value).GetWithDefault(_filmTransparent);
    if (newVal != _filmTransparent) {
      _filmTransparent = newVal;
      _renderParam->filmTransparent = newVal;
      // Tag the light manager dirty so that the next CommitResources->UpdateScene()
      // re-evaluates background transparency even when no light has changed.
      {
        const thread_scoped_lock sceneLock(scene->mutex);
        scene->light_manager->tag_update(scene, LightManager::LIGHT_MODIFIED);
      }
      _appliedSettings[key] = value;
      ++_settingsVersion;
    }
    return;
  }

  {
    const std::string &keyString = key.GetString();
    if (keyString.rfind("cycles:integrator:", 0) == 0) {
      const ustring socketName(keyString, sizeof("cycles:integrator:") - 1);
      if (const SocketType *socket = scene->integrator->type->find_input(socketName)) {
        if (!kIntegratorExcluded.count(socketName.string())) {
          SetNodeValue(scene->integrator, *socket, value);
          _appliedSettings[key] = value;
          ++_settingsVersion;
        }
      }
      return;
    }
  }

  {
    const std::string &keyString = key.GetString();
    if (keyString.rfind("cycles:film:", 0) == 0) {
      const ustring socketName(keyString, sizeof("cycles:film:") - 1);
      if (kFilmSocketsExposed.count(socketName.string())) {
        if (const SocketType *socket = scene->film->type->find_input(socketName)) {
          SetNodeValue(scene->film, *socket, value);
          scene->film->tag_modified();
          _appliedSettings[key] = value;
          ++_settingsVersion;
        }
      }
      return;
    }
  }
}

// GetRenderSetting

VtValue HdCyclesDelegate::GetRenderSetting(const TfToken &key) const
{
  if (key == HdCyclesRenderSettingsTokens->settingsManifest) {
    Scene *const scene = _renderParam->session->scene.get();
    VtDictionary manifest;

    // Non-integrator settings from the canonical table.
    for (const HdCyclesSettingDesc &desc : GetSettingsTable()) {
      VtDictionary entry;
      entry["label"] = VtValue(std::string(desc.label));
      entry["type"] = VtValue(std::string(desc.typeName));
      entry["default"] = desc.defaultVal;
      entry["restart"] = VtValue(desc.restart);
      if (!desc.tokens.empty()) {
        entry["tokens"] = VtValue(desc.tokens);
      }
      manifest[desc.key.GetString()] = VtValue(entry);
    }

    // Integrator sockets are dynamic (from the node type).
    for (const SocketType &socket : scene->integrator->type->inputs) {
      if (kIntegratorExcluded.count(socket.name.string())) {
        continue;
      }
      const char *typeStr = SocketTypeToUsdTypeName(socket.type);
      if (!typeStr) {
        continue;
      }
      VtDictionary entry;
      entry["label"] = VtValue(socket.ui_name.string());
      entry["type"] = VtValue(std::string(typeStr));
      entry["default"] = GetSocketDefaultForManifest(scene->integrator, socket);
      entry["restart"] = VtValue(false);
      if (socket.type == SocketType::ENUM) {
        entry["tokens"] = VtValue(GetSocketEnumTokens(socket));
      }
      manifest["cycles:integrator:" + socket.name.string()] = VtValue(entry);
    }

    return VtValue(manifest);
  }

  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (key == HdCyclesRenderSettingsTokens->stageMetersPerUnit) {
    return VtValue(_renderParam->GetStageMetersPerUnit());
  }
  if (key == HdCyclesRenderSettingsTokens->device) {
    // Return the type actually in use, not what was requested.
    return VtValue(TfToken(_effectiveDeviceType.empty() ?
                               Device::string_from_type(session->params.device.type) :
                               _effectiveDeviceType));
  }
  if (key == HdCyclesRenderSettingsTokens->threads) {
    return VtValue(session->params.threads);
  }
  if (key == HdCyclesRenderSettingsTokens->timeLimit) {
    return VtValue(session->params.time_limit);
  }
  if (key == HdCyclesRenderSettingsTokens->samples) {
    return VtValue(session->params.samples);
  }
  if (key == HdCyclesRenderSettingsTokens->sampleOffset)
    return VtValue((session->params.use_sample_subset) ? session->params.sample_subset_offset : 0);
  if (key == HdCyclesRenderSettingsTokens->renderingColorSpace) {
    return _renderingColorSpace.empty() ? VtValue() : VtValue(TfToken(_renderingColorSpace));
  }
  if (key == HdCyclesRenderSettingsTokens->filmTransparent) {
    return VtValue(_filmTransparent);
  }
  if (key == HdCyclesRenderSettingsTokens->sessionUseAutoTile) {
    return VtValue(session->params.use_auto_tile);
  }
  if (key == HdCyclesRenderSettingsTokens->sessionTileSize) {
    return VtValue(session->params.tile_size);
  }
  if (key == HdCyclesRenderSettingsTokens->sceneUseBvhSpatialSplit) {
    return VtValue(scene->params.use_bvh_spatial_split);
  }
  if (key == HdCyclesRenderSettingsTokens->sceneUseTextureCache) {
    return VtValue(scene->params.use_texture_cache);
  }
  if (key == HdCyclesRenderSettingsTokens->sceneAutoTextureCache) {
    return VtValue(scene->params.auto_texture_cache);
  }
  if (key == HdCyclesRenderSettingsTokens->sceneTextureCachePath) {
    return VtValue(scene->params.texture_cache_path);
  }
  if (key == HdCyclesRenderSettingsTokens->sceneTextureLimit) {
    return VtValue(scene->params.texture_limit);
  }

  // cycles:integrator:<socket>
  {
    const std::string &keyString = key.GetString();
    if (keyString.rfind("cycles:integrator:", 0) == 0) {
      const ustring socketName(keyString, sizeof("cycles:integrator:") - 1);
      if (const SocketType *socket = scene->integrator->type->find_input(socketName)) {
        if (socket->type == SocketType::ENUM) {
          // Return enum name as TfToken rather than raw int.
          const int enumInt = scene->integrator->get_int(*socket);
          if (socket->enum_values && socket->enum_values->exists(enumInt)) {
            return VtValue(TfToken((*socket->enum_values)[enumInt].c_str()));
          }
        }
        return GetNodeValue(scene->integrator, *socket);
      }
    }
  }

  // cycles:film:<socket>
  {
    const std::string &keyString = key.GetString();
    if (keyString.rfind("cycles:film:", 0) == 0) {
      const ustring socketName(keyString, sizeof("cycles:film:") - 1);
      if (kFilmSocketsExposed.count(socketName.string())) {
        if (const SocketType *socket = scene->film->type->find_input(socketName)) {
          if (socket->type == SocketType::ENUM) {
            const int enumInt = scene->film->get_int(*socket);
            if (socket->enum_values && socket->enum_values->exists(enumInt)) {
              return VtValue(TfToken((*socket->enum_values)[enumInt].c_str()));
            }
          }
          return GetNodeValue(scene->film, *socket);
        }
      }
    }
  }

  return VtValue();
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
