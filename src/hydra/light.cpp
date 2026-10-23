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
#include "util/transform.h"

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

    /* Visible to the camera only when UsdLux says so. The cycles:light: settings can take
     * other rays away, and set the light's own Cycles settings. */
    unsigned visibility = PATH_RAY_VISIBILITY_ALL & ~PATH_RAY_VISIBILITY_CAMERA;
    if (GetTypedValue<bool>(lightDs, _tokens->visibleInPrimaryRay, false)) {
      visibility |= PATH_RAY_VISIBILITY_CAMERA;
    }
    /* Hydra hands a light's other attributes over as light parameters. */
    const HdCyclesSettingReader read = [sceneDelegate, &id](const char *name) {
      return sceneDelegate->GetLightParamValue(id, TfToken(std::string("cycles:light:") + name));
    };
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

      bool shaping = false;

      if (auto ds = GetTypedDataSource<float>(lightDs, HdLightTokens->shapingConeAngle)) {
        spot_light->set_angle(GfDegreesToRadians(ds->GetTypedValue(0.0f)) * 2.0f);
        shaping = true;
      }

      if (auto ds = GetTypedDataSource<float>(lightDs, HdLightTokens->shapingConeSoftness)) {
        spot_light->set_smooth(ds->GetTypedValue(0.0f));
        shaping = true;
      }

      _light->set_light_type(shaping ? LIGHT_SPOT : LIGHT_POINT);
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
    if (_lightType == HdPrimTypeTokens->domeLight &&
        !(lightLink.IsEmpty() && shadowLink.IsEmpty()))
    {
      HDCYCLES_WARN(
          "Cycles can't link a dome light: light:link and shadow:link on %s are ignored.",
          id.GetText());
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
        TextureCoordinateNode *coordNode = graph->create_node<TextureCoordinateNode>();
        coordNode->set_ob_tfm(_object->get_tfm());
        coordNode->set_use_transform(true);

        IESLightNode *iesNode = graph->create_node<IESLightNode>();
        iesNode->set_filename(ustring(filename));

        graph->connect(coordNode->output("Normal"), iesNode->input("Vector"));
        graph->connect(iesNode->output("Fac"), outputNode->input("Strength"));

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
          Transform tfm = _object->get_tfm();
          transform_set_column(&tfm, 3, zero_float3());  // Remove translation

          TextureCoordinateNode *coordNode = graph->create_node<TextureCoordinateNode>();
          coordNode->set_ob_tfm(tfm);
          coordNode->set_use_transform(true);

          textureNode = graph->create_node<EnvironmentTextureNode>();
          static_cast<EnvironmentTextureNode *>(textureNode)->set_filename(ustring(filename));

          graph->connect(coordNode->output("Object"), textureNode->input("Vector"));

          hasSpatialVarying = true;
        }
        else {
          GeometryNode *coordNode = graph->create_node<GeometryNode>();

          textureNode = graph->create_node<ImageTextureNode>();
          static_cast<ImageTextureNode *>(textureNode)->set_filename(ustring(filename));

          graph->connect(coordNode->output("Parametric"), textureNode->input("Vector"));
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
