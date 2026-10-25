/* SPDX-FileCopyrightText: 2025 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>

#include <functional>

CCL_NAMESPACE_BEGIN
class Object;
CCL_NAMESPACE_END

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Reads a setting by its name within its namespace, such as "visibility:camera", and gives
 * an empty value when it is not authored. */
using HdCyclesSettingReader = std::function<PXR_NS::VtValue(const char *name)>;

/* Cycles' per-object settings, named after the Object's sockets:
 *   visibility:camera, visibility:diffuse, visibility:glossy, visibility:transmission,
 *   visibility:volume_scatter, visibility:shadow  turn ray types off (they default on);
 *   use_holdout, is_shadow_catcher, is_caustics_caster, is_caustics_receiver, lightgroup.
 * Gprims author them as constant primvars cycles:object:<name>, which inherit down the
 * hierarchy as Karma's karma:object: primvars do; lights as attributes cycles:light:<name>.
 * `visibility` is the object's visibility without them; they can only take rays away. */
void HdCyclesApplyObjectSettings(CCL_NS::Object *object,
                                 unsigned visibility,
                                 const HdCyclesSettingReader &read);

/* An authored value as the type a setting takes, or the fallback. */
template<typename T> T HdCyclesSettingValue(const PXR_NS::VtValue &value, const T fallback)
{
  if (value.IsHolding<T>()) {
    return value.UncheckedGet<T>();
  }
  return PXR_NS::VtValue::Cast<T>(value).template GetWithDefault<T>(fallback);
}

/* Whether primvars:cycles:object:volume:space, as read, is "object", where a volume's step and
 * density are per object unit; anything else is world space. Authored as a token or string. */
bool HdCyclesVolumeObjectSpace(const PXR_NS::VtValue &space);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
