/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_buffer.h"
#include "hydra/diagnostic.h"
#include "hydra/session.h"
#include "util/half.h"

#include <algorithm>
#include <cmath>

#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesRenderBuffer::HdCyclesRenderBuffer(const SdfPath &bprimId) : HdRenderBuffer(bprimId) {}

HdCyclesRenderBuffer::~HdCyclesRenderBuffer() = default;

void HdCyclesRenderBuffer::Finalize(HdRenderParam *renderParam)
{
  // Remove this render buffer from AOV bindings
  // This ensures that 'OutputDriver' does not attempt to write to it anymore
  static_cast<HdCyclesSession *>(renderParam)->RemoveAovBinding(this);

  HdRenderBuffer::Finalize(renderParam);
}

void HdCyclesRenderBuffer::Sync(HdSceneDelegate *sceneDelegate,
                                HdRenderParam *renderParam,
                                HdDirtyBits *dirtyBits)
{
  const thread_scoped_lock lock(static_cast<HdCyclesSession *>(renderParam)->GetAovMutex());
  HdRenderBuffer::Sync(sceneDelegate, renderParam, dirtyBits);
}

bool HdCyclesRenderBuffer::Allocate(const GfVec3i &dimensions,
                                    HdFormat format,
                                    bool /*multiSampled*/)
{
  if (dimensions[0] < 0 || dimensions[1] < 0 || dimensions[2] != 1) {
    HDCYCLES_ERROR("HdCyclesRenderBuffer::Allocate called with dimensions that are not 2D.");
    return false;
  }

  if (unsigned(dimensions[0]) == _width && unsigned(dimensions[1]) == _height && format == _format)
  {
    return true;
  }

  if (IsMapped()) {
    HDCYCLES_ERROR("HdCyclesRenderBuffer::Allocate called while buffer is mapped.");
    return false;
  }

  _width = dimensions[0];
  _height = dimensions[1];
  _format = format;
  /* ID buffers start at -1, Hydra's "nothing", rather than at prim 0, until the first tile
   * arrives. */
  const bool isId = format >= HdFormatInt32 && format <= HdFormatInt32Vec4;
  _data.assign(size_t(_width) * _height * HdDataSizeOfFormat(format), isId ? 0xFF : 0);
  _resourceUsed = false;

  return true;
}

void HdCyclesRenderBuffer::_Deallocate()
{
  _width = 0u;
  _height = 0u;
  _format = HdFormatInvalid;

  _data.clear();
  _data.shrink_to_fit();

  _resource = VtValue();
}

void *HdCyclesRenderBuffer::Map()
{
  // Mapping is not implemented when a resource is set
  if (!_resource.IsEmpty()) {
    return nullptr;
  }

  ++_mapped;

  return _data.data();
}

void HdCyclesRenderBuffer::Unmap()
{
  --_mapped;
}

bool HdCyclesRenderBuffer::IsMapped() const
{
  return _mapped != 0;
}

void HdCyclesRenderBuffer::Resolve() {}

bool HdCyclesRenderBuffer::IsConverged() const
{
  return _converged;
}

void HdCyclesRenderBuffer::SetConverged(bool converged)
{
  _converged = converged;
}

bool HdCyclesRenderBuffer::IsResourceUsed() const
{
  return _resourceUsed;
}

VtValue HdCyclesRenderBuffer::GetResource(bool multiSampled) const
{
  TF_UNUSED(multiSampled);

  _resourceUsed = true;

  return _resource;
}

void HdCyclesRenderBuffer::SetResource(const VtValue &resource)
{
  _resource = resource;
}

namespace {

struct SimpleConversion {
  static float convert(const float value)
  {
    return value;
  }
};
struct IdConversion {
  /* IDs are stored offset by one (HdCyclesAovPass::isId). primId comes from the object ID
   * pass, written by the first sample; instanceId from an AOV value, averaged over the
   * samples, so an edge pixel holds a mix: rounding keeps a prim's own index where it covers
   * most of the pixel. */
  static int32_t convert(const float value)
  {
    return static_cast<int32_t>(std::lround(value)) - 1;
  }
};
struct UInt8Conversion {
  static uint8_t convert(const float value)
  {
    return static_cast<uint8_t>(value * 255.f);
  }
};
struct SInt8Conversion {
  static int8_t convert(const float value)
  {
    return static_cast<int8_t>(value * 127.f);
  }
};
struct HalfConversion {
  static half convert(const float value)
  {
    return float_to_half_image(value);
  }
};

/* Copies a size[0] x size[1] block, stepping rows by each side's width in pixels. */
template<typename SrcT, typename DstT, typename Convertor = SimpleConversion>
void writePixels(const SrcT *srcPtr,
                 const int srcWidth,
                 const int srcChannelCount,
                 DstT *dstPtr,
                 const int dstWidth,
                 const int dstChannelCount,
                 const GfVec2i &size,
                 const Convertor &convertor = {})
{
  const int writeChannelCount = GfMin(srcChannelCount, dstChannelCount);

  for (int y = 0; y < size[1]; ++y) {
    for (int x = 0; x < size[0]; ++x) {
      for (int c = 0; c < writeChannelCount; ++c) {
        dstPtr[x * dstChannelCount + c] = convertor.convert(srcPtr[x * srcChannelCount + c]);
      }
    }
    srcPtr += size_t(srcWidth) * srcChannelCount;
    dstPtr += size_t(dstWidth) * dstChannelCount;
  }
}

}  // namespace

void HdCyclesRenderBuffer::WritePixels(const float *srcPixels,
                                       const PXR_NS::GfVec2i &srcOffset,
                                       const GfVec2i &srcDims,
                                       const int srcChannels,
                                       bool isId)
{
  /* Only the part of the tile inside the buffer: a tile can start before the buffer or run
   * past its far edge. */
  const GfVec2i begin(std::max(0, -srcOffset[0]), std::max(0, -srcOffset[1]));
  const GfVec2i end(std::min(srcDims[0], int(_width) - srcOffset[0]),
                    std::min(srcDims[1], int(_height) - srcOffset[1]));
  if (end[0] <= begin[0] || end[1] <= begin[1]) {
    return;
  }
  const GfVec2i size = end - begin;
  const int srcWidth = srcDims[0];
  const int dstWidth = int(_width);
  srcPixels += (size_t(begin[1]) * srcWidth + begin[0]) * srcChannels;

  const size_t formatSize = HdDataSizeOfFormat(_format);
  uint8_t *const dstPixels = _data.data() +
                             (size_t(srcOffset[1] + begin[1]) * _width + srcOffset[0] + begin[0]) *
                                 formatSize;

  switch (_format) {
    case HdFormatUNorm8:
    case HdFormatUNorm8Vec2:
    case HdFormatUNorm8Vec3:
    case HdFormatUNorm8Vec4:
      writePixels(srcPixels,
                  srcWidth,
                  srcChannels,
                  dstPixels,
                  dstWidth,
                  1 + (_format - HdFormatUNorm8),
                  size,
                  UInt8Conversion());
      break;

    case HdFormatSNorm8:
    case HdFormatSNorm8Vec2:
    case HdFormatSNorm8Vec3:
    case HdFormatSNorm8Vec4:
      writePixels(srcPixels,
                  srcWidth,
                  srcChannels,
                  reinterpret_cast<int8_t *>(dstPixels),
                  dstWidth,
                  1 + (_format - HdFormatSNorm8),
                  size,
                  SInt8Conversion());
      break;

    case HdFormatFloat16:
    case HdFormatFloat16Vec2:
    case HdFormatFloat16Vec3:
    case HdFormatFloat16Vec4:
      writePixels(srcPixels,
                  srcWidth,
                  srcChannels,
                  reinterpret_cast<half *>(dstPixels),
                  dstWidth,
                  1 + (_format - HdFormatFloat16),
                  size,
                  HalfConversion());
      break;

    case HdFormatFloat32:
    case HdFormatFloat32Vec2:
    case HdFormatFloat32Vec3:
    case HdFormatFloat32Vec4:
      writePixels(srcPixels,
                  srcWidth,
                  srcChannels,
                  reinterpret_cast<float *>(dstPixels),
                  dstWidth,
                  1 + (_format - HdFormatFloat32),
                  size);
      break;

    case HdFormatInt32:
      // Special case for ID AOVs (see 'HdCyclesMesh::Sync')
      if (isId) {
        writePixels(srcPixels,
                    srcWidth,
                    srcChannels,
                    reinterpret_cast<int *>(dstPixels),
                    dstWidth,
                    1,
                    size,
                    IdConversion());
      }
      else {
        writePixels(srcPixels,
                    srcWidth,
                    srcChannels,
                    reinterpret_cast<int *>(dstPixels),
                    dstWidth,
                    1,
                    size);
      }
      break;
    case HdFormatInt32Vec2:
    case HdFormatInt32Vec3:
    case HdFormatInt32Vec4:
      writePixels(srcPixels,
                  srcWidth,
                  srcChannels,
                  reinterpret_cast<int *>(dstPixels),
                  dstWidth,
                  1 + (_format - HdFormatInt32),
                  size);
      break;

    default:
      HDCYCLES_ERROR("HdCyclesRenderBuffer::WritePixels called with unsupported format.");
      break;
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
