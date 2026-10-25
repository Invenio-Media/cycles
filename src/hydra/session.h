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
#include <unordered_set>
#include <vector>

#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/hd/renderDelegate.h>

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
  // Integer ID buffer: Cycles stores IDs offset by one (see 'HdCyclesMesh::Sync').
  bool isId = false;
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
  // Set once a host sends the shutter as render settings, as husk does. Houdini's viewport
  // doesn't, and the render pass then takes the render camera's.
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
  // the render camera, which syncs before them, when a host sends none (Houdini's viewport).
  bool UpdateShutter(const HdCyclesShutter &next);
  // Sets Cycles' motion blur from the shutter. Call with the scene locked.
  void ApplyShutter();

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

  // Where the rendered data window starts in the render buffers, which the output driver
  // places Cycles' tiles from. Read under GetAovMutex().
  PXR_NS::GfVec2i GetDataWindowOrigin() const
  {
    return _dataWindowOrigin;
  }

  void SetDataWindowOrigin(const PXR_NS::GfVec2i &origin)
  {
    const CCL_NS::thread_scoped_lock lock(_aovMutex);
    _dataWindowOrigin = origin;
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

  CCL_NS::Session *session;
  bool keep_nodes;

  // When true, the background is kept transparent even if a dome light (background shader)
  // is present. Controlled by cycles:film:transparent.
  bool filmTransparent = false;

  // Light and shadow linking; lights and geometry report to it as they sync.
  HdCyclesLightLinking lightLinking;

  // Set by husk; the render pass frames from it when it has no framing of its own.
  HdCyclesProductFraming productFraming;

  // Set by husk; geometry and the camera sample their motion across it.
  HdCyclesShutter shutter;

  // The render camera, which Houdini's viewport and husk name in the renderCameraPath setting.
  PXR_NS::SdfPath renderCameraPath;

 private:
  const bool _ownCyclesSession;
  double _stageMetersPerUnit = 0.01;
  PXR_NS::HdRenderPassAovBindingVector _aovBindings;
  std::vector<HdCyclesAovPass> _aovPasses;
  PXR_NS::HdRenderPassAovBinding _displayAovBinding;
  PXR_NS::GfVec2i _dataWindowOrigin = PXR_NS::GfVec2i(0);
  mutable CCL_NS::thread_mutex _aovMutex;
  std::unordered_set<HdCyclesMesh *> _pixelDiced;
  CCL_NS::thread_mutex _pixelDicedMutex;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
