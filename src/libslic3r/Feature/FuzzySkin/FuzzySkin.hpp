#ifndef libslic3r_FuzzySkin_hpp_
#define libslic3r_FuzzySkin_hpp_

#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/PerimeterGenerator.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Slic3r::Feature::FuzzySkin {

// Brightness of a displacement map image in [0, 1], row 0 at the top of the image.
struct DisplacementMap
{
    size_t             cols = 0;
    size_t             rows = 0;
    std::vector<float> height;

    // Bilinear, wrapping. u runs left to right and v bottom to top, one tile per unit.
    double sample(double u, double v) const;
};

// Loads a PNG through a process-wide cache keyed by path. With reload_if_modified the file's modification time is
// checked and a changed file is read again; without it a cached image is returned without touching the disk.
// Returns nullptr and fills error on failure.
std::shared_ptr<const DisplacementMap> load_displacement_map(const std::string& path, std::string* error, bool reload_if_modified);

// Displacement in [-1, 1] at a point p (mm, object coordinates centred on the object's XY bounding box) of a surface
// whose outward unit normal is n.
double displacement_map_value(const DisplacementMap& map, FuzzySkinTextureProjection projection, double size, const Vec3d& p, const Vec3d& n);

// material_on_left: whether the printed material lies to the left of the direction of travel. A displacement map pushes
// the wall away from the material; the noise types do not depend on it.
void fuzzy_polyline(Points& poly, bool closed, coordf_t slice_z, const FuzzySkinConfig& cfg, bool material_on_left = true);

void fuzzy_extrusion_line(Arachne::ExtrusionJunctions& ext_lines, coordf_t slice_z, const FuzzySkinConfig& cfg, bool material_on_left = true);

void group_region_by_fuzzify(PerimeterGenerator& g);

bool should_fuzzify(const FuzzySkinConfig& config, int layer_id, size_t loop_idx, bool is_contour);

// The outline of a layer as its outer walls are printed when they use a displacement map: the layer's lslices with
// the map applied, as it is to the walls. Overhang detection compares a wall with the layer below, and a bump in a
// wall is only supported if the bump below it is there as well, as it would be in a mesh with the texture modelled.
// Returns false, leaving out untouched, when no region of the layer uses a displacement map. When several regions
// use different maps, the first one is applied to the whole outline.
bool displaced_outline(const Layer& layer, ExPolygons& out);

Polygon apply_fuzzy_skin(const Polygon& polygon, const PerimeterGenerator& perimeter_generator, size_t loop_idx, bool is_contour);
void    apply_fuzzy_skin(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, bool is_contour);

} // namespace Slic3r::Feature::FuzzySkin

#endif // libslic3r_FuzzySkin_hpp_
