/* SPDX-FileCopyrightText: 2026 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/ramps.h"
#include "hydra/diagnostic.h"

#include "kernel/types.h"
#include "scene/shader_nodes.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>

#include <algorithm>
#include <cmath>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

CCL_NAMESPACE_USING_DIRECTIVE

namespace {

/* A ramp as Houdini authors it: keys in [0, 1], values of one or three channels. */
struct Ramp {
  std::vector<float> keys;
  std::vector<GfVec3f> values;
  TfToken basis;
};

struct RampSpec {
  const char *name;
  bool color;
  /* The VOP's default ramp, for an unauthored one: two keys, at 0 and 1. */
  float from;
  float to;
};

struct NodeRamps {
  const char *nodeType;
  std::vector<RampSpec> ramps;
};

const std::vector<NodeRamps> &Specs()
{
  static const std::vector<NodeRamps> specs = {
      {"rgb_ramp", {{"color_ramp", true, 0.0f, 1.0f}, {"alpha_ramp", false, 1.0f, 1.0f}}},
      {"float_curve", {{"curve_ramp", false, 0.0f, 1.0f}}},
      {"rgb_curves",
       {{"combined_curve", false, 0.0f, 1.0f},
        {"red_curve", false, 0.0f, 1.0f},
        {"green_curve", false, 0.0f, 1.0f},
        {"blue_curve", false, 0.0f, 1.0f}}},
      {"vector_curves",
       {{"x_curve", false, 0.0f, 1.0f},
        {"y_curve", false, 0.0f, 1.0f},
        {"z_curve", false, 0.0f, 1.0f}}},
  };
  return specs;
}

const NodeRamps *SpecsFor(const std::string &nodeType)
{
  for (const NodeRamps &node : Specs()) {
    if (nodeType == node.nodeType) {
      return &node;
    }
  }
  return nullptr;
}

const TfToken &ConstantBasis()
{
  static const TfToken token("constant");
  return token;
}
const TfToken &LinearBasis()
{
  static const TfToken token("linear");
  return token;
}
const TfToken &CatmullRomBasis()
{
  static const TfToken token("catmull-rom");
  return token;
}

VtValue Parameter(const HdMaterialNodeParameterContainerSchema &params, const std::string &name)
{
  const HdMaterialNodeParameterSchema param = params.Get(TfToken(name));
  if (!param) {
    return VtValue();
  }
  const HdSampledDataSourceHandle value = param.GetValue();
  return value ? value->GetValue(0.0f) : VtValue();
}

/* The authored ramp, or the default when it isn't authored or doesn't make sense. */
Ramp ReadRamp(const HdMaterialNodeParameterContainerSchema &params,
              const RampSpec &spec,
              const SdfPath &nodePath)
{
  Ramp ramp;
  const std::string name = spec.name;
  const VtValue keys = Parameter(params, name + "_keys");
  const VtValue values = Parameter(params, name + "_values");
  const VtValue basis = Parameter(params, name + "_basis");
  if (keys.IsHolding<VtFloatArray>()) {
    const VtFloatArray &k = keys.UncheckedGet<VtFloatArray>();
    ramp.keys.assign(k.begin(), k.end());
    if (values.IsHolding<VtVec3fArray>()) {
      const VtVec3fArray &v = values.UncheckedGet<VtVec3fArray>();
      ramp.values.assign(v.begin(), v.end());
    }
    else if (values.IsHolding<VtFloatArray>()) {
      for (const float v : values.UncheckedGet<VtFloatArray>()) {
        ramp.values.emplace_back(v);
      }
    }
  }
  if (basis.IsHolding<TfToken>()) {
    ramp.basis = basis.UncheckedGet<TfToken>();
  }
  else if (basis.IsHolding<std::string>()) {
    ramp.basis = TfToken(basis.UncheckedGet<std::string>());
  }

  if (ramp.keys.empty() || ramp.keys.size() != ramp.values.size()) {
    if (!ramp.keys.empty() || !ramp.values.empty()) {
      HDCYCLES_WARN("Ramp '%s' on '%s' has %zu keys and %zu values; using its default",
                    name.c_str(),
                    nodePath.GetText(),
                    ramp.keys.size(),
                    ramp.values.size());
    }
    ramp.keys = {0.0f, 1.0f};
    ramp.values = {GfVec3f(spec.from), GfVec3f(spec.to)};
    ramp.basis = LinearBasis();
    return ramp;
  }

  /* Houdini keeps keys in order, but nothing else has to. */
  std::vector<size_t> order(ramp.keys.size());
  for (size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [&ramp](const size_t a, const size_t b) {
    return ramp.keys[a] < ramp.keys[b];
  });
  Ramp sorted;
  sorted.basis = ramp.basis;
  for (const size_t i : order) {
    sorted.keys.push_back(ramp.keys[i]);
    sorted.values.push_back(ramp.values[i]);
  }

  if (sorted.basis.IsEmpty()) {
    sorted.basis = LinearBasis();
  }
  else if (sorted.basis != ConstantBasis() && sorted.basis != LinearBasis() &&
           sorted.basis != CatmullRomBasis())
  {
    HDCYCLES_WARN("Ramp '%s' on '%s' uses the %s basis, which Cycles evaluates as Catmull-Rom",
                  name.c_str(),
                  nodePath.GetText(),
                  sorted.basis.GetText());
    sorted.basis = CatmullRomBasis();
  }
  return sorted;
}

/* The ramp at t, as Houdini evaluates constant, linear and Catmull-Rom ramps: per segment
 * between keys, Catmull-Rom uniform in the segment's own parameter, with the end values
 * repeated beyond the ends. */
GfVec3f Lookup(const Ramp &ramp, const float t)
{
  const std::vector<float> &k = ramp.keys;
  const std::vector<GfVec3f> &v = ramp.values;
  const size_t n = k.size();
  if (n == 1 || t <= k.front()) {
    return v.front();
  }
  if (t >= k.back()) {
    return v.back();
  }
  size_t i = 0;
  while (i + 2 < n && t > k[i + 1]) {
    ++i;
  }
  if (ramp.basis == ConstantBasis()) {
    return v[i];
  }
  const float span = k[i + 1] - k[i];
  const float u = span > 0.0f ? (t - k[i]) / span : 1.0f;
  if (ramp.basis == LinearBasis()) {
    return v[i] + (v[i + 1] - v[i]) * u;
  }
  const GfVec3f &p0 = i > 0 ? v[i - 1] : v[i];
  const GfVec3f &p1 = v[i];
  const GfVec3f &p2 = v[i + 1];
  const GfVec3f &p3 = i + 2 < n ? v[i + 2] : v[i + 1];
  return 0.5f * (2.0f * p1 + (p2 - p0) * u + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u * u +
                 (3.0f * p1 - p0 - 3.0f * p2 + p3) * u * u * u);
}

float Position(const float x, const float minX, const float maxX)
{
  return maxX > minX ? (x - minX) / (maxX - minX) : 0.0f;
}

}  // namespace

bool HdCyclesIsRampParameter(const std::string &nodeType, const TfToken &name)
{
  const NodeRamps *node = SpecsFor(nodeType);
  if (!node) {
    return false;
  }
  const std::string &param = name.GetString();
  for (const RampSpec &spec : node->ramps) {
    const std::string ramp = spec.name;
    if (param == ramp || param == ramp + "_basis" || param == ramp + "_keys" ||
        param == ramp + "_values")
    {
      return true;
    }
  }
  return false;
}

void HdCyclesApplyRamps(ShaderNode *node,
                        const HdMaterialNodeParameterContainerSchema &params,
                        const SdfPath &nodePath)
{
  const NodeRamps *spec = SpecsFor(node->type->name.string());
  if (!spec) {
    return;
  }
  std::vector<Ramp> ramps;
  for (const RampSpec &rampSpec : spec->ramps) {
    ramps.push_back(ReadRamp(params, rampSpec, nodePath));
  }
  const auto sample = [](const int i) { return float(i) / float(RAMP_TABLE_SIZE - 1); };

  if (node->type->name == "rgb_ramp") {
    array<packed_float3> colors(RAMP_TABLE_SIZE);
    array<float> alphas(RAMP_TABLE_SIZE);
    for (int i = 0; i < RAMP_TABLE_SIZE; ++i) {
      const GfVec3f c = Lookup(ramps[0], sample(i));
      colors[i] = make_float3(c[0], c[1], c[2]);
      alphas[i] = Lookup(ramps[1], sample(i))[0];
    }
    auto *ramp = static_cast<RGBRampNode *>(node);
    ramp->set_ramp(colors);
    ramp->set_ramp_alpha(alphas);
  }
  else if (node->type->name == "float_curve") {
    array<float> curve(RAMP_TABLE_SIZE);
    for (int i = 0; i < RAMP_TABLE_SIZE; ++i) {
      curve[i] = Lookup(ramps[0], sample(i))[0];
    }
    static_cast<FloatCurveNode *>(node)->set_curve(curve);
  }
  else {
    /* RGB and vector curves: a table over min_x to max_x of each channel's output. */
    auto *curvesNode = static_cast<CurvesNode *>(node);
    const float minX = curvesNode->get_min_x();
    const float maxX = curvesNode->get_max_x();
    const bool rgb = node->type->name == "rgb_curves";
    array<packed_float3> curves(RAMP_TABLE_SIZE);
    for (int i = 0; i < RAMP_TABLE_SIZE; ++i) {
      const float t = sample(i);
      float out[3];
      for (int c = 0; c < 3; ++c) {
        if (rgb) {
          const float combined = Lookup(ramps[0], t)[0];
          out[c] = Lookup(ramps[c + 1], Position(combined, minX, maxX))[0];
        }
        else {
          out[c] = Lookup(ramps[c], t)[0];
        }
      }
      curves[i] = make_float3(out[0], out[1], out[2]);
    }
    curvesNode->set_curves(curves);
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
