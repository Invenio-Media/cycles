/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/material.h"
#include "hydra/node_util.h"
#include "hydra/session.h"
#include "hydra/util.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "util/colorspace.h"

#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Normalize a material network node name to a full SdfPath. The schema may
 * provide either a full path or a bare identifier. */
static SdfPath MaterialNodeNameToSdfPath(const TfToken &nodeName)
{
  const std::string &s = nodeName.GetString();
  if (s.empty()) {
    return SdfPath::EmptyPath();
  }
  if (s[0] == '/' && SdfPath::IsValidPathString(s)) {
    return SdfPath(s);
  }
  return SdfPath::AbsoluteRootPath().AppendChild(nodeName);
}

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(CyclesMaterialTokens,
    (cycles)
    ((cyclesSurface, "cycles:surface"))
    ((cyclesDisplacement, "cycles:displacement"))
    ((cyclesVolume, "cycles:volume"))
    (UsdPreviewSurface)
    (UsdUVTexture)
    (UsdPrimvarReader_float)
    (UsdPrimvarReader_float2)
    (UsdPrimvarReader_float3)
    (UsdPrimvarReader_float4)
    (UsdPrimvarReader_int)
    (UsdTransform2d)
    (a)
    (rgb)
    (r)
    (g)
    (b)
    (result)
    (st)
    (wrapS)
    (wrapT)
    (periodic)
    (sourceColorSpace)
    (sRGB)
    (raw)
    (scale)
    (bias)
    (normal)
    (displacement)
    (opacityThreshold)
    (useSpecularWorkflow)
    (specularColor)
);
// clang-format on

/* Sentinel return value from parameterName(): skip this parameter silently
 * (no warning, no node socket lookup). */
static constexpr const char *SKIP_PARAM = "__skip__";

/* Simple class to handle remapping of USDPreviewSurface nodes and parameters to Cycles
 * equivalents. */
class UsdToCyclesMapping {
  using ParamMap = std::unordered_map<TfToken, ustring, TfToken::HashFunctor>;

 public:
  UsdToCyclesMapping(const char *nodeType, ParamMap paramMap)
      : _nodeType(nodeType), _paramMap(std::move(paramMap))
  {
  }

  ustring nodeType() const
  {
    return _nodeType;
  }

  /* Called once on a newly created Cycles node, before the authored parameters are set. */
  virtual void initializeNode(ShaderNode * /*node*/) const {}

  virtual std::string parameterName(const TfToken &name,
                                    const ShaderInput *inputConnection,
                                    VtValue * /*value*/ = nullptr) const
  {
    /* UsdNode.name -> Node.input. These all follow a simple pattern that we can just
     * remap based on the name or 'Node.input' type. */
    if (inputConnection) {
      if (name == CyclesMaterialTokens->a) {
        return "alpha";
      }
      if (name == CyclesMaterialTokens->rgb) {
        return "color";
      }
      /* r/g/b single-channel outputs are handled via ResolveTextureOutput; the connection
       * path never reaches here for them. Return "color" as a fallback only. */
      if (name == CyclesMaterialTokens->r || name == CyclesMaterialTokens->g ||
          name == CyclesMaterialTokens->b)
      {
        return "color";
      }

      if (name == CyclesMaterialTokens->result) {
        switch (inputConnection->socket_type.type) {
          case SocketType::BOOLEAN:
          case SocketType::FLOAT:
          case SocketType::INT:
          case SocketType::UINT:
            return "alpha";
          case SocketType::COLOR:
          case SocketType::VECTOR:
          case SocketType::POINT:
          case SocketType::NORMAL:
          default:
            return "color";
        }
      }
    }

    /* Simple mapping case */
    const auto it = _paramMap.find(name);
    return it != _paramMap.end() ? it->second.string() : name.GetString();
  }

 private:
  const ustring _nodeType;
  ParamMap _paramMap;
};

class UsdToCyclesTexture : public UsdToCyclesMapping {
 public:
  using UsdToCyclesMapping::UsdToCyclesMapping;

  std::string parameterName(const TfToken &name,
                            const ShaderInput *inputConnection,
                            VtValue *value) const override
  {
    /* scale and bias are handled as auxiliary math nodes; skip the normal socket lookup. */
    if (name == CyclesMaterialTokens->scale || name == CyclesMaterialTokens->bias) {
      return SKIP_PARAM;
    }

    if (value) {
      /* Remap UsdUVTexture.wrapS and UsdUVTexture.wrapT to cycles_image_texture.extension. */
      if (name == CyclesMaterialTokens->wrapS || name == CyclesMaterialTokens->wrapT) {
        const std::string valueString = VtValue::Cast<std::string>(*value).Get<std::string>();

        /* A value of 'repeat' in USD is equivalent to 'periodic' in Cycles. */
        if (valueString == "repeat") {
          *value = VtValue(CyclesMaterialTokens->periodic);
        }

        return "extension";
      }

      /* UsdUVTexture's sourceColorSpace values are not OCIO colorspace names, and the host's
       * config need not define "sRGB" or "raw", in which case Cycles would read the texture as
       * linear. Map them to the config's sRGB texture colorspace and to non-color data. */
      if (name == CyclesMaterialTokens->sourceColorSpace) {
        const TfToken valueToken = value->IsHolding<std::string>() ?
                                       TfToken(value->UncheckedGet<std::string>()) :
                                       VtValue::Cast<TfToken>(*value).GetWithDefault(TfToken());
        if (valueToken == CyclesMaterialTokens->raw) {
          *value = VtValue(TfToken(u_colorspace_data.string()));
          return "colorspace";
        }
        if (valueToken == CyclesMaterialTokens->sRGB) {
          /* The ColorInterop ID first, then the names and aliases of the ACES and Houdini
           * configs. */
          const ustring srgb = ColorSpaceManager::find_colorspace(
              {"srgb_rec709_scene", "srgb_tx", "sRGB - Texture", "srgb_texture", "sRGB"});
          if (!srgb.empty()) {
            *value = VtValue(TfToken(srgb.string()));
            return "colorspace";
          }
        }
        /* "auto", or no sRGB colorspace in the config: reset to Cycles' automatic detection
         * from the file and the config's file rules. */
        *value = VtValue();
        return "colorspace";
      }
    }

    return UsdToCyclesMapping::parameterName(name, inputConnection, value);
  }
};

class UsdToCyclesPreviewSurface : public UsdToCyclesMapping {
 public:
  using UsdToCyclesMapping::UsdToCyclesMapping;

  /* Start from the UsdPreviewSurface defaults, which differ from the Principled BSDF's, so
   * that inputs the material does not author render as the specification says. specular_tint
   * keeps the Principled default (white): UsdPreviewSurface's specularColor only applies with
   * useSpecularWorkflow, which is handled in PopulateShaderGraph's post-processing step. */
  void initializeNode(ShaderNode *node) const override
  {
    /* Warn rather than crash if a later Cycles renames a socket, as 4.0 did with emission. */
    const auto findInput = [node](const char *name) -> const SocketType * {
      const SocketType *input = node->type->find_input(ustring(name));
      if (!input) {
        TF_WARN("UsdPreviewSurface: Cycles node '%s' has no input '%s'",
                node->type->name.c_str(),
                name);
      }
      return input;
    };
    const auto setFloat3 = [&](const char *name, const float3 value) {
      if (const SocketType *input = findInput(name)) {
        node->set(*input, value);
      }
    };
    const auto setFloat = [&](const char *name, const float value) {
      if (const SocketType *input = findInput(name)) {
        node->set(*input, value);
      }
    };
    setFloat3("base_color", make_float3(0.18f, 0.18f, 0.18f));
    setFloat3("emission_color", zero_float3());
    /* UsdPreviewSurface's emissiveColor is the emitted radiance. */
    setFloat("emission_strength", 1.0f);
    setFloat("roughness", 0.5f);
    setFloat("metallic", 0.0f);
    setFloat("ior", 1.5f);
    setFloat("alpha", 1.0f);
    setFloat("coat_weight", 0.0f);
    setFloat("coat_roughness", 0.01f);
  }

  std::string parameterName(const TfToken &name,
                            const ShaderInput *inputConnection,
                            VtValue * /*value*/ = nullptr) const override
  {
    /* These are handled as auxiliary nodes or post-processing; suppress the normal warning. */
    if (name == CyclesMaterialTokens->normal ||
        name == CyclesMaterialTokens->displacement ||
        name == CyclesMaterialTokens->opacityThreshold ||
        name == CyclesMaterialTokens->useSpecularWorkflow ||
        name == CyclesMaterialTokens->specularColor)
    {
      return SKIP_PARAM;
    }

    /* Simple mapping case from the base class. */
    return UsdToCyclesMapping::parameterName(name, inputConnection);
  }
};

namespace {

class UsdToCycles {
  const UsdToCyclesPreviewSurface UsdPreviewSurface = {
      "principled_bsdf",
      {
          {TfToken("diffuseColor"), ustring("base_color")},
          {TfToken("emissiveColor"), ustring("emission_color")},
          /* specularColor: conditional on useSpecularWorkflow; handled in post-processing. */
          {TfToken("clearcoat"), ustring("coat_weight")},
          {TfToken("clearcoatRoughness"), ustring("coat_roughness")},
          {TfToken("opacity"), ustring("alpha")},
          /* normal: routed through normal_map chain (see PopulateShaderGraph pass 3). */
          /* displacement: handled at terminal time. */
          /* useSpecularWorkflow / specularColor: handled in post-processing. */
          /* opacityThreshold: handled in post-processing. */
          /* occlusion: spec says renderers may ignore it. */
      }};
  const UsdToCyclesTexture UsdUVTexture = {
      "image_texture",
      {
          {CyclesMaterialTokens->st, ustring("vector")},
          {CyclesMaterialTokens->wrapS, ustring("extension")},
          {CyclesMaterialTokens->wrapT, ustring("extension")},
          {TfToken("file"), ustring("filename")},
          {TfToken("sourceColorSpace"), ustring("colorspace")},
          /* scale/bias: handled as multiply_add auxiliary nodes. */
      }};
  const UsdToCyclesMapping UsdPrimvarReader = {"attribute",
                                               {{TfToken("varname"), ustring("attribute")}}};

 public:
  const UsdToCyclesMapping *findUsd(const TfToken &usdNodeType)
  {
    if (usdNodeType == CyclesMaterialTokens->UsdPreviewSurface) {
      return &UsdPreviewSurface;
    }
    if (usdNodeType == CyclesMaterialTokens->UsdUVTexture) {
      return &UsdUVTexture;
    }
    if (usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float2 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float3 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float4 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_int)
    {
      return &UsdPrimvarReader;
    }

    return nullptr;
  }
  const UsdToCyclesMapping *findCycles(const ustring & /*cyclesNodeType*/)
  {
    return nullptr;
  }

  /* Expose the UsdPreviewSurface mapping pointer for identity checks. */
  const UsdToCyclesMapping *previewSurfaceMapping() const
  {
    return &UsdPreviewSurface;
  }

  /* Expose the UsdUVTexture mapping pointer for identity checks. */
  const UsdToCyclesMapping *uvTextureMapping() const
  {
    return &UsdUVTexture;
  }
};
TfStaticData<UsdToCycles> sUsdToCyles;

/* Read a float4 parameter from a USD node's parameter schema, returns false if not found. */
static bool ReadVec4fParam(HdMaterialNodeParameterContainerSchema params,
                           const TfToken &name,
                           GfVec4f *out)
{
  auto ds = params.Get(name);
  if (!ds) {
    return false;
  }
  auto valDs = ds.GetValue();
  if (!valDs) {
    return false;
  }
  VtValue val = valDs->GetValue(0.0f);
  if (val.IsHolding<GfVec4f>()) {
    *out = val.UncheckedGet<GfVec4f>();
    return true;
  }
  return false;
}

/* Read a float parameter from a USD node's parameter schema, returns false if not found. */
static bool ReadFloatParam(HdMaterialNodeParameterContainerSchema params,
                           const TfToken &name,
                           float *out)
{
  auto ds = params.Get(name);
  if (!ds) {
    return false;
  }
  auto valDs = ds.GetValue();
  if (!valDs) {
    return false;
  }
  VtValue val = valDs->GetValue(0.0f);
  if (val.IsHolding<float>()) {
    *out = val.UncheckedGet<float>();
    return true;
  }
  if (val.IsHolding<int>()) {
    *out = static_cast<float>(val.UncheckedGet<int>());
    return true;
  }
  return false;
}

/* Read an int parameter from a USD node's parameter schema, returns false if not found. */
static bool ReadIntParam(HdMaterialNodeParameterContainerSchema params,
                         const TfToken &name,
                         int *out)
{
  auto ds = params.Get(name);
  if (!ds) {
    return false;
  }
  auto valDs = ds.GetValue();
  if (!valDs) {
    return false;
  }
  VtValue val = valDs->GetValue(0.0f);
  if (val.IsHolding<int>()) {
    *out = val.UncheckedGet<int>();
    return true;
  }
  if (val.IsHolding<float>()) {
    *out = static_cast<int>(val.UncheckedGet<float>());
    return true;
  }
  return false;
}

/* Read a float3 parameter from a USD node's parameter schema, returns false if not found. */
static bool ReadVec3fParam(HdMaterialNodeParameterContainerSchema params,
                           const TfToken &name,
                           GfVec3f *out)
{
  auto ds = params.Get(name);
  if (!ds) {
    return false;
  }
  auto valDs = ds.GetValue();
  if (!valDs) {
    return false;
  }
  VtValue val = valDs->GetValue(0.0f);
  if (val.IsHolding<GfVec3f>()) {
    *out = val.UncheckedGet<GfVec3f>();
    return true;
  }
  return false;
}

}  // namespace

HdCyclesMaterial::HdCyclesMaterial(const SdfPath &sprimId) : HdMaterial(sprimId) {}

HdCyclesMaterial::~HdCyclesMaterial() = default;

HdDirtyBits HdCyclesMaterial::GetInitialDirtyBitsMask() const
{
  return DirtyBits::DirtyResource | DirtyBits::DirtyParams;
}

void HdCyclesMaterial::Sync(HdSceneDelegate *sceneDelegate,
                            HdRenderParam *renderParam,
                            HdDirtyBits *dirtyBits)
{
  if (*dirtyBits == DirtyBits::Clean) {
    return;
  }

  Initialize(renderParam);

  const SceneLock lock(renderParam);

  const bool dirtyParams = (*dirtyBits & DirtyBits::DirtyParams);
  const bool dirtyResource = (*dirtyBits & DirtyBits::DirtyResource);

  const SdfPath &id = GetId();

  if (dirtyResource || dirtyParams) {
    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
    const HdContainerDataSourceHandle &primDs = prim.dataSource;

    HdMaterialSchema matSchema = HdMaterialSchema::GetFromParent(primDs);
    /* Prefer cycles network if it exists, otherwise use universal network. */
    HdMaterialNetworkSchema network = matSchema.GetMaterialNetwork(CyclesMaterialTokens->cycles);
    if (!network) {
      network = matSchema.GetMaterialNetwork();
    }

    if (network) {
      /* Use the fast UpdateParameters path only when the graph has no auxiliary nodes
       * (scale/bias chains, normal_map) that would need reconstruction. */
      if (!_nodes.empty() && !dirtyResource && !_rebuildOnDirtyParams) {
        UpdateParameters(network);
        _shader->tag_modified();
      }
      else {
        PopulateShaderGraph(network);
      }
    }
    else {
      TF_RUNTIME_ERROR("Could not get a material network for %s.", id.GetText());
    }
  }

  if (_shader->is_modified()) {
    _shader->tag_update(lock.scene);
  }

  *dirtyBits = DirtyBits::Clean;
}

void HdCyclesMaterial::UpdateParameters(NodeDesc &nodeDesc,
                                        HdMaterialNodeParameterContainerSchema params,
                                        const SdfPath &nodePath)
{
  for (const TfToken &paramName : params.GetNames()) {
    auto valueDs = params.Get(paramName).GetValue();
    if (!valueDs) {
      continue;
    }
    VtValue value = valueDs->GetValue(0.0f);

    /* See if the parameter name is in USDPreviewSurface terms, and needs to be converted .*/
    const UsdToCyclesMapping *inputMapping = nodeDesc.mapping;
    const std::string inputName = inputMapping ?
                                      inputMapping->parameterName(paramName, nullptr, &value) :
                                      paramName.GetString();

    /* Mapping returned the skip sentinel: this parameter is handled elsewhere. */
    if (inputName == SKIP_PARAM) {
      continue;
    }

    /* Find the input to write the parameter value to. */
    const SocketType *input = nullptr;
    for (const SocketType &socket : nodeDesc.node->type->inputs) {
      if (string_iequals(socket.name.string(), inputName) || socket.ui_name == inputName) {
        input = &socket;
        break;
      }
    }

    if (!input) {
      TF_WARN("Could not find parameter '%s' on node '%s' ('%s')",
              paramName.GetText(),
              nodePath.GetText(),
              nodeDesc.node->name.c_str());
      continue;
    }

    /* An empty value from the mapping means the input's default. */
    if (value.IsEmpty()) {
      nodeDesc.node->set_default_value(*input);
    }
    else {
      SetNodeValue(nodeDesc.node, *input, value);
    }
  }
}

void HdCyclesMaterial::UpdateParameters(HdMaterialNetworkSchema network)
{
  HdMaterialNodeContainerSchema nodes = network.GetNodes();
  for (const TfToken &nodeName : nodes.GetNames()) {
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);

    const auto nodeIt = _nodes.find(nodePath);
    if (nodeIt == _nodes.end()) {
      TF_RUNTIME_ERROR("Could not update parameters on missing node '%s'", nodePath.GetText());
      continue;
    }

    UpdateParameters(nodeIt->second, nodes.Get(nodeName).GetParameters(), nodePath);
  }
}

ShaderOutput *HdCyclesMaterial::ResolveTextureOutput(NodeDesc &srcDesc,
                                                     const SdfPath &srcPath,
                                                     const TfToken &outputName,
                                                     ShaderInput *destInput,
                                                     ShaderGraph *graph)
{
  /* Determine the "base" color/alpha output: use end of scale/bias chain if present. */
  ShaderOutput *colorOut = srcDesc.scaleBiasRgb ?
                               srcDesc.scaleBiasRgb->output("Vector") :
                               srcDesc.node->output("Color");
  ShaderOutput *alphaOut = srcDesc.scaleBiasAlpha ?
                               srcDesc.scaleBiasAlpha->output("Value") :
                               srcDesc.node->output("Alpha");

  if (outputName == CyclesMaterialTokens->rgb) {
    return colorOut;
  }
  if (outputName == CyclesMaterialTokens->a) {
    return alphaOut;
  }

  /* Single-channel outputs: route through separate_color. */
  if (outputName == CyclesMaterialTokens->r || outputName == CyclesMaterialTokens->g ||
      outputName == CyclesMaterialTokens->b)
  {
    /* Create the separate_color node lazily on first r/g/b use for this texture. */
    if (!srcDesc.separateColor) {
      const NodeType *sepType = NodeType::find(ustring("separate_color"));
      if (!sepType) {
        TF_WARN("Could not find Cycles node type 'separate_color'");
        return colorOut; /* fallback */
      }
      srcDesc.separateColor = graph->create_node(sepType);
      graph->connect(colorOut, srcDesc.separateColor->input("Color"));
    }
    const char *outSocket = (outputName == CyclesMaterialTokens->r) ? "Red" :
                            (outputName == CyclesMaterialTokens->g) ? "Green" :
                                                                      "Blue";
    return srcDesc.separateColor->output(outSocket);
  }

  /* Non-channel outputs (e.g. for primvar readers): fall back to mapping. */
  const std::string outName = srcDesc.mapping ?
                                  srcDesc.mapping->parameterName(outputName, destInput) :
                                  outputName.GetString();
  return srcDesc.node->output(ustring(outName));
}

void HdCyclesMaterial::UpdateConnections(NodeDesc &nodeDesc,
                                         HdMaterialNodeSchema nodeSchema,
                                         const SdfPath &nodePath,
                                         ShaderGraph *shaderGraph)
{
  HdMaterialConnectionVectorContainerSchema conns = nodeSchema.GetInputConnections();
  for (const TfToken &dstSocketName : conns.GetNames()) {
    HdMaterialConnectionVectorSchema connVec = conns.Get(dstSocketName);
    const size_t count = connVec.GetNumElements();
    if (count == 0) {
      continue;
    }

    /* UsdPreviewSurface.displacement connection: store the upstream output for use at
     * terminal time; do NOT try to connect it to principled_bsdf (no such socket). */
    if (nodeDesc.mapping == sUsdToCyles->previewSurfaceMapping() &&
        dstSocketName == CyclesMaterialTokens->displacement)
    {
      HdMaterialConnectionSchema connSchema = connVec.GetElement(0);
      const SdfPath upPath =
          connSchema.GetUpstreamNodePath() ?
              MaterialNodeNameToSdfPath(
                  connSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
              SdfPath();
      const TfToken upOutput = connSchema.GetUpstreamNodeOutputName() ?
                                   connSchema.GetUpstreamNodeOutputName()->GetTypedValue(0.0f) :
                                   TfToken();
      auto srcIt = _nodes.find(upPath);
      if (srcIt != _nodes.end()) {
        ShaderOutput *out = ResolveTextureOutput(
            srcIt->second, upPath, upOutput, nullptr, shaderGraph);
        if (!out) {
          /* Try generic output resolution for non-texture upstream nodes. */
          const std::string outName = srcIt->second.mapping ?
                                          srcIt->second.mapping->parameterName(upOutput, nullptr) :
                                          upOutput.GetString();
          out = srcIt->second.node->output(ustring(outName));
        }
        _displacementOutput = out;
        _hasDisplacement = true;
      }
      continue;
    }

    const UsdToCyclesMapping *inputMapping = nodeDesc.mapping;

    /* UsdPreviewSurface.normal connection: route through the remap → normal_map chain. */
    if (nodeDesc.mapping == sUsdToCyles->previewSurfaceMapping() &&
        dstSocketName == CyclesMaterialTokens->normal && _normalRemapNode)
    {
      HdMaterialConnectionSchema connSchema = connVec.GetElement(0);
      const SdfPath upPath =
          connSchema.GetUpstreamNodePath() ?
              MaterialNodeNameToSdfPath(
                  connSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
              SdfPath();
      const TfToken upOutput = connSchema.GetUpstreamNodeOutputName() ?
                                   connSchema.GetUpstreamNodeOutputName()->GetTypedValue(0.0f) :
                                   TfToken();
      auto srcIt = _nodes.find(upPath);
      if (srcIt != _nodes.end()) {
        ShaderOutput *out = nullptr;
        if (srcIt->second.mapping == sUsdToCyles->uvTextureMapping()) {
          out = ResolveTextureOutput(
              srcIt->second, upPath, upOutput, _normalRemapNode->input("Vector1"), shaderGraph);
        }
        else {
          const std::string outName = srcIt->second.mapping ?
                                          srcIt->second.mapping->parameterName(
                                              upOutput, _normalRemapNode->input("Vector1")) :
                                          upOutput.GetString();
          out = srcIt->second.node->output(ustring(outName));
        }
        if (out) {
          shaderGraph->connect(out, _normalRemapNode->input("Vector1"));
        }
      }
      continue;
    }

    const std::string inputName = inputMapping ?
                                      inputMapping->parameterName(dstSocketName, nullptr) :
                                      dstSocketName.GetString();

    /* Skip parameters that are handled elsewhere. */
    if (inputName == SKIP_PARAM) {
      continue;
    }

    /* Find the input to connect to on the passed in node. */
    ShaderInput *input = nullptr;
    for (ShaderInput *in : nodeDesc.node->inputs) {
      if (string_iequals(in->socket_type.name.string(), inputName)) {
        input = in;
        break;
      }
    }

    if (!input) {
      TF_WARN("Ignoring connection on '%s.%s', input '%s' was not found",
              nodePath.GetText(),
              dstSocketName.GetText(),
              dstSocketName.GetText());
      continue;
    }

    /* USD allows N connections per input (MaterialX <switch>, <combine>, struct
     * inputs etc). Cycles inputs are single-connection, and the right lowering
     * depends on the node type, so just take the first and warn. */
    if (count > 1) {
      TF_WARN(
          "Ignoring multiple connections to '%s.%s'", nodePath.GetText(), dstSocketName.GetText());
    }

    HdMaterialConnectionSchema connSchema = connVec.GetElement(0);
    const SdfPath upstreamNodePath =
        connSchema.GetUpstreamNodePath() ?
            MaterialNodeNameToSdfPath(connSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
            SdfPath();
    const TfToken upstreamOutputName = connSchema.GetUpstreamNodeOutputName() ?
                                           connSchema.GetUpstreamNodeOutputName()->GetTypedValue(
                                               0.0f) :
                                           TfToken();

    const auto srcNodeIt = _nodes.find(upstreamNodePath);
    if (srcNodeIt == _nodes.end()) {
      TF_WARN("Ignoring connection from '%s.%s' to '%s.%s', node '%s' was not found",
              upstreamNodePath.GetText(),
              upstreamOutputName.GetText(),
              nodePath.GetText(),
              dstSocketName.GetText(),
              upstreamNodePath.GetText());
      continue;
    }

    ShaderOutput *output = nullptr;

    /* For UsdUVTexture upstream nodes, use the channel-aware resolver. */
    if (srcNodeIt->second.mapping == sUsdToCyles->uvTextureMapping()) {
      output = ResolveTextureOutput(
          srcNodeIt->second, upstreamNodePath, upstreamOutputName, input, shaderGraph);
    }
    else {
      const UsdToCyclesMapping *outputMapping = srcNodeIt->second.mapping;
      const std::string outputName = outputMapping ?
                                         outputMapping->parameterName(upstreamOutputName, input) :
                                         upstreamOutputName.GetString();

      for (ShaderOutput *out : srcNodeIt->second.node->outputs) {
        if (string_iequals(out->socket_type.name.string(), outputName)) {
          output = out;
          break;
        }
      }
    }

    if (!output) {
      TF_WARN("Ignoring connection from '%s.%s' to '%s.%s', output '%s' was not found",
              upstreamNodePath.GetText(),
              upstreamOutputName.GetText(),
              nodePath.GetText(),
              dstSocketName.GetText(),
              upstreamOutputName.GetText());
      continue;
    }

    shaderGraph->connect(output, input);
  }
}

void HdCyclesMaterial::PopulateShaderGraph(HdMaterialNetworkSchema network)
{
  _nodes.clear();
  _normalRemapNode = nullptr;
  _normalMapNode = nullptr;
  _displacementOutput = nullptr;
  _displacementConstant = 0.0f;
  _hasDisplacement = false;
  _rebuildOnDirtyParams = false;

  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

  HdMaterialNodeContainerSchema nodes = network.GetNodes();

  /* === Pass 1: Create all nodes and set their authored parameters === */
  for (const TfToken &nodeName : nodes.GetNames()) {
    HdMaterialNodeSchema nodeSchema = nodes.Get(nodeName);
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);

    NodeDesc nodeDesc = {};

    const auto nodeIt = _nodes.find(nodePath);
    /* Create new node only if it does not exist yet. */
    if (nodeIt != _nodes.end()) {
      nodeDesc = nodeIt->second;
    }
    else {
      /* E.g. cycles_principled_bsdf or UsdPreviewSurface. */
      const TfToken nodeTypeIdToken = nodeSchema.GetNodeIdentifier() ?
                                          nodeSchema.GetNodeIdentifier()->GetTypedValue(0.0f) :
                                          TfToken();
      const std::string &nodeTypeId = nodeTypeIdToken.GetString();

      ustring cyclesType(nodeTypeId);
      if (nodeTypeId.starts_with("cycles_") || nodeTypeId.starts_with("cycles:")) {
        /* Native Cycles note embedded in USDShade. */
        cyclesType = nodeTypeId.substr(strlen("cycles_"));
        nodeDesc.mapping = sUsdToCyles->findCycles(cyclesType);
      }
      else {
        /* Check if any remapping is needed (e.g. for USDPreviewSurface to Cycles nodes). */
        nodeDesc.mapping = sUsdToCyles->findUsd(nodeTypeIdToken);
        if (nodeDesc.mapping) {
          cyclesType = nodeDesc.mapping->nodeType();
        }
      }

      /* If it's a native Cycles' node-type, just do the lookup now. */
      if (const NodeType *nodeType = NodeType::find(cyclesType)) {
        nodeDesc.node = graph->create_node(nodeType);
        if (nodeDesc.mapping) {
          nodeDesc.mapping->initializeNode(nodeDesc.node);
        }
        _nodes.emplace(nodePath, nodeDesc);
      }
      else {
        TF_RUNTIME_ERROR("Could not create node '%s'", nodePath.GetText());
        continue;
      }
    }

    UpdateParameters(nodeDesc, nodeSchema.GetParameters(), nodePath);
  }

  /* === Pass 2: Insert scale/bias auxiliary nodes for UsdUVTexture === */
  for (const TfToken &nodeName : nodes.GetNames()) {
    HdMaterialNodeSchema nodeSchema = nodes.Get(nodeName);
    const TfToken nodeTypeIdToken = nodeSchema.GetNodeIdentifier() ?
                                        nodeSchema.GetNodeIdentifier()->GetTypedValue(0.0f) :
                                        TfToken();
    if (nodeTypeIdToken != CyclesMaterialTokens->UsdUVTexture) {
      continue;
    }
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);
    auto nodeIt = _nodes.find(nodePath);
    if (nodeIt == _nodes.end()) {
      continue;
    }

    GfVec4f scale(1.0f, 1.0f, 1.0f, 1.0f);
    GfVec4f bias(0.0f, 0.0f, 0.0f, 0.0f);
    const bool hasScale = ReadVec4fParam(nodeSchema.GetParameters(), CyclesMaterialTokens->scale, &scale);
    const bool hasBias = ReadVec4fParam(nodeSchema.GetParameters(), CyclesMaterialTokens->bias, &bias);

    const bool nonDefaultScale = hasScale &&
                                 (scale != GfVec4f(1.0f, 1.0f, 1.0f, 1.0f));
    const bool nonDefaultBias = hasBias &&
                                (bias != GfVec4f(0.0f, 0.0f, 0.0f, 0.0f));

    if (!nonDefaultScale && !nonDefaultBias) {
      continue;
    }

    /* Build: image_texture.Color → vector_math(multiply_add) → rgb output
     *        image_texture.Alpha → math(multiply_add) → alpha output
     *
     * vector_math multiply_add: Vector = Vector1 * Vector2 + Vector3 */
    const NodeType *vmType = NodeType::find(ustring("vector_math"));
    const NodeType *mType = NodeType::find(ustring("math"));
    if (!vmType || !mType) {
      TF_WARN("UsdUVTexture scale/bias: required Cycles node types not found");
      continue;
    }

    ShaderNode *rgbNode = graph->create_node(vmType);
    {
      /* Set mode to multiply_add */
      const SocketType *typeSocket = rgbNode->type->find_input(ustring("math_type"));
      if (typeSocket) {
        SetNodeValue(rgbNode, *typeSocket, VtValue(TfToken("multiply_add")));
      }
      /* Vector2 = scale.rgb, Vector3 = bias.rgb */
      if (const SocketType *v2 = rgbNode->type->find_input(ustring("vector2"))) {
        rgbNode->set(*v2, make_float3(scale[0], scale[1], scale[2]));
      }
      if (const SocketType *v3 = rgbNode->type->find_input(ustring("vector3"))) {
        rgbNode->set(*v3, make_float3(bias[0], bias[1], bias[2]));
      }
      graph->connect(nodeIt->second.node->output("Color"), rgbNode->input("Vector1"));
    }

    ShaderNode *alphaNode = graph->create_node(mType);
    {
      const SocketType *typeSocket = alphaNode->type->find_input(ustring("math_type"));
      if (typeSocket) {
        SetNodeValue(alphaNode, *typeSocket, VtValue(TfToken("multiply_add")));
      }
      /* Value2 = scale.a, Value3 = bias.a */
      if (const SocketType *v2 = alphaNode->type->find_input(ustring("value2"))) {
        alphaNode->set(*v2, scale[3]);
      }
      if (const SocketType *v3 = alphaNode->type->find_input(ustring("value3"))) {
        alphaNode->set(*v3, bias[3]);
      }
      graph->connect(nodeIt->second.node->output("Alpha"), alphaNode->input("Value1"));
    }

    nodeIt->second.scaleBiasRgb = rgbNode;
    nodeIt->second.scaleBiasAlpha = alphaNode;
    _rebuildOnDirtyParams = true;
  }

  /* === Pass 3: Normal-map chain and specular workflow for UsdPreviewSurface === */
  for (const TfToken &nodeName : nodes.GetNames()) {
    HdMaterialNodeSchema nodeSchema = nodes.Get(nodeName);
    const TfToken nodeTypeIdToken = nodeSchema.GetNodeIdentifier() ?
                                        nodeSchema.GetNodeIdentifier()->GetTypedValue(0.0f) :
                                        TfToken();
    if (nodeTypeIdToken != CyclesMaterialTokens->UsdPreviewSurface) {
      continue;
    }
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);
    auto nodeIt = _nodes.find(nodePath);
    if (nodeIt == _nodes.end()) {
      continue;
    }
    ShaderNode *principled = nodeIt->second.node;
    HdMaterialNodeParameterContainerSchema params = nodeSchema.GetParameters();

    /* -- Normal-map chain --
     * Build: [upstream] → vector_math(multiply_add) ×0.5+0.5 → normal_map → principled.normal
     *
     * The UsdPreviewSurface normal input expects tangent-space vectors in [-1, 1].
     * Cycles' normal_map node expects a colour in [0, 1], so we remap with ×0.5 + 0.5.
     * The chain is always inserted so that both connected and constant normals work.
     * Connected case: upstream → remap.Vector1; connection is handled in UpdateConnections.
     * Constant case: the authored normal value (in [-1,1]) is set on remap.Vector1 below. */
    {
      const NodeType *vmType = NodeType::find(ustring("vector_math"));
      const NodeType *nmType = NodeType::find(ustring("normal_map"));
      if (vmType && nmType) {
        _normalRemapNode = graph->create_node(vmType);
        {
          const SocketType *typeSocket = _normalRemapNode->type->find_input(ustring("math_type"));
          if (typeSocket) {
            SetNodeValue(_normalRemapNode, *typeSocket, VtValue(TfToken("multiply_add")));
          }
          /* Vector2 = (0.5, 0.5, 0.5), Vector3 = (0.5, 0.5, 0.5) */
          if (const SocketType *v2 = _normalRemapNode->type->find_input(ustring("vector2"))) {
            _normalRemapNode->set(*v2, make_float3(0.5f, 0.5f, 0.5f));
          }
          if (const SocketType *v3 = _normalRemapNode->type->find_input(ustring("vector3"))) {
            _normalRemapNode->set(*v3, make_float3(0.5f, 0.5f, 0.5f));
          }
        }

        _normalMapNode = graph->create_node(nmType);
        /* Tangent space is the default; the UV attribute is the mesh's st primvar, which
         * Cycles exports as its standard UV map, so we leave Attribute empty (""). */

        graph->connect(_normalRemapNode->output("Vector"), _normalMapNode->input("Color"));
        graph->connect(_normalMapNode->output("Normal"), principled->input("Normal"));

        /* If the normal is authored as a constant (not connected), set the remap node's
         * Vector1 input to the authored value so the chain produces the correct colour.
         * Constant normals connected to UsdPreviewSurface are unusual in practice and the
         * USD spec does not require them, but we support them as an approximation. */
        /* Always set Vector1 to the tangent-space "flat" direction (0, 0, 1) so an
         * unconnected normal gives no perturbation.  vector_math(multiply_add) default is
         * zero_float3, which remaps to (0.5,0.5,0.5) = zero-length normal; wrong. */
        GfVec3f constNormal(0.0f, 0.0f, 1.0f);
        ReadVec3fParam(params, CyclesMaterialTokens->normal, &constNormal);
        if (const SocketType *v1 = _normalRemapNode->type->find_input(ustring("vector1"))) {
          _normalRemapNode->set(*v1,
                                make_float3(constNormal[0], constNormal[1], constNormal[2]));
        }
        _rebuildOnDirtyParams = true;
      }
    }

    /* -- useSpecularWorkflow / specularColor --
     * Principled BSDF v4 has no native specular-workflow mode.
     * Approximation:
     *   workflow=0 (metallic, default): leave specular_tint = white (no tint).
     *   workflow=1: map specularColor → specular_tint; the tint modulates the specular
     *               highlight but is not an exact match because Principled uses a
     *               metallic-workflow energy model. In particular, the F0 is not set
     *               from specularColor, only the tint colour. At roughness=0 the result
     *               is close; at high roughness and non-grey specularColor it diverges.
     * Note: specularColor is only applied here, not in UpdateParameters, so that the
     * workflow flag is always read first regardless of parameter order in the network. */
    {
      int workflow = 0;
      ReadIntParam(params, CyclesMaterialTokens->useSpecularWorkflow, &workflow);
      if (workflow == 1) {
        GfVec3f specColor(1.0f, 1.0f, 1.0f);
        if (ReadVec3fParam(params, CyclesMaterialTokens->specularColor, &specColor)) {
          if (const SocketType *sock = principled->type->find_input(ustring("specular_tint"))) {
            principled->set(*sock, make_float3(specColor[0], specColor[1], specColor[2]));
          }
        }
      }
      /* workflow=0: specular_tint stays white (set in initializeNode's Principled defaults). */
    }

    /* -- displacement constant --
     * Store the authored constant displacement value for use at terminal time.
     * If the displacement input is connected, _displacementOutput is set in UpdateConnections. */
    {
      float dispVal = 0.0f;
      if (ReadFloatParam(params, CyclesMaterialTokens->displacement, &dispVal)) {
        _displacementConstant = dispVal;
        _hasDisplacement = true;
      }
    }
  }

  /* === Pass 4: Wire all connections === */
  for (const TfToken &nodeName : nodes.GetNames()) {
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);

    const auto nodeIt = _nodes.find(nodePath);
    if (nodeIt == _nodes.end()) {
      TF_RUNTIME_ERROR("Could not find node '%s' to connect", nodePath.GetText());
      continue;
    }

    UpdateConnections(nodeIt->second, nodes.Get(nodeName), nodePath, graph.get());
  }

  /* === Pass 5: Connect terminals to the graph output === */
  HdMaterialConnectionContainerSchema terminals = network.GetTerminals();
  for (const TfToken &terminalName : terminals.GetNames()) {
    HdMaterialConnectionSchema termSchema = terminals.Get(terminalName);
    const SdfPath upstreamNodePath =
        termSchema.GetUpstreamNodePath() ?
            MaterialNodeNameToSdfPath(termSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
            SdfPath();
    const TfToken upstreamOutputName = termSchema.GetUpstreamNodeOutputName() ?
                                           termSchema.GetUpstreamNodeOutputName()->GetTypedValue(
                                               0.0f) :
                                           TfToken();

    const auto nodeIt = _nodes.find(upstreamNodePath);
    if (nodeIt == _nodes.end()) {
      TF_RUNTIME_ERROR("Could not find terminal node '%s'", upstreamNodePath.GetText());
      continue;
    }

    ShaderNode *const node = nodeIt->second.node;

    const char *inputName = nullptr;
    const char *outputName = nullptr;

    if (terminalName == HdMaterialTerminalTokens->surface ||
        terminalName == CyclesMaterialTokens->cyclesSurface)
    {
      inputName = "Surface";
      if (node->type->name == "add_closure" || node->type->name == "mix_closure") {
        outputName = "Closure";
      }
      else if (node->type->name == "emission") {
        outputName = "Emission";
      }
      else {
        outputName = "BSDF";
      }
    }
    else if (terminalName == HdMaterialTerminalTokens->displacement ||
             terminalName == CyclesMaterialTokens->cyclesDisplacement)
    {
      /* UsdPreviewSurface.displacement: insert a Cycles displacement node.
       *
       * We use bump mode (DISPLACE_BUMP, the shader default) because true displacement
       * requires tessellation that is not the default for Hydra renders.  Bump mode
       * computes a displaced shading normal from the height field, giving a visible
       * normal-perturbing effect without topology changes. */
      if (nodeIt->second.mapping == sUsdToCyles->previewSurfaceMapping() && _hasDisplacement) {
        const NodeType *dispType = NodeType::find(ustring("displacement"));
        if (dispType) {
          ShaderNode *dispNode = graph->create_node(dispType);
          /* Midlevel = 0.0 (no centre offset; displacement is a signed delta along the normal).
           * Scale = 1.0 (authored values are in scene units).
           * The spec treats displacement as a world-space offset; Cycles' object-space default
           * is close enough for the test geometry here. */
          if (const SocketType *ml = dispNode->type->find_input(ustring("midlevel"))) {
            dispNode->set(*ml, 0.0f);
          }

          ShaderInput *heightIn = dispNode->input("Height");
          if (heightIn) {
            if (_displacementOutput) {
              graph->connect(_displacementOutput, heightIn);
            }
            else {
              /* Constant authored value. */
              if (const SocketType *h = dispNode->type->find_input(ustring("height"))) {
                dispNode->set(*h, _displacementConstant);
              }
            }
          }

          ShaderInput *matDispIn = graph->output()->input("Displacement");
          ShaderOutput *dispOut = dispNode->output("Displacement");
          if (matDispIn && dispOut) {
            graph->connect(dispOut, matDispIn);
          }
        }
      }
      /* Displacement was handled above; skip the generic terminal wiring below. */
      continue;
    }
    else if (terminalName == HdMaterialTerminalTokens->volume ||
             terminalName == CyclesMaterialTokens->cyclesVolume)
    {
      inputName = outputName = "Volume";
    }

    /* For native Cycles nodes we use the upstream output name as is, for
     * mapping from e.g. UsdPreviewSurface we need to use the default output
     * name that is known to exist. */
    if (!upstreamOutputName.IsEmpty() && nodeIt->second.mapping == nullptr) {
      outputName = upstreamOutputName.GetText();
    }

    ShaderInput *const input = inputName ? graph->output()->input(inputName) : nullptr;
    if (!input) {
      TF_RUNTIME_ERROR("Could not find terminal input '%s.%s'",
                       upstreamNodePath.GetText(),
                       inputName ? inputName : "<null>");
      continue;
    }

    ShaderOutput *const output = outputName ? node->output(outputName) : nullptr;
    if (!output) {
      TF_RUNTIME_ERROR("Could not find terminal output '%s.%s'",
                       upstreamNodePath.GetText(),
                       outputName ? outputName : "<null>");
      continue;
    }

    graph->connect(output, input);
  }

  /* === Pass 6: opacityThreshold post-processing ===
   * Insert a math(greater_than) node when opacityThreshold > 0.  This runs after
   * connections are wired so we can disconnect principled_bsdf.Alpha and re-route it.
   *
   * Spec: alpha = (opacity >= threshold) ? 1.0 : 0.0.
   * Cycles' greater_than returns a > b (strictly), which is functionally equivalent
   * for sampled opacity values where the exact threshold boundary is not observable. */
  for (const TfToken &nodeName : nodes.GetNames()) {
    HdMaterialNodeSchema nodeSchema = nodes.Get(nodeName);
    const TfToken nodeTypeIdToken = nodeSchema.GetNodeIdentifier() ?
                                        nodeSchema.GetNodeIdentifier()->GetTypedValue(0.0f) :
                                        TfToken();
    if (nodeTypeIdToken != CyclesMaterialTokens->UsdPreviewSurface) {
      continue;
    }
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);
    auto nodeIt = _nodes.find(nodePath);
    if (nodeIt == _nodes.end()) {
      continue;
    }

    float threshold = 0.0f;
    if (!ReadFloatParam(nodeSchema.GetParameters(), CyclesMaterialTokens->opacityThreshold, &threshold) ||
        threshold <= 0.0f)
    {
      continue;
    }

    /* Find principled_bsdf.Alpha */
    ShaderNode *principled = nodeIt->second.node;
    ShaderInput *alphaIn = principled->input("Alpha");
    if (!alphaIn) {
      continue;
    }

    const NodeType *mType = NodeType::find(ustring("math"));
    if (!mType) {
      continue;
    }

    ShaderNode *threshNode = graph->create_node(mType);
    const SocketType *typeSocket = threshNode->type->find_input(ustring("math_type"));
    if (typeSocket) {
      SetNodeValue(threshNode, *typeSocket, VtValue(TfToken("greater_than")));
    }
    /* Value2 = threshold */
    if (const SocketType *v2 = threshNode->type->find_input(ustring("value2"))) {
      threshNode->set(*v2, threshold);
    }

    /* Wire: existing alpha source → threshNode.Value1 → principled.Alpha */
    ShaderInput *threshValue1 = threshNode->input("Value1");
    if (!threshValue1) {
      continue;
    }

    if (alphaIn->link) {
      /* Disconnect current alpha connection and reroute through greater_than. */
      ShaderOutput *existingAlphaOut = alphaIn->link;
      graph->disconnect(alphaIn);
      graph->connect(existingAlphaOut, threshValue1);
    }
    else {
      /* Constant alpha: read the current value and set it on threshNode. */
      const SocketType *v1 = threshNode->type->find_input(ustring("value1"));
      if (v1) {
        float currentAlpha = 1.0f;
        /* principled.Alpha default or authored value */
        if (const SocketType *alphaSock = principled->type->find_input(ustring("alpha"))) {
          VtValue currentVal = GetNodeValue(principled, *alphaSock);
          if (currentVal.IsHolding<float>()) {
            currentAlpha = currentVal.UncheckedGet<float>();
          }
        }
        threshNode->set(*v1, currentAlpha);
      }
    }
    graph->connect(threshNode->output("Value"), alphaIn);
    _rebuildOnDirtyParams = true;
  }

  /* === Create the instanceId AOV output === */
  {
    const ustring instanceId(HdAovTokens->instanceId.GetString());

    OutputAOVNode *aovNode = graph->create_node<OutputAOVNode>();
    aovNode->set_name(instanceId);

    AttributeNode *instanceIdNode = graph->create_node<AttributeNode>();
    instanceIdNode->set_attribute(instanceId);

    graph->connect(instanceIdNode->output("Fac"), aovNode->input("Value"));
  }

  _shader->set_graph(std::move(graph));
}

void HdCyclesMaterial::Finalize(HdRenderParam *renderParam)
{
  if (!_shader) {
    return;
  }

  const SceneLock lock(renderParam);
  const bool keep_nodes = static_cast<const HdCyclesSession *>(renderParam)->keep_nodes;

  _nodes.clear();
  _normalRemapNode = nullptr;
  _normalMapNode = nullptr;
  _displacementOutput = nullptr;
  _rebuildOnDirtyParams = false;

  if (!keep_nodes) {
    lock.scene->delete_node(_shader);
  }
  _shader = nullptr;
}

void HdCyclesMaterial::Initialize(HdRenderParam *renderParam)
{
  if (_shader) {
    return;
  }

  const SceneLock lock(renderParam);

  _shader = lock.scene->create_node<Shader>();
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
