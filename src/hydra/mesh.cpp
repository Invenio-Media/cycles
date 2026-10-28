/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/mesh.h"
#include "hydra/diagnostic.h"
#include "hydra/geometry.inl"
#include "hydra/object_settings.h"
#include "hydra/util.h"
#include "scene/camera.h"
#include "scene/mesh.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/legacyDisplayStyleSchema.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/subdivisionTagsSchema.h>

#include <algorithm>

HDCYCLES_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    ((maxLevel, "cycles:object:subdivision:max_level"))
    ((dicingRate, "cycles:object:subdivision:dicing_rate"))
    ((adaptiveSpace, "cycles:object:subdivision:adaptive_space"))
);
// clang-format on

namespace {

template<typename T>
VtValue ComputeTriangulatedUniformPrimvar(VtValue value, const VtIntArray &primitiveParams)
{
  T output;
  output.reserve(primitiveParams.size());
  const T &input = value.Get<T>();

  for (size_t i = 0; i < primitiveParams.size(); ++i) {
    const int faceIndex = HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(primitiveParams[i]);

    output.push_back(input[faceIndex]);
  }

  return VtValue(output);
}

VtValue ComputeTriangulatedUniformPrimvar(VtValue value,
                                          const HdType valueType,
                                          const VtIntArray &primitiveParams)
{
  switch (valueType) {
    case HdTypeFloat:
      return ComputeTriangulatedUniformPrimvar<VtFloatArray>(value, primitiveParams);
    case HdTypeFloatVec2:
      return ComputeTriangulatedUniformPrimvar<VtVec2fArray>(value, primitiveParams);
    case HdTypeFloatVec3:
      return ComputeTriangulatedUniformPrimvar<VtVec3fArray>(value, primitiveParams);
    case HdTypeFloatVec4:
      return ComputeTriangulatedUniformPrimvar<VtVec4fArray>(value, primitiveParams);
    default:
      HDCYCLES_ERROR("Unsupported attribute type %d", static_cast<int>(valueType));
      return VtValue();
  }
}

VtValue ComputeTriangulatedFaceVaryingPrimvar(VtValue value,
                                              const HdType valueType,
                                              HdMeshUtil &meshUtil)
{
  if (meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
          HdGetValueData(value), value.GetArraySize(), valueType, &value)
#if PXR_VERSION >= 2511
      != HdMeshComputationResult::Error
#endif
  )
  {
    return value;
  }

  return VtValue();
}

}  // namespace

Transform convert_transform(const GfMatrix4d &matrix)
{
  return make_transform(matrix[0][0],
                        matrix[1][0],
                        matrix[2][0],
                        matrix[3][0],
                        matrix[0][1],
                        matrix[1][1],
                        matrix[2][1],
                        matrix[3][1],
                        matrix[0][2],
                        matrix[1][2],
                        matrix[2][2],
                        matrix[3][2]);
}

HdCyclesMesh::HdCyclesMesh(const SdfPath &rprimId)
    : HdCyclesGeometry(rprimId), _util(&_topology, rprimId)
{
}

HdCyclesMesh::~HdCyclesMesh() = default;

void HdCyclesMesh::Sync(HdSceneDelegate *sceneDelegate,
                        HdRenderParam *renderParam,
                        HdDirtyBits *dirtyBits,
                        const TfToken &reprToken)
{
  HdCyclesGeometry::Sync(sceneDelegate, renderParam, dirtyBits, reprToken);
  const bool pixelDiced = _geom && _geom->get_subdivision_type() != Mesh::SUBDIVISION_NONE &&
                          !_subdObjectSpace;
  if (pixelDiced != _pixelDiced) {
    static_cast<HdCyclesSession *>(renderParam)->SetPixelDiced(this, pixelDiced);
    _pixelDiced = pixelDiced;
  }
}

/* A factor of 1.5 keeps edges within 1.5 times the dicing rate, or two thirds of it. */
static constexpr float kRediceFactor = 1.5f;

void HdCyclesMesh::UpdateDicing(Scene *scene)
{
  const Camera *camera = scene->dicing_camera;
  const Transform &tfm = _instances[0]->get_tfm();
  const float3 center = transform_point(&tfm, _boundsCenter);
  const float scale = (len(transform_direction(&tfm, make_float3(1.0f, 0.0f, 0.0f))) +
                       len(transform_direction(&tfm, make_float3(0.0f, 1.0f, 0.0f))) +
                       len(transform_direction(&tfm, make_float3(0.0f, 0.0f, 1.0f)))) /
                      3.0f;
  const float radius = _boundsRadius * scale;
  /* The mesh's size in pixels: through the lens and onto the film, so a zoom or a new
   * resolution counts as a move does. An orthographic camera sees the same size at any
   * distance; its view plane is in world units. A camera inside the bounds sees the mesh at
   * its radius, so moving about inside it doesn't re-dice. */
  const float height = float(camera->get_full_height());
  float size;
  if (camera->get_camera_type() == CAMERA_ORTHOGRAPHIC) {
    size = radius * height / std::max(camera->viewplane.top - camera->viewplane.bottom, 1e-6f);
  }
  else {
    const float depth = std::max(len(center - transform_get_column(&camera->get_matrix(), 3)),
                                 radius);
    size = radius * height / std::max(depth * 2.0f * std::tan(0.5f * camera->get_fov()), 1e-6f);
  }

  if (!_geom->need_tesselation()) {
    if (_dicedSize > 0.0f && size < _dicedSize * kRediceFactor &&
        size * kRediceFactor > _dicedSize)
    {
      return;
    }
    /* A moved camera leaves the transform as it was, so dicing has to be asked for. */
    _geom->tag_subd_dicing_rate_modified();
    _geom->tag_update(scene, true);
  }
  _geom->set_subd_objecttoworld(tfm);
  _dicedSize = size;
  ResetTessellation();
}

HdDirtyBits HdCyclesMesh::GetInitialDirtyBitsMask() const
{
  HdDirtyBits bits = HdCyclesGeometry::GetInitialDirtyBitsMask();
  bits |= HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyNormals |
          HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyTopology |
          HdChangeTracker::DirtyDisplayStyle | HdChangeTracker::DirtySubdivTags;
  return bits;
}

HdDirtyBits HdCyclesMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
  if (bits & (HdChangeTracker::DirtyMaterialId)) {
    // Update used shaders from geometry subsets if any exist in the topology
    bits |= HdChangeTracker::DirtyTopology;
  }

  if (bits & (HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyDisplayStyle |
              HdChangeTracker::DirtySubdivTags))
  {
    // Do full topology update when display style or subdivision changes
    bits |= HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyDisplayStyle |
            HdChangeTracker::DirtySubdivTags;
  }

  if (bits & (HdChangeTracker::DirtyTopology)) {
    // Changing topology clears the geometry, so need to populate everything again
    bits |= HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyNormals |
            HdChangeTracker::DirtyPrimvar;
  }

  return bits;
}

void HdCyclesMesh::Populate(HdSceneDelegate *sceneDelegate, HdDirtyBits dirtyBits, bool &rebuild)
{
  /* The subdivision settings. A new max_level can switch subdivision on or off, which takes a
   * new topology, and with it everything populated from it. */
  const bool settingsDirty = HdChangeTracker::IsTopologyDirty(dirtyBits, GetId()) ||
                             (dirtyBits & HdChangeTracker::DirtyPrimvar);
  if (settingsDirty && ReadSubdivisionSettings(sceneDelegate)) {
    dirtyBits |= HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyPoints |
                 HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyPrimvar;
  }

  if (HdChangeTracker::IsTopologyDirty(dirtyBits, GetId())) {
    PopulateTopology(sceneDelegate);
  }

  if (dirtyBits & HdChangeTracker::DirtyPoints) {
    PopulatePoints(sceneDelegate);
  }

  // Must happen after topology update, so that normals attribute size can be calculated
  if (dirtyBits & HdChangeTracker::DirtyNormals) {
    PopulateNormals(sceneDelegate);
  }

  // Must happen after topology update, so that appropriate attribute set can be selected
  if (dirtyBits & HdChangeTracker::DirtyPrimvar) {
    PopulatePrimvars(sceneDelegate);
  }

  if (settingsDirty) {
    ApplySubdivisionSettings();
  }
  ResetTessellation();

  /* Cycles dices only subdivision meshes; a polygon mesh's true displacement moves its
   * vertices and nothing between them, where Karma dices it. Said once, with the remedy. */
  if (!_warnedUndiced && _geom->get_subdivision_type() == Mesh::SUBDIVISION_NONE) {
    for (const Node *node : _geom->get_used_shaders()) {
      const Shader *const shader = static_cast<const Shader *>(node);
      if (shader->has_displacement && shader->get_displacement_method() != DISPLACE_BUMP) {
        HDCYCLES_WARN(
            "%s: its material's true displacement moves only the mesh's vertices, since "
            "Cycles dices only subdivision surfaces; set its subdivisionScheme to bilinear "
            "or catmullClark to displace between them",
            GetId().GetText());
        _warnedUndiced = true;
        break;
      }
    }
  }

  /* A re-tessellated subdivision mesh can have a different number of triangles, so its BVH is
   * rebuilt, not refit: new dicing settings or new points re-tessellate it. A move only
   * re-tessellates it when it changes the mesh's size on screen enough (UpdateDicing). */
  rebuild = (_geom->triangles_is_modified()) || (_geom->subd_start_corner_is_modified()) ||
            (_geom->subd_num_corners_is_modified()) || (_geom->subd_shader_is_modified()) ||
            (_geom->subd_smooth_is_modified()) || (_geom->subd_ptex_offset_is_modified()) ||
            (_geom->subd_face_corners_is_modified()) || _geom->need_tesselation();
}

void HdCyclesMesh::PopulatePoints(HdSceneDelegate *sceneDelegate)
{
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  /* Across the shutter when the points move; the middle sample is the position. */
  const std::vector<VtVec3fArray> samples = SamplePoints(primvars);
  if (samples.empty()) {
    HDCYCLES_WARN("Invalid points data for %s", GetId().GetText());
    return;
  }

  const VtVec3fArray &points = samples[samples.size() / 2];
  const size_t numPoints = size_t(_topology.GetNumPoints());

  TF_VERIFY(points.size() >= numPoints);

  const bool subdivision = _geom->get_subdivision_type() != Mesh::SUBDIVISION_NONE;
  AttributeSet &attributes = (subdivision) ? _geom->subd_attributes : _geom->attributes;
  Attribute *attr_P = attributes.add(ATTR_STD_POSITION);
  packed_float3 *verts = attr_P->data_for_write<packed_float3>();

  std::copy_n(reinterpret_cast<const packed_float3 *>(points.data()), numPoints, verts);
  ApplyPointsMotion(attr_P, samples, numPoints);
  _geom->tag_position_modified();

  /* The rest points' bounds, for the size the dicing camera sees (UpdateDicing): enough for
   * a heuristic, though displacement and deformation can reach past them. */
  if (subdivision) {
    BoundBox bounds = BoundBox::empty;
    for (size_t i = 0; i < numPoints; ++i) {
      bounds.grow(make_float3(points[i][0], points[i][1], points[i][2]));
    }
    _boundsCenter = bounds.valid() ? bounds.center() : zero_float3();
    _boundsRadius = bounds.valid() ? 0.5f * len(bounds.size()) : 0.0f;
  }
}

void HdCyclesMesh::PopulateNormals(HdSceneDelegate *sceneDelegate)
{
  _geom->attributes.remove(ATTR_STD_VERTEX_NORMAL);

  // Authored normals should only exist on triangle meshes
  if (_geom->get_subdivision_type() != Mesh::SUBDIVISION_NONE) {
    return;
  }

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const HdInterpolation interpolation = ReadPrimvarInterpolation(primvars, HdTokens->normals);

  // Cycles computes face normals in the kernel for faces that are not smooth. Per-face
  // normals render flat that way, and so does a bilinear mesh without normals, whose limit
  // surface is flat across each face (the implicit cube is one). Other authored normals are
  // interpolated; without any, Cycles averages vertex normals.
  const bool smooth = !_flatShading && interpolation != HdInterpolationUniform &&
                      (interpolation != HdInterpolationCount ||
                       _topology.GetScheme() != PxOsdOpenSubdivTokens->bilinear);
  std::ranges::fill(_geom->get_smooth(), smooth);
  _geom->tag_smooth_modified();

  if (interpolation == HdInterpolationCount) {
    return;  // Ignore missing normals
  }
  const VtValue value = ReadPrimvar(primvars, HdTokens->normals);

  if (!value.IsHolding<VtVec3fArray>()) {
    HDCYCLES_WARN("Invalid normals data for %s", GetId().GetText());
    return;
  }

  const auto &normals = value.UncheckedGet<VtVec3fArray>();

  if (interpolation == HdInterpolationConstant) {
    TF_VERIFY(normals.size() == 1);

    const GfVec3f constantNormal = normals[0];

    packed_normal *const N =
        _geom->attributes.add(ATTR_STD_VERTEX_NORMAL)->data_for_write<packed_normal>();
    for (size_t i = 0; i < _geom->num_verts(); ++i) {
      N[i] = packed_normal(make_float3(constantNormal[0], constantNormal[1], constantNormal[2]));
    }
  }
  else if (interpolation == HdInterpolationUniform) {
    TF_VERIFY(normals.size() == static_cast<size_t>(_topology.GetNumFaces()));
    /* Nothing to do, face normals are computed on demand in the kernel. */
  }
  else if (interpolation == HdInterpolationVertex || interpolation == HdInterpolationVarying) {
    TF_VERIFY(normals.size() == static_cast<size_t>(_topology.GetNumPoints()) &&
              static_cast<size_t>(_topology.GetNumPoints()) == _geom->num_verts());

    packed_normal *const N =
        _geom->attributes.add(ATTR_STD_VERTEX_NORMAL)->data_for_write<packed_normal>();
    for (size_t i = 0; i < _geom->num_verts(); ++i) {
      N[i] = packed_normal(make_float3(normals[i][0], normals[i][1], normals[i][2]));
    }
  }
  else if (interpolation == HdInterpolationFaceVarying) {
    TF_VERIFY(normals.size() == static_cast<size_t>(_topology.GetNumFaceVaryings()));

    // TODO: Cycles has no per-corner normals, so ignore until supported.
#if 0
    if (!_util.ComputeTriangulatedFaceVaryingPrimvar(
            normals.data(), normals.size(), HdTypeFloatVec3, &value))
    {
      return;
    }

    const auto &normalsTriangulated = value.UncheckedGet<VtVec3fArray>();
#endif
  }
}

void HdCyclesMesh::PopulatePrimvars(HdSceneDelegate *sceneDelegate)
{
  Scene *const scene = (Scene *)_geom->get_owner();

  const bool subdivision = _geom->get_subdivision_type() != Mesh::SUBDIVISION_NONE;
  AttributeSet &attributes = subdivision ? _geom->subd_attributes : _geom->attributes;

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);

  const std::pair<HdInterpolation, AttributeElement> interpolations[] = {
      std::make_pair(HdInterpolationFaceVarying, ATTR_ELEMENT_CORNER),
      std::make_pair(HdInterpolationUniform, ATTR_ELEMENT_FACE),
      std::make_pair(HdInterpolationVertex, ATTR_ELEMENT_VERTEX),
      std::make_pair(HdInterpolationVarying, ATTR_ELEMENT_VERTEX),
      std::make_pair(HdInterpolationConstant, ATTR_ELEMENT_OBJECT),
  };

  for (const auto &interpolation : interpolations) {
    for (const TfToken &primvarName : PrimvarNamesAtInterpolation(primvars, interpolation.first)) {
      // Skip special primvars that are handled separately
      if (primvarName == HdTokens->points || primvarName == HdTokens->normals) {
        continue;
      }

      VtValue value = ReadPrimvar(primvars, primvarName);
      if (value.IsEmpty()) {
        continue;
      }

      const TfToken role = ReadPrimvarRole(primvars, primvarName);
      const ustring name(primvarName.GetString());

      AttributeStandard std = ATTR_STD_NONE;
      if (role == HdPrimvarRoleTokens->textureCoordinate) {
        std = ATTR_STD_UV;
      }
      else if (interpolation.first == HdInterpolationVertex) {
        if (primvarName == HdTokens->displayColor || role == HdPrimvarRoleTokens->color) {
          std = ATTR_STD_VERTEX_COLOR;
        }
        else if (primvarName == HdTokens->normals) {
          std = ATTR_STD_VERTEX_NORMAL;
        }
      }
      else if (primvarName == HdTokens->displayColor &&
               interpolation.first == HdInterpolationConstant)
      {
        if (value.IsHolding<VtVec3fArray>() && value.GetArraySize() == 1) {
          SetDisplayColor(value.UncheckedGet<VtVec3fArray>()[0]);
        }
      }

      // Skip attributes that are not needed. Texture coordinates are always kept: the UV
      // pass needs them, and passes are created after rprims sync.
      if (std == ATTR_STD_UV || (std != ATTR_STD_NONE && _geom->need_attribute(scene, std)) ||
          _geom->need_attribute(scene, name))
      {
        const HdType valueType = HdGetValueTupleType(value).type;

        if (!subdivision) {
          // Adjust attributes for polygons that were triangulated
          if (interpolation.first == HdInterpolationUniform) {
            value = ComputeTriangulatedUniformPrimvar(value, valueType, _primitiveParams);
            if (value.IsEmpty()) {
              continue;
            }
          }
          else if (interpolation.first == HdInterpolationFaceVarying) {
            value = ComputeTriangulatedFaceVaryingPrimvar(value, valueType, _util);
            if (value.IsEmpty()) {
              continue;
            }
          }
        }

        Attribute *const attr = ApplyPrimvars(attributes, name, value, interpolation.second, std);
        /* USD interpolates a vertex primvar by the subdivision scheme, as the points are, as
         * Karma does, and a varying one linearly (Karma smooths that too); Cycles' default for
         * both is linear. */
        if (attr && subdivision && interpolation.first == HdInterpolationVertex) {
          attr->flags |= ATTR_SUBDIVIDE_SMOOTH_VERTEX;
        }
      }
    }
  }
}

void HdCyclesMesh::PopulateTopology(HdSceneDelegate *sceneDelegate)
{
  // Clear geometry before populating it again with updated topology
  _geom->clear(true);

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdLegacyDisplayStyleSchema displayStyleSchema = HdLegacyDisplayStyleSchema::GetFromParent(
      prim.dataSource);

  /* Subdivision levels: primvars:cycles:object:subdivision:max_level, or 8. Hydra's display
   * refine level is a rasteriser's hint that hosts set unlike each other: husk passes 8,
   * Houdini's viewport 0, which rendered subdivision surfaces as their cages there. Karma
   * subdivides the same in both, and so does this. */
  constexpr int kDefaultMaxLevel = 8;
  const int refineLevel = _subdMaxLevel >= 0 ? _subdMaxLevel : kDefaultMaxLevel;
  bool flatShadingEnabled = false;
  if (auto ds = displayStyleSchema.GetFlatShadingEnabled()) {
    flatShadingEnabled = ds->GetTypedValue(0.0f);
  }
  _flatShading = flatShadingEnabled;

  const HdMeshSchema meshSchema = HdMeshSchema::GetFromParent(prim.dataSource);
  const HdMeshTopologySchema topoSchema = meshSchema.GetTopology();

  TfToken scheme = PxOsdOpenSubdivTokens->none;
  if (auto ds = meshSchema.GetSubdivisionScheme()) {
    scheme = ds->GetTypedValue(0.0f);
  }
  TfToken orientation = HdTokens->rightHanded;
  if (auto ds = topoSchema.GetOrientation()) {
    orientation = ds->GetTypedValue(0.0f);
  }
  VtIntArray faceVertexCounts;
  if (auto ds = topoSchema.GetFaceVertexCounts()) {
    faceVertexCounts = ds->GetTypedValue(0.0f);
  }
  VtIntArray faceVertexIndices;
  if (auto ds = topoSchema.GetFaceVertexIndices()) {
    faceVertexIndices = ds->GetTypedValue(0.0f);
  }
  VtIntArray holeIndices;
  if (auto ds = topoSchema.GetHoleIndices()) {
    holeIndices = ds->GetTypedValue(0.0f);
  }

  _topology = HdMeshTopology(
      scheme, orientation, faceVertexCounts, faceVertexIndices, holeIndices, refineLevel);

  /* Geom subsets are published as child prims of the mesh in the scene index. */
  HdGeomSubsets geomSubsetsList;
  if (HdSceneIndexBaseRefPtr si = sceneDelegate->GetRenderIndex().GetTerminalSceneIndex()) {
    for (const SdfPath &childPath : si->GetChildPrimPaths(GetId())) {
      const HdSceneIndexPrim childPrim = si->GetPrim(childPath);
      if (childPrim.primType != HdPrimTypeTokens->geomSubset) {
        continue;
      }
      const HdGeomSubsetSchema subsetSchema = HdGeomSubsetSchema::GetFromParent(
          childPrim.dataSource);
      /* Only face subsets supported here, not point or curve subsets. */
      if (auto typeDs = subsetSchema.GetType()) {
        if (typeDs->GetTypedValue(0.0f) != HdGeomSubsetSchemaTokens->typeFaceSet) {
          continue;
        }
      }
      HdGeomSubset subset;
      subset.type = HdGeomSubset::TypeFaceSet;
      if (auto ds = subsetSchema.GetIndices()) {
        subset.indices = ds->GetTypedValue(0.0f);
      }
      if (auto pathDs = HdMaterialBindingsSchema::GetFromParent(childPrim.dataSource)
                            .GetMaterialBinding()
                            .GetPath())
      {
        subset.materialId = pathDs->GetTypedValue(0.0f);
      }
      geomSubsetsList.push_back(subset);
    }
  }
  _topology.SetGeomSubsets(geomSubsetsList);

  const TfToken subdivScheme = _topology.GetScheme();
  if (subdivScheme == PxOsdOpenSubdivTokens->bilinear && _topology.GetRefineLevel() > 0) {
    _geom->set_subdivision_type(Mesh::SUBDIVISION_LINEAR);
  }
  else if (subdivScheme == PxOsdOpenSubdivTokens->catmullClark && _topology.GetRefineLevel() > 0) {
    _geom->set_subdivision_type(Mesh::SUBDIVISION_CATMULL_CLARK);
  }
  else {
    _geom->set_subdivision_type(Mesh::SUBDIVISION_NONE);
  }

  // A bilinear limit surface is flat across each face; other schemes shade smooth.
  const bool smooth = !flatShadingEnabled && subdivScheme != PxOsdOpenSubdivTokens->bilinear;
  const bool subdivision = _geom->get_subdivision_type() != Mesh::SUBDIVISION_NONE;

  // Initialize lookup table from polygon face to material shader index
  VtIntArray faceShaders(_topology.GetNumFaces(), 0);

  const HdGeomSubsets &geomSubsets = _topology.GetGeomSubsets();
  if (!geomSubsets.empty()) {
    array<Node *> usedShaders = std::move(_geom->get_used_shaders());
    // Remove any previous materials except for the material assigned to the prim
    usedShaders.resize(1);

    std::unordered_map<SdfPath, int, SdfPath::Hash> materials;

    for (const HdGeomSubset &geomSubset : geomSubsets) {
      TF_VERIFY(geomSubset.type == HdGeomSubset::TypeFaceSet);

      int shader = 0;
      const auto it = materials.find(geomSubset.materialId);
      if (it != materials.end()) {
        shader = it->second;
      }
      else {
        const auto *const material = static_cast<const HdCyclesMaterial *>(
            sceneDelegate->GetRenderIndex().GetSprim(HdPrimTypeTokens->material,
                                                     geomSubset.materialId));

        if (material && material->GetCyclesShader()) {
          shader = static_cast<int>(usedShaders.size());
          usedShaders.push_back_slow(material->GetCyclesShader());

          materials.emplace(geomSubset.materialId, shader);
        }
      }

      for (const int face : geomSubset.indices) {
        faceShaders[face] = shader;
      }
    }

    _geom->set_used_shaders(usedShaders);
  }

  const VtIntArray vertIndx = _topology.GetFaceVertexIndices();
  const VtIntArray vertCounts = _topology.GetFaceVertexCounts();

  if (!subdivision) {
    VtVec3iArray triangles;
    _util.ComputeTriangleIndices(&triangles, &_primitiveParams);

    _geom->resize_mesh(_topology.GetNumPoints(), triangles.size());

    int *geom_indices = _geom->get_triangles().data();
    for (size_t i = 0; i < _primitiveParams.size(); ++i) {
      const GfVec3i triangle = triangles[i];
      geom_indices[i * 3 + 0] = triangle[0];
      geom_indices[i * 3 + 1] = triangle[1];
      geom_indices[i * 3 + 2] = triangle[2];
    }

    int *shader = _geom->get_shader().data();
    for (size_t i = 0; i < _primitiveParams.size(); ++i) {
      const int faceIndex = HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(_primitiveParams[i]);
      shader[i] = faceShaders[faceIndex];
    }

    _geom->tag_triangles_modified();
    _geom->tag_shader_modified();
  }
  else {
    const HdSubdivisionTagsSchema subdivSchema = HdSubdivisionTagsSchema::GetFromParent(
        prim.dataSource);
    PxOsdSubdivTags subdivTags;
    if (auto ds = subdivSchema.GetInterpolateBoundary()) {
      subdivTags.SetVertexInterpolationRule(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetFaceVaryingLinearInterpolation()) {
      subdivTags.SetFaceVaryingInterpolationRule(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetTriangleSubdivisionRule()) {
      subdivTags.SetTriangleSubdivision(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetCornerIndices()) {
      subdivTags.SetCornerIndices(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetCornerSharpnesses()) {
      subdivTags.SetCornerWeights(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetCreaseIndices()) {
      subdivTags.SetCreaseIndices(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetCreaseLengths()) {
      subdivTags.SetCreaseLengths(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetCreaseSharpnesses()) {
      subdivTags.SetCreaseWeights(ds->GetTypedValue(0.0f));
    }
    _topology.SetSubdivTags(subdivTags);

    size_t numCorners = 0;
    for (const int vertCount : vertCounts) {
      numCorners += vertCount;
    }

    _geom->resize_subd_faces(_topology.GetNumFaces(), numCorners);
    _geom->resize_mesh(_topology.GetNumPoints(), 0);
    Attribute *subd_attr_P = _geom->subd_attributes.add(ATTR_STD_POSITION);
    subd_attr_P->resize(_topology.GetNumPoints());

    std::copy_n(vertIndx.data(), vertIndx.size(), _geom->get_subd_face_corners().data());

    int *subd_start_corner = _geom->get_subd_start_corner().data();
    int *subd_num_corners = _geom->get_subd_num_corners().data();
    int *subd_ptex_offset = _geom->get_subd_ptex_offset().data();

    // TODO: Handle hole indices
    int ptex_offset = 0;
    size_t faceIndex = 0;
    size_t indexOffset = 0;
    for (const int vertCount : vertCounts) {
      subd_start_corner[faceIndex] = indexOffset;
      subd_num_corners[faceIndex] = vertCount;
      subd_ptex_offset[faceIndex] = ptex_offset;
      const int num_ptex = (vertCount == 4) ? 1 : vertCount;
      ptex_offset += num_ptex;

      faceIndex++;
      indexOffset += vertCount;
    }

    std::copy_n(faceShaders.data(), faceShaders.size(), _geom->get_subd_shader().data());
    std::ranges::fill(_geom->get_subd_smooth(), smooth);

    _geom->tag_subd_face_corners_modified();
    _geom->tag_subd_start_corner_modified();
    _geom->tag_subd_num_corners_modified();
    _geom->tag_subd_shader_modified();
    _geom->tag_subd_smooth_modified();
    _geom->tag_subd_ptex_offset_modified();

    const VtIntArray creaseLengths = subdivTags.GetCreaseLengths();
    if (!creaseLengths.empty()) {
      size_t numCreases = 0;
      for (const int creaseLength : creaseLengths) {
        numCreases += creaseLength - 1;
      }

      _geom->reserve_subd_creases(numCreases);

      const VtIntArray creaseIndices = subdivTags.GetCreaseIndices();
      const VtFloatArray creaseWeights = subdivTags.GetCreaseWeights();

      indexOffset = 0;
      size_t creaseLengthOffset = 0;
      size_t createWeightOffset = 0;
      for (const int creaseLength : creaseLengths) {
        for (int j = 0; j < creaseLength - 1; ++j, ++createWeightOffset) {
          const int v0 = creaseIndices[indexOffset + j];
          const int v1 = creaseIndices[indexOffset + j + 1];

          const float weight = creaseWeights.size() == creaseLengths.size() ?
                                   creaseWeights[creaseLengthOffset] :
                                   creaseWeights[createWeightOffset];

          _geom->add_edge_crease(v0, v1, weight);
        }

        indexOffset += creaseLength;
        creaseLengthOffset++;
      }

      const VtIntArray cornerIndices = subdivTags.GetCornerIndices();
      const VtFloatArray cornerWeights = subdivTags.GetCornerWeights();

      for (size_t i = 0; i < cornerIndices.size(); ++i) {
        _geom->add_vertex_crease(cornerIndices[i], cornerWeights[i]);
      }
    }
  }
}

bool HdCyclesMesh::ReadSubdivisionSettings(HdSceneDelegate *sceneDelegate)
{
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(
      GetPrim(sceneDelegate, GetId()).dataSource);

  /* Unauthored, or not an int, leaves the default of 8. Blender's largest level bounds it;
   * the dicing rate bounds the tessellation well before that. */
  const VtValue maxLevel = ReadPrimvar(primvars, _tokens->maxLevel);
  int level = HdCyclesSettingValue<int>(maxLevel, -1);
  if (level < 0 && !maxLevel.IsEmpty()) {
    HDCYCLES_WARN("%s: cycles:object:subdivision:max_level must be a non-negative int; ignored.",
                  GetId().GetText());
  }
  level = std::min(level, 16);
  /* Blender's smallest dicing rate; smaller ones tessellate without bound. */
  _subdDicingRate = std::max(
      HdCyclesSettingValue<float>(ReadPrimvar(primvars, _tokens->dicingRate), 1.0f), 0.1f);
  const VtValue space = ReadPrimvar(primvars, _tokens->adaptiveSpace);
  _subdObjectSpace = (space.IsHolding<TfToken>() ?
                          space.UncheckedGet<TfToken>().GetString() :
                          HdCyclesSettingValue<std::string>(space, "")) == "object";

  const bool changed = level != _subdMaxLevel;
  _subdMaxLevel = level;
  return changed;
}

void HdCyclesMesh::ApplySubdivisionSettings()
{
  if (_geom->get_subdivision_type() == Mesh::SUBDIVISION_NONE) {
    return;
  }
  /* Pixel-space dicing measures edges from the dicing camera, which follows the render
   * camera (HdCyclesRenderPass), through the transform UpdateDicing sets. */
  _geom->set_subd_dicing_rate(_subdDicingRate);
  _geom->set_subd_max_level(_topology.GetRefineLevel());
  _geom->set_subd_adaptive_space(_subdObjectSpace ? Mesh::SUBDIVISION_ADAPTIVE_SPACE_OBJECT :
                                                    Mesh::SUBDIVISION_ADAPTIVE_SPACE_PIXEL);
}

/* Cycles tessellates into the mesh's own vertices and triangles, and takes the vertices the
 * mesh holds when it starts as the base mesh's (EdgeDice counts the ones it adds from them), as
 * a Blender mesh is each time it re-dices, being synced anew. A mesh re-tessellated while it
 * still held its last tessellation, after a re-dice or new points, had the old diced vertices
 * counted as base ones, and Cycles read past the base positions: Houdini crashed in
 * OsdData::build or in the attribute interpolation. The base positions are the subdivision
 * positions, which stay as they were populated. */
void HdCyclesMesh::ResetTessellation()
{
  if (_geom->get_subdivision_type() == Mesh::SUBDIVISION_NONE || !_geom->need_tesselation()) {
    return;
  }
  const size_t baseVerts = size_t(_topology.GetNumPoints());
  if (_geom->num_verts() != baseVerts || !_geom->get_triangles().empty()) {
    _geom->resize_mesh(int(baseVerts), 0);
  }
}

void HdCyclesMesh::Finalize(PXR_NS::HdRenderParam *renderParam)
{
  if (_pixelDiced) {
    static_cast<HdCyclesSession *>(renderParam)->SetPixelDiced(this, false);
    _pixelDiced = false;
  }
  _topology = HdMeshTopology();
  _primitiveParams.clear();
  _dicedSize = 0.0f;

  HdCyclesGeometry<PXR_NS::HdMesh, Mesh>::Finalize(renderParam);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
