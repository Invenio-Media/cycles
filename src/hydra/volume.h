/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "hydra/geometry.h"

#include <pxr/imaging/hd/volume.h>

#include <utility>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesVolume final : public HdCyclesGeometry<PXR_NS::HdVolume, CCL_NS::Volume> {
 public:
  HdCyclesVolume(const PXR_NS::SdfPath &rprimId);
  ~HdCyclesVolume() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

 private:
  void Populate(PXR_NS::HdSceneDelegate *sceneDelegate,
                PXR_NS::HdDirtyBits dirtyBits,
                bool &rebuild) override;

  // The shutter last checked for velocity blur's fit, so each shutter warns once if inexact.
  std::pair<float, float> _checkedShutter = {0.0f, 0.0f};
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
