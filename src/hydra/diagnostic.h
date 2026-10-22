/* SPDX-FileCopyrightText: 2025 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/log.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/stringUtils.h>

#include <string>

/* husk shows Cycles' log but not Tf diagnostics, at any verbosity, so the delegate reports
 * problems through both: Tf for hosts that collect its diagnostics, Cycles' log for husk and
 * the terminal. A host that prints Tf diagnostics to the terminal too shows each message
 * twice there. Arguments are printf-style, as for TF_WARN. */
#define HDCYCLES_WARN(...) \
  do { \
    const std::string hdcycles_message_ = PXR_NS::TfStringPrintf(__VA_ARGS__); \
    TF_WARN("%s", hdcycles_message_.c_str()); \
    LOG_WARNING << hdcycles_message_; \
  } while (0)

#define HDCYCLES_ERROR(...) \
  do { \
    const std::string hdcycles_message_ = PXR_NS::TfStringPrintf(__VA_ARGS__); \
    TF_RUNTIME_ERROR("%s", hdcycles_message_.c_str()); \
    LOG_ERROR << hdcycles_message_; \
  } while (0)
