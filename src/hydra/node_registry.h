/* SPDX-FileCopyrightText: 2026 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <string>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Cycles' shader nodes as JSON: for each node type, its inputs (name, UI name, socket type,
 * whether it can be connected, default value, enum names) and outputs. Hosts generate their
 * native node libraries from it, so they follow the Cycles release the plugin is built with. */
std::string HdCyclesShaderNodesJson();

HDCYCLES_NAMESPACE_CLOSE_SCOPE
