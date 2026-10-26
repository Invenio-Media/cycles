/* SPDX-FileCopyrightText: 2026 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <string>

#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/usd/sdf/path.h>

CCL_NAMESPACE_BEGIN
class ShaderNode;
CCL_NAMESPACE_END

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Cycles' Color Ramp and curve nodes take their curves as tables, which Blender bakes from its
 * ramps and curves. A USD network carries them as ramps instead, as Houdini authors a ramp
 * parameter: for a ramp named R, inputs:R (the key count), R_basis (the interpolation),
 * R_keys (positions in [0, 1]) and R_values (float or colour). The ramps of each node:
 * - rgb_ramp: color_ramp (colour) and alpha_ramp, into ramp and ramp_alpha;
 * - float_curve: curve_ramp, into curve;
 * - rgb_curves: combined_curve, red_curve, green_curve and blue_curve, into curves, each channel
 *   through its own curve after the combined one, as in Blender;
 * - vector_curves: x_curve, y_curve and z_curve, into curves.
 * A curve's [0, 1] maps to the node's min_x to max_x. An unauthored ramp is the VOP's default:
 * black to white, alpha 1, or the identity. Constant, linear and Catmull-Rom interpolate as
 * Houdini's ramps do; other bases are evaluated as Catmull-Rom, with a warning. A ramp has one
 * basis: Houdini's translator writes the first key's, so keys with other bases follow it. */

/* Whether the parameter is a part of one of the node type's ramps, which
 * HdCyclesApplyRamps reads rather than a socket. */
bool HdCyclesIsRampParameter(const std::string &nodeType, const PXR_NS::TfToken &name);

/* Bakes the node's ramps from the parameters into its tables. Nodes without ramps are left
 * alone. */
void HdCyclesApplyRamps(CCL_NS::ShaderNode *node,
                        const PXR_NS::HdMaterialNodeParameterContainerSchema &params,
                        const PXR_NS::SdfPath &nodePath);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
