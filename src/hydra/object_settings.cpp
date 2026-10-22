/* SPDX-FileCopyrightText: 2025 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/object_settings.h"
#include "kernel/types.h"
#include "scene/object.h"

#include <pxr/base/tf/token.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

struct RayVisibility {
  const char *name;
  unsigned flags;
};

const RayVisibility kRayVisibility[] = {
    {"visibility:camera", PATH_RAY_VISIBILITY_CAMERA},
    {"visibility:diffuse", PATH_RAY_VISIBILITY_DIFFUSE},
    {"visibility:glossy", PATH_RAY_VISIBILITY_GLOSSY},
    {"visibility:transmission", PATH_RAY_VISIBILITY_TRANSMIT},
    {"visibility:volume_scatter", PATH_RAY_VISIBILITY_VOLUME_SCATTER},
    {"visibility:shadow", PATH_RAY_VISIBILITY_SHADOW},
};

std::string ReadString(const VtValue &value)
{
  if (value.IsHolding<TfToken>()) {
    return value.UncheckedGet<TfToken>().GetString();
  }
  return HdCyclesSettingValue<std::string>(value, std::string());
}

}  // namespace

void HdCyclesApplyObjectSettings(Object *object,
                                 unsigned visibility,
                                 const HdCyclesSettingReader &read)
{
  for (const RayVisibility &ray : kRayVisibility) {
    if (!HdCyclesSettingValue<bool>(read(ray.name), true)) {
      visibility &= ~ray.flags;
    }
  }
  object->set_visibility(visibility);
  object->set_use_holdout(HdCyclesSettingValue<bool>(read("use_holdout"), false));
  object->set_is_shadow_catcher(HdCyclesSettingValue<bool>(read("is_shadow_catcher"), false));
  object->set_is_caustics_caster(HdCyclesSettingValue<bool>(read("is_caustics_caster"), false));
  object->set_is_caustics_receiver(
      HdCyclesSettingValue<bool>(read("is_caustics_receiver"), false));
  object->set_lightgroup(ustring(ReadString(read("lightgroup"))));
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
