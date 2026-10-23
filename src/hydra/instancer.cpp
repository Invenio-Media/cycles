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
    SyncPrimvars();
  }
}

void HdCyclesInstancer::SyncPrimvars()
{
  HdSceneDelegate *const sceneDelegate = GetDelegate();
  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
  HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);

  auto read_array_primvar = [&](const TfToken &name, auto &out) {
    using T = std::remove_reference_t<decltype(out)>;
    if (HdPrimvarSchema pv = primvars.GetPrimvar(name)) {
      if (auto valueDs = pv.GetPrimvarValue()) {
        const VtValue v = valueDs->GetValue(0.0f);
        if (v.IsHolding<T>()) {
          out = v.UncheckedGet<T>();
        }
      }
    }
  };

  read_array_primvar(HdInstancerTokens->instanceTranslations, _translate);
  read_array_primvar(HdInstancerTokens->instanceScales, _scale);
  read_array_primvar(HdInstancerTokens->instanceTransforms, _instanceTransform);

  /* Accept different array types for quaternions. */
  if (HdPrimvarSchema pv = primvars.GetPrimvar(HdInstancerTokens->instanceRotations)) {
    if (auto valueDs = pv.GetPrimvarValue()) {
      const VtValue v = valueDs->GetValue(0.0f);
      if (v.IsHolding<VtVec4fArray>()) {
        _rotate = v.UncheckedGet<VtVec4fArray>();
      }
      else if (v.IsHolding<VtQuatfArray>()) {
        const VtQuatfArray &src = v.UncheckedGet<VtQuatfArray>();
        _rotate.clear();
        _rotate.reserve(src.size());
        for (const GfQuatf &q : src) {
          const GfVec3f &im = q.GetImaginary();
          _rotate.push_back(GfVec4f(q.GetReal(), im[0], im[1], im[2]));
        }
      }
      else if (v.IsHolding<VtQuathArray>()) {
        const VtQuathArray &src = v.UncheckedGet<VtQuathArray>();
        _rotate.clear();
        _rotate.reserve(src.size());
        for (const GfQuath &q : src) {
          const GfVec3h &im = q.GetImaginary();
          _rotate.push_back(GfVec4f(q.GetReal(), im[0], im[1], im[2]));
        }
      }
    }
  }

  sceneDelegate->GetRenderIndex().GetChangeTracker().MarkInstancerClean(id);
}

VtMatrix4dArray HdCyclesInstancer::ComputeInstanceTransforms(const SdfPath &prototypeId)
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
    instanceTransform = matrixDs->GetTypedValue(0.0f);
  }

  VtMatrix4dArray transforms;
  transforms.reserve(instanceIndices.size());

  for (const int index : instanceIndices) {
    GfMatrix4d transform = instanceTransform;

    if (index < _translate.size()) {
      GfMatrix4d translateMat(1);
      translateMat.SetTranslate(_translate[index]);
      transform *= translateMat;
    }

    if (index < _rotate.size()) {
      GfMatrix4d rotateMat(1);
      const GfVec4f &quat = _rotate[index];
      rotateMat.SetRotate(GfQuatd(quat[0], quat[1], quat[2], quat[3]));
      transform *= rotateMat;
    }

    if (index < _scale.size()) {
      GfMatrix4d scaleMat(1);
      scaleMat.SetScale(_scale[index]);
      transform *= scaleMat;
    }

    if (index < _instanceTransform.size()) {
      transform *= _instanceTransform[index];
    }

    transforms.push_back(transform);
  }

  /* Recurse into the parent instancer. */
  if (HdCyclesInstancer *const parentInstancer = GetParentInstancer(prim.dataSource)) {
    VtMatrix4dArray resultTransforms;
    for (const GfMatrix4d &parentTransform : parentInstancer->ComputeInstanceTransforms(id)) {
      for (const GfMatrix4d &localTransform : transforms) {
        resultTransforms.push_back(parentTransform * localTransform);
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
