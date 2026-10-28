/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hgi/hgi.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
    ((shaderNodes,      "cycles:shader_nodes")) \
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
    ((sceneTextureLimit,       "cycles:scene:texture_limit")) \
    ((sceneHairShape,          "cycles:scene:hair_shape")) \
    ((sceneHairSubdivisions,   "cycles:scene:hair_subdivisions"))
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
  // A batch render (husk's renderMode), which tiles large frames by default.
  bool _batch = false;
  std::unique_ptr<HdCyclesSession> _renderParam;
  // Working colorspace from UsdRenderSettings.renderingColorSpace; empty means the OCIO
  // config's scene_linear role.
  std::string _renderingColorSpace;
  // The renderingColorSpace value last received, which differs from _renderingColorSpace
  // when it named a colorspace the config does not have.
  std::string _requestedColorSpace;
  // The last rendering color space not in the OCIO config, warned about once.
  std::string _unknownColorSpace;

  // Whether the background should be kept transparent even when a dome light is present.
  bool _filmTransparent = false;

  // Device type actually in use (upper-case Cycles type name, e.g. "CPU", "METAL").
  std::string _effectiveDeviceType;

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
  // The requested device types already warned to have no devices: BuildSessionParams, which
  // falls back to the CPU, runs for every restart-only setting.
  mutable std::unordered_set<std::string> _warnedNoDevice;

  // Count of real (non-fallback) prims that have been created. While this is zero
  // restart-only settings trigger a Session rebuild; after it goes positive they are ignored.
  int _realPrimCount = 0;

  // Count of live HdCyclesRenderPass instances, shared into each pass so the pass
  // destructor can decrement it without a backpointer to the delegate.
  std::shared_ptr<int> _renderPassCount;

  // Hydra's scene state version at the last CommitResources.
  std::atomic<unsigned> _sceneStateVersion = 0;
  // Cryptomatte render stats, rebuilt only when the scene state version or the film's
  // cryptomatte types change, not on every GetRenderStats() poll.
  mutable std::mutex _cryptomatteStatsMutex;
  mutable PXR_NS::VtDictionary _cryptomatteStats;
  mutable unsigned _cryptomatteStatsVersion = 0;
  mutable int _cryptomatteStatsTypes = 0;

  // Makes Cycles render in this colorspace of the OCIO config; empty, or a name the config
  // does not have, means the config's scene_linear role.
  void ApplyRenderingColorSpace(std::string colorSpace);

  // The cryptomatte render stats, from the cache unless the scene changed. Unless told to
  // wait for the scene, returns the cache while the scene is being updated.
  PXR_NS::VtDictionary GetCryptomatteStats(bool waitForScene) const;

  // Rebuild the Cycles Session from _restartSettings, then replay _appliedSettings.
  void RebuildSession();

  // Build the current session params (device selection + threads + tile).
  std::pair<CCL_NS::SessionParams, std::string> BuildSessionParams() const;
  // Build the current scene params (BVH / texture cache).
  CCL_NS::SceneParams BuildSceneParams() const;

  // Returns true when a restart-only setting change should trigger a rebuild.
  // Defined in render_delegate.cpp where HdCyclesSession is fully defined.
  bool CanRebuildSession() const;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
