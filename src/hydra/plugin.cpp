/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/plugin.h"
#include "hydra/render_delegate.h"
#include "util/log.h"
#include "util/path.h"

#include <algorithm>

#include <pxr/base/arch/fileSystem.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/thisPlugin.h>
#include <pxr/base/tf/envSetting.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#include <pxr/imaging/hdsi/lightLinkingSceneIndex.h>
#include <pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_ENV_SETTING(CYCLES_LOGGING, false, "Enable Cycles logging")
TF_DEFINE_ENV_SETTING(CYCLES_LOGGING_LEVEL, "warning", "Cycles logging level")

HdCyclesPlugin::HdCyclesPlugin()
{
  const PlugPluginPtr plugin = PLUG_THIS_PLUGIN;
  // Initialize Cycles paths relative to the plugin resource path
  const std::string rootPath = PXR_NS::ArchAbsPath(plugin->GetResourcePath());
  CCL_NS::path_init(std::move(rootPath));

  if (TfGetEnvSetting(CYCLES_LOGGING)) {
    CCL_NS::log_level_set(TfGetEnvSetting(CYCLES_LOGGING_LEVEL));
  }
}

HdCyclesPlugin::~HdCyclesPlugin() {}

#if PXR_VERSION < 2302
bool HdCyclesPlugin::IsSupported() const
{
  return true;
}
#else
#  if PXR_VERSION >= 2511
bool HdCyclesPlugin::IsSupported(HdRendererCreateArgs const & /*rendererCreateArgs*/,
                                 std::string * /*reasonWhyNot*/) const
{
  return true;
}
#  endif
bool HdCyclesPlugin::IsSupported(bool /*gpuEnabled*/) const
{
  return true;
}
#endif

HdRenderDelegate *HdCyclesPlugin::CreateRenderDelegate()
{
  return CreateRenderDelegate({});
}

HdRenderDelegate *HdCyclesPlugin::CreateRenderDelegate(const HdRenderSettingsMap &settingsMap)
{
  return new HD_CYCLES_NS::HdCyclesDelegate(settingsMap);
}

void HdCyclesPlugin::DeleteRenderDelegate(HdRenderDelegate *renderDelegate)
{
  delete renderDelegate;
}

// USD's type system accounts for namespace, so we'd have to register our name as
// HdCycles::HdCyclesPlugin in plugInfo.json, which isn't all that bad for JSON,
// but those colons may cause issues for any USD specific tooling. So just put our
// plugin class in the pxr namespace (which USD's type system will elide).
TF_REGISTRY_FUNCTION(TfType)
{
  HdRendererPluginRegistry::Define<PXR_NS::HdCyclesPlugin>();
}

namespace {

/* A mesh light's settings (UsdLuxMeshLightAPI) reach its geometry as a light data source,
 * which Hydra's dirty bits for rprims don't cover: an edit left the geometry clean. This
 * dirties the geometry's material bindings as well, so it syncs and rebuilds its light
 * shader (HdCyclesGeometry::UpdateMeshLight). */
TF_DECLARE_REF_PTRS(HdCyclesMeshLightSceneIndex);

class HdCyclesMeshLightSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
 public:
  static HdCyclesMeshLightSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr &inputScene)
  {
    return TfCreateRefPtr(new HdCyclesMeshLightSceneIndex(inputScene));
  }

  HdSceneIndexPrim GetPrim(const SdfPath &primPath) const override
  {
    return _GetInputSceneIndex()->GetPrim(primPath);
  }

  SdfPathVector GetChildPrimPaths(const SdfPath &primPath) const override
  {
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
  }

 protected:
  void _PrimsAdded(const HdSceneIndexBase & /*sender*/,
                   const HdSceneIndexObserver::AddedPrimEntries &entries) override
  {
    _SendPrimsAdded(entries);
  }

  void _PrimsRemoved(const HdSceneIndexBase & /*sender*/,
                     const HdSceneIndexObserver::RemovedPrimEntries &entries) override
  {
    _SendPrimsRemoved(entries);
  }

  void _PrimsDirtied(const HdSceneIndexBase & /*sender*/,
                     const HdSceneIndexObserver::DirtiedPrimEntries &entries) override
  {
    const HdDataSourceLocator &bindings = HdMaterialBindingsSchema::GetDefaultLocator();
    const auto meshLightEdit = [&](const HdSceneIndexObserver::DirtiedPrimEntry &entry) {
      return entry.dirtyLocators.Intersects(HdLightSchema::GetDefaultLocator()) &&
             !entry.dirtyLocators.Intersects(bindings) &&
             HdPrimTypeIsGprim(_GetInputSceneIndex()->GetPrim(entry.primPath).primType);
    };
    const auto first = std::find_if(entries.begin(), entries.end(), meshLightEdit);
    if (first == entries.end()) {
      _SendPrimsDirtied(entries);
      return;
    }
    HdSceneIndexObserver::DirtiedPrimEntries dirtied(entries);
    for (size_t i = first - entries.begin(); i < entries.size(); ++i) {
      if (meshLightEdit(entries[i])) {
        dirtied[i].dirtyLocators.insert(bindings);
      }
    }
    _SendPrimsDirtied(dirtied);
  }

 private:
  explicit HdCyclesMeshLightSceneIndex(const HdSceneIndexBaseRefPtr &inputScene)
      : HdSingleInputFilteringSceneIndexBase(inputScene)
  {
  }
};

}  // namespace

// Cycles has no implicit surfaces, so turn UsdGeom spheres, cubes, cones, cylinders, capsules
// and planes into meshes before they reach the delegate, as Storm and HdPrman do.
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
  const HdDataSourceBaseHandle toMesh = HdRetainedTypedSampledDataSource<TfToken>::New(
      HdsiImplicitSurfaceSceneIndexTokens->toMesh);
  const HdContainerDataSourceHandle inputArgs = HdRetainedContainerDataSource::New(
      HdPrimTypeTokens->sphere,
      toMesh,
      HdPrimTypeTokens->cube,
      toMesh,
      HdPrimTypeTokens->cone,
      toMesh,
      HdPrimTypeTokens->cylinder,
      toMesh,
      HdPrimTypeTokens->capsule,
      toMesh,
      HdPrimTypeTokens->plane,
      toMesh);

  HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
      "Cycles",
      [](const std::string & /*renderInstanceId*/,
         const HdSceneIndexBaseRefPtr &inputScene,
         const HdContainerDataSourceHandle &inputArgs) -> HdSceneIndexBaseRefPtr {
        return HdsiImplicitSurfaceSceneIndex::New(inputScene, inputArgs);
      },
      inputArgs,
      0,
      HdSceneIndexPluginRegistry::InsertionOrderAtStart);

  // UsdLux light:link and shadow:link collections, as categories on lights and geometry
  // (hydra/light_linking.h), as HdPrman does.
  HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
      "Cycles",
      [](const std::string & /*renderInstanceId*/,
         const HdSceneIndexBaseRefPtr &inputScene,
         const HdContainerDataSourceHandle &inputArgs) -> HdSceneIndexBaseRefPtr {
        return HdsiLightLinkingSceneIndex::New(inputScene, inputArgs);
      },
      nullptr,
      0,
      HdSceneIndexPluginRegistry::InsertionOrderAtStart);

  // UsdGeom velocities and accelerations, as points that change over time, so deformation
  // motion blur samples them like time-sampled points (HdCyclesGeometry::SamplePoints). It
  // takes the stage's time codes per second from the scene globals.
  HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
      "Cycles",
      [](const std::string & /*renderInstanceId*/,
         const HdSceneIndexBaseRefPtr &inputScene,
         const HdContainerDataSourceHandle &inputArgs) -> HdSceneIndexBaseRefPtr {
        return HdsiVelocityMotionResolvingSceneIndex::New(inputScene, inputArgs);
      },
      nullptr,
      0,
      HdSceneIndexPluginRegistry::InsertionOrderAtStart);

  // Mesh light edits, as edits of the geometry (HdCyclesMeshLightSceneIndex).
  HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
      "Cycles",
      [](const std::string & /*renderInstanceId*/,
         const HdSceneIndexBaseRefPtr &inputScene,
         const HdContainerDataSourceHandle & /*inputArgs*/) -> HdSceneIndexBaseRefPtr {
        return HdCyclesMeshLightSceneIndex::New(inputScene);
      },
      nullptr,
      0,
      HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

PXR_NAMESPACE_CLOSE_SCOPE
