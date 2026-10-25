/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "hydra/geometry.h"
#include "util/types.h"

#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshUtil.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesMesh final : public HdCyclesGeometry<PXR_NS::HdMesh, CCL_NS::Mesh> {
 public:
  HdCyclesMesh(const PXR_NS::SdfPath &rprimId);
  ~HdCyclesMesh() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits,
            const PXR_NS::TfToken &reprToken) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

  /* Pixel-space dicing goes stale as the mesh or the camera moves. Re-dices the mesh when its
   * size as the dicing camera sees it has changed by more than kRediceFactor since it was last
   * diced, so small moves keep the tessellation and large ones get a new one. The render pass
   * calls it under the scene lock, after it sets the dicing camera. */
  void UpdateDicing(CCL_NS::Scene *scene);

 private:
  PXR_NS::HdDirtyBits _PropagateDirtyBits(PXR_NS::HdDirtyBits bits) const override;

  void Populate(PXR_NS::HdSceneDelegate *sceneDelegate,
                PXR_NS::HdDirtyBits dirtyBits,
                bool &rebuild) override;

  void PopulatePoints(PXR_NS::HdSceneDelegate *sceneDelegate);
  void PopulateNormals(PXR_NS::HdSceneDelegate *sceneDelegate);

  void PopulatePrimvars(PXR_NS::HdSceneDelegate *sceneDelegate);

  void PopulateTopology(PXR_NS::HdSceneDelegate *sceneDelegate);

  // Reads primvars:cycles:object:subdivision:*; returns whether max_level changed.
  bool ReadSubdivisionSettings(PXR_NS::HdSceneDelegate *sceneDelegate);
  // Sets the Cycles mesh's dicing parameters from them.
  void ApplySubdivisionSettings();

  PXR_NS::HdMeshUtil _util;
  PXR_NS::HdMeshTopology _topology;
  PXR_NS::VtIntArray _primitiveParams;
  // The display style's flat shading, kept for PopulateNormals, which sets the smooth flags.
  bool _flatShading = false;
  // The subdivision settings: an authored max_level replaces the default of 8 levels (-1 when
  // unauthored); the dicing rate is in pixels, or object units in object space.
  int _subdMaxLevel = -1;
  float _subdDicingRate = 1.0f;
  bool _subdObjectSpace = false;
  // The points' bounds in object space, for the size the dicing camera sees.
  CCL_NS::float3 _boundsCenter = CCL_NS::zero_float3();
  float _boundsRadius = 0.0f;
  // That size when the mesh was last diced; 0 when it hasn't been.
  float _dicedSize = 0.0f;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
