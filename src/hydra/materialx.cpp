/* SPDX-FileCopyrightText: 2025 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/materialx.h"
#include "graph/node_type.h"
#include "hydra/diagnostic.h"
#include "util/colorspace.h"

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/dataSourceMaterialNetworkInterface.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderProperty.h>

#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(_tokens, (file)(defaultgeomprop)(mtlx));

using InputConnection = HdMaterialNetworkInterface::InputConnection;

/* What feeds an input: an upstream output, or a constant (float, GfVec3f, or as authored). */
struct Operand {
  std::optional<InputConnection> connection;
  VtValue constant;

  bool IsConstant() const
  {
    return !connection && !constant.IsEmpty();
  }

  static Operand Constant(VtValue value)
  {
    Operand operand;
    operand.constant = std::move(value);
    return operand;
  }

  static Operand Output(const TfToken &node, const char *output)
  {
    Operand operand;
    operand.connection = InputConnection{node, TfToken(output)};
    return operand;
  }
};

bool IsVector(const VtValue &value)
{
  return value.IsHolding<GfVec2f>() || value.IsHolding<GfVec3f>() || value.IsHolding<GfVec4f>() ||
         value.IsHolding<GfVec2d>() || value.IsHolding<GfVec3d>() || value.IsHolding<GfVec4d>();
}

GfVec3f ToVec3(const VtValue &value);

float ToFloat(const VtValue &value)
{
  if (IsVector(value)) {
    return ToVec3(value)[0];
  }
  if (value.IsHolding<bool>()) {
    return value.UncheckedGet<bool>() ? 1.0f : 0.0f;
  }
  return VtValue::Cast<float>(value).GetWithDefault(0.0f);
}

GfVec3f ToVec3(const VtValue &value)
{
  if (value.IsHolding<GfVec3f>()) {
    return value.UncheckedGet<GfVec3f>();
  }
  if (value.IsHolding<GfVec2f>()) {
    const GfVec2f v = value.UncheckedGet<GfVec2f>();
    return GfVec3f(v[0], v[1], 0.0f);
  }
  if (value.IsHolding<GfVec4f>()) {
    const GfVec4f v = value.UncheckedGet<GfVec4f>();
    return GfVec3f(v[0], v[1], v[2]);
  }
  if (value.IsHolding<GfVec2d>()) {
    const GfVec2d v = value.UncheckedGet<GfVec2d>();
    return GfVec3f(float(v[0]), float(v[1]), 0.0f);
  }
  if (value.IsHolding<GfVec3d>()) {
    return GfVec3f(value.UncheckedGet<GfVec3d>());
  }
  if (value.IsHolding<GfVec4d>()) {
    const GfVec4d v = value.UncheckedGet<GfVec4d>();
    return GfVec3f(float(v[0]), float(v[1]), float(v[2]));
  }
  const float f = ToFloat(value);
  return GfVec3f(f, f, f);
}

/* Cycles sockets take floats and 3-vectors: widen 2-vectors, which Cycles has no socket for,
 * and narrow doubles. Other values (strings, assets, integers) pass as authored. */
VtValue ToCycles(const VtValue &value)
{
  if (value.IsHolding<GfVec2f>() || value.IsHolding<GfVec2d>() || value.IsHolding<GfVec3d>()) {
    return VtValue(ToVec3(value));
  }
  if (value.IsHolding<double>()) {
    return VtValue(float(value.UncheckedGet<double>()));
  }
  return value;
}

/* A constant shaped for a vector or colour socket. */
Operand AsVector(const Operand &operand)
{
  return operand.IsConstant() ? Operand::Constant(VtValue(ToVec3(operand.constant))) : operand;
}

/* A constant shaped for a float socket: a colour gives its average. (Cycles converts a
 * connected colour by its luminance in the working space; the two agree on greys, such as an
 * opacity.) */
Operand AsFloat(const Operand &operand)
{
  if (!operand.IsConstant()) {
    return operand;
  }
  if (IsVector(operand.constant)) {
    const GfVec3f v = ToVec3(operand.constant);
    return Operand::Constant(VtValue((v[0] + v[1] + v[2]) / 3.0f));
  }
  return Operand::Constant(VtValue(ToFloat(operand.constant)));
}

/* The Cycles math operations that fold when both operands are constant. */
std::optional<float> Fold(const std::string &op, const float a, const float b)
{
  if (op == "add") {
    return a + b;
  }
  if (op == "subtract") {
    return a - b;
  }
  if (op == "multiply") {
    return a * b;
  }
  if (op == "divide") {
    return b != 0.0f ? a / b : 0.0f;
  }
  if (op == "minimum") {
    return std::min(a, b);
  }
  if (op == "maximum") {
    return std::max(a, b);
  }
  if (op == "power") {
    return std::pow(a, b);
  }
  if (op == "floored_modulo") {
    return b != 0.0f ? a - b * std::floor(a / b) : 0.0f;
  }
  return std::nullopt;
}

/* The unary Cycles math operations that fold on a constant. */
std::optional<float> FoldUnary(const std::string &op, const float a)
{
  static const std::unordered_map<std::string, float (*)(float)> ops = {
      {"absolute", [](const float x) { return std::fabs(x); }},
      {"floor", [](const float x) { return std::floor(x); }},
      {"ceil", [](const float x) { return std::ceil(x); }},
      /* As Cycles rounds: halves up, -0.5 to 0. */
      {"round", [](const float x) { return std::floor(x + 0.5f); }},
      {"sign", [](const float x) { return float((x > 0.0f) - (x < 0.0f)); }},
      {"sine", [](const float x) { return std::sin(x); }},
      {"cosine", [](const float x) { return std::cos(x); }},
      {"tangent", [](const float x) { return std::tan(x); }},
      {"sqrt", [](const float x) { return x > 0.0f ? std::sqrt(x) : 0.0f; }},
      {"exponent", [](const float x) { return std::exp(x); }},
  };
  const auto it = ops.find(op);
  return it != ops.end() ? std::optional<float>(it->second(a)) : std::nullopt;
}

/* MaterialX nodes whose operation is one Cycles math (floats) or vector_math (colors and
 * vectors) operation on in1 and in2, or on in. */
struct MathOp {
  const char *floatOp;
  const char *vectorOp;
  bool unary;
};

const std::unordered_map<std::string, MathOp> kMathOps = {
    {"add", {"add", "add", false}},
    {"subtract", {"subtract", "subtract", false}},
    {"multiply", {"multiply", "multiply", false}},
    {"divide", {"divide", "divide", false}},
    {"modulo", {"floored_modulo", "modulo", false}},
    {"power", {"power", "power", false}},
    {"min", {"minimum", "minimum", false}},
    {"max", {"maximum", "maximum", false}},
    {"absval", {"absolute", "absolute", true}},
    {"floor", {"floor", "floor", true}},
    {"ceil", {"ceil", "ceil", true}},
    {"round", {"round", "round", true}},
    {"sign", {"sign", "sign", true}},
    {"sin", {"sine", "sine", true}},
    {"cos", {"cosine", "cosine", true}},
    {"tan", {"tangent", "tangent", true}},
    {"sqrt", {"sqrt", nullptr, true}},
    {"exp", {"exponent", nullptr, true}},
};

class MaterialXTranslator {
 public:
  explicit MaterialXTranslator(HdMaterialNetworkInterface &network) : _network(network) {}

  void Translate();

 private:
  HdMaterialNetworkInterface &_network;
  /* What now provides each MaterialX output, keyed by (node, output). */
  std::map<std::pair<TfToken, TfToken>, Operand> _outputs;
  std::set<TfToken> _untranslated;
  /* The nodes this translation authored, and the connections it authored on them. The network
   * interface does not read back connections set in the same session, so they are kept here
   * and written once every node's outputs are known. */
  struct PendingConnection {
    TfToken node;
    TfToken input;
    InputConnection connection;
  };
  std::set<TfToken> _authored;
  std::vector<PendingConnection> _pending;
  /* The Cycles type of each node this translation authored, e.g. vector_math. */
  std::unordered_map<TfToken, std::string, TfToken::HashFunctor> _cyclesTypes;
  /* The MaterialX definitions of the nodes retyped to the delegate's UsdPreviewSurface
   * mapping, which keep their MaterialX inputs. */
  std::unordered_map<TfToken, SdrShaderNodeConstPtr, TfToken::HashFunctor> _retyped;
  /* What the translation could not carry over, reported once per material. */
  std::set<std::string> _notes;

  /* A constant shaped for the socket it is written to: a float fills a colour or vector
   * socket, a colour's average fills a float socket. The socket's type comes from Cycles, or
   * for a retyped node from its MaterialX definition. */
  VtValue ShapeForSocket(const TfToken &node, const TfToken &input, const VtValue &value) const
  {
    const VtValue widened = ToCycles(value);
    bool isFloat = false;
    bool isVector = false;
    if (const auto it = _cyclesTypes.find(node); it != _cyclesTypes.end()) {
      const NodeType *type = NodeType::find(ustring(it->second));
      const SocketType *socket = type ? type->find_input(ustring(input.GetString())) : nullptr;
      isFloat = socket && socket->type == SocketType::FLOAT;
      isVector = socket &&
                 (socket->type == SocketType::COLOR || socket->type == SocketType::VECTOR ||
                  socket->type == SocketType::POINT || socket->type == SocketType::NORMAL);
    }
    else if (const auto it = _retyped.find(node); it != _retyped.end()) {
      const SdrShaderPropertyConstPtr property = it->second->GetShaderInput(input);
      const std::string type = property ? property->GetType().GetString() : std::string();
      isFloat = type == "float";
      isVector = type == "color" || type == "vector" || type == "normal" || type == "point";
    }
    if (isFloat) {
      return AsFloat(Operand::Constant(widened)).constant;
    }
    if (isVector) {
      return AsVector(Operand::Constant(widened)).constant;
    }
    return widened;
  }

  /* The node being translated: its name, its MaterialX value type (the nodedef's suffix, such
   * as color3 or vector3FA) and its inputs as authored or defaulted. */
  TfToken _node;
  TfToken _nodeDefinition;
  std::string _valueType;
  std::unordered_map<TfToken, Operand, TfToken::HashFunctor> _inputs;
  /* Its inputs that are connected, or authored with a value other than the default. */
  std::set<TfToken> _setInputs;
  TfToken _fileColorSpace;
  int _helpers = 0;

  /* Notes inputs of the current node that Cycles has no equivalent for, when they are set. */
  void Ignore(std::initializer_list<const char *> names)
  {
    for (const char *name : names) {
      if (_setInputs.count(TfToken(name))) {
        _notes.insert(_nodeDefinition.GetString() + "." + name + " has no Cycles equivalent");
      }
    }
  }

  void ReadInputs(const SdrShaderNodeConstPtr &sdrNode);
  void ClearNode();
  bool TranslateNode(const std::string &family);
  void Write(const TfToken &node, const TfToken &input, const Operand &value);
  void Rewire();
  std::optional<Operand> Resolve(const InputConnection &connection);

  /* The current node's value type is floats, not colors or vectors. */
  bool IsFloatType() const
  {
    return _valueType.starts_with("float") || _valueType.starts_with("integer") ||
           _valueType.starts_with("boolean");
  }

  const Operand &In(const char *name) const
  {
    static const Operand empty;
    const auto it = _inputs.find(TfToken(name));
    return it != _inputs.end() ? it->second : empty;
  }

  /* An input's constant value, or the fallback when it is connected or missing. */
  float ConstantFloat(const char *name, const float fallback) const
  {
    const Operand &operand = In(name);
    return operand.IsConstant() ? ToFloat(operand.constant) : fallback;
  }

  std::string ConstantString(const char *name) const
  {
    const Operand &operand = In(name);
    if (!operand.IsConstant()) {
      return std::string();
    }
    if (operand.constant.IsHolding<TfToken>()) {
      return operand.constant.UncheckedGet<TfToken>().GetString();
    }
    return VtValue::Cast<std::string>(operand.constant).GetWithDefault(std::string());
  }

  void SetType(const TfToken &node, const char *cyclesType)
  {
    _cyclesTypes[node] = cyclesType;
    _network.SetNodeType(node, TfToken(std::string("cycles_") + cyclesType));
  }

  TfToken AddNode(const char *cyclesType)
  {
    const TfToken node(_node.GetString() + "_hdc" + std::to_string(_helpers++));
    _authored.insert(node);
    SetType(node, cyclesType);
    return node;
  }

  void Set(const TfToken &node, const char *input, const Operand &value)
  {
    if (value.connection) {
      _pending.push_back({node, TfToken(input), *value.connection});
    }
    else if (!value.constant.IsEmpty()) {
      _network.SetNodeParameterValue(
          node, TfToken(input), ShapeForSocket(node, TfToken(input), value.constant));
    }
  }

  void SetValue(const TfToken &node, const char *input, const VtValue &value)
  {
    _network.SetNodeParameterValue(node, TfToken(input), value);
  }

  void Provide(const char *output, const Operand &provider)
  {
    _outputs[{_node, TfToken(output)}] = provider;
  }

  /* The default UV map, for inputs MaterialX binds to UV0. */
  Operand DefaultUV()
  {
    return Operand::Output(AddNode("uvmap"), "uv");
  }

  /* An input bound to UV0 unless connected. */
  Operand Texcoord(const char *name)
  {
    const Operand &texcoord = In(name);
    return texcoord.connection ? texcoord : DefaultUV();
  }

  /* An input bound to the object-space position unless connected. */
  Operand ObjectPosition(const char *name)
  {
    const Operand &position = In(name);
    return position.connection ? position :
                                 Operand::Output(AddNode("texture_coordinate"), "object");
  }

  Operand Math(const char *op, const Operand &a, const Operand &b, bool vector);
  Operand Unary(const char *op, const Operand &a, bool vector);
  Operand Mix(const Operand &a, const Operand &b, const Operand &factor, bool vector);
  Operand Component(const Operand &vector, int index, bool color);

  void TranslateMath(const MathOp &op);
  void TranslateStandardSurface();
  void TranslateOpenPbrSurface();
  void TranslateUnlit();
  void TranslateDisplacement();
  void TranslateImage(bool tiled);
};

Operand MaterialXTranslator::Math(const char *op,
                                  const Operand &a,
                                  const Operand &b,
                                  const bool vector)
{
  if (a.IsConstant() && b.IsConstant()) {
    if (vector || IsVector(a.constant) || IsVector(b.constant)) {
      const GfVec3f x = ToVec3(a.constant);
      const GfVec3f y = ToVec3(b.constant);
      const std::optional<float> r0 = Fold(op, x[0], y[0]);
      if (r0) {
        return Operand::Constant(
            VtValue(GfVec3f(*r0, *Fold(op, x[1], y[1]), *Fold(op, x[2], y[2]))));
      }
    }
    else if (const std::optional<float> r = Fold(op, ToFloat(a.constant), ToFloat(b.constant))) {
      return Operand::Constant(VtValue(*r));
    }
  }
  const TfToken node = AddNode(vector ? "vector_math" : "math");
  SetValue(node, "math_type", VtValue(TfToken(op)));
  Set(node, vector ? "vector1" : "value1", vector ? AsVector(a) : AsFloat(a));
  Set(node, vector ? "vector2" : "value2", vector ? AsVector(b) : AsFloat(b));
  return Operand::Output(node, vector ? "vector" : "value");
}

Operand MaterialXTranslator::Unary(const char *op, const Operand &a, const bool vector)
{
  if (a.IsConstant()) {
    if (vector) {
      const GfVec3f x = ToVec3(a.constant);
      if (const std::optional<float> r0 = FoldUnary(op, x[0])) {
        return Operand::Constant(
            VtValue(GfVec3f(*r0, *FoldUnary(op, x[1]), *FoldUnary(op, x[2]))));
      }
    }
    else if (const std::optional<float> r = FoldUnary(op, ToFloat(a.constant))) {
      return Operand::Constant(VtValue(*r));
    }
  }
  const TfToken node = AddNode(vector ? "vector_math" : "math");
  SetValue(node, "math_type", VtValue(TfToken(op)));
  Set(node, vector ? "vector1" : "value1", vector ? AsVector(a) : AsFloat(a));
  return Operand::Output(node, vector ? "vector" : "value");
}

/* a where factor is 0, b where it is 1. */
Operand MaterialXTranslator::Mix(const Operand &a,
                                 const Operand &b,
                                 const Operand &factor,
                                 const bool vector)
{
  if (factor.IsConstant()) {
    const float t = ToFloat(factor.constant);
    if (t == 0.0f) {
      return a;
    }
    if (t == 1.0f) {
      return b;
    }
  }
  const TfToken node = AddNode(vector ? "mix_color" : "mix_float");
  if (vector) {
    SetValue(node, "blend_type", VtValue(TfToken("mix")));
  }
  Set(node, "fac", AsFloat(factor));
  Set(node, "a", vector ? AsVector(a) : AsFloat(a));
  Set(node, "b", vector ? AsVector(b) : AsFloat(b));
  return Operand::Output(node, "result");
}

/* One channel of a color (r, g, b) or vector (x, y, z). */
Operand MaterialXTranslator::Component(const Operand &vector, const int index, const bool color)
{
  if (vector.IsConstant()) {
    return Operand::Constant(VtValue(ToVec3(vector.constant)[std::clamp(index, 0, 2)]));
  }
  static const char *const colorOutputs[] = {"r", "g", "b"};
  static const char *const vectorOutputs[] = {"x", "y", "z"};
  const TfToken node = AddNode(color ? "separate_color" : "separate_xyz");
  Set(node, color ? "color" : "vector", vector);
  return Operand::Output(node, (color ? colorOutputs : vectorOutputs)[std::clamp(index, 0, 2)]);
}

void MaterialXTranslator::ReadInputs(const SdrShaderNodeConstPtr &sdrNode)
{
  _inputs.clear();
  _setInputs.clear();
  for (const TfToken &name : sdrNode->GetShaderInputNames()) {
    const auto connections = _network.GetNodeInputConnection(_node, name);
    if (!connections.empty()) {
      _inputs[name].connection = connections[0];
      _setInputs.insert(name);
      continue;
    }
    const SdrShaderPropertyConstPtr input = sdrNode->GetShaderInput(name);
    VtValue value = _network.GetNodeParameterValue(_node, name);
    if (!value.IsEmpty()) {
      /* Compared as Cycles takes them, so a double authored at a float default is unset. */
      if (!input || ToCycles(value) != ToCycles(input->GetDefaultValue())) {
        _setInputs.insert(name);
      }
    }
    else {
      /* Inputs bound to geometry (UV0, the normal, the tangent) have no default; their
       * translations use the geometry. */
      if (!input || input->GetMetadata().count(_tokens->defaultgeomprop)) {
        continue;
      }
      value = input->GetDefaultValue();
    }
    if (!value.IsEmpty()) {
      _inputs[name].constant = value;
    }
  }
  _fileColorSpace = _network.GetNodeParameterData(_node, _tokens->file).colorSpace;
}

void MaterialXTranslator::ClearNode()
{
  for (const TfToken &name : _network.GetAuthoredNodeParameterNames(_node)) {
    _network.DeleteNodeParameter(_node, name);
  }
  for (const TfToken &name : _network.GetNodeInputConnectionNames(_node)) {
    _network.DeleteNodeInputConnection(_node, name);
  }
}

void MaterialXTranslator::TranslateMath(const MathOp &op)
{
  const bool vector = !IsFloatType();
  const char *cyclesOp = vector ? op.vectorOp : op.floatOp;
  if (op.unary) {
    Provide("out", Unary(cyclesOp, In("in"), vector));
    return;
  }
  /* MaterialX's modulo is floored, as Cycles' float floored_modulo is; its vector modulo
   * truncates, so vectors take x − y × floor(x ÷ y). */
  if (vector && std::string_view(op.floatOp) == "floored_modulo") {
    const Operand &x = In("in1");
    const Operand &y = In("in2");
    Provide("out",
            Math("subtract",
                 x,
                 Math("multiply", y, Unary("floor", Math("divide", x, y, true), true), true),
                 true));
    return;
  }
  Provide("out", Math(cyclesOp, In("in1"), In("in2"), vector));
}

/* standard_surface onto the Principled BSDF, which Cycles 4 modelled on OpenPBR. Weights that
 * Principled lacks multiply their colours; Principled's specular level of 0.5 is
 * standard_surface's specular weight of 1. */
void MaterialXTranslator::TranslateStandardSurface()
{
  const TfToken node = _node;
  SetType(node, "principled_bsdf");

  /* Principled colours subsurface and transmission with the base colour. */
  Operand baseColor = Math("multiply", In("base"), In("base_color"), true);
  baseColor = Mix(baseColor, In("subsurface_color"), In("subsurface"), true);
  Set(node, "base_color", baseColor);
  Set(node, "diffuse_roughness", In("diffuse_roughness"));
  Set(node, "metallic", In("metalness"));
  Set(node,
      "specular_ior_level",
      Math("multiply", In("specular"), Operand::Constant(VtValue(0.5f)), false));
  Set(node, "specular_tint", In("specular_color"));
  Set(node, "roughness", In("specular_roughness"));
  Set(node, "ior", In("specular_IOR"));
  Set(node, "anisotropic", In("specular_anisotropy"));
  Set(node, "anisotropic_rotation", In("specular_rotation"));
  Set(node, "transmission_weight", In("transmission"));
  Set(node, "subsurface_weight", In("subsurface"));
  Set(node, "subsurface_radius", In("subsurface_radius"));
  Set(node, "subsurface_scale", In("subsurface_scale"));
  Set(node, "subsurface_anisotropy", In("subsurface_anisotropy"));
  Set(node, "sheen_weight", In("sheen"));
  Set(node, "sheen_tint", In("sheen_color"));
  Set(node, "sheen_roughness", In("sheen_roughness"));
  Set(node, "coat_weight", In("coat"));
  Set(node, "coat_tint", In("coat_color"));
  Set(node, "coat_roughness", In("coat_roughness"));
  Set(node, "coat_ior", In("coat_IOR"));
  Set(node, "coat_normal", In("coat_normal"));
  Set(node, "thin_film_thickness", In("thin_film_thickness"));
  Set(node, "thin_film_ior", In("thin_film_IOR"));
  Set(node, "emission_strength", In("emission"));
  Set(node, "emission_color", In("emission_color"));
  /* opacity is a colour; Cycles' alpha is a float. */
  Set(node, "alpha", AsFloat(In("opacity")));
  Set(node, "thin_wall", AsFloat(In("thin_walled")));
  Set(node, "normal", In("normal"));
  Set(node, "tangent", In("tangent"));
  Ignore({"transmission_color",
          "transmission_depth",
          "transmission_scatter",
          "transmission_scatter_anisotropy",
          "transmission_dispersion",
          "transmission_extra_roughness",
          "coat_anisotropy",
          "coat_rotation",
          "coat_affect_color",
          "coat_affect_roughness"});
  Provide("out", Operand::Output(node, "BSDF"));
}

/* open_pbr_surface onto the Principled BSDF. Fuzz is Principled's sheen; the subsurface radius
 * is a distance scaling a colour; thin films are in micrometres where Cycles takes
 * nanometres. */
void MaterialXTranslator::TranslateOpenPbrSurface()
{
  const TfToken node = _node;
  SetType(node, "principled_bsdf");

  Operand baseColor = Math("multiply", In("base_weight"), In("base_color"), true);
  baseColor = Mix(baseColor, In("subsurface_color"), In("subsurface_weight"), true);
  Set(node, "base_color", baseColor);
  Set(node, "diffuse_roughness", In("base_diffuse_roughness"));
  Set(node, "metallic", In("base_metalness"));
  Set(node,
      "specular_ior_level",
      Math("multiply", In("specular_weight"), Operand::Constant(VtValue(0.5f)), false));
  Set(node, "specular_tint", In("specular_color"));
  Set(node, "roughness", In("specular_roughness"));
  Set(node, "ior", In("specular_ior"));
  Set(node, "anisotropic", In("specular_roughness_anisotropy"));
  Set(node, "transmission_weight", In("transmission_weight"));
  Set(node, "subsurface_weight", In("subsurface_weight"));
  Set(node, "subsurface_scale", In("subsurface_radius"));
  Set(node, "subsurface_radius", In("subsurface_radius_scale"));
  Set(node, "subsurface_anisotropy", In("subsurface_scatter_anisotropy"));
  Set(node, "sheen_weight", In("fuzz_weight"));
  Set(node, "sheen_tint", In("fuzz_color"));
  Set(node, "sheen_roughness", In("fuzz_roughness"));
  Set(node, "coat_weight", In("coat_weight"));
  Set(node, "coat_tint", In("coat_color"));
  Set(node, "coat_roughness", In("coat_roughness"));
  Set(node, "coat_ior", In("coat_ior"));
  Set(node, "coat_normal", In("geometry_coat_normal"));
  /* A thin film shows only where its weight is on: thickness × weight, in nanometres. */
  Set(node,
      "thin_film_thickness",
      Math("multiply",
           Math("multiply", In("thin_film_thickness"), In("thin_film_weight"), false),
           Operand::Constant(VtValue(1000.0f)),
           false));
  Set(node, "thin_film_ior", In("thin_film_ior"));
  Set(node, "emission_strength", In("emission_luminance"));
  Set(node, "emission_color", In("emission_color"));
  Set(node, "alpha", AsFloat(In("geometry_opacity")));
  Set(node, "thin_wall", AsFloat(In("geometry_thin_walled")));
  Set(node, "normal", In("geometry_normal"));
  Set(node, "tangent", In("geometry_tangent"));
  Ignore({"transmission_color",
          "transmission_depth",
          "transmission_scatter",
          "transmission_scatter_anisotropy",
          "transmission_dispersion_scale",
          "transmission_dispersion_abbe_number",
          "coat_roughness_anisotropy",
          "coat_darkening"});
  Provide("out", Operand::Output(node, "BSDF"));
}

/* surface_unlit: emission × emission_color, see-through where opacity is below 1. */
void MaterialXTranslator::TranslateUnlit()
{
  const TfToken emission = _node;
  SetType(emission, "emission");
  Set(emission, "color", Math("multiply", In("emission"), In("emission_color"), true));
  SetValue(emission, "strength", VtValue(1.0f));

  const Operand &opacity = In("opacity");
  if (opacity.IsConstant() && ToFloat(opacity.constant) >= 1.0f) {
    Provide("out", Operand::Output(emission, "Emission"));
    return;
  }
  const TfToken transparent = AddNode("transparent_bsdf");
  SetValue(transparent, "color", VtValue(GfVec3f(1.0f)));
  const TfToken mix = AddNode("mix_closure");
  Set(mix, "fac", opacity);
  Set(mix, "closure1", Operand::Output(transparent, "bsdf"));
  Set(mix, "closure2", Operand::Output(emission, "emission"));
  Provide("out", Operand::Output(mix, "Closure"));
}

/* displacement along the normal: Cycles' displacement node with a midlevel of 0. A constant
 * zero displacement is no displacement at all. */
void MaterialXTranslator::TranslateDisplacement()
{
  const Operand &displacement = In("displacement");
  if (!_valueType.starts_with("float")) {
    HDCYCLES_WARN("MaterialX %s: only float displacement is translated", _node.GetText());
    Provide("out", Operand());
    _network.DeleteNode(_node);
    return;
  }
  if (!displacement.connection && ToFloat(displacement.constant) == 0.0f) {
    Provide("out", Operand());
    _network.DeleteNode(_node);
    return;
  }
  SetType(_node, "displacement");
  SetValue(_node, "midlevel", VtValue(0.0f));
  Set(_node, "height", displacement);
  Set(_node, "scale", In("scale"));
  Provide("out", Operand::Output(_node, "Displacement"));
}

/* image and tiledimage onto Cycles' image texture. Colour images are read in their authored
 * colour space, or in the one the config's file rules give; other types are data. */
void MaterialXTranslator::TranslateImage(const bool tiled)
{
  const TfToken image = _node;
  SetType(image, "image_texture");
  Set(image, "filename", In("file"));

  /* Non-colour images are data. A colour image without a colour space is read as the file
   * and the config's file rules say, as Karma does: the image node's default. */
  const bool color = _valueType.starts_with("color");
  if (!color) {
    SetValue(image, "colorspace", VtValue(TfToken(u_colorspace_data.string())));
  }
  if (color && !_fileColorSpace.IsEmpty()) {
    /* MaterialX's colour space names, then the names and aliases of the ACES and Houdini
     * configs. */
    static const std::unordered_map<std::string, std::vector<std::string>> aliases = {
        {"srgb_texture", {"srgb_texture", "srgb_tx", "sRGB - Texture", "sRGB"}},
        {"lin_rec709", {"lin_rec709", "lin_rec709_scene", "Linear Rec.709 (sRGB)"}},
        {"acescg", {"acescg", "lin_ap1_scene", "ACEScg"}},
        {"lin_ap1", {"lin_ap1", "lin_ap1_scene", "ACEScg"}},
    };
    const auto it = aliases.find(_fileColorSpace.GetString());
    const std::vector<std::string> names = it != aliases.end() ?
                                               it->second :
                                               std::vector<std::string>{_fileColorSpace};
    ustring found;
    for (const std::string &name : names) {
      found = ColorSpaceManager::find_colorspace({name.c_str()});
      if (!found.empty()) {
        break;
      }
    }
    if (_fileColorSpace.GetString() == "raw") {
      SetValue(image, "colorspace", VtValue(TfToken(u_colorspace_data.string())));
    }
    else if (!found.empty()) {
      SetValue(image, "colorspace", VtValue(TfToken(found.string())));
    }
    else {
      HDCYCLES_WARN(
          "MaterialX %s: colour space '%s' is not in the OCIO config; detecting it from the "
          "file instead",
          _node.GetText(),
          _fileColorSpace.GetText());
    }
  }

  /* MaterialX's address modes, per axis, onto Cycles' one extension mode, by the U axis. */
  const std::string addressMode = ConstantString("uaddressmode");
  const std::string vAddressMode = ConstantString("vaddressmode");
  if (!vAddressMode.empty() && vAddressMode != addressMode) {
    _notes.insert(_nodeDefinition.GetString() + " uses uaddressmode '" + addressMode +
                  "' for both axes, not vaddressmode '" + vAddressMode + "'");
  }
  if (!addressMode.empty()) {
    static const std::unordered_map<std::string, const char *> extensions = {
        {"periodic", "periodic"}, {"clamp", "clamp"}, {"mirror", "mirror"}, {"constant", "black"}};
    const auto it = extensions.find(addressMode);
    if (it != extensions.end()) {
      SetValue(image, "extension", VtValue(TfToken(it->second)));
    }
  }
  const std::string filter = ConstantString("filtertype");
  if (filter == "closest") {
    SetValue(image, "interpolation", VtValue(TfToken("closest")));
  }
  else if (filter == "cubic") {
    SetValue(image, "interpolation", VtValue(TfToken("cubic")));
  }

  if (tiled) {
    /* uv × tiling − offset, then ÷ image size × tile size: one scale and one offset. */
    Operand uv = Texcoord("texcoord");
    uv = Math("multiply", uv, In("uvtiling"), true);
    uv = Math("subtract", uv, In("uvoffset"), true);
    uv = Math("divide", uv, In("realworldimagesize"), true);
    uv = Math("multiply", uv, In("realworldtilesize"), true);
    Set(image, "vector", uv);
  }
  else if (In("texcoord").connection) {
    Set(image, "vector", In("texcoord"));
  }

  /* A float image is the file's first channel. */
  if (_valueType.starts_with("float")) {
    Provide("out", Component(Operand::Output(image, "color"), 0, true));
  }
  else {
    Provide("out", Operand::Output(image, "color"));
  }
}

bool MaterialXTranslator::TranslateNode(const std::string &family)
{
  const bool vector = !IsFloatType();

  if (family == "standard_surface") {
    TranslateStandardSurface();
    return true;
  }
  if (family == "open_pbr_surface") {
    TranslateOpenPbrSurface();
    return true;
  }
  if (family == "surface_unlit") {
    TranslateUnlit();
    return true;
  }
  if (family == "displacement") {
    TranslateDisplacement();
    return true;
  }
  if (family == "image" || family == "tiledimage") {
    TranslateImage(family == "tiledimage");
    return true;
  }

  if (family == "UsdPrimvarReader") {
    const TfToken node = _node;
    SetType(node, "attribute");
    Set(node, "attribute", In("varname"));
    Provide("out", Operand::Output(node, IsFloatType() ? "fac" : "vector"));
    return true;
  }

  if (family == "texcoord") {
    if (const int index = int(ConstantFloat("index", 0.0f)); index != 0) {
      _notes.insert(_nodeDefinition.GetString() + " index " + std::to_string(index) +
                    " reads the default UV set");
    }
    Provide("out", DefaultUV());
    _network.DeleteNode(_node);
    return true;
  }
  if (family == "geompropvalue") {
    const TfToken node = _node;
    SetType(node, "attribute");
    Set(node, "attribute", In("geomprop"));
    Provide("out",
            Operand::Output(node,
                            IsFloatType()                   ? "fac" :
                            _valueType.starts_with("color") ? "color" :
                                                              "vector"));
    return true;
  }
  if (family == "position" || family == "normal" || family == "tangent") {
    const TfToken geometry = AddNode("geometry");
    Operand value = Operand::Output(geometry, family.c_str());
    if (ConstantString("space") != "world") {
      const TfToken transform = _node;
      SetType(transform, "vector_transform");
      SetValue(transform,
               "transform_type",
               VtValue(TfToken(family == "position" ? "point" :
                               family == "normal"   ? "normal" :
                                                      "vector")));
      SetValue(transform, "convert_from", VtValue(TfToken("world")));
      SetValue(transform, "convert_to", VtValue(TfToken("object")));
      Set(transform, "vector", value);
      value = Operand::Output(transform, "vector");
    }
    else {
      _network.DeleteNode(_node);
    }
    Provide("out", value);
    return true;
  }
  if (family == "normalmap") {
    const TfToken node = _node;
    SetType(node, "normal_map");
    SetValue(node, "space", VtValue(TfToken("tangent")));
    Set(node, "color", In("in"));
    const Operand &scale = In("scale");
    Set(node,
        "strength",
        scale.IsConstant() ? Operand::Constant(VtValue(ToFloat(scale.constant))) : scale);
    Provide("out", Operand::Output(node, "normal"));
    return true;
  }

  /* Nodes that pass a value through: Cycles converts between floats, colours and vectors at
   * its sockets. */
  if (family == "dot" || family == "convert") {
    Provide("out", In("in"));
    _network.DeleteNode(_node);
    return true;
  }
  if (family == "constant") {
    Provide("out", In("value"));
    _network.DeleteNode(_node);
    return true;
  }

  if (const auto it = kMathOps.find(family); it != kMathOps.end()) {
    if (vector && !it->second.vectorOp) {
      return false;
    }
    TranslateMath(it->second);
    return true;
  }
  if (family == "clamp") {
    if (vector) {
      Provide("out",
              Math("minimum", Math("maximum", In("in"), In("low"), true), In("high"), true));
    }
    else {
      const TfToken node = _node;
      SetType(node, "clamp");
      Set(node, "value", In("in"));
      Set(node, "min", In("low"));
      Set(node, "max", In("high"));
      Provide("out", Operand::Output(node, "result"));
    }
    return true;
  }
  if (family == "mix") {
    Provide("out", Mix(In("bg"), In("fg"), In("mix"), vector));
    return true;
  }
  if (family == "invert") {
    Provide("out", Math("subtract", In("amount"), In("in"), vector));
    return true;
  }
  if (family == "dotproduct") {
    const TfToken node = AddNode("vector_math");
    SetValue(node, "math_type", VtValue(TfToken("dot_product")));
    Set(node, "vector1", In("in1"));
    Set(node, "vector2", In("in2"));
    Provide("out", Operand::Output(node, "value"));
    return true;
  }
  if (family == "crossproduct" || family == "normalize" || family == "magnitude" ||
      family == "distance")
  {
    const char *op = family == "crossproduct" ? "cross_product" :
                     family == "normalize"    ? "normalize" :
                     family == "magnitude"    ? "length" :
                                                "distance";
    const TfToken node = AddNode("vector_math");
    SetValue(node, "math_type", VtValue(TfToken(op)));
    Set(node, "vector1", family == "crossproduct" || family == "distance" ? In("in1") : In("in"));
    Set(node, "vector2", In("in2"));
    Provide(
        "out",
        Operand::Output(node, family == "magnitude" || family == "distance" ? "value" : "vector"));
    return true;
  }
  if (family == "extract") {
    Provide(
        "out",
        Component(In("in"), int(ConstantFloat("index", 0.0f)), _valueType.starts_with("color")));
    return true;
  }
  if (family == "separate2" || family == "separate3") {
    const bool color = _valueType.starts_with("color");
    static const char *const colorOutputs[] = {"outr", "outg", "outb"};
    static const char *const vectorOutputs[] = {"outx", "outy", "outz"};
    for (int i = 0; i < (family == "separate2" ? 2 : 3); ++i) {
      Provide((color ? colorOutputs : vectorOutputs)[i], Component(In("in"), i, color));
    }
    return true;
  }
  if (family == "combine2" || family == "combine3") {
    const bool color = _valueType.starts_with("color");
    const TfToken node = _node;
    SetType(node, color ? "combine_color" : "combine_xyz");
    Set(node, color ? "r" : "x", In("in1"));
    Set(node, color ? "g" : "y", In("in2"));
    if (family == "combine3") {
      Set(node, color ? "b" : "z", In("in3"));
    }
    Provide("out", Operand::Output(node, color ? "color" : "vector"));
    return true;
  }
  if (family == "luminance") {
    const TfToken node = AddNode("vector_math");
    SetValue(node, "math_type", VtValue(TfToken("dot_product")));
    Set(node, "vector1", In("in"));
    Set(node, "vector2", In("lumacoeffs"));
    Provide("out", Operand::Output(node, "value"));
    return true;
  }
  if (family == "remap") {
    /* outlow + (in − inlow) ÷ (inhigh − inlow) × (outhigh − outlow) */
    const Operand t = Math("divide",
                           Math("subtract", In("in"), In("inlow"), vector),
                           Math("subtract", In("inhigh"), In("inlow"), vector),
                           vector);
    Provide(
        "out",
        Math("add",
             Math("multiply", t, Math("subtract", In("outhigh"), In("outlow"), vector), vector),
             In("outlow"),
             vector));
    return true;
  }
  if (family == "smoothstep" && !vector) {
    const TfToken node = _node;
    SetType(node, "map_range");
    SetValue(node, "range_type", VtValue(TfToken("smoothstep")));
    Set(node, "value", In("in"));
    Set(node, "from_min", In("low"));
    Set(node, "from_max", In("high"));
    SetValue(node, "to_min", VtValue(0.0f));
    SetValue(node, "to_max", VtValue(1.0f));
    Provide("out", Operand::Output(node, "result"));
    return true;
  }
  if (family == "ifgreater" || family == "ifgreatereq" || family == "ifequal") {
    /* in1 where the condition holds, in2 otherwise. */
    Operand condition;
    if (family == "ifgreater") {
      condition = Math("greater_than", In("value1"), In("value2"), false);
    }
    else if (family == "ifgreatereq") {
      condition = Math("subtract",
                       Operand::Constant(VtValue(1.0f)),
                       Math("less_than", In("value1"), In("value2"), false),
                       false);
    }
    else {
      const TfToken node = AddNode("math");
      SetValue(node, "math_type", VtValue(TfToken("compare")));
      Set(node, "value1", In("value1"));
      Set(node, "value2", In("value2"));
      SetValue(node, "value3", VtValue(0.0f));
      condition = Operand::Output(node, "value");
    }
    Provide("out", Mix(In("in2"), In("in1"), condition, vector));
    return true;
  }
  if (family == "noise2d" || family == "noise3d" || family == "fractal3d") {
    /* MaterialX's noise library and Cycles' come from OSL's: the same hash, gradients, fade
     * and scale, and the same fractal sum. A float noise is Cycles' unnormalised Fac with
     * one octave, or octaves - 1 more for fractal3d. Colour and vector noises hash each
     * channel differently from Cycles' Color output: the same noise, other values. */
    const bool is2d = family == "noise2d";
    const bool scalar = IsFloatType();
    const TfToken node = _node;
    SetType(node, "noise_texture");
    SetValue(node, "dimensions", VtValue(TfToken(is2d ? "2D" : "3D")));
    SetValue(node, "type", VtValue(TfToken("fBM")));
    SetValue(node, "use_normalize", VtValue(false));
    SetValue(node, "scale", VtValue(1.0f));
    SetValue(node, "distortion", VtValue(0.0f));
    Set(node, "vector", is2d ? Texcoord("texcoord") : ObjectPosition("position"));
    if (family == "fractal3d") {
      Set(node,
          "detail",
          Math("subtract", AsFloat(In("octaves")), Operand::Constant(VtValue(1.0f)), false));
      Set(node, "roughness", In("diminish"));
      Set(node, "lacunarity", In("lacunarity"));
    }
    else {
      SetValue(node, "detail", VtValue(0.0f));
    }
    Operand value = Operand::Output(node, scalar ? "fac" : "color");
    value = Math("multiply", value, In("amplitude"), !scalar);
    if (family != "fractal3d") {
      value = Math("add", value, In("pivot"), !scalar);
    }
    Provide("out", value);
    return true;
  }
  if (family == "cellnoise2d" || family == "cellnoise3d") {
    /* A random value per integer cell: Cycles' white noise of the cell's corner, which
     * hashes the corner's float bits where MaterialX hashes its integers. */
    const bool is2d = family == "cellnoise2d";
    const TfToken node = _node;
    SetType(node, "white_noise_texture");
    SetValue(node, "dimensions", VtValue(TfToken(is2d ? "2D" : "3D")));
    Set(node,
        "vector",
        Unary("floor", is2d ? Texcoord("texcoord") : ObjectPosition("position"), true));
    Provide("out", Operand::Output(node, "value"));
    return true;
  }
  if ((family == "worleynoise2d" || family == "worleynoise3d") &&
      !_valueType.starts_with("vector3"))
  {
    /* The distance to the nearest of one jittered point per cell: Cycles' Voronoi F1, with
     * other hashes. A vector2 is the nearest two distances. */
    const bool is2d = family == "worleynoise2d";
    const Operand position = is2d ? Texcoord("texcoord") : ObjectPosition("position");
    const auto voronoi = [&](const char *feature) {
      const TfToken node = AddNode("voronoi_texture");
      SetValue(node, "dimensions", VtValue(TfToken(is2d ? "2D" : "3D")));
      SetValue(node, "metric", VtValue(TfToken("euclidean")));
      SetValue(node, "feature", VtValue(TfToken(feature)));
      SetValue(node, "scale", VtValue(1.0f));
      Set(node, "randomness", In("jitter"));
      Set(node, "vector", position);
      return node;
    };
    if (IsFloatType()) {
      /* style 1 is the nearest cell's random value. */
      const TfToken f1 = voronoi("f1");
      Provide("out",
              ConstantFloat("style", 0.0f) == 1.0f ?
                  Component(Operand::Output(f1, "color"), 0, true) :
                  Operand::Output(f1, "distance"));
    }
    else {
      const TfToken combine = AddNode("combine_xyz");
      Set(combine, "x", Operand::Output(voronoi("f1"), "distance"));
      Set(combine, "y", Operand::Output(voronoi("f2"), "distance"));
      Provide("out", Operand::Output(combine, "vector"));
    }
    return true;
  }
  if (family == "ramplr" || family == "ramptb") {
    const bool lr = family == "ramplr";
    const TfToken clamp = AddNode("clamp");
    Set(clamp, "value", Component(Texcoord("texcoord"), lr ? 0 : 1, false));
    SetValue(clamp, "min", VtValue(0.0f));
    SetValue(clamp, "max", VtValue(1.0f));
    Provide("out",
            Mix(In(lr ? "valuel" : "valuet"),
                In(lr ? "valuer" : "valueb"),
                Operand::Output(clamp, "result"),
                vector));
    return true;
  }
  return false;
}

/* What provides a MaterialX output after translation, following pass-through nodes. Empty
 * when the upstream node was left out. */
std::optional<Operand> MaterialXTranslator::Resolve(const InputConnection &connection)
{
  InputConnection current = connection;
  for (int depth = 0; depth < 64; ++depth) {
    const auto it = _outputs.find({current.upstreamNodeName, current.upstreamOutputName});
    if (it == _outputs.end()) {
      if (_untranslated.count(current.upstreamNodeName)) {
        return Operand();
      }
      return depth == 0 ? std::nullopt : std::optional<Operand>(Operand{current, VtValue()});
    }
    if (!it->second.connection) {
      return it->second;
    }
    current = *it->second.connection;
  }
  _notes.insert("a cycle of pass-through nodes at " + connection.upstreamNodeName.GetString() +
                " is left unconnected");
  return Operand();
}

/* Writes what feeds an input: a connection, a constant, or nothing for a node left out. */
void MaterialXTranslator::Write(const TfToken &node, const TfToken &input, const Operand &value)
{
  if (value.connection) {
    HdMaterialNetworkInterface::InputConnectionVector connections;
    connections.push_back(*value.connection);
    _network.SetNodeInputConnection(node, input, connections);
  }
  else if (!value.constant.IsEmpty()) {
    _network.SetNodeParameterValue(node, input, ShapeForSocket(node, input, value.constant));
  }
}

void MaterialXTranslator::Rewire()
{
  for (const PendingConnection &pending : _pending) {
    const std::optional<Operand> resolved = Resolve(pending.connection);
    Write(pending.node, pending.input, resolved ? *resolved : Operand{pending.connection, {}});
  }

  /* The nodes left as authored, such as UsdPreviewSurface, may read translated nodes. */
  for (const TfToken &node : _network.GetNodeNames()) {
    if (_authored.count(node)) {
      continue;
    }
    for (const TfToken &input : _network.GetNodeInputConnectionNames(node)) {
      const auto connections = _network.GetNodeInputConnection(node, input);
      const std::optional<Operand> resolved = connections.empty() ? std::nullopt :
                                                                    Resolve(connections[0]);
      if (resolved) {
        _network.DeleteNodeInputConnection(node, input);
        Write(node, input, *resolved);
      }
    }
  }

  for (const TfToken &terminal : _network.GetTerminalNames()) {
    const auto [connected, connection] = _network.GetTerminalConnection(terminal);
    if (!connected) {
      continue;
    }
    const std::optional<Operand> resolved = Resolve(connection);
    if (!resolved) {
      continue;
    }
    if (resolved->connection) {
      _network.SetTerminalConnection(terminal, *resolved->connection);
    }
    else {
      _network.DeleteTerminal(terminal);
    }
  }
}

void MaterialXTranslator::Translate()
{
  SdrRegistry &registry = SdrRegistry::GetInstance();
  std::set<std::string> untranslatedTypes;

  for (const TfToken &node : _network.GetNodeNames()) {
    const TfToken type = _network.GetNodeType(node);
    if (!type.GetString().starts_with("ND_")) {
      continue;
    }
    /* The MaterialX library's own definition: without a source type, Sdr may return another
     * parser's, such as Karma's, which under husk has no family. */
    const SdrShaderNodeConstPtr sdrNode = registry.GetShaderNodeByIdentifierAndType(type,
                                                                                    _tokens->mtlx);
    if (!sdrNode) {
      untranslatedTypes.insert(type.GetString());
      _untranslated.insert(node);
      _network.DeleteNode(node);
      continue;
    }

    _node = node;
    _nodeDefinition = type;
    _helpers = 0;
    const std::string family = sdrNode->GetFamily().GetString();
    /* ND_<family>_<type>, e.g. ND_add_color3FA or ND_image_vector3. */
    const std::string &id = type.GetString();
    const size_t typeStart = 3 + family.size() + 1;
    _valueType = id.size() > typeStart && id.compare(3, family.size(), family) == 0 ?
                     id.substr(typeStart) :
                     std::string();

    /* MaterialX's UsdPreviewSurface nodes are the UsdPreviewSurface and UsdUVTexture the
     * delegate maps already, with the same inputs and outputs: retype them in place. */
    if (family == "UsdPreviewSurface" || family == "UsdUVTexture") {
      _network.SetNodeType(node, TfToken(family));
      _retyped[node] = sdrNode;
      if (family == "UsdPreviewSurface") {
        _outputs[{node, TfToken("out")}] = Operand::Output(node, "surface");
      }
      continue;
    }

    _authored.insert(node);
    ReadInputs(sdrNode);
    ClearNode();
    if (!TranslateNode(family)) {
      untranslatedTypes.insert(type.GetString());
      _untranslated.insert(node);
      _network.DeleteNode(node);
    }
    /* A node translated into helper nodes alone goes. */
    else if (_network.GetNodeType(node) == type) {
      _network.DeleteNode(node);
    }
  }

  Rewire();

  for (const std::string &type : untranslatedTypes) {
    HDCYCLES_WARN(
        "MaterialX material %s: %s has no Cycles translation; the inputs it feeds are left "
        "unconnected, at the Cycles node's defaults",
        _network.GetMaterialPrimPath().GetText(),
        type.c_str());
  }
  for (const std::string &note : _notes) {
    HDCYCLES_WARN(
        "MaterialX material %s: %s", _network.GetMaterialPrimPath().GetText(), note.c_str());
  }
}

}  // namespace

HdContainerDataSourceHandle HdCyclesTranslateMaterialX(const SdfPath &materialPath,
                                                       const HdContainerDataSourceHandle &network,
                                                       const HdContainerDataSourceHandle &prim)
{
  HdDataSourceMaterialNetworkInterface networkInterface(materialPath, network, prim);
  MaterialXTranslator(networkInterface).Translate();
  return networkInterface.Finish();
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
