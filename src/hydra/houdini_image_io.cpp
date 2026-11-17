/* SPDX-FileCopyrightText: 2026 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Built with the HDK's compile flags: only Houdini's headers here, no Cycles or USD header. */

#include "hydra/houdini_image_io.h"

#include <IMG/IMG_File.h>
#include <IMG/IMG_FileParms.h>
#include <IMG/IMG_Plane.h>
#include <IMG/IMG_Stat.h>
#include <PXL/PXL_Common.h>

#include <algorithm>
#include <cctype>

namespace HdCyclesHoudiniImage {

namespace {

IMG_DataType ToImg(const DataType type)
{
  switch (type) {
    case DataType::Byte:
      return IMG_UCHAR;
    case DataType::UShort:
      return IMG_USHORT;
    case DataType::Half:
      return IMG_HALF;
    case DataType::Float:
      return IMG_FLOAT;
  }
  return IMG_FLOAT;
}

int64_t ByteSize(const DataType type)
{
  switch (type) {
    case DataType::Byte:
      return 1;
    case DataType::UShort:
    case DataType::Half:
      return 2;
    case DataType::Float:
      return 4;
  }
  return 4;
}

/* The file with its colour and alpha planes interleaved into one, rows bottom first, in the
 * given data type (IMG_DT_ANY: the file's own). */
IMG_FilePtr Open(const std::string &path, const IMG_DataType dataType)
{
  IMG_FileParms parms;
  parms.setInterleaved(IMG_INTERLEAVED);
  parms.orientImage(IMG_ORIENT_LEFT_FIRST, IMG_ORIENT_BOTTOM_FIRST);
  if (dataType != IMG_DT_ANY) {
    parms.setDataType(dataType);
  }
  return IMG_File::open(path.c_str(), &parms);
}

}  // namespace

bool IsHoudiniFormat(const std::string &path)
{
  static const std::string extension = ".rat";
  if (path.size() < extension.size()) {
    return false;
  }
  return std::equal(
      extension.begin(),
      extension.end(),
      path.end() - extension.size(),
      [](const char a, const char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
}

bool ReadInfo(const std::string &path, Info &info, std::string &error)
{
  const IMG_FilePtr file = Open(path, IMG_DT_ANY);
  if (!file) {
    error = "Houdini's image library could not open it";
    return false;
  }
  const IMG_Stat &stat = file->getStat();
  /* A cube map, its six faces as planes. Cycles' environment texture maps a latlong or
   * mirror ball image, and its first face would show stretched over the whole sky. */
  if (stat.isEnvMap()) {
    error = "it's a cube map, which Cycles can't map; convert it to a latlong image";
    return false;
  }
  const IMG_Plane *plane = stat.getNumPlanes() > 0 ? stat.getPlane(0) : nullptr;
  if (!plane) {
    error = "it has no image planes";
    return false;
  }

  info.width = stat.getXres();
  info.height = stat.getYres();
  info.channels = plane->getComponentCount();
  switch (plane->getDataType()) {
    case IMG_UCHAR:
      info.type = DataType::Byte;
      break;
    case IMG_USHORT:
      info.type = DataType::UShort;
      break;
    case IMG_HALF:
      info.type = DataType::Half;
      break;
    default:
      /* Float, and 32-bit integers, which Cycles has no texture type for. */
      info.type = DataType::Float;
      break;
  }
  switch (plane->getColorSpace()) {
    case PXL_CS_LINEAR:
      info.colorspace = ColorSpace::Linear;
      break;
    case PXL_CS_SRGB:
      info.colorspace = ColorSpace::Srgb;
      break;
    case PXL_CS_OCIO:
      info.colorspace = ColorSpace::Ocio;
      info.ocioColorspace = plane->getOCIOColorSpace().toStdString();
      break;
    default:
      /* Rec.709 and gamma curves, which OpenColorIO configs needn't name. */
      info.colorspace = ColorSpace::Unknown;
      break;
  }
  return true;
}

bool ReadPixels(const std::string &path,
                const Info &info,
                const DataType type,
                void *pixels,
                std::string &error)
{
  const IMG_FilePtr file = Open(path, ToImg(type));
  if (!file) {
    error = "Houdini's image library could not open it";
    return false;
  }
  const IMG_Stat &stat = file->getStat();
  const IMG_Plane *plane = stat.getNumPlanes() > 0 ? stat.getPlane(0) : nullptr;
  if (!plane || stat.getXres() != info.width || stat.getYres() != info.height ||
      plane->getComponentCount() != info.channels)
  {
    error = "it changed after its size was read";
    return false;
  }

  /* Rows straight into Cycles' buffer, which must take them exactly. */
  const int64_t rowBytes = info.width * info.channels * ByteSize(type);
  if (stat.bytesPerPlaneScan(*plane) != rowBytes) {
    error = "its rows aren't the size expected";
    return false;
  }
  unsigned char *row = static_cast<unsigned char *>(pixels);
  for (int64_t y = 0; y < info.height; y++, row += rowBytes) {
    if (!file->readIntoBuffer(int(y), row, plane)) {
      error = "reading row " + std::to_string(y) + " failed";
      return false;
    }
  }
  return true;
}

}  // namespace HdCyclesHoudiniImage
