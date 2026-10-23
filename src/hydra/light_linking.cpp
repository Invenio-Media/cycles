/* SPDX-FileCopyrightText: 2025 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/light_linking.h"
#include "hydra/diagnostic.h"
#include "kernel/types.h"
#include "scene/light.h"
#include "scene/object.h"
#include "scene/scene.h"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

using Categories = std::vector<TfToken>;
using CategorySet = std::unordered_set<TfToken, TfToken::HashFunctor>;

/* Gives each object the set of the linked categories it is in, and returns each set's
 * categories, sorted, by set index. Set 0 is the objects in none. */
template<typename Assign>
std::vector<Categories> AssignSets(const std::unordered_map<Object *, VtArray<TfToken>> &objects,
                                   const CategorySet &linked,
                                   const Assign &assign,
                                   bool &warned)
{
  std::vector<Categories> sets(1);
  std::map<Categories, uint> index = {{Categories(), 0}};
  for (const auto &[object, categories] : objects) {
    Categories key;
    for (const TfToken &category : categories) {
      if (linked.count(category)) {
        key.push_back(category);
      }
    }
    uint set = 0;
    if (!key.empty()) {
      auto it = index.find(key);
      if (it != index.end()) {
        set = it->second;
      }
      else if (sets.size() < LIGHT_LINK_SET_MAX) {
        set = uint(sets.size());
        index.emplace(key, set);
        sets.push_back(std::move(key));
      }
      else if (!warned) {
        warned = true;
        HDCYCLES_WARN(
            "Light linking: the scene needs more than %d different combinations of linked "
            "lights, which is as many as Cycles has; objects beyond them are lit and shadowed "
            "only by unlinked lights.",
            int(LIGHT_LINK_SET_MAX));
      }
    }
    assign(object, set);
  }
  return sets;
}

/* The sets a light reaches: all when it isn't linked, else those holding its category. */
uint64_t Membership(const TfToken &category, const std::vector<Categories> &sets)
{
  if (category.IsEmpty()) {
    return LIGHT_LINK_MASK_ALL;
  }
  uint64_t mask = 0;
  for (size_t i = 0; i < sets.size(); ++i) {
    if (std::binary_search(sets[i].begin(), sets[i].end(), category)) {
      mask |= uint64_t(1) << i;
    }
  }
  return mask;
}

}  // namespace

void HdCyclesLightLinking::SetObjectCategories(Object *object, VtArray<TfToken> categories)
{
  if (categories.empty()) {
    /* Objects in no category are in set 0, the default; only those in one are kept. */
    const auto it = _objects.find(object);
    if (it != _objects.end()) {
      _objects.erase(it);
      object->set_receiver_light_set(0);
      object->set_blocker_shadow_set(0);
      _dirty = true;
    }
    return;
  }

  std::sort(categories.begin(), categories.end());
  const auto [it, inserted] = _objects.try_emplace(object);
  if (!inserted && it->second == categories) {
    return;
  }
  it->second = std::move(categories);
  _dirty = true;
}

void HdCyclesLightLinking::RemoveObject(Object *object)
{
  /* The sets the object was in stay until the next change; reaching them does no harm. */
  _objects.erase(object);
}

void HdCyclesLightLinking::SetEmitter(Object *object,
                                      const TfToken &lightLink,
                                      const TfToken &shadowLink)
{
  const auto [it, inserted] = _emitters.try_emplace(object);
  if (!inserted && it->second.light == lightLink && it->second.shadow == shadowLink) {
    return;
  }
  it->second = {lightLink, shadowLink};
  /* A new unlinked light keeps the default, reaching every set. */
  if (!inserted || !lightLink.IsEmpty() || !shadowLink.IsEmpty()) {
    _dirty = true;
  }
}

void HdCyclesLightLinking::RemoveEmitter(Object *object)
{
  _emitters.erase(object);
}

void HdCyclesLightLinking::Update(Scene *scene)
{
  if (!_dirty) {
    return;
  }
  _dirty = false;

  CategorySet lightCategories;
  CategorySet shadowCategories;
  for (const auto &[object, links] : _emitters) {
    if (!links.light.IsEmpty()) {
      lightCategories.insert(links.light);
    }
    if (!links.shadow.IsEmpty()) {
      shadowCategories.insert(links.shadow);
    }
  }

  bool changed = false;
  const auto tag = [scene, &changed](Object *object) {
    if (object->is_modified()) {
      object->tag_update(scene);
      changed = true;
    }
  };

  const std::vector<Categories> lightSets = AssignSets(
      _objects,
      lightCategories,
      [&](Object *object, const uint set) {
        object->set_receiver_light_set(set);
        tag(object);
      },
      _warnedTooManySets);
  const std::vector<Categories> shadowSets = AssignSets(
      _objects,
      shadowCategories,
      [&](Object *object, const uint set) {
        object->set_blocker_shadow_set(set);
        tag(object);
      },
      _warnedTooManySets);

  for (const auto &[object, links] : _emitters) {
    object->set_light_set_membership(Membership(links.light, lightSets));
    object->set_shadow_set_membership(Membership(links.shadow, shadowSets));
    tag(object);
  }

  /* The light tree is built from the emitters' and the receivers' sets. */
  if (changed) {
    scene->light_manager->tag_update(scene, LightManager::LIGHT_MODIFIED);
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
