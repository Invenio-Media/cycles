/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "hydra/geometry.h"

#include <pxr/imaging/hd/basisCurves.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

// A Cycles curve key as a weighted sum of up to four authored values.
struct HdCyclesCurveKey {
  int count = 0;
  int index[4];
  float weight[4];
};

class HdCyclesCurves final : public HdCyclesGeometry<PXR_NS::HdBasisCurves, CCL_NS::Hair> {
 public:
  HdCyclesCurves(const PXR_NS::SdfPath &rprimId);
  ~HdCyclesCurves() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

 private:
  PXR_NS::HdDirtyBits _PropagateDirtyBits(PXR_NS::HdDirtyBits bits) const override;

  void Populate(PXR_NS::HdSceneDelegate *sceneDelegate,
                PXR_NS::HdDirtyBits dirtyBits,
                bool &rebuild) override;

  void PopulatePoints(PXR_NS::HdSceneDelegate *sceneDelegate);
  void PopulateWidths(PXR_NS::HdSceneDelegate *sceneDelegate);

  void PopulatePrimvars(PXR_NS::HdSceneDelegate *sceneDelegate);

  void PopulateTopology(PXR_NS::HdSceneDelegate *sceneDelegate);

  // A per-key array of vertex or varying data, resampled into Cycles' keys; empty when a value
  // of that kind can't be resampled or the authored array is short.
  PXR_NS::VtValue ResampleVertex(const PXR_NS::VtValue &value) const;
  PXR_NS::VtValue ResampleVarying(const PXR_NS::VtValue &value) const;

  // Cycles' curves are Catmull-Rom splines through every key. Cubic curves of other bases, or
  // whose ends are phantom points, are sampled along their basis into keys; these say how,
  // from the authored vertex and varying data. Empty when the keys are the authored points.
  std::vector<HdCyclesCurveKey> _vertexKeys;
  std::vector<HdCyclesCurveKey> _varyingKeys;
  size_t _numVertex = 0;
  size_t _numVarying = 0;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
