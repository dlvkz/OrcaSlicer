#ifndef libslic3r_FuzzySkin_TextureMapping_hpp_
#define libslic3r_FuzzySkin_TextureMapping_hpp_

#include <algorithm>
#include <cmath>

#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::Feature::FuzzySkin {

// How a displacement map is placed on an object. Coordinates are the object's, in mm, with XY centred on the middle of
// its bounding box and Z up from its bottom.
struct TextureMapping
{
    FuzzySkinTextureProjection projection = FuzzySkinTextureProjection::Box;
    double                     size       = 20.;   // width of one tile, mm; its height follows the image's aspect ratio
    double                     rotation   = 0.;    // degrees, the image turned counter-clockwise as seen on the surface
    bool                       invert     = false; // black out, white in
    double                     offset     = 0.5;   // which way the map moves the surface: 0 only in, 1 only out, 0.5 both
    double                     radius     = 0.;    // half the object's larger XY extent, mm (cylindrical, spherical)
    double                     center_z   = 0.;    // height of the object's middle, mm (spherical)

    bool operator==(const TextureMapping& r) const
    {
        return projection == r.projection && size == r.size && rotation == r.rotation && invert == r.invert && offset == r.offset &&
               radius == r.radius &&
               center_z == r.center_z;
    }
    bool operator!=(const TextureMapping& r) const { return !(*this == r); }
};

// The surface's movement for a map value from 0 (black) to 1 (white), in texture depths: 2 from black to white. The
// offset sets where the value that leaves the surface in place lies, as Blender's midlevel does: at 0 white stays
// put and the rest goes in, at 1 black stays put and the rest goes out, at 0.5 mid grey stays put.
inline double signed_height(const TextureMapping& m, double value)
{
    if (m.invert)
        value = 1. - value;
    return (value - (1. - m.offset)) * 2.;
}

// The farthest the surface moves out and in, in texture depths.
inline double max_outward(const TextureMapping& m) { return 2. * std::clamp(m.offset, 0., 1.); }
inline double max_reach(const TextureMapping& m) { return 2. * std::max(std::clamp(m.offset, 0., 1.), 1. - std::clamp(m.offset, 0., 1.)); }

// Tiles once around the object for the cylindrical and spherical projections: the whole number closest to the
// object's circumference over the tile width, so the image meets itself at the back.
inline int tiles_around(const TextureMapping& m)
{
    if (m.radius <= 0. || m.size <= 0.)
        return 1;
    return std::max(1, int(std::lround(2. * M_PI * m.radius / m.size)));
}

// The mapping a region's settings ask for, on an object of the given size (mm).
inline TextureMapping texture_mapping(const PrintRegionConfig& config, const Vec3d& object_size)
{
    TextureMapping m;
    m.projection = config.fuzzy_skin_texture_projection.value;
    m.size       = config.fuzzy_skin_texture_size.value;
    m.rotation   = config.fuzzy_skin_texture_rotation.value;
    m.invert     = config.fuzzy_skin_texture_invert.value;
    m.offset     = config.fuzzy_skin_texture_offset.value;
    m.radius     = 0.5 * std::max(object_size.x(), object_size.y());
    m.center_z   = 0.5 * object_size.z();
    return m;
}

} // namespace Slic3r::Feature::FuzzySkin

#endif // libslic3r_FuzzySkin_TextureMapping_hpp_
