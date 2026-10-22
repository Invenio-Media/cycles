/* SPDX-FileCopyrightText: 2025 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/dataSource.h>
#include <pxr/usd/sdf/path.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Translates a material's MaterialX network (its mtlx render context) into native Cycles nodes
 * (cycles_*), which HdCyclesMaterial builds like any other network. Unauthored inputs take the
 * MaterialX node definitions' defaults. A node without a translation is left out with a
 * warning, and the inputs it fed are left unconnected. */
PXR_NS::HdContainerDataSourceHandle HdCyclesTranslateMaterialX(
    const PXR_NS::SdfPath &materialPath,
    const PXR_NS::HdContainerDataSourceHandle &network,
    const PXR_NS::HdContainerDataSourceHandle &prim);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
