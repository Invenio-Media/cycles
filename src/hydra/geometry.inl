/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/geometry.h"
#include "hydra/instancer.h"
#include "hydra/material.h"
#include "hydra/object_settings.h"
#include "hydra/session.h"
#include "hydra/util.h"
#include "scene/attribute.h"
#include "scene/geometry.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "util/hash.h"

#include <pxr/imaging/hd/categoriesSchema.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/xformSchema.h>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

HDCYCLES_NAMESPACE_OPEN_SCOPE

extern Transform convert_transform(const GfMatrix4d &matrix);

template<typename Base, typename CyclesBase>
HdCyclesGeometry<Base, CyclesBase>::HdCyclesGeometry(const SdfPath &rprimId)
    : Base(rprimId), _geomTransform(1.0)
{
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::_InitRepr(const TfToken &reprToken,
                                                   HdDirtyBits *dirtyBits)
{
  TF_UNUSED(reprToken);
  TF_UNUSED(dirtyBits);
}

template<typename Base, typename CyclesBase>
HdDirtyBits HdCyclesGeometry<Base, CyclesBase>::GetInitialDirtyBitsMask() const
{
  return HdChangeTracker::DirtyPrimID | HdChangeTracker::DirtyTransform |
         HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyVisibility |
         HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyCategories;
}

template<typename Base, typename CyclesBase>
HdDirtyBits HdCyclesGeometry<Base, CyclesBase>::_PropagateDirtyBits(HdDirtyBits bits) const
{
  return bits;
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::Sync(HdSceneDelegate *sceneDelegate,
                                              HdRenderParam *renderParam,
                                              HdDirtyBits *dirtyBits,
                                              const TfToken &reprToken)
{
  TF_UNUSED(reprToken);

  if (*dirtyBits == HdChangeTracker::Clean) {
    return;
  }

  Initialize(renderParam);

  Base::_UpdateInstancer(sceneDelegate, dirtyBits);
  HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), Base::GetInstancerId());
  Base::_UpdateVisibility(sceneDelegate, dirtyBits);

  const SceneLock lock(renderParam);

  const SdfPath &id = Base::GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
  const HdContainerDataSourceHandle &primDs = prim.dataSource;

  if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
    SdfPath materialId;
    /* Purpose here matches #HdCyclesDelegate::GetMaterialBindingPurpose. */
    if (auto pathDs =
            HdMaterialBindingsSchema::GetFromParent(primDs).GetMaterialBinding().GetPath())
    {
      materialId = pathDs->GetTypedValue(0.0f);
    }
    Base::SetMaterialId(materialId);

    const auto *const material = static_cast<const HdCyclesMaterial *>(
        sceneDelegate->GetRenderIndex().GetSprim(HdPrimTypeTokens->material,
                                                 Base::GetMaterialId()));

    array<Node *> usedShaders(1);
    if (material && material->GetCyclesShader()) {
      usedShaders[0] = material->GetCyclesShader();
    }
    else {
      usedShaders[0] = DefaultSurface(lock.scene, primDs, renderParam);
    }

    for (Node *shader : usedShaders) {
      static_cast<Shader *>(shader)->tag_used(lock.scene);
    }

    _geom->set_used_shaders(usedShaders);
  }

  if (HdChangeTracker::IsPrimIdDirty(*dirtyBits, id)) {
    // Offset by one, which the AOV read-back takes off again (HdCyclesAovPass::isId)
    for (Object *const instance : _instances) {
      instance->set_pass_id(Base::GetPrimId() + 1);
    }
  }

  /* The transform at the frame, and at evenly spaced times across the shutter for motion blur.
   * primvars:cycles:object:motion_blur (on by default) and motion_steps (3 by default; odd, as
   * Cycles centres the samples) set how many; changing either resamples. */
  const HdCyclesShutter &shutter = static_cast<HdCyclesSession *>(renderParam)->shutter;
  int motionSteps = 1;
  if (shutter.IsOpen()) {
    static const TfToken motionBlurToken("cycles:object:motion_blur");
    static const TfToken motionStepsToken("cycles:object:motion_steps");
    const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(primDs);
    if (HdCyclesSettingValue<bool>(ReadPrimvar(primvars, motionBlurToken), true)) {
      motionSteps = std::clamp(
                        HdCyclesSettingValue<int>(ReadPrimvar(primvars, motionStepsToken), 3),
                        1,
                        int(Object::MAX_MOTION_STEPS)) |
                    1;
    }
  }
  const bool motionChanged = motionSteps != _motionSteps;
  _motionSteps = motionSteps;
  _shutterOpen = shutter.IsOpen() ? shutter.open : 0.0f;
  _shutterClose = shutter.IsOpen() ? shutter.close : 0.0f;
  _metersPerUnit = static_cast<HdCyclesSession *>(renderParam)->GetStageMetersPerUnit();
  /* Deforming points sample across the shutter with the same steps. */
  if (motionChanged) {
    *dirtyBits |= HdChangeTracker::DirtyPoints;
  }

  if (HdChangeTracker::IsTransformDirty(*dirtyBits, id) || motionChanged) {
    _geomTransform = GfMatrix4d(1.0);
    _geomMotion.clear();
    if (auto matrixDs = HdXformSchema::GetFromParent(primDs).GetMatrix()) {
      _geomTransform = matrixDs->GetTypedValue(0.0f);
      const std::vector<float> times = MotionSampleTimes(
          matrixDs, shutter.open, shutter.close, motionSteps);
      if (times.size() > 1) {
        for (const float time : times) {
          _geomMotion.push_back(matrixDs->GetTypedValue(time));
        }
      }
    }
  }

  if (HdChangeTracker::IsTransformDirty(*dirtyBits, id) ||
      HdChangeTracker::IsInstancerDirty(*dirtyBits, id) || motionChanged)
  {
    auto *const instancer = static_cast<HdCyclesInstancer *>(
        sceneDelegate->GetRenderIndex().GetInstancer(Base::GetInstancerId()));

    VtMatrix4dArray transforms;
    if (instancer) {
      transforms = instancer->ComputeInstanceTransforms(id);
    }
    else {
      // Default to a single instance with an identity transform
      transforms.push_back(GfMatrix4d(1.0));
    }

    /* An instancer can give the prim no instances (a prototype it doesn't use, or a hidden
     * native instance). Keep the first object, which the rest of the sync writes to, at the
     * prim's own transform, and hide it below. */
    _hasInstances = !transforms.empty();
    if (!_hasInstances) {
      transforms.push_back(GfMatrix4d(1.0));
    }

    const size_t oldSize = _instances.size();
    const size_t newSize = transforms.size();

    // Resize instance list
    HdCyclesLightLinking &lightLinking = static_cast<HdCyclesSession *>(renderParam)->lightLinking;
    for (size_t i = newSize; i < oldSize; ++i) {
      lightLinking.RemoveObject(_instances[i]);
      lock.scene->delete_node(_instances[i]);
    }
    _instances.resize(newSize);
    for (size_t i = oldSize; i < newSize; ++i) {
      _instances[i] = lock.scene->create_node<Object>();
      InitializeInstance(static_cast<int>(i));
    }

    /* The instances across the shutter when the instancer moves (its transform, instance
     * primvars or velocities), at the times the prim's own transform is sampled. Each step reads
     * the instancer and its parents afresh, per prototype: simple, and cheap at the motion steps
     * used. Caching per sync is the place to start if large nested instancers get slow. */
    std::vector<VtMatrix4dArray> instanceMotion;
    if (instancer && motionSteps > 1 && instancer->MovesAcross(_shutterOpen, _shutterClose)) {
      for (const float time : ShutterTimes(_shutterOpen, _shutterClose, motionSteps)) {
        instanceMotion.push_back(instancer->ComputeInstanceTransforms(id, time));
        if (instanceMotion.back().size() != transforms.size()) {
          /* Instances come or go within the shutter: no motion to give them. */
          instanceMotion.clear();
          break;
        }
      }
    }
    /* Both are sampled at the prim's motion steps when they move. */
    if (!_geomMotion.empty() && !instanceMotion.empty() &&
        !TF_VERIFY(_geomMotion.size() == instanceMotion.size()))
    {
      instanceMotion.clear();
    }
    const size_t steps = std::max(_geomMotion.size(), instanceMotion.size());

    // Update transforms of all instances, with their motion across the shutter.
    const float metersPerUnit =
        static_cast<HdCyclesSession *>(renderParam)->GetStageMetersPerUnit();
    const Transform scale = transform_scale(make_float3(metersPerUnit));
    for (size_t i = 0; i < transforms.size(); ++i) {
      _instances[i]->set_tfm(scale * convert_transform(_geomTransform * transforms[i]));

      array<Transform> motion(steps);
      for (size_t step = 0; step < steps; ++step) {
        const GfMatrix4d &prim = _geomMotion.empty() ? _geomTransform : _geomMotion[step];
        const GfMatrix4d &instance = instanceMotion.empty() ? transforms[i] :
                                                              instanceMotion[step][i];
        motion[step] = scale * convert_transform(prim * instance);
      }
      _instances[i]->set_motion(motion);
    }
  }

  /* Light-linking categories, the prim's and each instance's (hydra/light_linking.h). The
   * instances are rebuilt when the transforms or the instancer change. */
  if ((*dirtyBits & HdChangeTracker::DirtyCategories) ||
      HdChangeTracker::IsTransformDirty(*dirtyBits, id) ||
      HdChangeTracker::IsInstancerDirty(*dirtyBits, id))
  {
    const VtArray<TfToken> own =
        HdCategoriesSchema::GetFromParent(primDs).GetIncludedCategoryNames();
    auto *const instancer = static_cast<HdCyclesInstancer *>(
        sceneDelegate->GetRenderIndex().GetInstancer(Base::GetInstancerId()));
    const std::vector<VtArray<TfToken>> perInstance = instancer ?
                                                          instancer->ComputeInstanceCategories(
                                                              id) :
                                                          std::vector<VtArray<TfToken>>();

    HdCyclesLightLinking &lightLinking = static_cast<HdCyclesSession *>(renderParam)->lightLinking;
    for (size_t i = 0; i < _instances.size(); ++i) {
      VtArray<TfToken> categories = own;
      if (i < perInstance.size()) {
        for (const TfToken &category : perInstance[i]) {
          categories.push_back(category);
        }
      }
      lightLinking.SetObjectCategories(_instances[i], std::move(categories));
    }
  }

  /* Visibility and the cycles:object: primvars, on every sync: they can change with the
   * primvars or the visibility, and new instances need them. Setting an unchanged value does
   * not tag the object. */
  {
    const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(primDs);
    const HdCyclesSettingReader read = [&primvars](const char *name) {
      return ReadPrimvar(primvars, TfToken(std::string("cycles:object:") + name));
    };
    auto *const instancer = static_cast<HdCyclesInstancer *>(
        sceneDelegate->GetRenderIndex().GetInstancer(Base::GetInstancerId()));
    const bool visible = _hasInstances && Base::IsVisible() &&
                         (!instancer || instancer->IsVisible());
    /* Volume density per stage unit, as Karma has it: the scene is in metres here, where
     * density is per metre. A volume in object space has it per object unit, which Cycles'
     * own factor already gives. Read from the primvar, as the volume sync sets its space after
     * this, with the same parser. */
    const bool objectSpace = _geom->is_volume() && HdCyclesVolumeObjectSpace(read("volume:space"));
    const float densityScale = objectSpace ? 1.0f : 1.0f / _metersPerUnit;
    /* A mesh light, as Karma renders it: hidden from the camera, unless
     * cycles:object:visibility:camera (or the light's visibleInPrimaryRay) asks for it, but
     * casting shadows. */
    unsigned visibility = visible ? PATH_RAY_VISIBILITY_ALL : 0;
    if (const HdContainerDataSourceHandle lightDs =
            HdLightSchema::GetFromParent(primDs).GetContainer())
    {
      static const TfToken visibleInPrimaryRay("visibleInPrimaryRay");
      visibility &= ~PATH_RAY_VISIBILITY_CAMERA;
      if (visible &&
          HdCyclesSettingValue<bool>(read("visibility:camera"),
                                     GetTypedValue<bool>(lightDs, visibleInPrimaryRay, false)))
      {
        visibility |= PATH_RAY_VISIBILITY_CAMERA;
      }
    }
    for (Object *instance : _instances) {
      HdCyclesApplyObjectSettings(instance, visibility, read);
      instance->set_volume_density_scale(densityScale);
    }
  }

  // Must happen after material ID update, so that attribute decisions can be made
  // based on it (e.g. check whether an attribute is actually needed)
  bool rebuild = false;
  Populate(sceneDelegate, *dirtyBits, rebuild);
  rebuild = rebuild || std::exchange(_motionRebuild, false);

  /* After the material and its subsets, which a mesh light's shader replaces. */
  UpdateMeshLight(sceneDelegate, lock.scene, primDs, renderParam);

  if (_geom->is_modified() || rebuild) {
    _geom->tag_update(lock.scene, rebuild);
  }

  for (Object *instance : _instances) {
    instance->tag_update(lock.scene);
  }

  *dirtyBits = HdChangeTracker::Clean;
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::Finalize(HdRenderParam *renderParam)
{
  if (!_geom && _instances.empty()) {
    return;
  }

  const SceneLock lock(renderParam);
  const bool keep_nodes = static_cast<const HdCyclesSession *>(renderParam)->keep_nodes;

  /* Delete the instances before the geometry, since the objects reference the geometry. */
  HdCyclesLightLinking &lightLinking = static_cast<HdCyclesSession *>(renderParam)->lightLinking;
  for (Object *instance : _instances) {
    lightLinking.RemoveObject(instance);
  }
  if (!keep_nodes) {
    lock.scene->delete_nodes(set<Object *>(_instances.begin(), _instances.end()));
  }
  _instances.clear();
  _instances.shrink_to_fit();

  if (!keep_nodes) {
    lock.scene->delete_node(_geom);
    if (_lightShader) {
      lock.scene->delete_node(_lightShader);
    }
  }
  _geom = nullptr;
  _lightShader = nullptr;
  _lightKey.clear();
  static_cast<HdCyclesSession *>(renderParam)->SetMeshLight(this, false);
}

template<typename Base, typename CyclesBase>
float HdCyclesGeometry<Base, CyclesBase>::SurfaceArea() const
{
  if constexpr (std::is_same_v<CyclesBase, Mesh>) {
    const Attribute *attr_P = _geom->attributes.find(ATTR_STD_POSITION);
    if (!attr_P) {
      return 0.0f;
    }
    const packed_float3 *verts = attr_P->data<packed_float3>();
    const array<int> &triangles = _geom->get_triangles();
    double area = 0.0;
    for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
      GfVec3d p[3];
      for (int k = 0; k < 3; ++k) {
        const packed_float3 v = verts[triangles[i + k]];
        p[k] = _geomTransform.Transform(GfVec3d(v.x, v.y, v.z));
      }
      area += 0.5 * GfCross(p[1] - p[0], p[2] - p[0]).GetLength();
    }
    return float(area);
  }
  else {
    return 0.0f;
  }
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::UpdateMeshLight(HdSceneDelegate *sceneDelegate,
                                                         Scene *scene,
                                                         const HdContainerDataSourceHandle &primDs,
                                                         HdRenderParam *renderParam)
{
  auto *const session = static_cast<HdCyclesSession *>(renderParam);
  const HdContainerDataSourceHandle lightDs = HdLightSchema::GetFromParent(primDs).GetContainer();
  if (!lightDs) {
    if (_lightShader) {
      /* No longer a light: back to the prim's material. Geometry subsets get theirs back with
       * the next topology change. */
      const auto *const material = static_cast<const HdCyclesMaterial *>(
          sceneDelegate->GetRenderIndex().GetSprim(HdPrimTypeTokens->material,
                                                   Base::GetMaterialId()));
      array<Node *> usedShaders(1);
      usedShaders[0] = material && material->GetCyclesShader() ?
                           material->GetCyclesShader() :
                           DefaultSurface(scene, primDs, renderParam);
      static_cast<Shader *>(usedShaders[0])->tag_used(scene);
      _geom->set_used_shaders(usedShaders);
      scene->delete_node(_lightShader);
      _lightShader = nullptr;
      _lightKey.clear();
      session->SetMeshLight(this, false);
    }
    return;
  }

  /* The emitted radiance, as for UsdLux's other area lights: colour, intensity and exposure,
   * the colour temperature's colour, and with normalize, divided by the surface area, in
   * stage units. */
  const GfVec3f color = GetTypedValue<GfVec3f>(lightDs, HdLightTokens->color, GfVec3f(1.0f));
  float radiance = GetTypedValue<float>(lightDs, HdLightTokens->intensity, 1.0f) *
                   std::exp2(GetTypedValue<float>(lightDs, HdLightTokens->exposure, 0.0f));
  const bool temperature = GetTypedValue<bool>(
      lightDs, HdLightTokens->enableColorTemperature, false);
  const float kelvin = GetTypedValue<float>(lightDs, HdLightTokens->colorTemperature, 6500.0f);
  if (GetTypedValue<bool>(lightDs, HdLightTokens->normalize, false)) {
    const float area = SurfaceArea();
    if (area > 0.0f) {
      radiance /= area;
    }
  }

  const std::string key = TfStringPrintf(
      "%g %g %g %g %d %g", color[0], color[1], color[2], radiance, int(temperature), kelvin);
  if (!_lightShader) {
    _lightShader = scene->create_node<Shader>();
    _lightShader->name = ustring(Base::GetId().GetString() + ".light");
    session->SetMeshLight(this, true);
  }
  if (key != _lightKey) {
    _lightKey = key;
    unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();
    EmissionNode *emission = graph->create_node<EmissionNode>();
    emission->set_color(make_float3(color[0], color[1], color[2]));
    emission->set_strength(radiance);
    if (temperature) {
      BlackbodyNode *blackbody = graph->create_node<BlackbodyNode>();
      blackbody->set_temperature(kelvin);
      VectorMathNode *tint = graph->create_node<VectorMathNode>();
      tint->set_math_type(NODE_VECTOR_MATH_MULTIPLY);
      tint->set_vector2(make_float3(color[0], color[1], color[2]));
      graph->connect(blackbody->output("Color"), tint->input("Vector1"));
      graph->connect(tint->output("Vector"), emission->input("Color"));
    }
    graph->connect(emission->output("Emission"), graph->output()->input("Surface"));
    _lightShader->set_graph(std::move(graph));
    _lightShader->tag_update(scene);
  }

  /* Every face emits, whatever its material or subset. */
  array<Node *> usedShaders(std::max<size_t>(_geom->get_used_shaders().size(), 1));
  for (Node *&shader : usedShaders) {
    shader = _lightShader;
  }
  if (!(usedShaders == _geom->get_used_shaders())) {
    _lightShader->tag_used(scene);
    _geom->set_used_shaders(usedShaders);
  }
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::Initialize(HdRenderParam *renderParam)
{
  if (_geom) {
    return;
  }

  const SceneLock lock(renderParam);

  // Create geometry
  _geom = lock.scene->create_node<CyclesBase>();
  _geom->name = Base::GetId().GetString();

  // Create default instance
  _instances.push_back(lock.scene->create_node<Object>());
  InitializeInstance(0);
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::InitializeInstance(int index)
{
  Object *instance = _instances[index];
  instance->set_geometry(_geom);
  /* Cryptomatte's object and asset IDs and manifests come from these names. Every instance of
   * a prim is that prim; its asset is the prim's parent. */
  instance->name = _geom->name;
  instance->set_asset_name(ustring(Base::GetId().GetParentPath().GetString()));

  instance->attributes.emplace_back(HdAovTokens->instanceId.GetString(),
                                    static_cast<float>(index + 1));
  instance->set_pass_id(Base::GetPrimId() + 1);

  instance->set_color(make_float3(_displayColor[0], _displayColor[1], _displayColor[2]));
  instance->set_random_id(hash_uint2(hash_string(_geom->name.c_str()), index));
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::SetDisplayColor(const GfVec3f &color)
{
  _displayColor = color;
  for (Object *instance : _instances) {
    instance->set_color(make_float3(color[0], color[1], color[2]));
  }
}

template<typename Base, typename CyclesBase>
Shader *HdCyclesGeometry<Base, CyclesBase>::DefaultSurface(
    Scene *scene, const HdContainerDataSourceHandle &primDs, HdRenderParam *renderParam) const
{
  /* A volume without a material renders its density grid, as Karma and Blender do; the
   * default surface has no volume part and would leave it black. */
  if (_geom->is_volume()) {
    return scene->default_volume;
  }
  Shader *const displayColorSurface =
      static_cast<HdCyclesSession *>(renderParam)->displayColorSurface;
  const HdInterpolation interpolation = ReadPrimvarInterpolation(
      HdPrimvarsSchema::GetFromParent(primDs), HdTokens->displayColor);
  if (displayColorSurface && interpolation != HdInterpolationConstant &&
      interpolation != HdInterpolationCount)
  {
    return displayColorSurface;
  }
  return scene->default_surface;
}

template<typename Base, typename CyclesBase>
std::vector<VtVec3fArray> HdCyclesGeometry<Base, CyclesBase>::SamplePoints(
    const HdPrimvarsSchema &primvars) const
{
  std::vector<VtVec3fArray> samples;
  const HdSampledDataSourceHandle valueDs =
      primvars.GetPrimvar(HdTokens->points).GetPrimvarValue();
  if (!valueDs) {
    return samples;
  }
  /* With UsdGeom velocities, HdsiVelocityMotionResolvingSceneIndex has already turned them into
   * points that change over time. */
  for (const float time : MotionSampleTimes(valueDs, _shutterOpen, _shutterClose, _motionSteps)) {
    const VtValue value = valueDs->GetValue(time);
    if (!value.IsHolding<VtVec3fArray>()) {
      return {};
    }
    samples.push_back(value.UncheckedGet<VtVec3fArray>());
  }
  return samples;
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::ApplyPointsMotion(
    Attribute *attr_P, const std::vector<VtVec3fArray> &samples, const size_t count)
{
  if (!attr_P) {
    return;
  }
  const int oldSteps = attr_P->num_motion_steps();
  /* The motion steps are odd (Sync), so the middle sample, the position, is Cycles' centre. */
  const bool moves = samples.size() > 1 && TF_VERIFY(samples.size() % 2 == 1) &&
                     std::all_of(samples.begin(), samples.end(), [count](const VtVec3fArray &s) {
                       return s.size() >= count;
                     });
  if (moves) {
    const int steps = int(samples.size());
    _geom->set_motion_steps(steps);
    _geom->set_use_motion_blur(true);
    attr_P->add_motion(_geom);
    static_assert(sizeof(GfVec3f) == sizeof(packed_float3));
    for (int step = 0; step < steps; ++step) {
      const int attrStep = attr_P->time_step_to_attr_step(step, steps);
      if (attrStep != 0) {
        std::copy_n(reinterpret_cast<const packed_float3 *>(samples[step].data()),
                    count,
                    attr_P->data_for_write<packed_float3>(attrStep));
      }
    }
  }
  else {
    attr_P->remove_motion();
    _geom->set_use_motion_blur(false);
  }
  if (attr_P->num_motion_steps() != oldSteps) {
    _motionRebuild = true;
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
