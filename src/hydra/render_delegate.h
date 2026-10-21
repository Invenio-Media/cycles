/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hgi/hgi.h>

#include <unordered_map>
#include <unordered_set>

HDCYCLES_NAMESPACE_OPEN_SCOPE

// clang-format off
#define HD_CYCLES_RENDER_SETTINGS_TOKENS \
    (stageMetersPerUnit) \
    ((device,           "cycles:device")) \
    ((threads,          "cycles:threads")) \
    ((timeLimit,        "cycles:time_limit")) \
    ((samples,          "cycles:samples")) \
    ((sampleOffset,     "cycles:sample_offset")) \
    (renderingColorSpace) \
    ((settingsManifest, "cycles:settings_manifest")) \
    /* film sockets */ \
    ((filmExposure,                 "cycles:film:exposure")) \
    ((filmPassAlphaThreshold,       "cycles:film:pass_alpha_threshold")) \
    ((filmFilterType,               "cycles:film:filter_type")) \
    ((filmFilterWidth,              "cycles:film:filter_width")) \
    ((filmUseApproxShadowCatcher,   "cycles:film:use_approximate_shadow_catcher")) \
    ((filmTransparent,              "cycles:film:transparent")) \
    /* session restart params */ \
    ((sessionUseAutoTile,  "cycles:session:use_auto_tile")) \
    ((sessionTileSize,     "cycles:session:tile_size")) \
    /* scene restart params */ \
    ((sceneUseBvhSpatialSplit, "cycles:scene:use_bvh_spatial_split")) \
    ((sceneUseTextureCache,    "cycles:scene:use_texture_cache")) \
    ((sceneAutoTextureCache,   "cycles:scene:auto_texture_cache")) \
    ((sceneTextureCachePath,   "cycles:scene:texture_cache_path")) \
    ((sceneTextureLimit,       "cycles:scene:texture_limit"))
// clang-format on

TF_DECLARE_PUBLIC_TOKENS(HdCyclesRenderSettingsTokens, HD_CYCLES_RENDER_SETTINGS_TOKENS);

class HdCyclesDelegate final : public PXR_NS::HdRenderDelegate {
 public:
  HdCyclesDelegate(const PXR_NS::HdRenderSettingsMap &settingsMap,
                   CCL_NS::Session *session_ = nullptr,
                   const bool keep_nodes = false);
  ~HdCyclesDelegate() override;

  void SetDrivers(const PXR_NS::HdDriverVector &drivers) override;

  bool IsDisplaySupported() const;

  PXR_NS::Hgi *GetHgi() const
  {
    return _hgi;
  }

  const PXR_NS::TfTokenVector &GetSupportedRprimTypes() const override;
  const PXR_NS::TfTokenVector &GetSupportedSprimTypes() const override;
  const PXR_NS::TfTokenVector &GetSupportedBprimTypes() const override;

  PXR_NS::HdRenderParam *GetRenderParam() const override;

  PXR_NS::HdResourceRegistrySharedPtr GetResourceRegistry() const override;

  PXR_NS::HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;

  bool IsPauseSupported() const override;

  bool Pause() override;
  bool Resume() override;

  PXR_NS::HdRenderPassSharedPtr CreateRenderPass(
      PXR_NS::HdRenderIndex *index, const PXR_NS::HdRprimCollection &collection) override;

  // Called by HdCyclesRenderPass destructor to keep _renderPassCount accurate.
  void NotifyRenderPassDestroyed()
  {
    if (_renderPassCount > 0) {
      --_renderPassCount;
    }
  }

  PXR_NS::HdInstancer *CreateInstancer(PXR_NS::HdSceneDelegate *delegate,
                                       const PXR_NS::SdfPath &id) override;
  void DestroyInstancer(PXR_NS::HdInstancer *instancer) override;

  PXR_NS::HdRprim *CreateRprim(const PXR_NS::TfToken &typeId,
                               const PXR_NS::SdfPath &rprimId) override;
  void DestroyRprim(PXR_NS::HdRprim *rPrim) override;

  PXR_NS::HdSprim *CreateSprim(const PXR_NS::TfToken &typeId,
                               const PXR_NS::SdfPath &sprimId) override;
  PXR_NS::HdSprim *CreateFallbackSprim(const PXR_NS::TfToken &typeId) override;
  void DestroySprim(PXR_NS::HdSprim *sPrim) override;

  PXR_NS::HdBprim *CreateBprim(const PXR_NS::TfToken &typeId,
                               const PXR_NS::SdfPath &bprimId) override;
  PXR_NS::HdBprim *CreateFallbackBprim(const PXR_NS::TfToken &typeId) override;
  void DestroyBprim(PXR_NS::HdBprim *bPrim) override;

  void CommitResources(PXR_NS::HdChangeTracker *tracker) override;

  PXR_NS::TfToken GetMaterialBindingPurpose() const override;

  PXR_NS::TfTokenVector GetMaterialRenderContexts() const override;

  PXR_NS::VtDictionary GetRenderStats() const override;

  PXR_NS::HdAovDescriptor GetDefaultAovDescriptor(const PXR_NS::TfToken &name) const override;

  void SetRenderSetting(const PXR_NS::TfToken &key, const PXR_NS::VtValue &value) override;

  PXR_NS::VtValue GetRenderSetting(const PXR_NS::TfToken &key) const override;

 private:
  PXR_NS::Hgi *_hgi = nullptr;
  std::unique_ptr<HdCyclesSession> _renderParam;
  // Working colorspace from UsdRenderSettings.renderingColorSpace; empty means the OCIO
  // config's scene_linear role.
  std::string _renderingColorSpace;
  // The renderingColorSpace value last received, which differs from _renderingColorSpace
  // when it named a colorspace the config does not have.
  std::string _requestedColorSpace;

  // Whether the background should be kept transparent even when a dome light is present.
  bool _filmTransparent = false;

  // Device type actually in use (upper-case Cycles type name, e.g. "CPU", "METAL").
  mutable std::string _effectiveDeviceType;

  // Settings that require a full Session rebuild (restart:true).
  // Stored so they can be replayed when a rebuild is triggered before any prims exist.
  std::unordered_map<PXR_NS::TfToken, PXR_NS::VtValue, PXR_NS::TfToken::HashFunctor>
      _restartSettings;

  // Non-restart settings applied so far; replayed on rebuild so that film/integrator
  // settings that arrived before the restart setting are not lost.
  std::unordered_map<PXR_NS::TfToken, PXR_NS::VtValue, PXR_NS::TfToken::HashFunctor>
      _appliedSettings;

  // Keys for which we have already emitted "restart required" warnings (post-prim).
  std::unordered_set<PXR_NS::TfToken, PXR_NS::TfToken::HashFunctor> _warnedRestartOnly;

  // Count of real (non-fallback) prims that have been created. While this is zero
  // restart-only settings trigger a Session rebuild; after it goes positive they are ignored.
  int _realPrimCount = 0;

  // Count of live HdCyclesRenderPass instances. Session rebuild is only safe when both
  // _realPrimCount and _renderPassCount are zero; a live render pass holds a raw pointer
  // into _renderParam and would dangle after a rebuild.
  int _renderPassCount = 0;

  // Makes Cycles render in this colorspace of the OCIO config; empty, or a name the config
  // does not have, means the config's scene_linear role.
  void ApplyRenderingColorSpace(std::string colorSpace);

  // Rebuild the Cycles Session from _restartSettings, then replay _appliedSettings.
  void RebuildSession();

  // Build the current session params (device selection + threads + tile).
  CCL_NS::SessionParams BuildSessionParams() const;
  // Build the current scene params (BVH / texture cache).
  CCL_NS::SceneParams BuildSceneParams() const;

  // Returns true when a restart-only setting change should trigger a rebuild.
  // False means the setting should be ignored with a TF_WARN.
  bool CanRebuildSession() const
  {
    return _realPrimCount == 0 && _renderPassCount == 0;
  }
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
