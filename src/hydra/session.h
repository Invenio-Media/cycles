/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "hydra/light_linking.h"
#include "scene/pass.h"
#include "scene/scene.h"
#include "util/thread.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/usd/sdf/path.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesMesh;

struct SceneLock {
  SceneLock(const PXR_NS::HdRenderParam *renderParam);
  ~SceneLock();

  CCL_NS::Scene *scene;

 private:
  CCL_NS::thread_scoped_lock sceneLock;
};

// The Cycles pass type that renders a RenderVar source name, or a Hydra AOV name when the
// binding has no source name. PASS_NONE when Cycles has no matching pass.
CCL_NS::PassType HdCyclesPassTypeForAov(const PXR_NS::TfToken &sourceName);

// How one AOV binding is read back from Cycles.
struct HdCyclesAovPass {
  // The Cycles pass the binding reads; empty when the binding has no pass.
  std::string passName;
  /* Integer ID buffer. Cycles stores IDs offset by one, so that its empty pass value 0 reads
   * back as Hydra's -1: primId in the object's pass_id, instanceId in an object attribute
   * (HdCyclesGeometry::Sync and InitializeInstance). */
  bool isId = false;
  /* Cycles fills a depth pass with 1e10 where nothing was hit; Karma writes 0 there, and so
   * does this, which keeps the viewport's display of depth to the scene's range. */
  bool isDepth = false;
  // Multiplies the pass values. The scene is in metres inside Cycles; positions and depths
  // go back to stage units.
  float scale = 1.0f;
};

// A render product's framing, as husk sends it: render settings rather than a render-pass
// framing, with render buffers the size of the data window.
struct HdCyclesProductFraming {
  // The display window; (0, 0) when no product framing was sent.
  PXR_NS::GfVec2i resolution = PXR_NS::GfVec2i(0);
  // xmin, ymin, xmax, ymax of the display window, with y up.
  PXR_NS::GfVec4f dataWindowNDC = PXR_NS::GfVec4f(0.0f, 0.0f, 1.0f, 1.0f);
  float pixelAspectRatio = 1.0f;
  PXR_NS::CameraUtilConformWindowPolicy windowPolicy = PXR_NS::CameraUtilFit;
};

// The render camera's shutter, as husk sends it: frames from the current frame. Motion blur is
// on when the shutter is open for a time, unless disableMotionBlur or instantaneousShutter.
struct HdCyclesShutter {
  float open = 0.0f;
  float close = 0.0f;
  bool disableMotionBlur = false;
  bool instantaneous = false;
  // Set once a host sends the shutter's open or close as render settings, as husk does.
  // Houdini's viewport doesn't, and the render camera's shutter applies (SetCameraShutter).
  bool fromSettings = false;
  // Bumped on every change, so the render pass can resample prims that synced before it.
  unsigned int version = 0;

  bool IsOpen() const
  {
    return !disableMotionBlur && !instantaneous && close > open;
  }

  // Where the frame falls across the shutter: 0 at its open, 1 at its close; the middle while
  // it is closed.
  float FrameOffset() const
  {
    return close > open ? -open / (close - open) : 0.5f;
  }

  // Cycles places volume velocity blur at offset 0 or 0.5 only, whichever is nearer.
  bool VolumeBlurCentred() const
  {
    return FrameOffset() >= 0.25f;
  }
  bool VolumeBlurExact() const
  {
    return std::fabs(FrameOffset() - (VolumeBlurCentred() ? 0.5f : 0.0f)) <= 1e-3f;
  }
};

class HdCyclesSession final : public PXR_NS::HdRenderParam {
 public:
  HdCyclesSession(CCL_NS::Session *session_, const bool keep_nodes);
  HdCyclesSession(const CCL_NS::SessionParams &params,
                  const CCL_NS::SceneParams &sceneParams = CCL_NS::SceneParams());
  ~HdCyclesSession() override;

  bool OwnsSession() const
  {
    return _ownCyclesSession;
  }

  void UpdateScene();

  // Applies a new render camera shutter and returns whether it changed; prims resample their
  // motion across it when they next sync. From the shutter render settings husk sends, or from
  // the render camera when a host sends none (Houdini's viewport).
  bool UpdateShutter(const HdCyclesShutter &next);
  // Sets Cycles' motion blur from the shutter. Call with the scene locked.
  void ApplyShutter();
  // The shutter, for cameras, which sync in parallel with the render camera that may change
  // it. Geometry, which syncs after the cameras, reads `shutter`.
  HdCyclesShutter GetShutter() const;

  // Each camera's shutter, recorded as it syncs. Without shutter settings the render camera's
  // applies, whether the camera or the renderCameraPath setting naming it comes first; Hydra
  // syncs cameras before the geometry, which then samples its motion across it. Return
  // whether the shutter changed.
  bool SetCameraShutter(const PXR_NS::SdfPath &camera, float open, float close);
  bool SetRenderCamera(const PXR_NS::SdfPath &camera);
  void RemoveCameraShutter(const PXR_NS::SdfPath &camera);
  // A rebuilt session keeps the cameras, which don't hold it back from rebuilding.
  void TakeCameras(HdCyclesSession &previous);

  double GetStageMetersPerUnit() const
  {
    return _stageMetersPerUnit;
  }

  void SetStageMetersPerUnit(const double stageMetersPerUnit)
  {
    _stageMetersPerUnit = stageMetersPerUnit;
  }

  PXR_NS::HdRenderPassAovBinding GetDisplayAovBinding() const
  {
    return _displayAovBinding;
  }

  void SetDisplayAovBinding(const PXR_NS::HdRenderPassAovBinding &aovBinding)
  {
    const CCL_NS::thread_scoped_lock lock(_aovMutex);
    _displayAovBinding = aovBinding;
  }

  // Cycles' render thread reads the bindings through the output driver while Hydra changes
  // them; it holds this lock while it does. Hydra's thread changes them only through the
  // methods here, which take the lock.
  CCL_NS::thread_mutex &GetAovMutex() const
  {
    return _aovMutex;
  }

  const PXR_NS::HdRenderPassAovBindingVector &GetAovBindings() const
  {
    return _aovBindings;
  }

  // Parallel to GetAovBindings().
  const std::vector<HdCyclesAovPass> &GetAovPasses() const
  {
    return _aovPasses;
  }

  void SyncAovBindings(const PXR_NS::HdRenderPassAovBindingVector &aovBindings);

  void RemoveAovBinding(PXR_NS::HdRenderBuffer *renderBuffer);

  /* Where the rendered data window starts in a render buffer of the given height, which the
   * output driver places Cycles' tiles from. Buffers run bottom up. husk's buffers are the data
   * window; a host's start at the framing's origin, top down, and hold the data window where
   * the framing says. Read under GetAovMutex(). */
  PXR_NS::GfVec2i GetDataWindowOrigin(const int bufferHeight) const
  {
    if (_buffersAtDataWindow) {
      return PXR_NS::GfVec2i(0);
    }
    return PXR_NS::GfVec2i(_dataWindow.GetMinX(), bufferHeight - (_dataWindow.GetMaxY() + 1));
  }

  void SetDataWindow(const PXR_NS::GfRect2i &dataWindow, const bool buffersAtDataWindow)
  {
    const CCL_NS::thread_scoped_lock lock(_aovMutex);
    _dataWindow = dataWindow;
    _buffersAtDataWindow = buffersAtDataWindow;
  }

  // Meshes subdivided with pixel-space dicing, which the render pass re-dices when they or the
  // camera move far enough (HdCyclesMesh::UpdateDicing). Meshes add and remove themselves as
  // they sync, which Hydra does in parallel; the render pass reads the set between syncs.
  void SetPixelDiced(HdCyclesMesh *mesh, const bool diced)
  {
    const CCL_NS::thread_scoped_lock lock(_pixelDicedMutex);
    if (diced) {
      _pixelDiced.insert(mesh);
    }
    else {
      _pixelDiced.erase(mesh);
    }
  }

  const std::unordered_set<HdCyclesMesh *> &PixelDicedMeshes() const
  {
    return _pixelDiced;
  }

  // Geometry that is a mesh light (HdCyclesGeometry::UpdateMeshLight), which counts as a light
  // for the default background. Geometry adds and removes itself as it syncs.
  void SetMeshLight(const void *geometry, const bool light)
  {
    const CCL_NS::thread_scoped_lock lock(_meshLightsMutex);
    const size_t before = _meshLights.size();
    if (light) {
      _meshLights.insert(geometry);
    }
    else {
      _meshLights.erase(geometry);
    }
    _meshLightsChanged = _meshLightsChanged || _meshLights.size() != before;
  }

  CCL_NS::Session *session;
  bool keep_nodes;

  // When true, the background is kept transparent even if a dome light (background shader)
  // is present. Controlled by cycles:film:transparent.
  bool filmTransparent = false;

  // Light and shadow linking; lights and geometry report to it as they sync.
  HdCyclesLightLinking lightLinking;

  /* The surface of geometry with no material and a displayColor that varies over it (uniform,
   * vertex, varying or face-varying): a diffuse of that primvar, as Karma shades it. Geometry
   * with a constant displayColor, or none, keeps Cycles' default surface, a diffuse of its
   * object colour. Null when Cycles' session is the host's (Blender). */
  CCL_NS::Shader *displayColorSurface = nullptr;

  // Set by husk; the render pass frames from it when it has no framing of its own.
  HdCyclesProductFraming productFraming;

  // Set by husk; geometry and the camera sample their motion across it.
  HdCyclesShutter shutter;

 private:
  const bool _ownCyclesSession;
  double _stageMetersPerUnit = 0.01;
  PXR_NS::HdRenderPassAovBindingVector _aovBindings;
  std::vector<HdCyclesAovPass> _aovPasses;
  PXR_NS::HdRenderPassAovBinding _displayAovBinding;
  PXR_NS::GfRect2i _dataWindow;
  bool _buffersAtDataWindow = false;
  mutable CCL_NS::thread_mutex _aovMutex;
  std::unordered_set<HdCyclesMesh *> _pixelDiced;
  CCL_NS::thread_mutex _pixelDicedMutex;
  std::unordered_set<const void *> _meshLights;
  bool _meshLightsChanged = false;
  CCL_NS::thread_mutex _meshLightsMutex;
  mutable CCL_NS::thread_mutex _shutterMutex;
  std::unordered_map<PXR_NS::SdfPath, std::pair<float, float>, PXR_NS::SdfPath::Hash>
      _cameraShutters;
  PXR_NS::SdfPath _renderCamera;
  CCL_NS::thread_mutex _cameraMutex;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
