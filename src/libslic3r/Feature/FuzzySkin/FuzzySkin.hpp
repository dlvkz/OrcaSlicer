#ifndef libslic3r_FuzzySkin_hpp_
#define libslic3r_FuzzySkin_hpp_

#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "TextureMapping.hpp"

#include <memory>
#include <string>
#include <vector>

#include "admesh/stl.h"

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
// PNG and JPEG images are read.
std::shared_ptr<const DisplacementMap> load_displacement_map(const std::string& path, std::string* error, bool reload_if_modified);

// The image at path, or a built-in pattern (see TexturePatterns.hpp).
std::shared_ptr<const DisplacementMap> load_texture(FuzzySkinTexturePattern pattern, const std::string& path, std::string* error,
                                                    bool reload_if_modified);
// The texture a region's settings ask for.
std::shared_ptr<const DisplacementMap> load_texture(const PrintRegionConfig& config, std::string* error, bool reload_if_modified);

// A region's fuzzy skin settings, of the given type, on an object of the given size (mm).
FuzzySkinConfig fuzzy_skin_config(const PrintRegionConfig& config, FuzzySkinType type, const Vec3d& object_size);

// A mesh with texture coordinates, for the UV projection: the triangles of an OBJ file (polygons split into fans) with
// the (u, v) of each corner, v running bottom to top as in OBJ.
struct UVMesh
{
    std::vector<Vec3d>                       vertices;
    std::vector<stl_triangle_vertex_indices> faces;
    std::vector<Vec2d>                       uvs; // three per face, in corner order
    Vec3d                                    min = Vec3d::Zero(), max = Vec3d::Zero();
};

// Loads an OBJ file through a cache like load_displacement_map. Fails when the file has no texture coordinates.
std::shared_ptr<const UVMesh> load_uv_mesh(const std::string& path, std::string* error, bool reload_if_modified);

// The file the UV projection takes its texture coordinates from: the UV mesh setting if it is set, otherwise the model
// file the part was loaded from (its own, else its object's) when that is a GLB, glTF or OBJ. Empty when there is none.
std::string uv_mesh_source(const std::string& configured, const std::string& volume_file, const std::string& object_file);

// Displacement in [-1, 1] at a point p (mm, object coordinates centred on the object's XY bounding box, Z up from its
// bottom) of a surface whose outward unit normal is n. Always 0 for the UV projection, see SurfaceDisplacement.
double displacement_map_value(const DisplacementMap& map, const TextureMapping& mapping, const Vec3d& p, const Vec3d& n);

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
