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

  /* Resolve the ShaderOutput* for (srcPath, outputToken) from a UsdUVTexture,
   * accounting for scale/bias chains and single-channel extraction.
   * Creates a separate_color node lazily on first r/g/b use. */
  CCL_NS::ShaderOutput *ResolveTextureOutput(NodeDesc &srcDesc,
                                             const PXR_NS::SdfPath &srcPath,
                                             const PXR_NS::TfToken &outputName,
                                             CCL_NS::ShaderInput *destInput,
                                             CCL_NS::ShaderGraph *graph);

  CCL_NS::Shader *_shader = nullptr;
  std::unordered_map<PXR_NS::SdfPath, NodeDesc, PXR_NS::SdfPath::Hash> _nodes;

  /* Normal map chain nodes for UsdPreviewSurface (null when none was needed). */
  CCL_NS::ShaderNode *_normalRemapNode = nullptr; /* vector_math multiply_add ×0.5+0.5 */
  CCL_NS::ShaderNode *_normalMapNode = nullptr;   /* normal_map (tangent space) */

  /* Upstream output to route to the Cycles displacement node (null = use constant). */
  CCL_NS::ShaderOutput *_displacementOutput = nullptr;
  float _displacementConstant = 0.0f;
  bool _hasDisplacement = false;

  /* When true, DirtyParams triggers a full graph rebuild because auxiliary nodes
   * (scale/bias chains, normal_map) require it to stay in sync with parameters. */
  bool _rebuildOnDirtyParams = false;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
