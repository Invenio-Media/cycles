/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/curves.h"
#include "hydra/diagnostic.h"
#include "hydra/geometry.inl"
#include "hydra/util.h"
#include "scene/hair.h"
#include "util/types_float3.h"

#include <pxr/imaging/hd/basisCurvesSchema.h>
#include <pxr/imaging/hd/basisCurvesTopologySchema.h>

#include <algorithm>
#include <numeric>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesCurves::HdCyclesCurves(const SdfPath &rprimId) : HdCyclesGeometry(rprimId) {}

HdCyclesCurves::~HdCyclesCurves() = default;

HdDirtyBits HdCyclesCurves::GetInitialDirtyBitsMask() const
{
  HdDirtyBits bits = HdCyclesGeometry::GetInitialDirtyBitsMask();
  bits |= HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyWidths |
          HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyTopology;
  return bits;
}

HdDirtyBits HdCyclesCurves::_PropagateDirtyBits(HdDirtyBits bits) const
{
  if (bits & (HdChangeTracker::DirtyTopology)) {
    // Changing topology clears the geometry, so need to populate everything again
    bits |= HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyWidths |
            HdChangeTracker::DirtyPrimvar;
  }

  return bits;
}

void HdCyclesCurves::Populate(HdSceneDelegate *sceneDelegate, HdDirtyBits dirtyBits, bool &rebuild)
{
  if (HdChangeTracker::IsTopologyDirty(dirtyBits, GetId())) {
    PopulateTopology(sceneDelegate);
  }

  if (dirtyBits & HdChangeTracker::DirtyPoints) {
    PopulatePoints(sceneDelegate);
  }

  if (dirtyBits & HdChangeTracker::DirtyWidths) {
    PopulateWidths(sceneDelegate);
  }

  if (dirtyBits & HdChangeTracker::DirtyPrimvar) {
    PopulatePrimvars(sceneDelegate);
  }

  rebuild = (_geom->position_is_modified()) || (_geom->radius_is_modified());
}

void HdCyclesCurves::PopulatePoints(HdSceneDelegate *sceneDelegate)
{
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  /* Across the shutter when the points move; the middle sample is the position. Each sample is
   * resampled into Cycles' keys when the curves' basis needs it. */
  std::vector<VtVec3fArray> samples = SamplePoints(primvars);
  if (samples.empty()) {
    HDCYCLES_WARN("Invalid points data for %s", GetId().GetText());
    return;
  }
  if (!_vertexKeys.empty()) {
    for (VtVec3fArray &sample : samples) {
      const VtValue keyed = ResampleVertex(VtValue(sample));
      if (keyed.IsEmpty()) {
        /* A sample shorter than the topology's points can't be resampled; neither can the
         * curves be drawn from it. */
        HDCYCLES_WARN("Invalid points data for %s", GetId().GetText());
        return;
      }
      sample = keyed.UncheckedGet<VtVec3fArray>();
    }
  }

  const VtVec3fArray &points = samples[samples.size() / 2];

  TF_VERIFY(points.size() >= _geom->num_keys());

  static_assert(sizeof(GfVec3f) == sizeof(packed_float3));

  const size_t numKeys = std::min(points.size(), _geom->num_keys());
  std::copy_n(reinterpret_cast<const packed_float3 *>(points.data()),
              numKeys,
              _geom->get_position_for_write());
  ApplyPointsMotion(_geom->attributes.find(ATTR_STD_POSITION), samples, numKeys);
}

void HdCyclesCurves::PopulateWidths(HdSceneDelegate *sceneDelegate)
{
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const HdInterpolation interpolation = ReadPrimvarInterpolation(primvars, HdTokens->widths);
  VtValue value = ReadPrimvar(primvars, HdTokens->widths);
  if (interpolation == HdInterpolationVertex) {
    value = ResampleVertex(value);
  }
  else if (interpolation == HdInterpolationVarying) {
    value = ResampleVarying(value);
  }

  if (!value.IsHolding<VtFloatArray>()) {
    HDCYCLES_WARN("Invalid widths data for %s", GetId().GetText());
    return;
  }

  const auto &widths = value.UncheckedGet<VtFloatArray>();
  float *radius = _geom->get_radius_for_write();

  if (interpolation == HdInterpolationConstant) {
    TF_VERIFY(widths.size() == 1);

    const float constantRadius = widths[0] * 0.5f;

    for (size_t i = 0; i < _geom->num_keys(); ++i) {
      radius[i] = constantRadius;
    }
  }
  else if (interpolation == HdInterpolationVertex || interpolation == HdInterpolationVarying) {
    TF_VERIFY(widths.size() == _geom->num_keys());

    for (size_t i = 0; i < std::min(widths.size(), _geom->num_keys()); ++i) {
      radius[i] = widths[i] * 0.5f;
    }
  }
}

void HdCyclesCurves::PopulatePrimvars(HdSceneDelegate *sceneDelegate)
{
  Scene *const scene = (Scene *)_geom->get_owner();

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);

  const std::pair<HdInterpolation, AttributeElement> interpolations[] = {
      std::make_pair(HdInterpolationVertex, ATTR_ELEMENT_CURVE_KEY),
      std::make_pair(HdInterpolationVarying, ATTR_ELEMENT_CURVE_KEY),
      std::make_pair(HdInterpolationUniform, ATTR_ELEMENT_CURVE),
      std::make_pair(HdInterpolationConstant, ATTR_ELEMENT_OBJECT),
  };

  for (const auto &interpolation : interpolations) {
    for (const TfToken &primvarName : PrimvarNamesAtInterpolation(primvars, interpolation.first)) {
      // Skip special primvars that are handled separately
      if (primvarName == HdTokens->points || primvarName == HdTokens->widths) {
        continue;
      }

      const VtValue value = ReadPrimvar(primvars, primvarName);
      if (value.IsEmpty()) {
        continue;
      }

      const TfToken role = ReadPrimvarRole(primvars, primvarName);
      const ustring name(primvarName.GetString());

      AttributeStandard std = ATTR_STD_NONE;
      if (role == HdPrimvarRoleTokens->textureCoordinate) {
        std = ATTR_STD_UV;
      }
      else if (primvarName == HdTokens->normals && interpolation.first == HdInterpolationVertex) {
        std = ATTR_STD_VERTEX_NORMAL;
      }
      else if (primvarName == HdTokens->displayColor &&
               interpolation.first == HdInterpolationConstant)
      {
        if (value.IsHolding<VtVec3fArray>() && value.GetArraySize() == 1) {
          const GfVec3f color = value.UncheckedGet<VtVec3fArray>()[0];
          _instances[0]->set_color(make_float3(color[0], color[1], color[2]));
        }
      }

      // Skip attributes that are not needed. Texture coordinates are always kept: the UV
      // pass needs them, and passes are created after rprims sync.
      if (std == ATTR_STD_UV || (std != ATTR_STD_NONE && _geom->need_attribute(scene, std)) ||
          _geom->need_attribute(scene, name))
      {
        AttributeElement elem = interpolation.second;
        if (std == ATTR_STD_VERTEX_NORMAL) {
          elem = ATTR_ELEMENT_CURVE_KEY_NORMAL;
        }
        VtValue keyed = interpolation.first == HdInterpolationVertex  ? ResampleVertex(value) :
                        interpolation.first == HdInterpolationVarying ? ResampleVarying(value) :
                                                                        value;
        if (keyed.IsEmpty()) {
          HDCYCLES_WARN(
              "%s: primvar %s, of type %s, can't be sampled along the curves' basis; "
              "ignored.",
              GetId().GetText(),
              primvarName.GetText(),
              value.GetTypeName().c_str());
          continue;
        }
        if (std == ATTR_STD_VERTEX_NORMAL && !_vertexKeys.empty() &&
            keyed.IsHolding<VtVec3fArray>())
        {
          /* Sampled normals are blends of the authored ones; unit length again. */
          VtVec3fArray normals = keyed.UncheckedGet<VtVec3fArray>();
          for (GfVec3f &normal : normals) {
            normal.Normalize();
          }
          keyed = VtValue(std::move(normals));
        }
        ApplyPrimvars(_geom->attributes, name, keyed, elem, std);
      }
    }
  }
}

namespace {

/* Keys sampled along each span of a resampled cubic curve: 4 a span of one control interval
 * (B-spline, Catmull-Rom) and 8 a Bezier span of three, which bends more. With 4, a long S-shaped
 * Bezier span drifted a pixel off its curve. */
constexpr int kKeysPerSpan = 4;
constexpr int kKeysPerBezierSpan = 8;

void AddWeight(HdCyclesCurveKey &key, const int index, const float weight)
{
  for (int i = 0; i < key.count; ++i) {
    if (key.index[i] == index) {
      key.weight[i] += weight;
      return;
    }
  }
  if (TF_VERIFY(key.count < 4)) {
    key.index[key.count] = index;
    key.weight[key.count] = weight;
    ++key.count;
  }
}

/* The basis functions of a cubic span at t, from 0 to 1, for its four control points. */
void BasisWeights(const TfToken &basis, const float t, float w[4])
{
  const float s = 1.0f - t, t2 = t * t, t3 = t2 * t;
  if (basis == HdTokens->bezier) {
    w[0] = s * s * s;
    w[1] = 3.0f * t * s * s;
    w[2] = 3.0f * t2 * s;
    w[3] = t3;
  }
  else if (basis == HdTokens->bSpline) {
    w[0] = s * s * s / 6.0f;
    w[1] = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
    w[2] = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
    w[3] = t3 / 6.0f;
  }
  else {
    /* Catmull-Rom. */
    w[0] = (-t3 + 2.0f * t2 - t) * 0.5f;
    w[1] = (3.0f * t3 - 5.0f * t2 + 2.0f) * 0.5f;
    w[2] = (-3.0f * t3 + 4.0f * t2 + t) * 0.5f;
    w[3] = (t3 - t2) * 0.5f;
  }
}

template<typename T>
VtArray<T> Resample(const VtArray<T> &in, const std::vector<HdCyclesCurveKey> &keys)
{
  VtArray<T> out(keys.size());
  for (size_t k = 0; k < keys.size(); ++k) {
    const HdCyclesCurveKey &key = keys[k];
    T value = in[key.index[0]] * key.weight[0];
    for (int i = 1; i < key.count; ++i) {
      value += in[key.index[i]] * key.weight[i];
    }
    out[k] = value;
  }
  return out;
}

template<typename T>
bool ResampleIf(const VtValue &value,
                const std::vector<HdCyclesCurveKey> &keys,
                const size_t count,
                VtValue &out)
{
  if (!value.IsHolding<VtArray<T>>()) {
    return false;
  }
  const auto &in = value.UncheckedGet<VtArray<T>>();
  if (in.size() >= count) {
    out = VtValue(Resample(in, keys));
  }
  return true;
}

VtValue ResampleValue(const VtValue &value,
                      const std::vector<HdCyclesCurveKey> &keys,
                      const size_t count)
{
  if (keys.empty()) {
    return value;
  }
  VtValue out;
  (void)(ResampleIf<float>(value, keys, count, out) ||
         ResampleIf<double>(value, keys, count, out) ||
         ResampleIf<GfVec2f>(value, keys, count, out) ||
         ResampleIf<GfVec3f>(value, keys, count, out) ||
         ResampleIf<GfVec4f>(value, keys, count, out) ||
         ResampleIf<GfVec2d>(value, keys, count, out) ||
         ResampleIf<GfVec3d>(value, keys, count, out) ||
         ResampleIf<GfVec4d>(value, keys, count, out));
  return out;
}

}  // namespace

VtValue HdCyclesCurves::ResampleVertex(const VtValue &value) const
{
  return ResampleValue(value, _vertexKeys, _numVertex);
}

VtValue HdCyclesCurves::ResampleVarying(const VtValue &value) const
{
  return ResampleValue(value, _varyingKeys, _numVarying);
}

void HdCyclesCurves::PopulateTopology(HdSceneDelegate *sceneDelegate)
{
  // Clear geometry before populating it again with updated topology
  _geom->clear(true);

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdBasisCurvesTopologySchema topoSchema =
      HdBasisCurvesSchema::GetFromParent(prim.dataSource).GetTopology();

  TfToken curveType = HdTokens->linear;
  if (auto ds = topoSchema.GetType()) {
    curveType = ds->GetTypedValue(0.0f);
  }
  TfToken curveBasis = HdTokens->bezier;
  if (auto ds = topoSchema.GetBasis()) {
    curveBasis = ds->GetTypedValue(0.0f);
  }
  TfToken curveWrap = HdTokens->nonperiodic;
  if (auto ds = topoSchema.GetWrap()) {
    curveWrap = ds->GetTypedValue(0.0f);
  }
  VtIntArray curveVertexCounts;
  if (auto ds = topoSchema.GetCurveVertexCounts()) {
    curveVertexCounts = ds->GetTypedValue(0.0f);
  }

  /* Cycles' keys for each curve, as weighted sums of its authored points (vertex data) and of
   * its segment ends (varying data), following UsdGeomBasisCurves. Cubic spans are sampled
   * along their basis; a periodic curve repeats its start to close; a pinned B-spline or
   * Catmull-Rom curve reaches its ends through phantom points (2 p0 - p1). */
  const bool cubic = curveType == HdTokens->cubic;
  const bool periodic = curveWrap == HdTokens->periodic;
  const bool pinned = curveWrap == HdTokens->pinned && curveBasis != HdTokens->bezier;
  const int vstep = curveBasis == HdTokens->bezier ? 3 : 1;
  const int keysPerSpan = vstep == 3 ? kKeysPerBezierSpan : kKeysPerSpan;
  _vertexKeys.clear();
  _varyingKeys.clear();
  _numVertex = 0;
  _numVarying = 0;
  if (std::any_of(
          curveVertexCounts.begin(), curveVertexCounts.end(), [](const int n) { return n < 1; }))
  {
    HDCYCLES_WARN("%s: a curve has no points; the curves are not rendered.", GetId().GetText());
    _geom->resize_curves(0, 0);
    return;
  }
  std::vector<int> keysPerCurve;
  keysPerCurve.reserve(curveVertexCounts.size());
  bool identity = !periodic;
  size_t vertexBase = 0;
  size_t varyingBase = 0;
  for (const int n : curveVertexCounts) {
    const size_t keysBefore = _vertexKeys.size();
    int segments = 0;
    if (cubic) {
      segments = periodic ? n / vstep : pinned ? n - 1 : (n - 4) / vstep + 1;
    }
    if (!cubic || segments < 1 || n < 2) {
      /* Linear, or too few points for a cubic span: the points are the keys, a periodic curve's
       * first repeated to close, and a lone point twice, as Cycles' curves need two keys. */
      const int count = std::max(n + (periodic ? 1 : 0), 2);
      identity = identity && count == n;
      for (int i = 0; i < count; ++i) {
        HdCyclesCurveKey key;
        AddWeight(key, int(vertexBase) + i % n, 1.0f);
        _vertexKeys.push_back(key);
        _varyingKeys.push_back(key);
      }
      varyingBase += n;
    }
    else {
      identity = false;
      const int varyingCount = periodic ? segments : segments + 1;
      auto addKey = [&](const int segment, const float t) {
        float w[4];
        BasisWeights(curveBasis, t, w);
        HdCyclesCurveKey key;
        for (int j = 0; j < 4; ++j) {
          const int v = segment * vstep + j - (pinned ? 1 : 0);
          if (periodic) {
            AddWeight(key, int(vertexBase) + (v % n + n) % n, w[j]);
          }
          else if (v < 0) {
            AddWeight(key, int(vertexBase), 2.0f * w[j]);
            AddWeight(key, int(vertexBase) + 1, -w[j]);
          }
          else if (v >= n) {
            AddWeight(key, int(vertexBase) + n - 1, 2.0f * w[j]);
            AddWeight(key, int(vertexBase) + n - 2, -w[j]);
          }
          else {
            AddWeight(key, int(vertexBase) + v, w[j]);
          }
        }
        _vertexKeys.push_back(key);
        HdCyclesCurveKey varying;
        const int next = segment + 1;
        AddWeight(varying, int(varyingBase) + segment, 1.0f - t);
        AddWeight(varying, int(varyingBase) + (periodic && next == varyingCount ? 0 : next), t);
        _varyingKeys.push_back(varying);
      };
      for (int segment = 0; segment < segments; ++segment) {
        for (int k = 0; k < keysPerSpan; ++k) {
          addKey(segment, float(k) / keysPerSpan);
        }
      }
      addKey(segments - 1, 1.0f);
      varyingBase += varyingCount;
    }
    vertexBase += n;
    keysPerCurve.push_back(int(_vertexKeys.size() - keysBefore));
  }
  _numVertex = vertexBase;
  _numVarying = varyingBase;
  if (identity) {
    _vertexKeys.clear();
    _varyingKeys.clear();
  }

  const size_t numKeys = std::accumulate(keysPerCurve.begin(), keysPerCurve.end(), size_t(0));
  _geom->resize_curves(int(keysPerCurve.size()), numKeys);

  /* The shape the render settings ask for (cycles:scene:hair_shape, in the scene's params),
   * which Cycles reads per curve object. A thick linear curve is straight tube segments rather
   * than a Catmull-Rom spline through its points. */
  const Scene *const scene = static_cast<const Scene *>(_geom->get_owner());
  _geom->curve_shape = scene->params.hair_shape != CURVE_THICK ? CURVE_RIBBON :
                       curveType == HdTokens->linear           ? CURVE_THICK_LINEAR :
                                                                 CURVE_THICK;

  int *curve_first_key = _geom->get_curve_first_key().data();
  for (size_t curve = 0, key = 0; curve < keysPerCurve.size(); ++curve) {
    curve_first_key[curve] = int(key);
    key += keysPerCurve[curve];
  }

  // Always reference shader at index zero, which is the primitive material
  std::ranges::fill(_geom->get_curve_shader(), 0);

  _geom->tag_curve_first_key_modified();
  _geom->tag_curve_shader_modified();
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
