/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/session.h"
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

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (sourceName)
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

void HdCyclesSession::UpdateScene()
{
  Scene *const scene = session->scene.get();

  // Update background depending on presence of a background light
  if (scene->light_manager->need_update()) {
    Light *background_light = nullptr;
    bool have_lights = false;
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
  _aovBindings = aovBindings;
  _aovPasses.assign(aovBindings.size(), HdCyclesAovPass());
  std::unordered_set<std::string> createdPasses;
  for (size_t i = 0; i < aovBindings.size(); ++i) {
    const TfToken sourceName = GetSourceName(aovBindings[i]);
    const PassType type = HdCyclesPassTypeForAov(sourceName);
    if (type == PASS_NONE) {
      // elementId is a standard Hydra AOV that Cycles has no pass for.
      if (sourceName != HdAovTokens->elementId) {
        TF_WARN("Unknown pass %s", sourceName.GetText());
      }
      continue;
    }

    HdCyclesAovPass &aovPass = _aovPasses[i];
    aovPass.isId = sourceName == HdAovTokens->primId || sourceName == HdAovTokens->instanceId;
    aovPass.passName = type == PASS_AOV_VALUE ? sourceName.GetString() :
                                                pass_type_as_string(type);
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
