/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/volume.h"
#include "hydra/diagnostic.h"
#include "hydra/field.h"
#include "hydra/geometry.inl"
#include "hydra/util.h"
#include "scene/volume.h"

#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneGlobalsSchema.h>
#include <pxr/imaging/hd/volumeFieldBindingSchema.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
   (openvdbAsset)
);
// clang-format on

HdCyclesVolume::HdCyclesVolume(const SdfPath &rprimId) : HdCyclesGeometry(rprimId) {}

HdCyclesVolume::~HdCyclesVolume() = default;

HdDirtyBits HdCyclesVolume::GetInitialDirtyBitsMask() const
{
  HdDirtyBits bits = HdCyclesGeometry::GetInitialDirtyBitsMask();
  bits |= HdChangeTracker::DirtyVolumeField;
  return bits;
}

namespace {

/* The standard attribute a volume field binds to: Cycles' own names, plus Houdini's `vel` and
 * `v` for velocity. */
AttributeStandard FieldStandard(const ustring &name)
{
  for (const AttributeStandard standard : {ATTR_STD_VOLUME_DENSITY,
                                           ATTR_STD_VOLUME_COLOR,
                                           ATTR_STD_VOLUME_FLAME,
                                           ATTR_STD_VOLUME_HEAT,
                                           ATTR_STD_VOLUME_TEMPERATURE,
                                           ATTR_STD_VOLUME_VELOCITY})
  {
    if (name == Attribute::standard_name(standard)) {
      return standard;
    }
  }
  if (name == "vel" || name == "v") {
    return ATTR_STD_VOLUME_VELOCITY;
  }
  return ATTR_STD_NONE;
}

}  // namespace

void HdCyclesVolume::Populate(HdSceneDelegate *sceneDelegate, HdDirtyBits dirtyBits, bool &rebuild)
{
  Scene *const scene = (Scene *)_geom->get_owner();

  /* Velocity blur: the kernel moves each lookup by velocity_scale * velocity across the
   * shutter, velocities being in units a second. Off with the object's motion blur. */
  float velocityScale = 0.0f;
  if (_motionSteps > 1) {
    double timeCodesPerSecond = 24.0;
    const HdSceneGlobalsSchema globals = HdSceneGlobalsSchema::GetFromSceneIndex(
        sceneDelegate->GetRenderIndex().GetTerminalSceneIndex());
    if (const HdDoubleDataSourceHandle ds = globals.GetTimeCodesPerSecond()) {
      timeCodesPerSecond = ds->GetTypedValue(0.0f);
    }
    if (timeCodesPerSecond > 0.0) {
      velocityScale = float((_shutterClose - _shutterOpen) / timeCodesPerSecond);
    }
  }
  _geom->set_velocity_scale(velocityScale);

  if (dirtyBits & HdChangeTracker::DirtyVolumeField) {
    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
    HdVolumeFieldBindingSchema bindings = HdVolumeFieldBindingSchema::GetFromParent(
        prim.dataSource);

    for (const TfToken &fieldName : bindings.GetVolumeFieldBindingNames()) {
      auto pathDs = bindings.GetVolumeFieldBinding(fieldName);
      if (!pathDs) {
        continue;
      }
      const SdfPath fieldId = pathDs->GetTypedValue(0.0f);

      if (auto *const openvdbAsset = static_cast<HdCyclesField *>(
              sceneDelegate->GetRenderIndex().GetBprim(_tokens->openvdbAsset, fieldId)))
      {
        const ustring name(fieldName.GetString());
        const AttributeStandard standard = FieldStandard(name);

        // Skip attributes that are not needed
        if ((standard != ATTR_STD_NONE && _geom->need_attribute(scene, standard)) ||
            _geom->need_attribute(scene, name))
        {
          Attribute *const attr = (standard != ATTR_STD_NONE) ?
                                      _geom->attributes.add(standard) :
                                      _geom->attributes.add(name, TypeFloat, ATTR_ELEMENT_VOXEL);
          attr->data_voxel_for_write() = openvdbAsset->GetImageHandle();
        }
      }
    }

    _geom->merge_grids(scene);

    rebuild = true;
  }

  /* The frame's place in the shutter is the camera's, set from the final shutter. Checked once
   * per shutter; tests/motion_blur matches "is treated as". */
  const std::pair<float, float> shutterPair(_shutterOpen, _shutterClose);
  if (velocityScale != 0.0f && _geom->attributes.find(ATTR_STD_VOLUME_VELOCITY) &&
      std::exchange(_checkedShutter, shutterPair) != shutterPair)
  {
    HdCyclesShutter shutter;
    shutter.open = _shutterOpen;
    shutter.close = _shutterClose;
    if (!shutter.VolumeBlurExact()) {
      HDCYCLES_WARN(
          "%s: volume velocity blur is placed exactly only for a shutter centred on the frame or "
          "opening at it; the shutter %g to %g is treated as %s.",
          GetId().GetText(),
          _shutterOpen,
          _shutterClose,
          shutter.VolumeBlurCentred() ? "centred" : "opening at the frame");
    }
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
