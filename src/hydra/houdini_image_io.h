/* SPDX-FileCopyrightText: 2026 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Houdini's own image formats (.rat), which OpenImageIO can't read, through Houdini's image
 * library. Plain C++ on both sides: Houdini's headers need the HDK's compile flags, so only
 * houdini_image_io.cpp includes them, and no Cycles or USD header. */

#include <cstdint>
#include <string>

namespace HdCyclesHoudiniImage {

enum class DataType { Byte, UShort, Half, Float };

/* What Houdini's image library knows of the file's colour space. */
enum class ColorSpace { Unknown, Linear, Srgb, Ocio };

struct Info {
  int64_t width = 0;
  int64_t height = 0;
  int channels = 0;
  DataType type = DataType::Float;
  ColorSpace colorspace = ColorSpace::Unknown;
  /* The OpenColorIO colour space, for ColorSpace::Ocio. */
  std::string ocioColorspace;
};

/* Whether the file is in a format that Houdini reads and OpenImageIO doesn't. */
bool IsHoudiniFormat(const std::string &path);

/* The file's resolution, channels and data type; false, with the reason, if it can't be read. */
bool ReadInfo(const std::string &path, Info &info, std::string &error);

/* The pixels, in the given data type: interleaved, rows bottom first, as Cycles stores images. */
bool ReadPixels(
    const std::string &path, const Info &info, DataType type, void *pixels, std::string &error);

}  // namespace HdCyclesHoudiniImage
