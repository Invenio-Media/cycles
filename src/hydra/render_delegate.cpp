/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_delegate.h"
#include "hydra/camera.h"
#include "hydra/curves.h"
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
#include "graph/node_enum.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "session/session.h"
#include "util/colorspace.h"

#include <pxr/base/tf/getenv.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hgi/tokens.h>

#include <mutex>
#include <algorithm>

HDCYCLES_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdCyclesRenderSettingsTokens, HD_CYCLES_RENDER_SETTINGS_TOKENS);

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (cycles)
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

// Map SocketType to the USD type name used in the manifest.
const char *SocketTypeToUsdTypeName(SocketType::Type type)
{
  switch (type) {
    case SocketType::BOOLEAN:  return "bool";
    case SocketType::INT:
    case SocketType::UINT:     return "int";
    case SocketType::FLOAT:    return "float";
    case SocketType::ENUM:     return "token";
    case SocketType::STRING:   return "string";
    default:                   return nullptr;  // not exposed
  }
}

// Returns the default VtValue for a socket, converting ENUM int → TfToken.
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

// Returns a sorted VtStringArray of all names in an ENUM socket.
VtStringArray GetSocketEnumTokens(const SocketType &socket)
{
  VtStringArray tokens;
  if (socket.enum_values) {
    for (auto it = socket.enum_values->begin(); it != socket.enum_values->end(); ++it) {
      tokens.push_back(it->first.c_str());
    }
    std::sort(tokens.begin(), tokens.end());
  }
  return tokens;
}

// Build a single manifest entry dictionary.
VtDictionary MakeManifestEntry(const std::string &label,
                               const char *typeStr,
                               VtValue defaultVal,
                               bool restart,
                               const VtStringArray *tokens = nullptr)
{
  VtDictionary entry;
  entry["label"]   = VtValue(label);
  entry["type"]    = VtValue(std::string(typeStr));
  entry["default"] = std::move(defaultVal);
  entry["restart"] = VtValue(restart);
  if (tokens) {
    entry["tokens"] = VtValue(*tokens);
  }
  return entry;
}

SessionParams GetSessionParams(const HdRenderSettingsMap &settings)
{
  SessionParams params;
  params.threads = 0;
  params.background = false;
  params.use_resolution_divider = false;

  HdRenderSettingsMap::const_iterator it;

  // Pull all setting that contribute to device creation first
  it = settings.find(HdCyclesRenderSettingsTokens->threads);
  if (it != settings.end()) {
    params.threads = VtValue::Cast<int>(it->second).GetWithDefault(params.threads);
  }

  // Get the Cycles device from settings or environment, falling back to CPU
  std::string deviceType = Device::string_from_type(DEVICE_CPU);
  it = settings.find(HdCyclesRenderSettingsTokens->device);
  if (it != settings.end()) {
    deviceType = VtValue::Cast<std::string>(it->second).GetWithDefault(deviceType);
  }
  else {
    const std::string deviceTypeEnv = TfGetenv("CYCLES_DEVICE");
    if (!deviceTypeEnv.empty()) {
      deviceType = deviceTypeEnv;
    }
  }

  // Move to all uppercase for Device::type_from_string
  std::transform(deviceType.begin(), deviceType.end(), deviceType.begin(), ::toupper);

  vector<DeviceInfo> devices = Device::available_devices(
      DEVICE_MASK(Device::type_from_string(deviceType.c_str())));
  if (devices.empty()) {
    devices = Device::available_devices(DEVICE_MASK_CPU);
    if (!devices.empty()) {
      params.device = devices.front();
    }
  }
  else {
    params.device = Device::get_multi_device(devices, params.threads, params.background);
  }

  /* Set same device for denoising for now. */
  params.denoise_device = params.device;

  return params;
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

}  // namespace

HdCyclesDelegate::HdCyclesDelegate(const HdRenderSettingsMap &settingsMap,
                                   Session *session_,
                                   const bool keep_nodes)
    : HdRenderDelegate()
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

  _renderParam = session_ ? std::make_unique<HdCyclesSession>(session_, keep_nodes) :
                            std::make_unique<HdCyclesSession>(GetSessionParams(settingsMap));

  for (const auto &setting : settingsMap) {
    // Skip over the settings known to be used for initialization only
    if (setting.first == HdCyclesRenderSettingsTokens->device ||
        setting.first == HdCyclesRenderSettingsTokens->threads ||
        setting.first == HdCyclesRenderSettingsTokens->renderingColorSpace)
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
    TF_WARN("Rendering color space '%s' is not in the OCIO config, using its scene_linear role",
            colorSpace.c_str());
    colorSpace.clear();
    CCL_NS::ColorSpaceManager::set_scene_linear_colorspace(colorSpace);
  }
  _renderingColorSpace = std::move(colorSpace);

  const std::lock_guard<std::mutex> lock(g_colorSpaceMutex);
  if (g_liveDelegates > 1 && g_activeColorSpace != _renderingColorSpace) {
    TF_WARN(
        "Cycles renderers with different rendering color spaces are open ('%s' and '%s'); "
        "all of them now render in '%s'",
        g_activeColorSpace.c_str(),
        _renderingColorSpace.c_str(),
        _renderingColorSpace.c_str());
  }
  g_activeColorSpace = _renderingColorSpace;
}

HdCyclesDelegate::~HdCyclesDelegate()
{
  const std::lock_guard<std::mutex> lock(g_colorSpaceMutex);
  --g_liveDelegates;
}

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

HdRenderParam *HdCyclesDelegate::GetRenderParam() const
{
  return _renderParam.get();
}

HdResourceRegistrySharedPtr HdCyclesDelegate::GetResourceRegistry() const
{
  return HdResourceRegistrySharedPtr();
}

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

HdRenderPassSharedPtr HdCyclesDelegate::CreateRenderPass(HdRenderIndex *index,
                                                         const HdRprimCollection &collection)
{
  return HdRenderPassSharedPtr(new HdCyclesRenderPass(index, collection, _renderParam.get()));
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
  if (typeId == HdPrimTypeTokens->mesh) {
    return new HdCyclesMesh(rprimId);
  }
  if (typeId == HdPrimTypeTokens->basisCurves) {
    return new HdCyclesCurves(rprimId);
  }
  if (typeId == HdPrimTypeTokens->points) {
    return new HdCyclesPoints(rprimId);
  }
#ifdef WITH_OPENVDB
  if (typeId == HdPrimTypeTokens->volume) {
    return new HdCyclesVolume(rprimId);
  }
#endif

  TF_CODING_ERROR("Unknown Rprim type %s", typeId.GetText());
  return nullptr;
}

void HdCyclesDelegate::DestroyRprim(HdRprim *rPrim)
{
  delete rPrim;
}

HdSprim *HdCyclesDelegate::CreateSprim(const TfToken &typeId, const SdfPath &sprimId)
{
  if (typeId == HdPrimTypeTokens->camera) {
    return new HdCyclesCamera(sprimId);
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
  delete sPrim;
}

HdBprim *HdCyclesDelegate::CreateBprim(const TfToken &typeId, const SdfPath &bprimId)
{
  if (typeId == HdPrimTypeTokens->renderBuffer) {
    return new HdCyclesRenderBuffer(bprimId);
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
  delete bPrim;
}

void HdCyclesDelegate::CommitResources(HdChangeTracker *tracker)
{
  TF_UNUSED(tracker);

  const SceneLock lock(_renderParam.get());

  _renderParam->UpdateScene();
}

TfToken HdCyclesDelegate::GetMaterialBindingPurpose() const
{
  return HdTokens->full;
}

TfTokenVector HdCyclesDelegate::GetMaterialRenderContexts() const
{
  return {_tokens->cycles};
}

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
  return {{"rendererName", VtValue("Cycles")},
          {"rendererVersion", VtValue(GfVec3i(0, 0, 0))},
          {"percentDone", VtValue(floor_to_int(fractionDone * 100))},
          {"fractionDone", VtValue(fractionDone)},
          {"loadClockTime", VtValue(totalTime - renderTime)},
          {"peakMemory", VtValue(stats.mem_peak)},
          {"totalClockTime", VtValue(totalTime)},
          {"totalMemory", VtValue(stats.mem_used)},
          {"renderProgressAnnotation", VtValue(status)},
          {"huskErrorStatus", VtValue(progress.get_error() ? 1 : 0)}};
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

  return HdAovDescriptor();
}

HdRenderSettingDescriptorList HdCyclesDelegate::GetRenderSettingDescriptors() const
{
  Scene *const scene = _renderParam->session->scene.get();
  HdRenderSettingDescriptorList descriptors;

  // Session scalars
  descriptors.push_back({"Time Limit",    HdCyclesRenderSettingsTokens->timeLimit,    VtValue(0.0)});
  descriptors.push_back({"Sample Count",  HdCyclesRenderSettingsTokens->samples,      VtValue(1024)});
  descriptors.push_back({"Sample Offset", HdCyclesRenderSettingsTokens->sampleOffset, VtValue(0)});
  descriptors.push_back({"Threads",       HdCyclesRenderSettingsTokens->threads,      VtValue(0)});

  // Device token
  descriptors.push_back({"Device", HdCyclesRenderSettingsTokens->device, VtValue(TfToken("AUTO"))});

  // Integrator sockets (excluding delegate-managed ones)
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

  // Film sockets
  for (const SocketType &socket : scene->film->type->inputs) {
    if (!kFilmSocketsExposed.count(socket.name.string())) {
      continue;
    }
    if (!SocketTypeToUsdTypeName(socket.type)) {
      continue;
    }
    descriptors.push_back({socket.ui_name.string(),
                           TfToken("cycles:film:" + socket.name.string()),
                           GetSocketDefaultForManifest(scene->film, socket)});
  }

  // cycles:film:transparent (custom bool, not a Film node socket)
  descriptors.push_back({"Film Transparent", HdCyclesRenderSettingsTokens->filmTransparent, VtValue(false)});

  // Session restart params
  descriptors.push_back({"Use Auto Tile", HdCyclesRenderSettingsTokens->sessionUseAutoTile, VtValue(true)});
  descriptors.push_back({"Tile Size",     HdCyclesRenderSettingsTokens->sessionTileSize,    VtValue(2048)});

  // Scene restart params
  descriptors.push_back({"Use BVH Spatial Split", HdCyclesRenderSettingsTokens->sceneUseBvhSpatialSplit, VtValue(false)});
  descriptors.push_back({"Use Texture Cache",      HdCyclesRenderSettingsTokens->sceneUseTextureCache,   VtValue(true)});
  descriptors.push_back({"Auto Texture Cache",     HdCyclesRenderSettingsTokens->sceneAutoTextureCache,  VtValue(false)});
  descriptors.push_back({"Texture Cache Path",     HdCyclesRenderSettingsTokens->sceneTextureCachePath,  VtValue(std::string(""))});
  descriptors.push_back({"Texture Limit",          HdCyclesRenderSettingsTokens->sceneTextureLimit,      VtValue(0)});

  return descriptors;
}

void HdCyclesDelegate::SetRenderSetting(const PXR_NS::TfToken &key, const PXR_NS::VtValue &value)
{
  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (key == HdCyclesRenderSettingsTokens->stageMetersPerUnit) {
    _renderParam->SetStageMetersPerUnit(
        VtValue::Cast<double>(value).GetWithDefault(_renderParam->GetStageMetersPerUnit()));
  }
  else if (key == HdCyclesRenderSettingsTokens->timeLimit) {
    session->set_time_limit(
        VtValue::Cast<double>(value).GetWithDefault(session->params.time_limit));
  }
  else if (key == HdCyclesRenderSettingsTokens->samples) {
    static const int max_samples = Integrator::MAX_SAMPLES;
    int samples = VtValue::Cast<int>(value).GetWithDefault(session->params.samples);
    samples = std::min(std::max(1, samples), max_samples);
    session->set_samples(samples);
  }
  else if (key == HdCyclesRenderSettingsTokens->sampleOffset) {
    session->params.sample_subset_offset = VtValue::Cast<int>(value).GetWithDefault(
        session->params.sample_subset_offset);
    session->params.sample_subset_length = Integrator::MAX_SAMPLES;
    session->params.use_sample_subset = session->params.sample_subset_offset > 0;
    ++_settingsVersion;
  }
  else if (key == HdCyclesRenderSettingsTokens->renderingColorSpace) {
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
  }
  else if (key == HdCyclesRenderSettingsTokens->filmTransparent) {
    const bool newVal = VtValue::Cast<bool>(value).GetWithDefault(_filmTransparent);
    if (newVal != _filmTransparent) {
      _filmTransparent = newVal;
      _renderParam->filmTransparent = newVal;
      ++_settingsVersion;
    }
  }
  else {
    const std::string &keyString = key.GetString();
    if (keyString.rfind("cycles:integrator:", 0) == 0) {
      const ustring socketName(keyString, sizeof("cycles:integrator:") - 1);
      if (!kIntegratorExcluded.count(socketName.string())) {
        if (const SocketType *socket = scene->integrator->type->find_input(socketName)) {
          SetNodeValue(scene->integrator, *socket, value);
          ++_settingsVersion;
        }
      }
    }
    else if (keyString.rfind("cycles:film:", 0) == 0) {
      const ustring socketName(keyString, sizeof("cycles:film:") - 1);
      if (kFilmSocketsExposed.count(socketName.string())) {
        if (const SocketType *socket = scene->film->type->find_input(socketName)) {
          SetNodeValue(scene->film, *socket, value);
          scene->film->tag_modified();
          ++_settingsVersion;
        }
      }
    }
  }
}

VtValue HdCyclesDelegate::GetRenderSetting(const TfToken &key) const
{
  // settings_manifest ---------------------------------------------------
  if (key == HdCyclesRenderSettingsTokens->settingsManifest) {
    Scene *const scene = _renderParam->session->scene.get();
    VtDictionary manifest;

    manifest["cycles:samples"]      = VtValue(MakeManifestEntry("Sample Count",  "int",    VtValue(1024),           false));
    manifest["cycles:time_limit"]   = VtValue(MakeManifestEntry("Time Limit",    "double", VtValue(0.0),            false));
    manifest["cycles:sample_offset"]= VtValue(MakeManifestEntry("Sample Offset", "int",    VtValue(0),              false));
    manifest["cycles:threads"]      = VtValue(MakeManifestEntry("Threads",       "int",    VtValue(0),              true));
    {
      static const VtStringArray kDeviceTokens = {"AUTO","CPU","METAL","CUDA","OPTIX","HIP","ONEAPI"};
      manifest["cycles:device"] = VtValue(MakeManifestEntry("Device", "token", VtValue(TfToken("AUTO")), true, &kDeviceTokens));
    }
    for (const SocketType &socket : scene->integrator->type->inputs) {
      if (kIntegratorExcluded.count(socket.name.string())) continue;
      const char *typeStr = SocketTypeToUsdTypeName(socket.type);
      if (!typeStr) continue;
      VtDictionary entry;
      entry["label"]   = VtValue(socket.ui_name.string());
      entry["type"]    = VtValue(std::string(typeStr));
      entry["default"] = GetSocketDefaultForManifest(scene->integrator, socket);
      entry["restart"] = VtValue(false);
      if (socket.type == SocketType::ENUM) {
        entry["tokens"] = VtValue(GetSocketEnumTokens(socket));
      }
      manifest["cycles:integrator:" + socket.name.string()] = VtValue(entry);
    }
    for (const SocketType &socket : scene->film->type->inputs) {
      if (!kFilmSocketsExposed.count(socket.name.string())) continue;
      const char *typeStr = SocketTypeToUsdTypeName(socket.type);
      if (!typeStr) continue;
      VtDictionary entry;
      entry["label"]   = VtValue(socket.ui_name.string());
      entry["type"]    = VtValue(std::string(typeStr));
      entry["default"] = GetSocketDefaultForManifest(scene->film, socket);
      entry["restart"] = VtValue(false);
      if (socket.type == SocketType::ENUM) {
        entry["tokens"] = VtValue(GetSocketEnumTokens(socket));
      }
      manifest["cycles:film:" + socket.name.string()] = VtValue(entry);
    }
    manifest["cycles:film:transparent"]         = VtValue(MakeManifestEntry("Film Transparent",   "bool",   VtValue(false),         false));
    manifest["cycles:session:use_auto_tile"]    = VtValue(MakeManifestEntry("Use Auto Tile",       "bool",   VtValue(true),          true));
    manifest["cycles:session:tile_size"]        = VtValue(MakeManifestEntry("Tile Size",           "int",    VtValue(2048),          true));
    manifest["cycles:scene:use_bvh_spatial_split"] = VtValue(MakeManifestEntry("Use BVH Spatial Split","bool", VtValue(false),       true));
    manifest["cycles:scene:use_texture_cache"]  = VtValue(MakeManifestEntry("Use Texture Cache",   "bool",   VtValue(true),          true));
    manifest["cycles:scene:auto_texture_cache"] = VtValue(MakeManifestEntry("Auto Texture Cache",  "bool",   VtValue(false),         true));
    manifest["cycles:scene:texture_cache_path"] = VtValue(MakeManifestEntry("Texture Cache Path",  "string", VtValue(std::string("")),true));
    manifest["cycles:scene:texture_limit"]      = VtValue(MakeManifestEntry("Texture Limit",       "int",    VtValue(0),             true));
    return VtValue(manifest);
  }

  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (key == HdCyclesRenderSettingsTokens->stageMetersPerUnit)
    return VtValue(_renderParam->GetStageMetersPerUnit());
  if (key == HdCyclesRenderSettingsTokens->device)
    return VtValue(TfToken(_effectiveDeviceType.empty() ?
                           Device::string_from_type(session->params.device.type) :
                           _effectiveDeviceType));
  if (key == HdCyclesRenderSettingsTokens->threads)      return VtValue(session->params.threads);
  if (key == HdCyclesRenderSettingsTokens->timeLimit)    return VtValue(session->params.time_limit);
  if (key == HdCyclesRenderSettingsTokens->samples)      return VtValue(session->params.samples);
  if (key == HdCyclesRenderSettingsTokens->sampleOffset)
    return VtValue((session->params.use_sample_subset) ? session->params.sample_subset_offset : 0);
  if (key == HdCyclesRenderSettingsTokens->renderingColorSpace)
    return _renderingColorSpace.empty() ? VtValue() : VtValue(TfToken(_renderingColorSpace));
  if (key == HdCyclesRenderSettingsTokens->filmTransparent)         return VtValue(_filmTransparent);
  if (key == HdCyclesRenderSettingsTokens->sessionUseAutoTile)      return VtValue(session->params.use_auto_tile);
  if (key == HdCyclesRenderSettingsTokens->sessionTileSize)         return VtValue(session->params.tile_size);
  if (key == HdCyclesRenderSettingsTokens->sceneUseBvhSpatialSplit) return VtValue(scene->params.use_bvh_spatial_split);
  if (key == HdCyclesRenderSettingsTokens->sceneUseTextureCache)    return VtValue(scene->params.use_texture_cache);
  if (key == HdCyclesRenderSettingsTokens->sceneAutoTextureCache)   return VtValue(scene->params.auto_texture_cache);
  if (key == HdCyclesRenderSettingsTokens->sceneTextureCachePath)   return VtValue(scene->params.texture_cache_path);
  if (key == HdCyclesRenderSettingsTokens->sceneTextureLimit)       return VtValue(scene->params.texture_limit);

  // cycles:integrator:<socket> — return enum sockets as TfToken
  {
    const std::string &ks = key.GetString();
    if (ks.rfind("cycles:integrator:", 0) == 0) {
      const ustring socketName(ks, sizeof("cycles:integrator:") - 1);
      if (const SocketType *socket = scene->integrator->type->find_input(socketName)) {
        if (socket->type == SocketType::ENUM) {
          const int v = scene->integrator->get_int(*socket);
          if (socket->enum_values && socket->enum_values->exists(v))
            return VtValue(TfToken((*socket->enum_values)[v].c_str()));
        }
        return GetNodeValue(scene->integrator, *socket);
      }
    }
  }

  // cycles:film:<socket>
  {
    const std::string &ks = key.GetString();
    if (ks.rfind("cycles:film:", 0) == 0) {
      const ustring socketName(ks, sizeof("cycles:film:") - 1);
      if (kFilmSocketsExposed.count(socketName.string())) {
        if (const SocketType *socket = scene->film->type->find_input(socketName)) {
          if (socket->type == SocketType::ENUM) {
            const int v = scene->film->get_int(*socket);
            if (socket->enum_values && socket->enum_values->exists(v))
              return VtValue(TfToken((*socket->enum_values)[v].c_str()));
          }
          return GetNodeValue(scene->film, *socket);
        }
      }
    }
  }

  return VtValue();
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
