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
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

using Categories = std::vector<TfToken>;
using CategorySet = std::unordered_set<TfToken, TfToken::HashFunctor>;

/* Gives each object the set of the linked categories it is in, and returns each set's
 * categories, sorted, by set index. Set 0 is the objects in none. The sets are numbered in
 * the sorted order of their categories, so the numbering, and which combinations are left out
 * past Cycles' limit, don't depend on the order of the objects. */
template<typename Assign>
std::vector<Categories> AssignSets(const std::unordered_map<Object *, VtArray<TfToken>> &objects,
                                   const CategorySet &linked,
                                   const Assign &assign,
                                   const char *kind,
                                   bool &warned)
{
  /* Nothing linked of this kind: every object is in set 0. */
  if (linked.empty()) {
    for (const auto &[object, categories] : objects) {
      assign(object, 0u);
    }
    warned = false;
    return std::vector<Categories>(1);
  }

  std::vector<std::pair<Object *, Categories>> keys;
  keys.reserve(objects.size());
  std::set<Categories> distinct;
  for (const auto &[object, categories] : objects) {
    Categories key;
    for (const TfToken &category : categories) {
      if (linked.count(category)) {
        key.push_back(category);
      }
    }
    if (!key.empty()) {
      distinct.insert(key);
    }
    keys.emplace_back(object, std::move(key));
  }

  std::vector<Categories> sets(1);
  std::map<Categories, uint> index;
  for (const Categories &key : distinct) {
    if (sets.size() == LIGHT_LINK_SET_MAX) {
      break;
    }
    index.emplace(key, uint(sets.size()));
    sets.push_back(key);
  }

  /* Warned once per time the scene goes past the limit. */
  const bool overflow = distinct.size() >= LIGHT_LINK_SET_MAX;
  if (overflow && !warned) {
    HDCYCLES_WARN(
        "Light linking: the scene needs %zu different combinations of %s-linked lights, more "
        "than the %d Cycles has; objects beyond them are reached only by unlinked lights.",
        distinct.size(),
        kind,
        int(LIGHT_LINK_SET_MAX) - 1);
  }
  warned = overflow;

  for (const auto &[object, key] : keys) {
    const auto it = index.find(key);
    assign(object, it == index.end() ? 0u : it->second);
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
    /* Objects in no category are in set 0, the default; only those in one are kept. One that
     * leaves its last category stays until Update has put it back in set 0. */
    const auto it = _objects.find(object);
    if (it != _objects.end() && !it->second.empty()) {
      it->second = VtArray<TfToken>();
      _dirty = true;
    }
    return;
  }

  /* Sorted and unique: a category can arrive from the prim and from its instancers, and
   * repeats would make objects in the same categories take different sets. */
  std::sort(categories.begin(), categories.end());
  categories.resize(std::unique(categories.begin(), categories.end()) - categories.begin());
  const auto [it, inserted] = _objects.try_emplace(object);
  if (!inserted && it->second == categories) {
    return;
  }
  it->second = std::move(categories);
  _dirty = true;
}

void HdCyclesLightLinking::RemoveObject(Object *object)
{
  /* Renumber when the object was in a category, so its sets don't stay taken. */
  const auto it = _objects.find(object);
  if (it != _objects.end()) {
    _dirty |= !it->second.empty();
    _objects.erase(it);
  }
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
  const auto it = _emitters.find(object);
  if (it != _emitters.end()) {
    _dirty |= !(it->second.light.IsEmpty() && it->second.shadow.IsEmpty());
    _emitters.erase(it);
  }
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
      "light",
      _warnedLightSets);
  const std::vector<Categories> shadowSets = AssignSets(
      _objects,
      shadowCategories,
      [&](Object *object, const uint set) {
        object->set_blocker_shadow_set(set);
        tag(object);
      },
      "shadow",
      _warnedShadowSets);

  /* Objects that left their last category are back in set 0. */
  for (auto it = _objects.begin(); it != _objects.end();) {
    it = it->second.empty() ? _objects.erase(it) : std::next(it);
  }

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
