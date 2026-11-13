#ifndef libslic3r_SurfaceDisplacement_hpp_
#define libslic3r_SurfaceDisplacement_hpp_

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "TextureMapping.hpp"
#include "admesh/stl.h"

namespace Slic3r::Feature::FuzzySkin {

struct DisplacementMap;
struct UVMesh;

// A displacement map applied to the whole surface of a part, as if the texture were modelled into the mesh, but
// computed per layer from the texture instead of from a subdivided mesh.
struct SurfaceDisplacement
{
    std::shared_ptr<const DisplacementMap> map;
    TextureMapping                         mapping;
    double                                 depth;       // white moves the surface out by depth, black in, mm
    double                                 spacing;     // sample spacing along outlines and over flat faces, mm
    bool                                   first_layer; // displace the walls of the first layer too
    std::shared_ptr<const UVMesh>          uv_mesh;     // texture coordinates, for the UV projection
    bool                                   smooth_support = false; // tree supports find overhangs without the texture
    FuzzySkinTexturePattern                pattern = FuzzySkinTexturePattern::Image; // laid on the surface by the on the surface projection
    // Per layer only: each layer's outline moves sideways by the texture, and nothing moves up or down (tops stay flat).
    // Much less work than the whole surface: no regions are added or cut, and the layers stay simple downstream.
    bool                                   per_layer = false;
};

// Whether a region's displacement map is applied while slicing (to the whole surface, or per layer).
bool uses_surface_displacement(const PrintRegionConfig& config);

// The displacement a region asks for, on an object of the given size (mm), or nothing. Throws SlicingError when the
// image cannot be read.
// uv_source: for the UV projection, the file with the texture coordinates (see uv_mesh_source()).
std::optional<SurfaceDisplacement> surface_displacement(const PrintRegionConfig& config, const Vec3d& object_size,
                                                        const std::string& uv_source = {});

// How far a region's displacement may raise the top of an object, so the layers above it get sliced.
double surface_displacement_height_margin(const PrintRegionConfig& config);

// Displaces the slices of one volume. its is the volume's mesh, trafo takes it to the coordinates the slices are in
// (XY scaled in layers, Z as in zs), zs are the slicing heights and layers the slices at those heights, in and out.
//
// Steep faces (walls, slopes down to 45 degrees) move the outline sideways by depth / sin(slope), which is where the
// displaced surface crosses the layer. Flat and shallow faces displace up or down: for the layers within the
// displacement of such a face, the part of its footprint where the displaced surface lies above (or below) the layer
// is added or cut away, so a bump on a top becomes islands on the layers above it and a dent removes material below.
// The face resting on the bed is left flat.
//
// Per layer only (displacement.per_layer): every outline moves sideways along its own normal by the texture at that point
// of the layer, and flat and shallow faces are left alone.
//
// With the on the surface projection, the pattern is laid out over its, moved by trafo (SurfacePattern).
// With the UV projection, the texture coordinates at a point are those of the closest point on the UV mesh, which is
// first fitted to its by bounding box and then moved by trafo like its.
void displace_slices(const indexed_triangle_set&   its,
                     const Transform3d&            trafo,
                     const std::vector<float>&     zs,
                     std::vector<ExPolygons>&      layers,
                     const SurfaceDisplacement&    displacement,
                     const std::function<void()>&  throw_on_cancel);

// Contours of where a scalar field sampled on a regular grid is positive, linearly interpolated (marching squares).
// values: rows x cols, row major, node (r, c) at (x0 + c * step, y0 + r * step) in mm. Nodes on the border should be
// negative so every contour closes. Returns outer contours counter-clockwise and holes clockwise, scaled.
Polygons positive_region_contours(const std::vector<float>& values, size_t rows, size_t cols, double x0, double y0, double step);

} // namespace Slic3r::Feature::FuzzySkin

#endif // libslic3r_SurfaceDisplacement_hpp_
