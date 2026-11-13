#ifndef libslic3r_FuzzySkin_TexturePatterns_hpp_
#define libslic3r_FuzzySkin_TexturePatterns_hpp_

#include <memory>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::Feature::FuzzySkin {

struct DisplacementMap;

// A built-in pattern as a displacement map: one tile that repeats seamlessly in both directions, heights spread over
// [0, 1]. Generated once per pattern and kept for the process. nullptr for FuzzySkinTexturePattern::Image.
std::shared_ptr<const DisplacementMap> pattern_displacement_map(FuzzySkinTexturePattern pattern);

} // namespace Slic3r::Feature::FuzzySkin

#endif // libslic3r_FuzzySkin_TexturePatterns_hpp_
