/* SPDX-FileCopyrightText: 2025 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <unordered_map>

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>

CCL_NAMESPACE_BEGIN
class Object;
class Scene;
CCL_NAMESPACE_END

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* UsdLux light and shadow linking, as Cycles' light and shadow sets.
 *
 * HdsiLightLinkingSceneIndex gives each light the category of its light:link and shadow:link
 * collections (empty when the collection holds the whole scene), and each geometry prim and
 * instance the categories it is in. A light lights an object when its category is empty or the
 * object is in it; it is shadowed by an object the same way.
 *
 * Cycles gives each object one receiver set and one blocker set, of at most 64, and each
 * emitter the sets it reaches. Objects in the same linked categories share a set; set 0 holds
 * the objects in none, which only unlinked lights reach. */
class HdCyclesLightLinking {
 public:
  /* The categories a receiver and blocker object is in. Remove an object before deleting it. */
  void SetObjectCategories(CCL_NS::Object *object, PXR_NS::VtArray<PXR_NS::TfToken> categories);
  void RemoveObject(CCL_NS::Object *object);

  /* A light's light:link and shadow:link categories. */
  void SetEmitter(CCL_NS::Object *object,
                  const PXR_NS::TfToken &lightLink,
                  const PXR_NS::TfToken &shadowLink);
  void RemoveEmitter(CCL_NS::Object *object);

  /* Assigns the sets after a change, and tags what changed. Call with the scene locked. */
  void Update(CCL_NS::Scene *scene);

 private:
  struct Links {
    PXR_NS::TfToken light;
    PXR_NS::TfToken shadow;
  };

  std::unordered_map<CCL_NS::Object *, PXR_NS::VtArray<PXR_NS::TfToken>> _objects;
  std::unordered_map<CCL_NS::Object *, Links> _emitters;
  bool _dirty = false;
  /* Whether the sets went past Cycles' limit at the last update, so each overflow warns once. */
  bool _warnedLightSets = false;
  bool _warnedShadowSets = false;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
