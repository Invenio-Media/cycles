/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/output_driver.h"
#include "hydra/render_buffer.h"
#include "hydra/session.h"

#include <algorithm>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesOutputDriver::HdCyclesOutputDriver(HdCyclesSession *renderParam)
    : _renderParam(renderParam)
{
}

void HdCyclesOutputDriver::write_render_tile(const Tile &tile)
{
  update_render_tile(tile);

  // Update convergence state of all render buffers
  const thread_scoped_lock lock(_renderParam->GetAovMutex());
  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      renderBuffer->SetConverged(true);
    }
  }
}

bool HdCyclesOutputDriver::update_render_tile(const Tile &tile)
{
  std::vector<float> pixels;

  // SyncAovBindings sets both vectors together under this lock, so they have the same size.
  const thread_scoped_lock lock(_renderParam->GetAovMutex());
  const HdRenderPassAovBindingVector &aovBindings = _renderParam->GetAovBindings();
  const std::vector<HdCyclesAovPass> &aovPasses = _renderParam->GetAovPasses();
  for (size_t i = 0; i < aovBindings.size(); ++i) {
    const HdRenderPassAovBinding &aovBinding = aovBindings[i];
    const HdCyclesAovPass &aovPass = aovPasses[i];
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      if (aovPass.passName.empty()) {
        continue;  // No Cycles pass for this binding; SyncAovBindings already warned
      }
      if (aovBinding == _renderParam->GetDisplayAovBinding() && renderBuffer->IsResourceUsed()) {
        continue;  // Display AOV binding is already updated by Cycles display driver
      }

      const HdFormat format = renderBuffer->GetFormat();
      if (format == HdFormatInvalid) {
        continue;  // Skip invalid AOV bindings
      }

      const size_t channels = HdGetComponentCount(format);
      const size_t numValues = channels * tile.size.x * tile.size.y;
      const auto applyScale = [&aovPass, numValues](float *values) {
        if (aovPass.scale != 1.0f) {
          std::for_each_n(values, numValues, [&aovPass](float &v) { v *= aovPass.scale; });
        }
      };
      // Avoid extra copy by mapping render buffer directly when dimensions/format match the tile
      if (tile.offset.x == 0 && tile.offset.y == 0 && tile.size.x == renderBuffer->GetWidth() &&
          tile.size.y == renderBuffer->GetHeight() &&
          (format >= HdFormatFloat32 && format <= HdFormatFloat32Vec4))
      {
        float *const data = static_cast<float *>(renderBuffer->Map());
        TF_VERIFY(tile.get_pass_pixels(aovPass.passName, channels, data));
        applyScale(data);
        renderBuffer->Unmap();
      }
      else {
        pixels.resize(numValues);
        if (tile.get_pass_pixels(aovPass.passName, channels, pixels.data())) {
          applyScale(pixels.data());
          renderBuffer->Map();
          renderBuffer->WritePixels(pixels.data(),
                                    GfVec2i(tile.offset.x, tile.offset.y),
                                    GfVec2i(tile.size.x, tile.size.y),
                                    channels,
                                    aovPass.isId);
          renderBuffer->Unmap();
        }
        else {
          TF_RUNTIME_ERROR("Could not find pass for AOV '%s'", aovBinding.aovName.GetText());
        }
      }
    }
  }

  return true;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
