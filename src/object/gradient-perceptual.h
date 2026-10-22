// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PROOF_GRADIENT_PERCEPTUAL_H
#define PROOF_GRADIENT_PERCEPTUAL_H

#include <vector>
#include "sp-paint-server-data.h"

class SPGradient;

namespace Inkscape::PerceptualGradient {
bool enabled(SPGradient const *gradient);
std::vector<double> midpoints(SPGradient const *gradient);
/// Rendering samples only. Original XML stops and native writer stops stay editable.
SPGradientVector rendering_vector(SPGradient const *gradient);
}

#endif
