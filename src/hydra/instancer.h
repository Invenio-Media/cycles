/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/instancer.h>

#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesInstancer final : public PXR_NS::HdInstancer {
 public:
  HdCyclesInstancer(PXR_NS::HdSceneDelegate *delegate, const PXR_NS::SdfPath &instancerId);
  ~HdCyclesInstancer() override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  PXR_NS::VtMatrix4dArray ComputeInstanceTransforms(const PXR_NS::SdfPath &prototypeId);

  // The light-linking categories of each instance of a prototype, in the order of
  // ComputeInstanceTransforms: the instancer's own, each instance's, and its parents'.
  std::vector<PXR_NS::VtArray<PXR_NS::TfToken>> ComputeInstanceCategories(
      const PXR_NS::SdfPath &prototypeId);

 private:
  void SyncPrimvars();

  // The instancer that instances this one, or null.
  HdCyclesInstancer *GetParentInstancer(const PXR_NS::HdContainerDataSourceHandle &primDs);

  PXR_NS::VtVec3fArray _translate;
  PXR_NS::VtVec4fArray _rotate;
  PXR_NS::VtVec3fArray _scale;
  PXR_NS::VtMatrix4dArray _instanceTransform;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
