/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/camera.h"
#include "hydra/session.h"
#include "hydra/util.h"
#include "scene/camera.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/imaging/hd/cameraSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/usd/usdGeom/tokens.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* A camera's transform samples across the shutter, an odd number as Cycles centres them. */
static constexpr int kCameraMotionSteps = 3;

extern Transform convert_transform(const GfMatrix4d &matrix);
static Transform convert_camera_transform(const GfMatrix4d &matrix, const float metersPerUnit)
{
  Transform t = convert_transform(matrix);
  // Flip Z axis
  t.x.z *= -1.0f;
  t.y.z *= -1.0f;
  t.z.z *= -1.0f;
  // Scale translation
  t.x.w *= metersPerUnit;
  t.y.w *= metersPerUnit;
  t.z.w *= metersPerUnit;
  return t;
}

HdCyclesCamera::HdCyclesCamera(const SdfPath &sprimId) : HdCamera(sprimId)
{
  // Synchronize default values
  _horizontalAperture = _data.GetHorizontalAperture() * GfCamera::APERTURE_UNIT;
  _verticalAperture = _data.GetVerticalAperture() * GfCamera::APERTURE_UNIT;
  _horizontalApertureOffset = _data.GetHorizontalApertureOffset() * GfCamera::APERTURE_UNIT;
  _verticalApertureOffset = _data.GetVerticalApertureOffset() * GfCamera::APERTURE_UNIT;
  _focalLength = _data.GetFocalLength() * GfCamera::FOCAL_LENGTH_UNIT;
  _clippingRange = _data.GetClippingRange();
  _fStop = _data.GetFStop();
  _focusDistance = _data.GetFocusDistance();
}

HdCyclesCamera::~HdCyclesCamera() = default;

HdDirtyBits HdCyclesCamera::GetInitialDirtyBitsMask() const
{
  return DirtyBits::AllDirty;
}

void HdCyclesCamera::Sync(HdSceneDelegate *sceneDelegate,
                          HdRenderParam *renderParam,
                          HdDirtyBits *dirtyBits)
{
  if (*dirtyBits == DirtyBits::Clean) {
    return;
  }

  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
  const HdContainerDataSourceHandle &primDs = prim.dataSource;

  HdCameraSchema cameraSchema = HdCameraSchema::GetFromParent(primDs);
  HdContainerDataSourceHandle cameraDs = cameraSchema.GetContainer();

  /* The transform at the frame, and across the render camera's shutter for motion blur. */
  if (*dirtyBits & DirtyBits::DirtyTransform) {
    const HdCyclesShutter &shutter = static_cast<HdCyclesSession *>(renderParam)->shutter;
    _motionTransforms.clear();
    if (auto matrixDs = HdXformSchema::GetFromParent(primDs).GetMatrix()) {
      _transform = matrixDs->GetTypedValue(0.0f);
      _data.SetTransform(_transform);
      const std::vector<float> times = MotionSampleTimes(matrixDs,
                                                         shutter.open,
                                                         shutter.IsOpen() ? shutter.close :
                                                                            shutter.open,
                                                         kCameraMotionSteps);
      if (times.size() > 1) {
        for (const float time : times) {
          _motionTransforms.push_back(matrixDs->GetTypedValue(time));
        }
      }
    }
  }

  if (*dirtyBits & DirtyBits::DirtyWindowPolicy) {
    /* The window policy is not part of the camera schema; it lives on the
     * legacy data source. Leave the default of `CameraUtilFit`. */
  }

  if (*dirtyBits & DirtyBits::DirtyClipPlanes) {
    if (auto ds = cameraSchema.GetClippingPlanes()) {
      const VtArray<GfVec4d> clipPlanes = ds->GetTypedValue(0.0f);
      _clipPlanes.assign(clipPlanes.cbegin(), clipPlanes.cend());
    }
  }

  if (*dirtyBits & DirtyBits::DirtyParams) {
    if (auto ds = cameraSchema.GetProjection()) {
      const TfToken projection = ds->GetTypedValue(0.0f);
      _projection = (projection == HdCameraSchemaTokens->orthographic) ? Orthographic :
                                                                         Perspective;
      _data.SetProjection(_projection != Orthographic ? GfCamera::Perspective :
                                                        GfCamera::Orthographic);
    }

    if (auto ds = cameraSchema.GetHorizontalAperture()) {
      const float horizontalAperture = ds->GetTypedValue(0.0f);
      _horizontalAperture = horizontalAperture;
      _data.SetHorizontalAperture(horizontalAperture / GfCamera::APERTURE_UNIT);
    }

    if (auto ds = cameraSchema.GetVerticalAperture()) {
      const float verticalAperture = ds->GetTypedValue(0.0f);
      _verticalAperture = verticalAperture;
      _data.SetVerticalAperture(verticalAperture / GfCamera::APERTURE_UNIT);
    }

    if (auto ds = cameraSchema.GetHorizontalApertureOffset()) {
      const float horizontalApertureOffset = ds->GetTypedValue(0.0f);
      _horizontalApertureOffset = horizontalApertureOffset;
      _data.SetHorizontalApertureOffset(horizontalApertureOffset / GfCamera::APERTURE_UNIT);
    }

    if (auto ds = cameraSchema.GetVerticalApertureOffset()) {
      const float verticalApertureOffset = ds->GetTypedValue(0.0f);
      _verticalApertureOffset = verticalApertureOffset;
      _data.SetVerticalApertureOffset(verticalApertureOffset / GfCamera::APERTURE_UNIT);
    }

    if (auto ds = cameraSchema.GetFocalLength()) {
      const float focalLength = ds->GetTypedValue(0.0f);
      _focalLength = focalLength;
      _data.SetFocalLength(focalLength / GfCamera::FOCAL_LENGTH_UNIT);
    }

    if (auto ds = cameraSchema.GetClippingRange()) {
      const GfVec2f range = ds->GetTypedValue(0.0f);
      const GfRange1f clippingRange(range[0], range[1]);
      _clippingRange = clippingRange;
      _data.SetClippingRange(clippingRange);
    }

    if (auto ds = cameraSchema.GetFStop()) {
      const float fStop = ds->GetTypedValue(0.0f);
      _fStop = fStop;
      _data.SetFStop(fStop);
    }

    if (auto ds = cameraSchema.GetFocusDistance()) {
      const float focusDistance = ds->GetTypedValue(0.0f);
      _focusDistance = focusDistance;
      _data.SetFocusDistance(focusDistance);
    }
  }

  *dirtyBits = DirtyBits::Clean;
}

void HdCyclesCamera::Finalize(HdRenderParam *renderParam)
{
  HdCamera::Finalize(renderParam);
}

void HdCyclesCamera::ApplyCameraSettings(HdRenderParam *renderParam,
                                         const CameraUtilFraming &framing,
                                         const CameraUtilConformWindowPolicy windowPolicy,
                                         Camera *cam) const
{
  ApplyCameraSettings(renderParam, _data, framing, windowPolicy, cam);

  const float metersPerUnit = static_cast<HdCyclesSession *>(renderParam)->GetStageMetersPerUnit();

  array<Transform> motion(_motionTransforms.size());
  for (size_t i = 0; i < _motionTransforms.size(); ++i) {
    motion[i] = convert_camera_transform(_motionTransforms[i], metersPerUnit);
  }
  cam->set_motion(motion);
}

void HdCyclesCamera::ApplyCameraSettings(HdRenderParam *renderParam,
                                         const GfCamera &dataUnconformedWindow,
                                         const CameraUtilFraming &framing,
                                         const CameraUtilConformWindowPolicy windowPolicy,
                                         Camera *cam)
{
  const GfRange2f &display = framing.displayWindow;
  const GfRect2i &dataWindow = framing.dataWindow;
  const int width = dataWindow.GetMaxX() + 1;
  const int height = dataWindow.GetMaxY() + 1;
  cam->set_full_width(width);
  cam->set_full_height(height);

  /* The filmback, conformed to the display window, pixel aspect included. */
  auto data = dataUnconformedWindow;
  CameraUtilConformWindow(
      &data, windowPolicy, display.GetSize()[0] * framing.pixelAspectRatio / display.GetSize()[1]);

  if (data.GetProjection() == GfCamera::Orthographic) {
    cam->set_camera_type(CAMERA_ORTHOGRAPHIC);
  }
  else {
    cam->set_camera_type(CAMERA_PERSPECTIVE);
  }

  const float metersPerUnit = static_cast<HdCyclesSession *>(renderParam)->GetStageMetersPerUnit();

  /* The conformed window (y up) covers the display window (pixels, y down). The viewplane covers
   * the raster, from the origin to the data window's far corner, in the same units. */
  const GfRange2d window = data.GetFrustum().GetWindow();
  const double unitsPerPixelX = window.GetSize()[0] / display.GetSize()[0];
  const double unitsPerPixelY = window.GetSize()[1] / display.GetSize()[1];
  const double left = window.GetMin()[0] - display.GetMin()[0] * unitsPerPixelX;
  const double top = window.GetMax()[1] + display.GetMin()[1] * unitsPerPixelY;
  GfRange2d viewplane(GfVec2d(left, top - height * unitsPerPixelY),
                      GfVec2d(left + width * unitsPerPixelX, top));
  auto focalLength = 1.0f;
  if (data.GetProjection() == GfCamera::Perspective) {
    /* Normalized so the conformed window's height is 2, which the vertical field of view
     * spans. */
    viewplane *= 2.0 / window.GetSize()[1];
    focalLength = data.GetFocalLength() * GfCamera::FOCAL_LENGTH_UNIT * metersPerUnit;

    cam->set_fov(GfDegreesToRadians(data.GetFieldOfView(GfCamera::FOVVertical)));
  }

  cam->set_sensorwidth(data.GetHorizontalAperture() * GfCamera::APERTURE_UNIT * metersPerUnit);
  cam->set_sensorheight(data.GetVerticalAperture() * GfCamera::APERTURE_UNIT * metersPerUnit);

  cam->set_nearclip(data.GetClippingRange().GetMin() * metersPerUnit);
  cam->set_farclip(data.GetClippingRange().GetMax() * metersPerUnit);

  cam->set_viewplane_left(viewplane.GetMin()[0]);
  cam->set_viewplane_right(viewplane.GetMax()[0]);
  cam->set_viewplane_bottom(viewplane.GetMin()[1]);
  cam->set_viewplane_top(viewplane.GetMax()[1]);

  if (data.GetFStop() != 0.0f) {
    cam->set_focaldistance(data.GetFocusDistance() * metersPerUnit);
    cam->set_aperturesize(focalLength / (2.0f * data.GetFStop()));
  }

  cam->set_matrix(convert_camera_transform(data.GetTransform(), metersPerUnit));
}

void HdCyclesCamera::ApplyCameraSettings(HdRenderParam *renderParam,
                                         const GfMatrix4d &worldToViewMatrix,
                                         const GfMatrix4d &projectionMatrix,
                                         const CameraUtilFraming &framing,
                                         const CameraUtilConformWindowPolicy windowPolicy,
                                         Camera *cam)
{
  GfCamera data;
  data.SetFromViewAndProjectionMatrix(worldToViewMatrix, projectionMatrix);

  ApplyCameraSettings(renderParam, data, framing, windowPolicy, cam);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
