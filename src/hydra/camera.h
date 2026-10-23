/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/base/gf/camera.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/timeSampleArray.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesCamera final : public PXR_NS::HdCamera {
 public:
  HdCyclesCamera(const PXR_NS::SdfPath &sprimId);
  ~HdCyclesCamera() override;

  /* Sets a Cycles camera to render a framing's data window. Its raster is framing pixel space,
   * from the origin to the data window's far corner (the render pass renders the data window
   * of it), and the camera is conformed to the display window, with its pixel aspect and the
   * window policy: a data window inside the display window crops, one beyond it overscans. */
  void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                           const PXR_NS::CameraUtilFraming &framing,
                           PXR_NS::CameraUtilConformWindowPolicy windowPolicy,
                           CCL_NS::Camera *cam) const;

  static void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                                  const PXR_NS::GfCamera &dataUnconformedWindow,
                                  const PXR_NS::CameraUtilFraming &framing,
                                  PXR_NS::CameraUtilConformWindowPolicy windowPolicy,
                                  CCL_NS::Camera *cam);
  /* For a render pass without a camera prim, as the free viewport camera. */
  static void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                                  const PXR_NS::GfMatrix4d &worldToViewMatrix,
                                  const PXR_NS::GfMatrix4d &projectionMatrix,
                                  const PXR_NS::CameraUtilFraming &framing,
                                  PXR_NS::CameraUtilConformWindowPolicy windowPolicy,
                                  CCL_NS::Camera *cam);

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

 private:
  PXR_NS::GfCamera _data;
  PXR_NS::HdTimeSampleArray<PXR_NS::GfMatrix4d, 2> _transformSamples;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
