/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>

/* Forward declarations not already in config.h */
namespace CCL_NS {
class ShaderInput;
class ShaderOutput;
}  // namespace CCL_NS

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesMaterial final : public PXR_NS::HdMaterial {
 public:
  HdCyclesMaterial(const PXR_NS::SdfPath &sprimId);
  ~HdCyclesMaterial() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

  CCL_NS::Shader *GetCyclesShader() const
  {
    return _shader;
  }

 private:
  struct NodeDesc {
    CCL_NS::ShaderNode *node = nullptr;
    const class UsdToCyclesMapping *mapping = nullptr;
    /* Auxiliary nodes for UsdUVTexture scale/bias (null when scale/bias are at defaults). */
    CCL_NS::ShaderNode *scaleBiasRgb = nullptr;   /* vector_math multiply_add */
    CCL_NS::ShaderNode *scaleBiasAlpha = nullptr; /* math multiply_add */
    /* Created lazily when an r/g/b output is first consumed. */
    CCL_NS::ShaderNode *separateColor = nullptr; /* separate_color */
    /* UsdPreviewSurface: the ×0.5+0.5 remap in front of its normal map, when it has one. */
    CCL_NS::ShaderNode *normalRemap = nullptr;
    /* UsdPreviewSurface displacement: the connected output, or else the authored constant. */
    CCL_NS::ShaderOutput *displacementOutput = nullptr;
    float displacementConstant = 0.0f;
    bool hasDisplacement = false;
  };

  void Initialize(PXR_NS::HdRenderParam *renderParam);

  void UpdateParameters(NodeDesc &nodeDesc,
                        PXR_NS::HdMaterialNodeParameterContainerSchema params,
                        const PXR_NS::SdfPath &nodePath);

  void UpdateParameters(PXR_NS::HdMaterialNetworkSchema network);

  void UpdateConnections(NodeDesc &nodeDesc,
                         PXR_NS::HdMaterialNodeSchema nodeSchema,
                         const PXR_NS::SdfPath &nodePath,
                         CCL_NS::ShaderGraph *shaderGraph);

  void PopulateShaderGraph(PXR_NS::HdMaterialNetworkSchema network);

  /* The output an upstream node provides for a USD output name. For a UsdUVTexture this
   * accounts for scale/bias chains and single-channel extraction, creating a separate_color
   * node on first r/g/b use. */
  CCL_NS::ShaderOutput *ResolveOutput(NodeDesc &srcDesc,
                                      const PXR_NS::TfToken &outputName,
                                      CCL_NS::ShaderInput *destInput,
                                      CCL_NS::ShaderGraph *graph);

  CCL_NS::Shader *_shader = nullptr;
  std::unordered_map<PXR_NS::SdfPath, NodeDesc, PXR_NS::SdfPath::Hash> _nodes;

  /* The parameters that add or remove helper nodes (UsdUVTexture scale/bias,
   * UsdPreviewSurface normal, cutout, displacement and specular workflow) as of the last
   * graph build. A parameter edit that changes them rebuilds the graph; any other edit
   * updates the nodes in place. */
  std::string _structuralKey;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
