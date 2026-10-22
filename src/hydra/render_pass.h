/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/renderPass.h>

#include <memory>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesRenderPass final : public PXR_NS::HdRenderPass {
 public:
  HdCyclesRenderPass(PXR_NS::HdRenderIndex *index,
                     const PXR_NS::HdRprimCollection &collection,
                     HdCyclesSession *renderParam,
                     std::shared_ptr<int> passCount);
  ~HdCyclesRenderPass() override;

  bool IsConverged() const override;

 private:
  void ResetConverged();

  void _Execute(const PXR_NS::HdRenderPassStateSharedPtr &renderPassState,
                const PXR_NS::TfTokenVector &renderTags) override;

  void _MarkCollectionDirty() override;

  HdCyclesSession *_renderParam;
  // Shared counter; destructor decrements without needing a delegate backpointer.
  std::shared_ptr<int> _passCount;
  unsigned int _lastSettingsVersion = 0;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
