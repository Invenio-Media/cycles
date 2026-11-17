/* SPDX-FileCopyrightText: 2026 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/houdini_image.h"
#include "hydra/houdini_image_io.h"

#include "util/colorspace.h"
#include "util/image_metadata.h"
#include "util/log.h"
#include "util/path.h"

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

class HoudiniImageLoader : public ImageLoader {
 public:
  explicit HoudiniImageLoader(const string &filepath) : _filepath(filepath) {}

  bool load_metadata(ImageMetaData &metadata,
                     const ImageLoaderParams &params,
                     Progress & /*progress*/) override
  {
    std::string error;
    if (!HdCyclesHoudiniImage::ReadInfo(_filepath, _info, error)) {
      LOG_WARNING << "Image file " << _filepath << " failed to load: " << error;
      params.load_failure_num++;
      return false;
    }

    metadata.width = _info.width;
    metadata.height = _info.height;
    metadata.channels = _info.channels;
    const bool rgba = _info.channels > 1;
    switch (_info.type) {
      case HdCyclesHoudiniImage::DataType::Byte:
        metadata.type = rgba ? IMAGE_DATA_TYPE_BYTE4 : IMAGE_DATA_TYPE_BYTE;
        break;
      case HdCyclesHoudiniImage::DataType::UShort:
        metadata.type = rgba ? IMAGE_DATA_TYPE_USHORT4 : IMAGE_DATA_TYPE_USHORT;
        break;
      case HdCyclesHoudiniImage::DataType::Half:
        metadata.type = rgba ? IMAGE_DATA_TYPE_HALF4 : IMAGE_DATA_TYPE_HALF;
        break;
      case HdCyclesHoudiniImage::DataType::Float:
        metadata.type = rgba ? IMAGE_DATA_TYPE_FLOAT4 : IMAGE_DATA_TYPE_FLOAT;
        break;
    }

    /* As OpenImageIO's loader gives them: the file's colour space, as a name of the scene's
     * config, and its path, for the config's file rules. */
    switch (_info.colorspace) {
      case HdCyclesHoudiniImage::ColorSpace::Ocio:
        metadata.colorspace_file_hint = _info.ocioColorspace;
        break;
      case HdCyclesHoudiniImage::ColorSpace::Srgb:
        metadata.colorspace_file_hint = ColorSpaceManager::find_srgb_texture_colorspace().string();
        break;
      case HdCyclesHoudiniImage::ColorSpace::Linear:
        metadata.colorspace_file_hint = "scene_linear";
        break;
      case HdCyclesHoudiniImage::ColorSpace::Unknown:
        break;
    }
    metadata.colorspace_file_format = "rat";
    metadata.colorspace_file_path = _filepath;
    return true;
  }

  bool load_pixels(const ImageMetaData &metadata, void *pixels) override
  {
    /* In the type the metadata settled on: a colour space conversion makes 8 and 16-bit
     * integer images half float. */
    HdCyclesHoudiniImage::DataType type;
    switch (metadata.type) {
      case IMAGE_DATA_TYPE_BYTE:
      case IMAGE_DATA_TYPE_BYTE4:
        type = HdCyclesHoudiniImage::DataType::Byte;
        break;
      case IMAGE_DATA_TYPE_USHORT:
      case IMAGE_DATA_TYPE_USHORT4:
        type = HdCyclesHoudiniImage::DataType::UShort;
        break;
      case IMAGE_DATA_TYPE_HALF:
      case IMAGE_DATA_TYPE_HALF4:
        type = HdCyclesHoudiniImage::DataType::Half;
        break;
      case IMAGE_DATA_TYPE_FLOAT:
      case IMAGE_DATA_TYPE_FLOAT4:
        type = HdCyclesHoudiniImage::DataType::Float;
        break;
      default:
        return false;
    }

    std::string error;
    if (!HdCyclesHoudiniImage::ReadPixels(_filepath, _info, type, pixels, error)) {
      LOG_WARNING << "Image file " << _filepath << " failed to load: " << error;
      return false;
    }
    metadata.conform_pixels(pixels);
    return true;
  }

  string name() const override
  {
    return path_filename(_filepath);
  }

  bool equals(const ImageLoader &other) const override
  {
    return _filepath == static_cast<const HoudiniImageLoader &>(other)._filepath;
  }

 private:
  string _filepath;
  HdCyclesHoudiniImage::Info _info;
};

}  // namespace

unique_ptr<ImageLoader> HdCyclesHoudiniImageLoader(const string &filepath)
{
  if (!HdCyclesHoudiniImage::IsHoudiniFormat(filepath)) {
    return nullptr;
  }
  return make_unique<HoudiniImageLoader>(filepath);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
