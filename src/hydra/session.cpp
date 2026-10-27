/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/session.h"
#include "hydra/diagnostic.h"
#include "scene/camera.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/object.h"
#include "scene/shader.h"
// Have to include shader.h before background.h so that 'set_shader' uses the correct 'set'
// overload taking a 'Node *', rather than the one taking a 'bool'
#include "scene/background.h"
#include "scene/light.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "session/session.h"

#include <unordered_set>

#include <pxr/imaging/hd/renderBuffer.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (sourceName)
    (dataType)
);
// clang-format on

// RenderVar source names, and the Hydra AOV names hosts use without a RenderVar, mapped to
// Cycles passes. Short names (C, N, P) are the usual RenderVar names in Solaris. Diffuse,
// glossy and transmission come as direct, indirect and color components only: Cycles' totals
// for them are lighting without the color, where other Solaris renderers' "diffuse" is the
// contribution to the beauty.
const std::unordered_map<TfToken, PassType, TfToken::HashFunctor> kAovToPass = {
    {HdAovTokens->color, PASS_COMBINED},
    {TfToken("C"), PASS_COMBINED},
    /* The light path expression for all light reaching the camera: husk's beauty when a stage
     * has no render settings, and Karma's. */
    {TfToken("C.*[LO]"), PASS_COMBINED},
    {HdAovTokens->depth, PASS_DEPTH},
    {HdAovTokens->normal, PASS_NORMAL},
    {TfToken("N"), PASS_NORMAL},
    {TfToken("position"), PASS_POSITION},
    {TfToken("P"), PASS_POSITION},
    {TfToken("roughness"), PASS_ROUGHNESS},
    {TfToken("uv"), PASS_UV},
    {TfToken("mist"), PASS_MIST},
    {TfToken("albedo"), PASS_DENOISING_ALBEDO},
    {TfToken("emission"), PASS_EMISSION},
    {TfToken("background"), PASS_BACKGROUND},
    {TfToken("ao"), PASS_AO},
    {TfToken("diffuse_direct"), PASS_DIFFUSE_DIRECT},
    {TfToken("diffuse_indirect"), PASS_DIFFUSE_INDIRECT},
    {TfToken("diffuse_color"), PASS_DIFFUSE_COLOR},
    {TfToken("glossy_direct"), PASS_GLOSSY_DIRECT},
    {TfToken("glossy_indirect"), PASS_GLOSSY_INDIRECT},
    {TfToken("glossy_color"), PASS_GLOSSY_COLOR},
    {TfToken("transmission_direct"), PASS_TRANSMISSION_DIRECT},
    {TfToken("transmission_indirect"), PASS_TRANSMISSION_INDIRECT},
    {TfToken("transmission_color"), PASS_TRANSMISSION_COLOR},
    {TfToken("volume"), PASS_VOLUME},
    {TfToken("volume_direct"), PASS_VOLUME_DIRECT},
    {TfToken("volume_indirect"), PASS_VOLUME_INDIRECT},
    {TfToken("sample_count"), PASS_SAMPLE_COUNT},
    {HdAovTokens->primId, PASS_OBJECT_ID},
    // Written by the AOV output node of the default surface shader (see the constructor).
    {HdAovTokens->instanceId, PASS_AOV_VALUE},
};

/* One RenderVar per cryptomatte layer, named like Cycles' passes and Blender's EXR layers:
 * CryptoObject00, CryptoMaterial01, CryptoAsset02. Each layer holds two ID/coverage pairs. */
struct CryptomatteLayer {
  CryptomatteType type;
  int index;
};

constexpr std::pair<const char *, CryptomatteType> kCryptomatteTypes[] = {
    {"CryptoObject", CRYPT_OBJECT},
    {"CryptoMaterial", CRYPT_MATERIAL},
    {"CryptoAsset", CRYPT_ASSET},
};

/* Blender's limit: 16 IDs per pixel. */
constexpr int kMaxCryptomatteLayers = 8;

bool ParseCryptomatteLayer(const TfToken &sourceName, CryptomatteLayer *layer)
{
  const auto digit = [](const char c) { return c >= '0' && c <= '9'; };
  const std::string &name = sourceName.GetString();
  for (const auto &[prefix, type] : kCryptomatteTypes) {
    const size_t length = strlen(prefix);
    if (name.size() == length + 2 && name.starts_with(prefix) && digit(name[length]) &&
        digit(name[length + 1]))
    {
      layer->type = type;
      layer->index = (name[length] - '0') * 10 + (name[length + 1] - '0');
      return true;
    }
  }
  return false;
}

TfToken GetSourceName(const HdRenderPassAovBinding &aovBinding)
{
  // husk names a binding after its output channel (for example "C") and passes the
  // RenderVar's source name ("color") in the AOV settings. Hosts that bind a Hydra AOV
  // directly, like the viewport, use the AOV name.
  const auto setting = aovBinding.aovSettings.find(_tokens->sourceName);
  if (setting != aovBinding.aovSettings.end()) {
    const VtValue &value = setting->second;
    if (value.IsHolding<std::string>()) {
      return TfToken(value.UncheckedGet<std::string>());
    }
    if (value.IsHolding<TfToken>()) {
      return value.UncheckedGet<TfToken>();
    }
  }
  return aovBinding.aovName;
}

/* Whether a shader AOV binding wants Cycles' value pass rather than its colour pass: from the
 * RenderVar's dataType, as husk passes it, or else from the buffer the host made. */
bool IsScalarAov(const HdRenderPassAovBinding &aovBinding)
{
  const auto setting = aovBinding.aovSettings.find(_tokens->dataType);
  if (setting != aovBinding.aovSettings.end()) {
    const VtValue &value = setting->second;
    const std::string dataType = value.IsHolding<TfToken>() ?
                                     value.UncheckedGet<TfToken>().GetString() :
                                     VtValue::Cast<std::string>(value).GetWithDefault(
                                         std::string());
    if (!dataType.empty()) {
      return dataType == "float" || dataType == "half";
    }
  }
  return aovBinding.renderBuffer && HdGetComponentCount(aovBinding.renderBuffer->GetFormat()) == 1;
}

}  // namespace

PassType HdCyclesPassTypeForAov(const TfToken &sourceName)
{
  const auto it = kAovToPass.find(sourceName);
  return it == kAovToPass.end() ? PASS_NONE : it->second;
}

SceneLock::SceneLock(const HdRenderParam *renderParam)
    : scene(static_cast<const HdCyclesSession *>(renderParam)->session->scene.get()),
      sceneLock(scene->mutex)
{
}

SceneLock::~SceneLock() = default;

HdCyclesSession::HdCyclesSession(Session *session_, const bool keep_nodes)
    : session(session_), keep_nodes(keep_nodes), _ownCyclesSession(false)
{
}

HdCyclesSession::HdCyclesSession(const SessionParams &params, const SceneParams &sceneParams)
    : session(new Session(params, sceneParams)), keep_nodes(false), _ownCyclesSession(true)
{
  Scene *const scene = session->scene.get();

  // Create background with ambient light
  {
    unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

    BackgroundNode *bgNode = graph->create_node<BackgroundNode>();
    bgNode->set_color(one_float3());

    graph->connect(bgNode->output("Background"), graph->output()->input("Surface"));

    scene->default_background->set_graph(std::move(graph));
    scene->default_background->tag_update(scene);
  }

  // Wire up object color in default surface material
  {
    unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

    ObjectInfoNode *objectNode = graph->create_node<ObjectInfoNode>();

    DiffuseBsdfNode *diffuseNode = graph->create_node<DiffuseBsdfNode>();

    graph->connect(objectNode->output("Color"), diffuseNode->input("Color"));
    graph->connect(diffuseNode->output("BSDF"), graph->output()->input("Surface"));

    // Create the instanceId AOV output
    const ustring instanceId(HdAovTokens->instanceId.GetString());

    OutputAOVNode *aovNode = graph->create_node<OutputAOVNode>();
    aovNode->set_name(instanceId);

    AttributeNode *instanceIdNode = graph->create_node<AttributeNode>();
    instanceIdNode->set_attribute(instanceId);

    graph->connect(instanceIdNode->output("Fac"), aovNode->input("Value"));

    scene->default_surface->set_graph(std::move(graph));
    scene->default_surface->tag_update(scene);
  }
}

HdCyclesSession::~HdCyclesSession()
{
  if (_ownCyclesSession) {
    delete session;
  }
}

/* Motion blur is on while the shutter is open; Cycles samples ray times only when the camera's
 * shutter time is positive. Nothing else sets the integrator's motion_blur, which isn't an
 * exposed setting (kIntegratorExcluded). Call with the scene locked. */
void HdCyclesSession::ApplyShutter()
{
  Scene *const scene = session->scene.get();
  scene->integrator->set_motion_blur(shutter.IsOpen());
  scene->camera->set_shuttertime(shutter.IsOpen() ? shutter.close - shutter.open : 1.0f);

  /* Volume velocity blur advects a grid from the frame by (t - offset) of the shutter, for
   * ray time t from 0 to 1. Cycles places the frame at offset 0.5 (a centred shutter) or 0 (a
   * shutter opening at the frame); take the nearer (volumes warn when neither is exact). Its
   * END position doesn't place the frame at the close. Only the volume kernel reads this. */
  if (shutter.IsOpen()) {
    scene->camera->set_motion_position(shutter.VolumeBlurCentred() ? MOTION_POSITION_CENTER :
                                                                     MOTION_POSITION_START);
  }
}

bool HdCyclesSession::UpdateShutter(const HdCyclesShutter &next)
{
  {
    const thread_scoped_lock lock(_shutterMutex);
    /* husk can resend the same shutter every frame; only a change resamples prims. */
    if (next.open == shutter.open && next.close == shutter.close &&
        next.disableMotionBlur == shutter.disableMotionBlur &&
        next.instantaneous == shutter.instantaneous)
    {
      return false;
    }
    const unsigned int version = shutter.version;
    shutter = next;
    shutter.version = version + 1;
  }
  const thread_scoped_lock sceneLock(session->scene->mutex);
  ApplyShutter();
  return true;
}

HdCyclesShutter HdCyclesSession::GetShutter() const
{
  const thread_scoped_lock lock(_shutterMutex);
  return shutter;
}

bool HdCyclesSession::SetCameraShutter(const SdfPath &camera, const float open, const float close)
{
  const thread_scoped_lock lock(_cameraMutex);
  _cameraShutters[camera] = {open, close};
  if (camera != _renderCamera || GetShutter().fromSettings) {
    return false;
  }
  HdCyclesShutter next = GetShutter();
  next.open = open;
  next.close = close;
  return UpdateShutter(next);
}

bool HdCyclesSession::SetRenderCamera(const SdfPath &camera)
{
  const thread_scoped_lock lock(_cameraMutex);
  if (camera == _renderCamera) {
    return false;
  }
  _renderCamera = camera;
  const auto it = _cameraShutters.find(camera);
  if (it == _cameraShutters.end() || GetShutter().fromSettings) {
    return false;
  }
  HdCyclesShutter next = GetShutter();
  next.open = it->second.first;
  next.close = it->second.second;
  return UpdateShutter(next);
}

void HdCyclesSession::RemoveCameraShutter(const SdfPath &camera)
{
  const thread_scoped_lock lock(_cameraMutex);
  _cameraShutters.erase(camera);
}

void HdCyclesSession::TakeCameras(HdCyclesSession &previous)
{
  const thread_scoped_lock lock(_cameraMutex);
  const thread_scoped_lock previousLock(previous._cameraMutex);
  _cameraShutters = previous._cameraShutters;
  _renderCamera = previous._renderCamera;
}

void HdCyclesSession::UpdateScene()
{
  Scene *const scene = session->scene.get();

  // Update background depending on presence of a background light
  bool meshLightsChanged = false;
  bool haveMeshLights = false;
  {
    const thread_scoped_lock lock(_meshLightsMutex);
    meshLightsChanged = std::exchange(_meshLightsChanged, false);
    haveMeshLights = !_meshLights.empty();
  }
  if (scene->light_manager->need_update() || meshLightsChanged) {
    Light *background_light = nullptr;
    bool have_lights = haveMeshLights;
    for (Object *object : scene->objects) {
      if (!object->get_geometry()->is_light()) {
        continue;
      }

      have_lights = true;

      Light *light = static_cast<Light *>(object->get_geometry());
      if (light->is_background_light()) {
        background_light = light;
        break;
      }
    }

    if (!background_light) {
      scene->background->set_shader(scene->default_background);
      scene->background->set_transparent(true);

      /* Set background color depending to non-zero value if there are no
       * lights in the scene, to match behavior of other renderers. */
      for (ShaderNode *node : scene->default_background->graph->nodes) {
        if (node->is_a(BackgroundNode::get_node_type())) {
          BackgroundNode *bgNode = static_cast<BackgroundNode *>(node);
          bgNode->set_color((have_lights) ? zero_float3() : make_float3(0.5f));
        }
      }
    }
    else {
      scene->background->set_shader(background_light->get_shader());
      // cycles:film:transparent overrides the default behaviour of making the background
      // opaque when a dome light is present, keeping it transparent so the dome colour
      // shows in passes while alpha remains 0 on the background.
      scene->background->set_transparent(filmTransparent);
    }

    scene->background->tag_update(scene);
  }
}

void HdCyclesSession::SyncAovBindings(const HdRenderPassAovBindingVector &aovBindings)
{
  Scene *const scene = session->scene.get();

  // Delete all existing passes
  const vector<Pass *> &scene_passes = scene->passes;
  scene->delete_nodes(set<Pass *>(scene_passes.begin(), scene_passes.end()));

  // Update passes with requested AOV bindings. Cycles writes one pass per type, so bindings
  // of the same type (aliases such as "C" and "color") share a pass, named after the type. A
  // shader AOV pass is named after the AOV output node that writes it.
  const thread_scoped_lock lock(_aovMutex);
  _aovBindings = aovBindings;
  _aovPasses.assign(aovBindings.size(), HdCyclesAovPass());

  // Cryptomatte: the film records the requested types to the deepest requested layer, in
  // passes the kernel expects in this order (object, material, asset) and with Blender's
  // names, which the bindings read by name. The film has one depth for all types, so every
  // requested type gets every layer up to the deepest one requested of any type.
  int cryptomatteTypes = CRYPT_NONE;
  int cryptomatteDepth = 0;
  for (const HdRenderPassAovBinding &aovBinding : aovBindings) {
    CryptomatteLayer layer;
    const TfToken sourceName = GetSourceName(aovBinding);
    if (!ParseCryptomatteLayer(sourceName, &layer)) {
      continue;
    }
    if (layer.index >= kMaxCryptomatteLayers) {
      HDCYCLES_WARN("Cryptomatte has at most %d layers; %s is not rendered",
                    kMaxCryptomatteLayers,
                    sourceName.GetText());
      continue;
    }
    cryptomatteTypes |= layer.type;
    cryptomatteDepth = max(cryptomatteDepth, layer.index + 1);
  }
  scene->film->set_cryptomatte_passes(CryptomatteType(cryptomatteTypes));
  scene->film->set_cryptomatte_depth(cryptomatteDepth);
  for (const auto &[prefix, type] : kCryptomatteTypes) {
    for (int index = 0; (cryptomatteTypes & type) && index < cryptomatteDepth; ++index) {
      Pass *pass = scene->create_node<Pass>();
      pass->set_type(PASS_CRYPTOMATTE);
      pass->set_mode(PassMode::NOISY);
      pass->set_name(ustring(string_printf("%s%02d", prefix, index)));
    }
  }

  std::unordered_set<std::string> createdPasses;
  for (size_t i = 0; i < aovBindings.size(); ++i) {
    const TfToken sourceName = GetSourceName(aovBindings[i]);
    HdCyclesAovPass &aovPass = _aovPasses[i];
    CryptomatteLayer layer;
    if (ParseCryptomatteLayer(sourceName, &layer)) {
      if (layer.index < kMaxCryptomatteLayers) {
        aovPass.passName = sourceName.GetString();
      }
      continue;
    }

    /* lightgroup:<name> is the beauty lit only by the lights, emitters and background in that
     * light group. Cycles makes a light group of each pass's group name. */
    const std::string &source = sourceName.GetString();
    if (source.starts_with("lightgroup:") && source.size() > strlen("lightgroup:")) {
      aovPass.passName = source;
      if (createdPasses.insert(source).second) {
        Pass *pass = scene->create_node<Pass>();
        pass->set_type(PASS_COMBINED);
        pass->set_mode(PassMode::DENOISED);
        pass->set_name(ustring(source));
        pass->set_lightgroup(ustring(source.substr(strlen("lightgroup:"))));
      }
      continue;
    }

    /* cycles:aov:<name> is the shader AOV that AOV Output nodes named <name> write: Cycles'
     * value pass for a float RenderVar, its colour pass otherwise. */
    if (source.starts_with("cycles:aov:") && source.size() > strlen("cycles:aov:")) {
      aovPass.passName = source.substr(strlen("cycles:aov:"));
      const PassType type = IsScalarAov(aovBindings[i]) ? PASS_AOV_VALUE : PASS_AOV_COLOR;
      if (createdPasses.insert(aovPass.passName).second) {
        Pass *pass = scene->create_node<Pass>();
        pass->set_type(type);
        pass->set_mode(PassMode::NOISY);
        pass->set_name(ustring(aovPass.passName));
      }
      continue;
    }

    const PassType type = HdCyclesPassTypeForAov(sourceName);
    if (type == PASS_NONE) {
      // elementId is a standard Hydra AOV that Cycles has no pass for.
      if (sourceName != HdAovTokens->elementId) {
        HDCYCLES_WARN("Unknown pass %s", sourceName.GetText());
      }
      continue;
    }

    aovPass.isId = sourceName == HdAovTokens->primId || sourceName == HdAovTokens->instanceId;
    aovPass.isDepth = type == PASS_DEPTH;
    if (type == PASS_POSITION || type == PASS_DEPTH) {
      aovPass.scale = float(1.0 / _stageMetersPerUnit);
    }
    aovPass.passName = type == PASS_AOV_VALUE ? sourceName.GetString() : pass_type_as_string(type);
    if (!createdPasses.insert(aovPass.passName).second) {
      continue;
    }

    Pass *pass = scene->create_node<Pass>();
    pass->set_type(type);
    // The film drops this to noisy for passes that cannot be denoised, or when denoising is
    // off.
    pass->set_mode(PassMode::DENOISED);
    pass->set_name(ustring(aovPass.passName));
  }
}

void HdCyclesSession::RemoveAovBinding(HdRenderBuffer *renderBuffer)
{
  const thread_scoped_lock lock(_aovMutex);
  for (HdRenderPassAovBinding &aovBinding : _aovBindings) {
    if (renderBuffer == aovBinding.renderBuffer) {
      aovBinding.renderBuffer = nullptr;
      break;
    }
  }

  if (renderBuffer == _displayAovBinding.renderBuffer) {
    _displayAovBinding.renderBuffer = nullptr;
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
