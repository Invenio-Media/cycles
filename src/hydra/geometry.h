/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/rprim.h>

#include <vector>

CCL_NAMESPACE_BEGIN
class Attribute;
CCL_NAMESPACE_END

HDCYCLES_NAMESPACE_OPEN_SCOPE

template<typename Base, typename CyclesBase> class HdCyclesGeometry : public Base {
 public:
  HdCyclesGeometry(const PXR_NS::SdfPath &rprimId);

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits,
            const PXR_NS::TfToken &reprToken) override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

 protected:
  void _InitRepr(const PXR_NS::TfToken &reprToken, PXR_NS::HdDirtyBits *dirtyBits) override;

  PXR_NS::HdDirtyBits _PropagateDirtyBits(PXR_NS::HdDirtyBits bits) const override;

  virtual void Populate(PXR_NS::HdSceneDelegate *sceneDelegate,
                        PXR_NS::HdDirtyBits dirtyBits,
                        bool &rebuild) = 0;

  /* The points primvar for deformation motion blur: the frame's points alone when the shutter
   * is closed, motion blur is off or the points don't change across the shutter; otherwise one
   * sample per motion step, evenly spaced across the shutter. Empty when points are missing or
   * not vec3f. Cycles takes the middle sample as the geometry's position. */
  std::vector<PXR_NS::VtVec3fArray> SamplePoints(const PXR_NS::HdPrimvarsSchema &primvars) const;

  /* Gives the position attribute the samples' other steps, or takes its motion away when there
   * is one sample or one is short of `count`. The BVH is rebuilt when the steps change. */
  void ApplyPointsMotion(CCL_NS::Attribute *attr_P,
                         const std::vector<PXR_NS::VtVec3fArray> &samples,
                         size_t count);

  CyclesBase *_geom = nullptr;
  std::vector<CCL_NS::Object *> _instances;

  // The shutter this sync samples motion across (open == close when it is closed), and the
  // object's motion steps; 1 when it doesn't blur.
  float _shutterOpen = 0.0f;
  float _shutterClose = 0.0f;
  int _motionSteps = 1;
  // Set when the motion steps of the geometry changed, so the BVH is rebuilt.
  bool _motionRebuild = false;

 private:
  void Initialize(PXR_NS::HdRenderParam *renderParam);

  void InitializeInstance(const int index);

  PXR_NS::GfMatrix4d _geomTransform;
  // The transform at evenly spaced times across the shutter; empty when it doesn't move.
  std::vector<PXR_NS::GfMatrix4d> _geomMotion;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
