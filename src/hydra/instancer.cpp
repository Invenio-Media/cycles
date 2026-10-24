/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/instancer.h"
#include "hydra/util.h"

#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/imaging/hd/categoriesSchema.h>
#include <pxr/imaging/hd/instanceCategoriesSchema.h>
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/xformSchema.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesInstancer::HdCyclesInstancer(HdSceneDelegate *delegate, const SdfPath &instancerId)
    : HdInstancer(delegate, instancerId)
{
}

HdCyclesInstancer::~HdCyclesInstancer() = default;

void HdCyclesInstancer::Sync(HdSceneDelegate *sceneDelegate,
                             HdRenderParam * /*renderParam*/,
                             HdDirtyBits *dirtyBits)
{
  _UpdateInstancer(sceneDelegate, dirtyBits);

  if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId())) {
    /* The primvars are read at the times they're needed (ComputeInstanceTransforms). */
    sceneDelegate->GetRenderIndex().GetChangeTracker().MarkInstancerClean(GetId());
  }
}

namespace {

/* The data source of an instancer primvar's value, or null. */
HdSampledDataSourceHandle InstancePrimvar(const HdPrimvarsSchema &primvars, const TfToken &name)
{
  if (HdPrimvarSchema pv = primvars.GetPrimvar(name)) {
    return pv.GetPrimvarValue();
  }
  return nullptr;
}

template<typename T>
T ReadInstancePrimvar(const HdPrimvarsSchema &primvars, const TfToken &name, const float time)
{
  if (const HdSampledDataSourceHandle ds = InstancePrimvar(primvars, name)) {
    const VtValue v = ds->GetValue(time);
    if (v.IsHolding<T>()) {
      return v.UncheckedGet<T>();
    }
  }
  return T();
}

/* Rotations as (real, imaginary) vectors, from any of the quaternion array types. */
VtVec4fArray ReadInstanceRotations(const HdPrimvarsSchema &primvars, const float time)
{
  VtVec4fArray rotations;
  const HdSampledDataSourceHandle ds = InstancePrimvar(primvars,
                                                       HdInstancerTokens->instanceRotations);
  if (!ds) {
    return rotations;
  }
  const VtValue v = ds->GetValue(time);
  if (v.IsHolding<VtVec4fArray>()) {
    return v.UncheckedGet<VtVec4fArray>();
  }
  auto append = [&rotations](const auto &src) {
    rotations.reserve(src.size());
    for (const auto &q : src) {
      const auto &im = q.GetImaginary();
      rotations.push_back(GfVec4f(q.GetReal(), im[0], im[1], im[2]));
    }
  };
  if (v.IsHolding<VtQuatfArray>()) {
    append(v.UncheckedGet<VtQuatfArray>());
  }
  else if (v.IsHolding<VtQuathArray>()) {
    append(v.UncheckedGet<VtQuathArray>());
  }
  return rotations;
}

}  // namespace

bool HdCyclesInstancer::MovesAcross(const float shutterOpen, const float shutterClose)
{
  const HdSceneIndexPrim prim = GetPrim(GetDelegate(), GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const HdSampledDataSourceHandle sources[] = {
      HdXformSchema::GetFromParent(prim.dataSource).GetMatrix(),
      InstancePrimvar(primvars, HdInstancerTokens->instanceTranslations),
      InstancePrimvar(primvars, HdInstancerTokens->instanceRotations),
      InstancePrimvar(primvars, HdInstancerTokens->instanceScales),
      InstancePrimvar(primvars, HdInstancerTokens->instanceTransforms),
  };
  for (const HdSampledDataSourceHandle &ds : sources) {
    if (MotionSampleTimes(ds, shutterOpen, shutterClose, 3).size() > 1) {
      return true;
    }
  }
  HdCyclesInstancer *const parentInstancer = GetParentInstancer(prim.dataSource);
  return parentInstancer && parentInstancer->MovesAcross(shutterOpen, shutterClose);
}

bool HdCyclesInstancer::IsVisible()
{
  const HdSceneIndexPrim prim = GetPrim(GetDelegate(), GetId());
  if (const HdBoolDataSourceHandle visibility =
          HdVisibilitySchema::GetFromParent(prim.dataSource).GetVisibility())
  {
    if (!visibility->GetTypedValue(0.0f)) {
      return false;
    }
  }
  HdCyclesInstancer *const parentInstancer = GetParentInstancer(prim.dataSource);
  return !parentInstancer || parentInstancer->IsVisible();
}

VtMatrix4dArray HdCyclesInstancer::ComputeInstanceTransforms(const SdfPath &prototypeId,
                                                             const float time)
{
  HdSceneDelegate *const sceneDelegate = GetDelegate();
  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);

  HdInstancerTopologySchema topology = HdInstancerTopologySchema::GetFromParent(prim.dataSource);
  const VtIntArray instanceIndices = topology ?
                                         topology.ComputeInstanceIndicesForProto(prototypeId) :
                                         VtIntArray();

  GfMatrix4d instanceTransform(1.0);
  if (auto matrixDs = HdXformSchema::GetFromParent(prim.dataSource).GetMatrix()) {
    instanceTransform = matrixDs->GetTypedValue(time);
  }

  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const auto translate = ReadInstancePrimvar<VtVec3fArray>(
      primvars, HdInstancerTokens->instanceTranslations, time);
  const VtVec4fArray rotate = ReadInstanceRotations(primvars, time);
  const auto scale = ReadInstancePrimvar<VtVec3fArray>(
      primvars, HdInstancerTokens->instanceScales, time);
  const auto transformsPrimvar = ReadInstancePrimvar<VtMatrix4dArray>(
      primvars, HdInstancerTokens->instanceTransforms, time);

  VtMatrix4dArray transforms;
  transforms.reserve(instanceIndices.size());

  /* GfMatrix4d transforms row vectors, so a product applies its left factor first. Hydra's
   * order: the instance's own transform, then scale, rotation, translation, and the
   * instancer's transform. */
  for (const int index : instanceIndices) {
    GfMatrix4d transform(1.0);

    if (index < transformsPrimvar.size()) {
      transform = transformsPrimvar[index];
    }

    if (index < scale.size()) {
      GfMatrix4d scaleMat(1);
      scaleMat.SetScale(scale[index]);
      transform *= scaleMat;
    }

    if (index < rotate.size()) {
      GfMatrix4d rotateMat(1);
      const GfVec4f &quat = rotate[index];
      rotateMat.SetRotate(GfQuatd(quat[0], quat[1], quat[2], quat[3]));
      transform *= rotateMat;
    }

    if (index < translate.size()) {
      GfMatrix4d translateMat(1);
      translateMat.SetTranslate(translate[index]);
      transform *= translateMat;
    }

    transforms.push_back(transform * instanceTransform);
  }

  /* Recurse into the parent instancer, at the same time. */
  if (HdCyclesInstancer *const parentInstancer = GetParentInstancer(prim.dataSource)) {
    VtMatrix4dArray resultTransforms;
    for (const GfMatrix4d &parentTransform : parentInstancer->ComputeInstanceTransforms(id, time))
    {
      for (const GfMatrix4d &localTransform : transforms) {
        resultTransforms.push_back(localTransform * parentTransform);
      }
    }
    return resultTransforms;
  }

  return transforms;
}

std::vector<VtArray<TfToken>> HdCyclesInstancer::ComputeInstanceCategories(
    const SdfPath &prototypeId)
{
  HdSceneDelegate *const sceneDelegate = GetDelegate();
  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);

  HdInstancerTopologySchema topology = HdInstancerTopologySchema::GetFromParent(prim.dataSource);
  const VtIntArray instanceIndices = topology ?
                                         topology.ComputeInstanceIndicesForProto(prototypeId) :
                                         VtIntArray();

  const VtArray<TfToken> own =
      HdCategoriesSchema::GetFromParent(prim.dataSource).GetIncludedCategoryNames();
  const HdVectorDataSourceHandle perInstance =
      HdInstanceCategoriesSchema::GetFromParent(prim.dataSource).GetCategoriesValues();

  std::vector<VtArray<TfToken>> categories;
  categories.reserve(instanceIndices.size());
  for (const int index : instanceIndices) {
    VtArray<TfToken> instance = own;
    if (perInstance && size_t(index) < perInstance->GetNumElements()) {
      HdCategoriesSchema schema(HdContainerDataSource::Cast(perInstance->GetElement(index)));
      for (const TfToken &category : schema.GetIncludedCategoryNames()) {
        instance.push_back(category);
      }
    }
    categories.push_back(std::move(instance));
  }

  /* Recurse into the parent instancer, as for the transforms. */
  if (HdCyclesInstancer *const parentInstancer = GetParentInstancer(prim.dataSource)) {
    std::vector<VtArray<TfToken>> result;
    for (const VtArray<TfToken> &parent : parentInstancer->ComputeInstanceCategories(id)) {
      for (const VtArray<TfToken> &local : categories) {
        VtArray<TfToken> combined = parent;
        for (const TfToken &category : local) {
          combined.push_back(category);
        }
        result.push_back(std::move(combined));
      }
    }
    return result;
  }

  return categories;
}

HdCyclesInstancer *HdCyclesInstancer::GetParentInstancer(const HdContainerDataSourceHandle &primDs)
{
  HdInstancedBySchema instancedBy = HdInstancedBySchema::GetFromParent(primDs);
  SdfPath parentId;
  if (auto pathsDs = instancedBy.GetPaths()) {
    const VtArray<SdfPath> parentPaths = pathsDs->GetTypedValue(0.0f);
    if (!parentPaths.empty()) {
      parentId = parentPaths.front();
    }
  }
  if (parentId.IsEmpty()) {
    parentId = GetParentId();
  }
  if (parentId.IsEmpty()) {
    return nullptr;
  }
  return static_cast<HdCyclesInstancer *>(GetDelegate()->GetRenderIndex().GetInstancer(parentId));
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
