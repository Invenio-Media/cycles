/* SPDX-FileCopyrightText: 2026 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "scene/image_loader.h"
#include "util/string.h"
#include "util/unique_ptr.h"

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* The loader for an image file in one of Houdini's own formats (.rat, the format of Houdini's
 * sample HDRIs, which Karma reads), through Houdini's image library; null for other files,
 * which OpenImageIO loads. For the scene's ImageManager::file_loader. */
unique_ptr<ImageLoader> HdCyclesHoudiniImageLoader(const string &filepath);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
