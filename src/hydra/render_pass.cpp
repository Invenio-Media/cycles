/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_pass.h"
#include "hydra/camera.h"
#include "hydra/output_driver.h"
#include "hydra/render_buffer.h"
#include "hydra/render_delegate.h"
#include "hydra/session.h"

#ifdef WITH_HYDRA_DISPLAY_DRIVER
#  include "hydra/display_driver.h"
#endif

#include "scene/camera.h"
#include "scene/integrator.h"
#include "scene/scene.h"

#include "session/session.h"

#include <pxr/imaging/hd/renderPassState.h>

#include <cmath>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesRenderPass::HdCyclesRenderPass(HdRenderIndex *index,
                                       const HdRprimCollection &collection,
                                       HdCyclesSession *renderParam,
                                       std::shared_ptr<int> passCount)
    : HdRenderPass(index, collection), _renderParam(renderParam), _passCount(std::move(passCount))
{
  ++(*_passCount);
  Session *const session = _renderParam->session;
  // Reset cancel state so session thread can continue rendering
  session->progress.reset();

  session->set_output_driver(make_unique<HdCyclesOutputDriver>(renderParam));

  const auto *const renderDelegate = static_cast<const HdCyclesDelegate *>(
      GetRenderIndex()->GetRenderDelegate());
  if (renderDelegate->IsDisplaySupported()) {
#ifdef WITH_HYDRA_DISPLAY_DRIVER
    session->set_display_driver(
        make_unique<HdCyclesDisplayDriver>(renderParam, renderDelegate->GetHgi()));
#endif
  }
}

HdCyclesRenderPass::~HdCyclesRenderPass()
{
  Session *const session = _renderParam->session;
  session->cancel(true);
  // Decrement through the shared counter; no delegate backpointer needed.
  --(*_passCount);
}

bool HdCyclesRenderPass::IsConverged() const
{
  // A failed session never fills the render buffers; report it as done so the host stops
  // waiting. The failure itself is reported through the render stats.
  if (_renderParam->session->progress.get_error()) {
    return true;
  }

  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (aovBinding.renderBuffer && !aovBinding.renderBuffer->IsConverged()) {
      return false;
    }
  }

  return true;
}

void HdCyclesRenderPass::ResetConverged()
{
  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      renderBuffer->SetConverged(false);
    }
  }
}

void HdCyclesRenderPass::_Execute(const HdRenderPassStateSharedPtr &renderPassState,
                                  const TfTokenVector & /*renderTags*/)
{
  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (session->progress.get_cancel()) {
    return;  // Something went wrong and cannot continue without recreating the session
  }

  /* Prims sample their motion across the shutter when they sync. husk sends the shutter before
   * the first sync; one that changes after it resamples every prim, and the render camera, on
   * the next sync. Volumes load their velocity grid only while the shutter is open. */
  const unsigned int shutterVersion = _renderParam->shutter.version;
  if (_shutterVersion && *_shutterVersion != shutterVersion) {
    HdChangeTracker &tracker = GetRenderIndex()->GetChangeTracker();
    tracker.MarkAllRprimsDirty(HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyPoints |
                               HdChangeTracker::DirtyVolumeField);
    if (const HdCamera *const camera = renderPassState->GetCamera()) {
      tracker.MarkSprimDirty(camera->GetId(), HdCamera::DirtyTransform);
    }
  }
  _shutterVersion = shutterVersion;

  if (scene->mutex.try_lock()) {
    auto *const renderDelegate = static_cast<HdCyclesDelegate *>(
        GetRenderIndex()->GetRenderDelegate());

    const unsigned int settingsVersion = renderDelegate->GetRenderSettingsVersion();

    // Update requested AOV bindings
    const HdRenderPassAovBindingVector &aovBindings = renderPassState->GetAovBindings();
    if (_renderParam->GetAovBindings() != aovBindings ||
        // Need to resync passes when denoising is enabled or disabled to update the pass mode
        (settingsVersion != _lastSettingsVersion && scene->integrator->use_denoise_is_modified()))
    {
      _renderParam->SyncAovBindings(aovBindings);

      if (renderDelegate->IsDisplaySupported()) {
        // Update display pass to the first requested color AOV
        const HdRenderPassAovBinding displayAovBinding = !aovBindings.empty() ?
                                                             aovBindings.front() :
                                                             HdRenderPassAovBinding();
        if (displayAovBinding.aovName == HdAovTokens->color && displayAovBinding.renderBuffer) {
          _renderParam->SetDisplayAovBinding(displayAovBinding);
        }
        else {
          _renderParam->SetDisplayAovBinding(HdRenderPassAovBinding());
        }
      }
    }

    /* The framing: the display window, with the pixel aspect and window policy, frames the
     * camera; the data window is the pixels to render, which can crop or overscan it. */
    CameraUtilFraming framing = renderPassState->GetFraming();
    CameraUtilConformWindowPolicy windowPolicy = renderPassState->GetWindowPolicy();
    if (!framing.IsValid()) {
      const GfVec4f vp = renderPassState->GetViewport();
      const GfRect2i viewport(GfVec2i(0), int(vp[2]), int(vp[3]));
      const HdCyclesProductFraming &product = _renderParam->productFraming;
      if (product.resolution[0] > 0 && product.resolution[1] > 0) {
        /* husk sends the product's framing as render settings, with render buffers the size of
         * the data window: the data window is at the origin and the display window, the
         * resolution, is offset from it. The data window holds the pixels whose centres are in
         * dataWindowNDC, which has y up. Its corner comes from dataWindowNDC and its size from
         * the buffers husk allocated, which are what the tiles must fill. */
        const GfVec4f &ndc = product.dataWindowNDC;
        const GfVec2f resolution(product.resolution[0], product.resolution[1]);
        const GfVec2f corner(std::ceil(ndc[0] * resolution[0] - 0.5f),
                             std::ceil((1.0f - ndc[3]) * resolution[1] - 0.5f));
        framing = CameraUtilFraming(
            GfRange2f(-corner, resolution - corner), viewport, product.pixelAspectRatio);
        windowPolicy = product.windowPolicy;
      }
      else {
        framing = CameraUtilFraming(viewport);
      }
    }

    /* The dicing camera is the render camera, as in Blender: adaptive subdivision measures its
     * dicing rate in pixels as this camera sees them. Cycles re-dices only when a mesh changes,
     * so a moving camera doesn't re-dice. */
    for (Camera *const cam : {scene->camera, scene->dicing_camera}) {
      if (const auto *const camera = static_cast<const HdCyclesCamera *>(
              renderPassState->GetCamera()))
      {
        camera->ApplyCameraSettings(_renderParam, framing, windowPolicy, cam);
      }
      else {
        HdCyclesCamera::ApplyCameraSettings(_renderParam,
                                            renderPassState->GetWorldToViewMatrix(),
                                            renderPassState->GetProjectionMatrix(),
                                            framing,
                                            windowPolicy,
                                            cam);
      }
    }

    // Reset session if the session, scene, camera or AOV bindings changed
    if (scene->need_reset() || settingsVersion != _lastSettingsVersion) {
      _lastSettingsVersion = settingsVersion;

      // Reset convergence state of all render buffers
      ResetConverged();

      /* Framing pixel space, which the render buffers are in: the full frame runs from the
       * origin to the data window's far corner, as the camera's raster does, and Cycles renders
       * the data window of it. */
      const GfRect2i &dataWindow = framing.dataWindow;
      BufferParams buffer_params;
      buffer_params.full_x = 0;
      buffer_params.full_y = 0;
      buffer_params.full_width = dataWindow.GetMaxX() + 1;
      buffer_params.full_height = dataWindow.GetMaxY() + 1;

      buffer_params.window_x = dataWindow.GetMinX();
      buffer_params.window_y = dataWindow.GetMinY();
      buffer_params.window_width = dataWindow.GetWidth();
      buffer_params.window_height = dataWindow.GetHeight();

      buffer_params.width = buffer_params.window_width;
      buffer_params.height = buffer_params.window_height;

      /* Cycles' tiles are placed from the data window's corner. */
      _renderParam->SetDataWindowOrigin(GfVec2i(dataWindow.GetMinX(), dataWindow.GetMinY()));

      session->reset(session->params, buffer_params);
    }

    scene->mutex.unlock();

    // Start Cycles render thread if not already running
    session->start();
  }

  session->draw();
}

void HdCyclesRenderPass::_MarkCollectionDirty() {}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
