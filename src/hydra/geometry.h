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

  /* A constant displayColor: the colour of every instance, which the default surface shades
   * with. Instances made later take it too. */
  void SetDisplayColor(const PXR_NS::GfVec3f &color);

  CyclesBase *_geom = nullptr;
  std::vector<CCL_NS::Object *> _instances;

  // The shutter this sync samples motion across (open == close when it is closed), and the
  // object's motion steps; 1 when it doesn't blur.
  float _shutterOpen = 0.0f;
  float _shutterClose = 0.0f;
  int _motionSteps = 1;
  // Set when the motion steps of the geometry changed, so the BVH is rebuilt.
  bool _motionRebuild = false;
  // The stage's metres per unit, which the delegate scales the scene by, as of this sync.
  float _metersPerUnit = 1.0f;

 private:
  void Initialize(PXR_NS::HdRenderParam *renderParam);

  void InitializeInstance(const int index);

  /* The surface of geometry without a material: Cycles' default volume for a volume, the
   * session's displayColor surface where displayColor varies over the geometry, else Cycles'
   * default surface. */
  CCL_NS::Shader *DefaultSurface(CCL_NS::Scene *scene,
                                 const PXR_NS::HdContainerDataSourceHandle &primDs,
                                 PXR_NS::HdRenderParam *renderParam) const;

  /* UsdLuxMeshLightAPI (a light API on the geometry): the geometry renders with an emission
   * shader of the light's own instead of its material, and is hidden from the camera, as
   * Karma has it. */
  void UpdateMeshLight(PXR_NS::HdSceneDelegate *sceneDelegate,
                       CCL_NS::Scene *scene,
                       const PXR_NS::HdContainerDataSourceHandle &primDs,
                       PXR_NS::HdRenderParam *renderParam);

  /* The geometry's surface area in stage units, through its transform, for a mesh light's
   * normalize; 0 for curves, points and volumes, which then aren't normalized. */
  float SurfaceArea() const;

  // A mesh light's emission shader, and the light settings it was built from.
  CCL_NS::Shader *_lightShader = nullptr;
  std::string _lightKey;

  PXR_NS::GfMatrix4d _geomTransform;
  // The transform at evenly spaced times across the shutter; empty when it doesn't move.
  std::vector<PXR_NS::GfMatrix4d> _geomMotion;
  // The constant displayColor, or Blender's default grey without one.
  PXR_NS::GfVec3f _displayColor{0.8f, 0.8f, 0.8f};
  // False when the instancer gives the prim no instances: the first object is kept, since the
  // prim's settings live on it, but hidden.
  bool _hasInstances = true;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
