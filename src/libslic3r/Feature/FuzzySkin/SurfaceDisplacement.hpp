#ifndef libslic3r_SurfaceDisplacement_hpp_
#define libslic3r_SurfaceDisplacement_hpp_

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "admesh/stl.h"

namespace Slic3r::Feature::FuzzySkin {

struct DisplacementMap;

// A displacement map applied to the whole surface of a part, as if the texture were modelled into the mesh, but
// computed per layer from the texture instead of from a subdivided mesh.
struct SurfaceDisplacement
{
    std::shared_ptr<const DisplacementMap> map;
    FuzzySkinTextureProjection             projection;
    double                                 size;        // tile size, mm
    double                                 depth;       // white moves the surface out by depth, black in, mm
    double                                 spacing;     // sample spacing along outlines and over flat faces, mm
    bool                                   first_layer; // displace the walls of the first layer too
};

// Whether a region asks for its whole surface to be displaced by a map.
bool uses_surface_displacement(const PrintRegionConfig& config);

// The displacement a region asks for, or nothing. Throws SlicingError when the image cannot be read.
std::optional<SurfaceDisplacement> surface_displacement(const PrintRegionConfig& config);

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
