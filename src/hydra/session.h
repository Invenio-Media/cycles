/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "scene/pass.h"
#include "scene/scene.h"
#include "util/thread.h"

#include <string>
#include <vector>

#include <pxr/imaging/hd/renderDelegate.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

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

  CCL_NS::Session *session;
  bool keep_nodes;

  // When true, the background is kept transparent even if a dome light (background shader)
  // is present. Controlled by cycles:film:transparent.
  bool filmTransparent = false;

 private:
  const bool _ownCyclesSession;
  double _stageMetersPerUnit = 0.01;
  PXR_NS::HdRenderPassAovBindingVector _aovBindings;
  std::vector<HdCyclesAovPass> _aovPasses;
  PXR_NS::HdRenderPassAovBinding _displayAovBinding;
  mutable CCL_NS::thread_mutex _aovMutex;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
