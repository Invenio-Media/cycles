/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/light.h"
#include "hydra/diagnostic.h"
#include "hydra/object_settings.h"
#include "hydra/session.h"
#include "hydra/util.h"
#include "kernel/types.h"
#include "scene/background.h"
#include "scene/light.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "util/hash.h"
#include "util/ies.h"
#include "util/path.h"
#include "util/transform.h"

#include <algorithm>
#include <cmath>

#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/usd/sdf/assetPath.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

extern Transform convert_transform(const GfMatrix4d &matrix);

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (visibleInPrimaryRay)
    (treatAsPoint)
    (falloff)
);
// clang-format on

HdCyclesLight::HdCyclesLight(const SdfPath &sprimId, const TfToken &lightType)
    : HdLight(sprimId), _lightType(lightType)
{
}

HdCyclesLight::~HdCyclesLight() = default;

HdDirtyBits HdCyclesLight::GetInitialDirtyBitsMask() const
{
  return DirtyBits::DirtyTransform | DirtyBits::DirtyParams | DirtyBits::DirtyCollection;
}

void HdCyclesLight::Sync(HdSceneDelegate *sceneDelegate,
                         HdRenderParam *renderParam,
                         HdDirtyBits *dirtyBits)
{
  if (*dirtyBits == DirtyBits::Clean) {
    return;
  }

  Initialize(renderParam);

  const SceneLock lock(renderParam);

  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
  const HdContainerDataSourceHandle &primDs = prim.dataSource;
  const HdContainerDataSourceHandle lightDs = HdLightSchema::GetFromParent(primDs).GetContainer();

  if (*dirtyBits & DirtyBits::DirtyTransform) {
    const float metersPerUnit =
        static_cast<HdCyclesSession *>(renderParam)->GetStageMetersPerUnit();

    GfMatrix4d xform(1.0);
    if (auto matrixDs = HdXformSchema::GetFromParent(primDs).GetMatrix()) {
      xform = matrixDs->GetTypedValue(0.0f);
    }

    const Transform tfm = transform_scale(make_float3(metersPerUnit)) * convert_transform(xform);
    _object->set_tfm(tfm);
  }

  if (*dirtyBits & DirtyBits::DirtyParams) {
    float3 strength = make_float3(1.0f, 1.0f, 1.0f);

    {
      const GfVec3f color = GetTypedValue<GfVec3f>(
          lightDs, HdLightTokens->color, GfVec3f(1.0f, 1.0f, 1.0f));
      strength = make_float3(color[0], color[1], color[2]);
    }

    strength *= exp2(GetTypedValue<float>(lightDs, HdLightTokens->exposure, 0.0f));
    strength *= GetTypedValue<float>(lightDs, HdLightTokens->intensity, 1.0f);

    // UsdLux intensities, as Karma renders them. A Cycles sun's strength is already the
    // irradiance (normalized) or the disk's radiance, and a background's strength is its
    // radiance. Cycles' point, spot and area lights take radiant flux.
    if (_lightType != HdPrimTypeTokens->distantLight && _lightType != HdPrimTypeTokens->domeLight)
    {
      strength *= M_PI_F;
    }

    _light->set_normalize(GetTypedValue<bool>(lightDs, HdLightTokens->normalize, false));

    /* Hidden from the camera unless cycles:light:visibility:camera asks for it, as Karma's
     * karma:light:renderlightgeo does (or Blender's visibleInPrimaryRay). The cycles:light:
     * settings can take other rays away, and set the light's own Cycles settings. */
    /* Hydra hands a light's other attributes over as light parameters. */
    const HdCyclesSettingReader read = [sceneDelegate, &id](const char *name) {
      return sceneDelegate->GetLightParamValue(id, TfToken(std::string("cycles:light:") + name));
    };
    unsigned visibility = PATH_RAY_VISIBILITY_ALL & ~PATH_RAY_VISIBILITY_CAMERA;
    if (HdCyclesSettingValue<bool>(
            read("visibility:camera"),
            GetTypedValue<bool>(lightDs, _tokens->visibleInPrimaryRay, false)))
    {
      visibility |= PATH_RAY_VISIBILITY_CAMERA;
    }
    HdCyclesApplyObjectSettings(_object, visibility, read);
    /* Unauthored, the light keeps Cycles' socket defaults, apart from use_mis, which the
     * delegate has always turned on. */
    _light->set_cast_shadow(HdCyclesSettingValue<bool>(
        read("cast_shadow"), GetTypedValue<bool>(lightDs, HdLightTokens->shadowEnable, true)));
    _light->set_use_mis(HdCyclesSettingValue<bool>(read("use_mis"), true));
    _light->set_use_caustics(HdCyclesSettingValue<bool>(read("use_caustics"), false));
    _light->set_max_bounces(HdCyclesSettingValue<int>(read("max_bounces"), 1024));
    /* A dome lights the scene through the background, whose light group is its own. Cycles has
     * one background, so with several domes the last one synced sets it. */
    if (_lightType == HdPrimTypeTokens->domeLight) {
      lock.scene->background->set_lightgroup(_object->get_lightgroup());
    }

    if (_lightType == HdPrimTypeTokens->distantLight) {
      if (auto ds = GetTypedDataSource<float>(lightDs, HdLightTokens->angle)) {
        static_cast<SunLight *>(_light)->set_angle(GfDegreesToRadians(ds->GetTypedValue(0.0f)));
      }
    }
    else if (_lightType == HdPrimTypeTokens->diskLight) {
      AreaLight *area_light = static_cast<AreaLight *>(_light);
      if (auto ds = GetTypedDataSource<float>(lightDs, HdLightTokens->radius)) {
        const float size = ds->GetTypedValue(0.0f) * 2.0f;
        area_light->set_sizeu(size);
        area_light->set_sizev(size);
      }
    }
    else if (_lightType == HdPrimTypeTokens->rectLight) {
      AreaLight *area_light = static_cast<AreaLight *>(_light);
      if (auto ds = GetTypedDataSource<float>(lightDs, HdLightTokens->width)) {
        area_light->set_sizeu(ds->GetTypedValue(0.0f));
      }
      if (auto ds = GetTypedDataSource<float>(lightDs, HdLightTokens->height)) {
        area_light->set_sizev(ds->GetTypedValue(0.0f));
      }
    }
    else if (_lightType == HdPrimTypeTokens->sphereLight) {
      SpotLight *spot_light = static_cast<SpotLight *>(_light);
      const bool treatAsPoint = GetTypedValue<bool>(lightDs, _tokens->treatAsPoint, false);
      if (treatAsPoint) {
        spot_light->set_radius(0.0f);
      }
      else if (auto ds = GetTypedDataSource<float>(lightDs, HdLightTokens->radius)) {
        spot_light->set_radius(ds->GetTypedValue(0.0f));
      }

      /* UsdLux shaping:cone: a cutoff angle off the light's axis (default 90 degrees), and a
       * softness that smooths the last part of it, from cutoff * (1 - softness) to the
       * cutoff, with a smoothstep in angle. Cycles' spot smooths with a smoothstep in the
       * cosine instead, over a fraction of 1 - cos(cutoff); the fraction is chosen so the
       * smooth part starts and ends at the same angles. */
      const auto coneAngle = GetTypedDataSource<float>(lightDs, HdLightTokens->shapingConeAngle);
      const auto coneSoftness = GetTypedDataSource<float>(lightDs,
                                                          HdLightTokens->shapingConeSoftness);
      if (coneAngle || coneSoftness) {
        const float cutoff = GfDegreesToRadians(
            std::clamp(coneAngle ? coneAngle->GetTypedValue(0.0f) : 90.0f, 0.0f, 180.0f));
        const float softness = std::clamp(
            coneSoftness ? coneSoftness->GetTypedValue(0.0f) : 0.0f, 0.0f, 1.0f);
        const float cosCutoff = std::cos(cutoff);
        const float cosStart = std::cos(cutoff * (1.0f - softness));
        spot_light->set_angle(cutoff * 2.0f);
        spot_light->set_smooth(cosCutoff < 1.0f ? (cosStart - cosCutoff) / (1.0f - cosCutoff) :
                                                  0.0f);
        _light->set_light_type(LIGHT_SPOT);
      }
      else {
        _light->set_light_type(LIGHT_POINT);
      }
    }

    bool visible = true;
    if (auto ds = HdVisibilitySchema::GetFromParent(primDs).GetVisibility()) {
      visible = ds->GetTypedValue(0.0f);
    }
    // Disable invisible lights by zeroing the strength
    // So 'LightManager::test_enabled_lights' updates the enabled flag correctly
    if (!visible) {
      strength = zero_float3();
    }

    _light->set_strength(strength);
    _light->set_is_enabled(visible);

    PopulateShaderGraph(lightDs);
  }
  // Need to update shader graph when transform changes in case transform was baked into it
  else if (_object->tfm_is_modified() && (_lightType == HdPrimTypeTokens->domeLight ||
                                          _light->get_shader()->has_surface_spatial_varying))
  {
    PopulateShaderGraph(lightDs);
  }

  /* The categories of the light:link and shadow:link collections; empty, the light reaches
   * everything (hydra/light_linking.h). Cycles links a background only where lights are
   * sampled, not where paths hit it, so a dome's links would apply to half its light; they are
   * left out instead, as Blender has no world linking. */
  if (*dirtyBits & (DirtyBits::DirtyParams | DirtyBits::DirtyCollection)) {
    TfToken lightLink = GetTypedValue<TfToken>(lightDs, HdTokens->lightLink, TfToken());
    TfToken shadowLink = GetTypedValue<TfToken>(lightDs, HdTokens->shadowLink, TfToken());
    if (_lightType == HdPrimTypeTokens->domeLight) {
      /* Warned once per change of the links, not on every parameter edit. */
      if ((lightLink != _ignoredLightLink || shadowLink != _ignoredShadowLink) &&
          !(lightLink.IsEmpty() && shadowLink.IsEmpty()))
      {
        HDCYCLES_WARN(
            "Cycles can't link a dome light: light:link and shadow:link on %s are ignored.",
            id.GetText());
      }
      _ignoredLightLink = lightLink;
      _ignoredShadowLink = shadowLink;
      lightLink = TfToken();
      shadowLink = TfToken();
    }
    static_cast<HdCyclesSession *>(renderParam)
        ->lightLinking.SetEmitter(_object, lightLink, shadowLink);
  }

  if (_light->is_modified()) {
    _light->tag_update(lock.scene);
  }

  *dirtyBits = DirtyBits::Clean;
}

namespace {

/* An IES profile's power, as UsdLux's shaping:ies:normalize divides by it: the intensity
 * integrated over the solid angle the profile covers, divided by that whole sphere's or
 * hemisphere's solid angle. The same integration as USD's reference (hdEmbree's PxrIESFile,
 * which "matches Karma & RIS"), over the profile as Cycles loads it: IESFile::load has
 * already mirrored a symmetric profile's horizontal angles round the whole circle. */
class HdCyclesIesProfile : public IESFile {
 public:
  float Power() const
  {
    if (v_angles.size() < 2 || h_angles.size() < 2) {
      return 0.0f;
    }
    const auto [vMin, vMax] = std::minmax_element(v_angles.begin(), v_angles.end());
    const bool sphere = *vMax - *vMin > M_PI_2_F + 0.1f;
    float power = 0.0f;
    for (size_t h = 0; h + 1 < h_angles.size(); ++h) {
      for (size_t v = 0; v + 1 < v_angles.size(); ++v) {
        const float dh = h_angles[h + 1] - h_angles[h];
        const float dv = v_angles[v + 1] - v_angles[v];
        const float i0 = (intensity[h][v] + intensity[h][v + 1]) * 0.5f;
        const float i1 = (intensity[h + 1][v] + intensity[h + 1][v + 1]) * 0.5f;
        power += dh * dv * std::sin(v_angles[v] + dv * 0.5f) * (i0 + i1) * 0.5f;
      }
    }
    return power / (M_PI_F * (sphere ? 4.0f : 2.0f));
  }
};

MathNode *AddMath(ShaderGraph *graph, const NodeMathType type, const float value2 = 0.0f)
{
  MathNode *node = graph->create_node<MathNode>();
  node->set_math_type(type);
  node->set_value2(value2);
  return node;
}

/* The direction of emission in the light's own space, from the light towards the shaded
 * point, for Cycles' IES node: the shader's incoming direction, against the light's axes. The
 * texture coordinate's normal, which Blender uses, is the normal of the point sampled on a
 * sphere light, which spreads over the whole cap the shaded point sees, and blurs an IES
 * profile badly. The IES node takes its azimuth as atan2(x, y) + pi; Karma's horizontal angles
 * run the other way round the light's axis from Cycles', so the light's X and Y go in as -Y
 * and X (checked against Karma with an asymmetric profile). */
ShaderOutput *IesDirection(ShaderGraph *graph, const Transform &tfm)
{
  GeometryNode *geometry = graph->create_node<GeometryNode>();
  CombineXYZNode *combine = graph->create_node<CombineXYZNode>();
  const float3 axes[3] = {-normalize(transform_get_column(&tfm, 1)),
                          normalize(transform_get_column(&tfm, 0)),
                          normalize(transform_get_column(&tfm, 2))};
  const char *const components[3] = {"X", "Y", "Z"};
  for (int axis = 0; axis < 3; ++axis) {
    VectorMathNode *dot = graph->create_node<VectorMathNode>();
    dot->set_math_type(NODE_VECTOR_MATH_DOT_PRODUCT);
    dot->set_vector2(axes[axis]);
    graph->connect(geometry->output("Incoming"), dot->input("Vector1"));
    graph->connect(dot->output("Value"), combine->input(components[axis]));
  }
  return combine->output("Vector");
}

/* UsdLux shaping:ies:angleScale: the polar angle off the light's -Z axis, theta, is looked up
 * at theta / angleScale for a positive scale, or at (theta - pi) / -angleScale + pi for a
 * negative one, where angles below 0 give no light. The direction is rebuilt at that angle
 * for Cycles' IES node, which takes the polar angle from -Z. Returns the direction, and sets
 * *inRange to 1 where the angle is valid and 0 where it isn't. */
ShaderOutput *ScaleIesAngle(ShaderGraph *graph,
                            ShaderOutput *direction,
                            const float angleScale,
                            ShaderOutput **inRange)
{
  SeparateXYZNode *split = graph->create_node<SeparateXYZNode>();
  graph->connect(direction, split->input("Vector"));

  /* theta = acos(-z / |d|) */
  VectorMathNode *length = graph->create_node<VectorMathNode>();
  length->set_math_type(NODE_VECTOR_MATH_LENGTH);
  graph->connect(direction, length->input("Vector1"));
  MathNode *ratio = AddMath(graph, NODE_MATH_DIVIDE);
  graph->connect(split->output("Z"), ratio->input("Value1"));
  graph->connect(length->output("Value"), ratio->input("Value2"));
  MathNode *negated = AddMath(graph, NODE_MATH_MULTIPLY, -1.0f);
  graph->connect(ratio->output("Value"), negated->input("Value1"));
  MathNode *theta = AddMath(graph, NODE_MATH_ARCCOSINE);
  graph->connect(negated->output("Value"), theta->input("Value1"));

  /* theta' = theta / s, or (theta - pi) / -s + pi = theta / -s + pi (1 + 1 / s) */
  MathNode *scaled = AddMath(graph, NODE_MATH_MULTIPLY_ADD);
  scaled->set_value2(angleScale > 0.0f ? 1.0f / angleScale : -1.0f / angleScale);
  scaled->set_value3(angleScale > 0.0f ? 0.0f : M_PI_F * (1.0f + 1.0f / angleScale));
  graph->connect(theta->output("Value"), scaled->input("Value1"));
  MathNode *valid = AddMath(graph, NODE_MATH_GREATER_THAN, -1e-6f);
  graph->connect(scaled->output("Value"), valid->input("Value1"));
  *inRange = valid->output("Value");
  MathNode *clamped = AddMath(graph, NODE_MATH_MINIMUM, M_PI_F);
  graph->connect(scaled->output("Value"), clamped->input("Value1"));

  /* (x, y) along the same azimuth, at sin(theta'), and z = -cos(theta'). The horizontal part
   * is (x, y) / |(x, y)|; straight along the axis it is 0, and the azimuth doesn't matter. */
  CombineXYZNode *flat = graph->create_node<CombineXYZNode>();
  graph->connect(split->output("X"), flat->input("X"));
  graph->connect(split->output("Y"), flat->input("Y"));
  VectorMathNode *azimuth = graph->create_node<VectorMathNode>();
  azimuth->set_math_type(NODE_VECTOR_MATH_NORMALIZE);
  graph->connect(flat->output("Vector"), azimuth->input("Vector1"));
  MathNode *sine = AddMath(graph, NODE_MATH_SINE);
  graph->connect(clamped->output("Value"), sine->input("Value1"));
  VectorMathNode *horizontal = graph->create_node<VectorMathNode>();
  horizontal->set_math_type(NODE_VECTOR_MATH_SCALE);
  graph->connect(azimuth->output("Vector"), horizontal->input("Vector1"));
  graph->connect(sine->output("Value"), horizontal->input("Scale"));
  MathNode *cosine = AddMath(graph, NODE_MATH_COSINE);
  graph->connect(clamped->output("Value"), cosine->input("Value1"));
  MathNode *down = AddMath(graph, NODE_MATH_MULTIPLY, -1.0f);
  graph->connect(cosine->output("Value"), down->input("Value1"));
  CombineXYZNode *vertical = graph->create_node<CombineXYZNode>();
  graph->connect(down->output("Value"), vertical->input("Z"));
  VectorMathNode *rebuilt = graph->create_node<VectorMathNode>();
  rebuilt->set_math_type(NODE_VECTOR_MATH_ADD);
  graph->connect(horizontal->output("Vector"), rebuilt->input("Vector1"));
  graph->connect(vertical->output("Vector"), rebuilt->input("Vector2"));
  return rebuilt->output("Vector");
}

/* UsdLux's shaping that Cycles' lights don't have, in the light's shader, along the direction
 * of emission (cos(theta) off the light's -Z axis):
 * - shaping:focus and shaping:focusTint, on every light: with f = |cos(theta)|^focus, the
 *   colour is scaled by lerp(focusTint, white, f);
 * - shaping:cone:angle and softness, on area lights (a sphere light takes them as a Cycles
 *   spot): 1 - smoothstep(theta, cutoff * (1 - softness), cutoff), 0 past the cutoff.
 * Returns the colour factor, or null when the light has none of them. */
ShaderOutput *Shaping(ShaderGraph *graph,
                      const HdContainerDataSourceHandle &lightContainer,
                      const Transform &tfm,
                      const bool cone)
{
  const float focus = GetTypedValue<float>(lightContainer, HdLightTokens->shapingFocus, 0.0f);
  const GfVec3f tint = GetTypedValue<GfVec3f>(
      lightContainer, HdLightTokens->shapingFocusTint, GfVec3f(0.0f));
  /* Karma shapes an area light only with a cone narrower than the hemisphere it already
   * lights: at 90 degrees or more, softness changes nothing, authored or not (compared with
   * softness alone, and with 90 and 0.5). */
  const float coneAngle = std::clamp(
      GetTypedValue<float>(lightContainer, HdLightTokens->shapingConeAngle, 90.0f), 0.0f, 180.0f);
  const float softness = std::clamp(
      GetTypedValue<float>(lightContainer, HdLightTokens->shapingConeSoftness, 0.0f), 0.0f, 1.0f);
  const bool hasCone = cone && coneAngle < 90.0f;
  if (focus <= 0.0f && !hasCone) {
    return nullptr;
  }

  GeometryNode *geometry = graph->create_node<GeometryNode>();
  VectorMathNode *cosine = graph->create_node<VectorMathNode>();
  cosine->set_math_type(NODE_VECTOR_MATH_DOT_PRODUCT);
  cosine->set_vector2(-normalize(transform_get_column(&tfm, 2)));
  graph->connect(geometry->output("Incoming"), cosine->input("Vector1"));

  ShaderOutput *factor = nullptr;
  if (focus > 0.0f) {
    MathNode *absolute = AddMath(graph, NODE_MATH_ABSOLUTE);
    graph->connect(cosine->output("Value"), absolute->input("Value1"));
    MathNode *power = AddMath(graph, NODE_MATH_POWER, focus);
    graph->connect(absolute->output("Value"), power->input("Value1"));
    VectorMathNode *lerp = graph->create_node<VectorMathNode>();
    lerp->set_math_type(NODE_VECTOR_MATH_MULTIPLY_ADD);
    lerp->set_vector2(make_float3(1.0f - tint[0], 1.0f - tint[1], 1.0f - tint[2]));
    lerp->set_vector3(make_float3(tint[0], tint[1], tint[2]));
    graph->connect(power->output("Value"), lerp->input("Vector1"));
    factor = lerp->output("Vector");
  }

  if (hasCone) {
    const float cutoff = GfDegreesToRadians(coneAngle);
    MathNode *theta = AddMath(graph, NODE_MATH_ARCCOSINE);
    graph->connect(cosine->output("Value"), theta->input("Value1"));
    ShaderOutput *attenuation;
    if (softness > 0.0f) {
      MapRangeNode *smooth = graph->create_node<MapRangeNode>();
      smooth->set_range_type(NODE_MAP_RANGE_SMOOTHSTEP);
      smooth->set_from_min(cutoff * (1.0f - softness));
      smooth->set_from_max(cutoff);
      smooth->set_to_min(1.0f);
      smooth->set_to_max(0.0f);
      smooth->set_clamp(true);
      graph->connect(theta->output("Value"), smooth->input("Value"));
      attenuation = smooth->output("Result");
    }
    else {
      MathNode *inside = AddMath(graph, NODE_MATH_LESS_THAN, cutoff);
      graph->connect(theta->output("Value"), inside->input("Value1"));
      attenuation = inside->output("Value");
    }
    if (factor) {
      VectorMathNode *scaled = graph->create_node<VectorMathNode>();
      scaled->set_math_type(NODE_VECTOR_MATH_SCALE);
      graph->connect(factor, scaled->input("Vector1"));
      graph->connect(attenuation, scaled->input("Scale"));
      factor = scaled->output("Vector");
    }
    else {
      factor = attenuation;
    }
  }
  return factor;
}

}  // namespace

/* UsdLux shaping:ies: the profile, looked up along the emission direction in the light's space
 * (and its angle scaled by shaping:ies:angleScale), scales the light; with
 * shaping:ies:normalize, divided by the profile's power. */
void HdCyclesLight::ApplyIes(ShaderGraph *graph,
                             const HdContainerDataSourceHandle &lightContainer,
                             const std::string &filename,
                             const Transform &tfm,
                             ShaderInput *strength)
{
  ShaderOutput *direction = IesDirection(graph, tfm);
  ShaderOutput *inRange = nullptr;
  const float angleScale = GetTypedValue<float>(
      lightContainer, HdLightTokens->shapingIesAngleScale, 0.0f);
  if (angleScale != 0.0f) {
    direction = ScaleIesAngle(graph, direction, angleScale, &inRange);
  }

  IESLightNode *iesNode = graph->create_node<IESLightNode>();
  iesNode->set_filename(ustring(filename));
  graph->connect(direction, iesNode->input("Vector"));

  /* Cycles loads a profile's candelas converted to watts per steradian (IESFile::parse, times
   * 4 pi / 177.83, for D65's luminous efficacy). UsdLux scales the light by the candelas
   * themselves, as Karma does; normalized, by the candelas over the profile's power, where the
   * conversion cancels. */
  constexpr float wattsPerSteradianPerCandela = 4.0f * M_PI_F / 177.83f;
  float scale = 1.0f / wattsPerSteradianPerCandela;
  if (GetTypedValue<bool>(lightContainer, HdLightTokens->shapingIesNormalize, false)) {
    string text;
    HdCyclesIesProfile profile;
    const float power = path_read_text(filename, text) && profile.load(text) ? profile.Power() :
                                                                               0.0f;
    if (power > 0.0f) {
      scale = 1.0f / power;
    }
    else {
      HDCYCLES_WARN("Could not normalize the IES profile '%s' of light '%s'",
                    filename.c_str(),
                    GetId().GetText());
    }
  }

  ShaderOutput *fac = iesNode->output("Fac");
  if (scale != 1.0f || inRange) {
    MathNode *scaled = AddMath(graph, NODE_MATH_MULTIPLY, scale);
    graph->connect(fac, scaled->input("Value1"));
    if (inRange) {
      graph->connect(inRange, scaled->input("Value2"));
      /* The constant scale moves to the IES node's own strength. */
      iesNode->set_strength(scale);
    }
    fac = scaled->output("Value");
  }
  graph->connect(fac, strength);
}

void HdCyclesLight::PopulateShaderGraph(const HdContainerDataSourceHandle &lightContainer)
{
  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();
  ShaderNode *outputNode = nullptr;

  if (_lightType == HdPrimTypeTokens->domeLight) {
    BackgroundNode *bgNode = graph->create_node<BackgroundNode>();
    // Bake strength into shader graph, since only the shader is used for background lights
    bgNode->set_color(_light->get_strength());

    graph->connect(bgNode->output("Background"), graph->output()->input("Surface"));

    outputNode = bgNode;
  }
  else if (lightContainer) {
    if (auto ds = HdStringDataSource::Cast(lightContainer->Get(_tokens->falloff))) {
      const std::string strVal = ds->GetTypedValue(0.0f);
      if (strVal == "Constant" || strVal == "Linear" || strVal == "Quadratic") {
        LightFalloffNode *lfoNode = graph->create_node<LightFalloffNode>();
        lfoNode->set_strength(1.f);
        graph->connect(lfoNode->output(strVal.c_str()), graph->output()->input("Surface"));
        outputNode = lfoNode;
      }
    }
  }

  if (outputNode == nullptr) {
    EmissionNode *emissionNode = graph->create_node<EmissionNode>();
    emissionNode->set_color(one_float3());
    emissionNode->set_strength(1.0f);

    graph->connect(emissionNode->output("Emission"), graph->output()->input("Surface"));

    outputNode = emissionNode;
  }

  bool hasSpatialVarying = false;
  bool hasColorTemperature = false;

  if (lightContainer) {
    const bool enableColorTemperature = GetTypedValue<bool>(
        lightContainer, HdLightTokens->enableColorTemperature, false);

    if (enableColorTemperature) {
      if (auto ds = HdFloatDataSource::Cast(lightContainer->Get(HdLightTokens->colorTemperature)))
      {
        BlackbodyNode *blackbodyNode = graph->create_node<BlackbodyNode>();
        blackbodyNode->set_temperature(ds->GetTypedValue(0.0f));

        if (_lightType == HdPrimTypeTokens->domeLight) {
          VectorMathNode *mathNode = graph->create_node<VectorMathNode>();
          mathNode->set_math_type(NODE_VECTOR_MATH_MULTIPLY);
          mathNode->set_vector2(_light->get_strength());

          graph->connect(blackbodyNode->output("Color"), mathNode->input("Vector1"));
          graph->connect(mathNode->output("Vector"), outputNode->input("Color"));
        }
        else {
          graph->connect(blackbodyNode->output("Color"), outputNode->input("Color"));
        }

        hasColorTemperature = true;
      }
    }

    if (auto ds = HdAssetPathDataSource::Cast(lightContainer->Get(HdLightTokens->shapingIesFile)))
    {
      const SdfAssetPath assetPath = ds->GetTypedValue(0.0f);
      std::string filename = assetPath.GetResolvedPath();
      if (filename.empty()) {
        filename = assetPath.GetAssetPath();
      }

      if (!filename.empty()) {
        ApplyIes(graph.get(),
                 lightContainer,
                 filename,
                 _object->get_tfm(),
                 outputNode->input("Strength"));
        hasSpatialVarying = true;
      }
    }

    if (auto ds = HdAssetPathDataSource::Cast(lightContainer->Get(HdLightTokens->textureFile))) {
      const SdfAssetPath assetPath = ds->GetTypedValue(0.0f);
      std::string filename = assetPath.GetResolvedPath();
      if (filename.empty()) {
        filename = assetPath.GetAssetPath();
      }

      if (!filename.empty()) {
        ImageSlotTextureNode *textureNode = nullptr;
        if (_lightType == HdPrimTypeTokens->domeLight) {
          /* UsdLux's latlong, as Karma maps it: +Y at the top of the image, and -Z at its
           * left edge, turning towards +X. Cycles' equirectangular map has +Z at the top and
           * -X at the left edge, so the dome's own directions go through (x, y, z) ->
           * (z, x, y) on their way to the texture. */
          Transform tfm = _object->get_tfm();
          transform_set_column(&tfm, 3, zero_float3());  // Remove translation
          const Transform toDome = make_transform(0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0);

          TextureCoordinateNode *coordNode = graph->create_node<TextureCoordinateNode>();
          coordNode->set_ob_tfm(tfm * toDome);
          coordNode->set_use_transform(true);

          textureNode = graph->create_node<EnvironmentTextureNode>();
          static_cast<EnvironmentTextureNode *>(textureNode)->set_filename(ustring(filename));

          graph->connect(coordNode->output("Object"), textureNode->input("Vector"));

          hasSpatialVarying = true;
        }
        else {
          /* The image as Karma lays it over the light, seen from the side it shines to:
           * Cycles' parametric u runs the other way, so it goes in as 1 - u. */
          GeometryNode *coordNode = graph->create_node<GeometryNode>();
          VectorMathNode *flip = graph->create_node<VectorMathNode>();
          flip->set_math_type(NODE_VECTOR_MATH_MULTIPLY_ADD);
          flip->set_vector2(make_float3(-1.0f, 1.0f, 1.0f));
          flip->set_vector3(make_float3(1.0f, 0.0f, 0.0f));
          graph->connect(coordNode->output("Parametric"), flip->input("Vector1"));

          textureNode = graph->create_node<ImageTextureNode>();
          static_cast<ImageTextureNode *>(textureNode)->set_filename(ustring(filename));

          graph->connect(flip->output("Vector"), textureNode->input("Vector"));
        }

        if (hasColorTemperature) {
          VectorMathNode *mathNode = graph->create_node<VectorMathNode>();
          mathNode->set_math_type(NODE_VECTOR_MATH_MULTIPLY);

          graph->connect(textureNode->output("Color"), mathNode->input("Vector1"));
          ShaderInput *const outputNodeInput = outputNode->input("Color");
          graph->connect(outputNodeInput->link, mathNode->input("Vector2"));
          graph->disconnect(outputNodeInput);
          graph->connect(mathNode->output("Vector"), outputNodeInput);
        }
        else if (_lightType == HdPrimTypeTokens->domeLight) {
          VectorMathNode *mathNode = graph->create_node<VectorMathNode>();
          mathNode->set_math_type(NODE_VECTOR_MATH_MULTIPLY);
          mathNode->set_vector2(_light->get_strength());

          graph->connect(textureNode->output("Color"), mathNode->input("Vector1"));
          graph->connect(mathNode->output("Vector"), outputNode->input("Color"));
        }
        else {
          graph->connect(textureNode->output("Color"), outputNode->input("Color"));
        }
      }
    }
  }

  /* Focus, and a cone on area lights, scale the emission's colour. */
  if (lightContainer && _lightType != HdPrimTypeTokens->domeLight &&
      _lightType != HdPrimTypeTokens->distantLight)
  {
    if (ShaderInput *color = outputNode->input("Color")) {
      if (ShaderOutput *factor = Shaping(graph.get(),
                                         lightContainer,
                                         _object->get_tfm(),
                                         _lightType != HdPrimTypeTokens->sphereLight))
      {
        VectorMathNode *shaped = graph->create_node<VectorMathNode>();
        shaped->set_math_type(NODE_VECTOR_MATH_MULTIPLY);
        if (ShaderOutput *source = color->link) {
          graph->disconnect(color);
          graph->connect(source, shaped->input("Vector1"));
        }
        else {
          shaped->set_vector1(static_cast<EmissionNode *>(outputNode)->get_color());
        }
        graph->connect(factor, shaped->input("Vector2"));
        graph->connect(shaped->output("Vector"), color);
        hasSpatialVarying = true;
      }
    }
  }

  Shader *const shader = _light->get_shader();
  shader->set_graph(std::move(graph));
  shader->tag_update((Scene *)_light->get_owner());

  shader->has_surface_spatial_varying = hasSpatialVarying;
}

void HdCyclesLight::Finalize(HdRenderParam *renderParam)
{
  if (!_light) {
    return;
  }

  const SceneLock lock(renderParam);
  const bool keep_nodes = static_cast<const HdCyclesSession *>(renderParam)->keep_nodes;

  /* The background outlives the dome that set its light group. */
  if (_lightType == HdPrimTypeTokens->domeLight) {
    lock.scene->background->set_lightgroup(ustring());
  }

  static_cast<HdCyclesSession *>(renderParam)->lightLinking.RemoveEmitter(_object);

  if (!keep_nodes) {
    /* Delete the object before the light, since the object references the light. */
    lock.scene->delete_node(_object);
    lock.scene->delete_node(_light);
  }

  _light = nullptr;
  _object = nullptr;
}

void HdCyclesLight::Initialize(HdRenderParam *renderParam)
{
  if (_light) {
    return;
  }

  const SceneLock lock(renderParam);

  _object = lock.scene->create_node<Object>();
  _object->name = GetId().GetString();

  if (_lightType == HdPrimTypeTokens->domeLight) {
    _light = lock.scene->create_node<BackgroundLight>();
  }
  else if (_lightType == HdPrimTypeTokens->distantLight) {
    _light = lock.scene->create_node<SunLight>();
  }
  else if (_lightType == HdPrimTypeTokens->diskLight) {
    _light = lock.scene->create_node<AreaLight>();
    static_cast<AreaLight *>(_light)->set_ellipse(true);
  }
  else if (_lightType == HdPrimTypeTokens->rectLight) {
    _light = lock.scene->create_node<AreaLight>();
    static_cast<AreaLight *>(_light)->set_ellipse(false);
  }
  else if (_lightType == HdPrimTypeTokens->sphereLight) {
    /* We can't know in advance if this is spot light or point light, so we set to derived class
     * SpotLight and change the type later. */
    _light = lock.scene->create_node<SpotLight>();
  }

  _light->set_use_mis(true);
  _light->name = GetId().GetString();

  _object->set_geometry(_light);
  _object->set_random_id(hash_uint2(hash_string(_light->name.c_str()), 0));
  _object->set_visibility(PATH_RAY_VISIBILITY_ALL & ~PATH_RAY_VISIBILITY_CAMERA);

  Shader *const shader = lock.scene->create_node<Shader>();
  array<Node *> used_shaders;
  used_shaders.push_back_slow(shader);
  _light->set_used_shaders(used_shaders);

  // Create default shader graph
  PopulateShaderGraph(HdContainerDataSourceHandle());
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
