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

#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesCamera final : public PXR_NS::HdCamera {
 public:
  HdCyclesCamera(const PXR_NS::SdfPath &sprimId);
  ~HdCyclesCamera() override;

  /* Sets a Cycles camera to render a framing's data window. Its raster is framing pixel space,
   * from the origin to the far corner of the data and display windows (HdCyclesRasterSize; the
   * render pass renders the data window of it), and the camera is conformed to the display
   * window, with its pixel aspect and the window policy: a data window inside the display
   * window crops, one beyond it overscans. */
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
  // The transform at evenly spaced times across the shutter; one when it doesn't move.
  std::vector<PXR_NS::GfMatrix4d> _motionTransforms;
};

/* The raster a framing renders in, in pixels from the origin: to the far corner of its data
 * window and of its display window, whichever is further. Cycles' raster runs bottom up, and
 * its sample sequences follow the raster's pixels, so a crop of the display window samples
 * each pixel as the whole window does only on a raster that reaches the display's far edge. */
PXR_NS::GfVec2i HdCyclesRasterSize(const PXR_NS::CameraUtilFraming &framing);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
