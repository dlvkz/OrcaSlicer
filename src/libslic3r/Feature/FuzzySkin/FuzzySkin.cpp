#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/cstdio.hpp>
#include <png.h>

#include "libslic3r/Algorithm/LineSplit.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "FuzzySkin.hpp"
#include "SurfaceDisplacement.hpp"

#include "libnoise/noise.h"

// #define DEBUG_FUZZY

using namespace Slic3r;

namespace Slic3r::Feature::FuzzySkin {

// Produces a random value between 0 and 1. Thread-safe.
static double random_value() {
    thread_local std::random_device rd;
    // Hash thread ID for random number seed if no hardware rng seed is available
    thread_local std::mt19937 gen(rd.entropy() > 0 ? rd() : std::hash<std::thread::id>()(std::this_thread::get_id()));
    thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(gen);
}

class UniformNoise: public noise::module::Module {
    public:
        UniformNoise(): Module (GetSourceModuleCount ()) {};

        virtual int GetSourceModuleCount() const { return 0; }
        virtual double GetValue(double x, double y, double z) const { return random_value() * 2 - 1; }
};

static std::unique_ptr<noise::module::Module> get_noise_module(const FuzzySkinConfig& cfg) {
    if (cfg.noise_type == NoiseType::Perlin) {
        auto perlin_noise = noise::module::Perlin();
        perlin_noise.SetFrequency(1 / cfg.noise_scale);
        perlin_noise.SetOctaveCount(cfg.noise_octaves);
        perlin_noise.SetPersistence(cfg.noise_persistence);
        return std::make_unique<noise::module::Perlin>(perlin_noise);
    } else if (cfg.noise_type == NoiseType::Billow) {
        auto billow_noise = noise::module::Billow();
        billow_noise.SetFrequency(1 / cfg.noise_scale);
        billow_noise.SetOctaveCount(cfg.noise_octaves);
        billow_noise.SetPersistence(cfg.noise_persistence);
        return std::make_unique<noise::module::Billow>(billow_noise);
    } else if (cfg.noise_type == NoiseType::RidgedMulti) {
        auto ridged_multi_noise = noise::module::RidgedMulti();
        ridged_multi_noise.SetFrequency(1 / cfg.noise_scale);
        ridged_multi_noise.SetOctaveCount(cfg.noise_octaves);
        return std::make_unique<noise::module::RidgedMulti>(ridged_multi_noise);
    } else if (cfg.noise_type == NoiseType::Voronoi) {
        auto voronoi_noise = noise::module::Voronoi();
        voronoi_noise.SetFrequency(1 / cfg.noise_scale);
        voronoi_noise.SetDisplacement(1.0);
        return std::make_unique<noise::module::Voronoi>(voronoi_noise);
    } else {
        return std::make_unique<UniformNoise>();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Displacement map
// ---------------------------------------------------------------------------------------------------------------------

// Decodes any PNG (grey, palette, RGB, with or without alpha, 1 to 16 bit) to brightness in [0, 1], row 0 at the top.
// Kept free of C++ objects with destructors between setjmp and the libpng calls that may longjmp.
static bool read_png_brightness(FILE* file, size_t& cols, size_t& rows, std::vector<float>& out, std::string& error)
{
    png_structp png  = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop   info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info) {
        png_destroy_read_struct(&png, &info, nullptr);
        error = "out of memory";
        return false;
    }
    std::vector<png_byte>  pixels;
    std::vector<png_bytep> row_ptrs;
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        error = "not a readable PNG image";
        return false;
    }
    png_init_io(png, file);
    png_read_info(png, info);
    const png_byte color_type = png_get_color_type(png, info);
    const png_byte bit_depth  = png_get_bit_depth(png, info);
    png_set_expand(png); // palette -> RGB, grey below 8 bit -> 8 bit, tRNS -> alpha
    png_set_strip_alpha(png);
    if (color_type & PNG_COLOR_MASK_COLOR)
        png_set_rgb_to_gray_fixed(png, 1, -1, -1); // default luma weights
    if (bit_depth == 16)
        png_set_swap(png); // native byte order on little endian hosts
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    cols                    = png_get_image_width(png, info);
    rows                    = png_get_image_height(png, info);
    const size_t row_bytes  = png_get_rowbytes(png, info);
    const bool   sixteen    = png_get_bit_depth(png, info) == 16;
    if (png_get_channels(png, info) != 1 || cols == 0 || rows == 0) {
        png_destroy_read_struct(&png, &info, nullptr);
        error = "unsupported PNG pixel format";
        return false;
    }
    pixels.resize(row_bytes * rows);
    row_ptrs.resize(rows);
    for (size_t r = 0; r < rows; ++r)
        row_ptrs[r] = pixels.data() + r * row_bytes;
    png_read_image(png, row_ptrs.data());
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);

    out.resize(cols * rows);
    for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < cols; ++c) {
            const png_byte* px = row_ptrs[r] + (sixteen ? 2 * c : c);
            out[r * cols + c]  = sixteen ? float(*reinterpret_cast<const uint16_t*>(px)) / 65535.f : float(*px) / 255.f;
        }
    return true;
}

struct DisplacementMapCacheEntry
{
    std::time_t                                 mtime = 0;
    std::shared_ptr<const DisplacementMap>      map;
};

static std::mutex                                       g_map_cache_mutex;
static std::map<std::string, DisplacementMapCacheEntry> g_map_cache;

std::shared_ptr<const DisplacementMap> load_displacement_map(const std::string& path, std::string* error, bool reload_if_modified)
{
    auto fail = [error](const std::string& msg) -> std::shared_ptr<const DisplacementMap> {
        if (error)
            *error = msg;
        return nullptr;
    };
    if (path.empty())
        return fail("no displacement map image is set");

    std::lock_guard<std::mutex> lock(g_map_cache_mutex);
    auto                        it = g_map_cache.find(path);
    if (it != g_map_cache.end() && !reload_if_modified)
        return it->second.map;

    boost::system::error_code ec;
    const std::time_t         mtime = boost::filesystem::last_write_time(boost::filesystem::path(path), ec);
    if (ec)
        return fail("cannot open \"" + path + "\"");
    if (it != g_map_cache.end() && it->second.mtime == mtime)
        return it->second.map;

    FILE* file = boost::nowide::fopen(path.c_str(), "rb");
    if (!file)
        return fail("cannot open \"" + path + "\"");
    auto        map = std::make_shared<DisplacementMap>();
    std::string why;
    const bool  ok = read_png_brightness(file, map->cols, map->rows, map->height, why);
    fclose(file);
    if (!ok)
        return fail("\"" + path + "\": " + why);

    g_map_cache[path] = {mtime, map};
    return map;
}

// Bilinear sample with wrap-around. u runs left to right and v bottom to top, one tile per unit.
double DisplacementMap::sample(double u, double v) const
{
    const double x  = (u - std::floor(u)) * double(cols) - 0.5;
    const double y  = (1. - (v - std::floor(v))) * double(rows) - 0.5;
    const double fx = std::floor(x), fy = std::floor(y);
    const double tx = x - fx, ty = y - fy;
    auto         wrap = [](long i, size_t n) { return size_t(((i % long(n)) + long(n)) % long(n)); };
    const size_t c0 = wrap(long(fx), cols), c1 = wrap(long(fx) + 1, cols);
    const size_t r0 = wrap(long(fy), rows), r1 = wrap(long(fy) + 1, rows);
    const double top    = height[r0 * cols + c0] * (1. - tx) + height[r0 * cols + c1] * tx;
    const double bottom = height[r1 * cols + c0] * (1. - tx) + height[r1 * cols + c1] * tx;
    return top * (1. - ty) + bottom * ty;
}

// Displacement in [-1, 1] at a wall point p (mm, object coordinates centred on the object's XY bounding box)
// whose outward normal is n, at height z.
double displacement_map_value(const DisplacementMap& map, FuzzySkinTextureProjection projection, double size, const Vec3d& p, const Vec3d& n)
{
    // Sides: tiles along the wall, v up. Tops and bottoms (normal more vertical than 45 degrees in the cylindrical
    // projection, blended in by the normal in the box projection): seen from above or below, +Y up in the image.
    const double tile_w   = size;
    const double tile_h   = size * double(map.rows) / double(map.cols);
    auto         top_down = [&](double nz) {
        return map.sample((nz >= 0. ? p.x() : -p.x()) / tile_w + 0.5, p.y() / tile_h + 0.5);
    };
    double value;
    if (projection == FuzzySkinTextureProjection::Cylindrical) {
        if (std::abs(n.z()) > M_SQRT1_2) {
            value = top_down(n.z());
        } else {
            // One turn around the object is one tile wide, counter-clockwise seen from above so the image is not mirrored.
            // The middle of the image faces the front (-Y) and the seam is at the back. The tile is size high.
            const double u = (std::atan2(p.y(), p.x()) + 0.5 * M_PI) / (2. * M_PI) + 0.5;
            value          = map.sample(u, p.z() / size);
        }
    } else {
        // Box: project from +-X, +-Y and +-Z, with u running to the viewer's right on every side, and blend by the normal.
        // The middle of a tile is on the object's centre line, so an image one side wide sits centred on that side.
        const double v  = p.z() / tile_h;
        double       wx = std::pow(n.x(), 4.), wy = std::pow(n.y(), 4.), wz = std::pow(n.z(), 4.);
        const double sum = wx + wy + wz;
        wx /= sum;
        wy /= sum;
        wz /= sum;
        value = 0.;
        if (wx > 1e-4)
            value += wx * map.sample((n.x() >= 0. ? p.y() : -p.y()) / tile_w + 0.5, v);
        if (wy > 1e-4)
            value += wy * map.sample((n.y() >= 0. ? -p.x() : p.x()) / tile_w + 0.5, v);
        if (wz > 1e-4)
            value += wz * top_down(n.z());
    }
    return value * 2. - 1.;
}

// Walls: a point of a layer at height z whose outward normal n is horizontal.
static double displacement_map_value(const DisplacementMap& map, const FuzzySkinConfig& cfg, const Vec2d& p, const Vec2d& n, double z)
{
    return displacement_map_value(map, cfg.texture_projection, cfg.texture_size, Vec3d(p.x(), p.y(), z), Vec3d(n.x(), n.y(), 0.));
}

static std::shared_ptr<const DisplacementMap> get_displacement_map(const FuzzySkinConfig& cfg)
{
    std::string error;
    auto        map = load_displacement_map(cfg.texture, &error, false);
    if (!map)
        throw Slic3r::SlicingError("Fuzzy skin displacement map: " + error);
    return map;
}

// Unit outward normal of the segment p0p1. perp() points to the left of the direction of travel.
static Vec2d outward_normal(const Vec2d& p0p1, bool material_on_left)
{
    const Vec2d left = perp(p0p1).normalized();
    return material_on_left ? Vec2d(-left) : left;
}

// Thanks Cura developers for this function.
void fuzzy_polyline(Points& poly, bool closed, coordf_t slice_z, const FuzzySkinConfig& cfg, bool material_on_left)
{
    const bool use_map = cfg.noise_type == NoiseType::DisplacementMap;
    std::shared_ptr<const DisplacementMap> map;
    std::unique_ptr<noise::module::Module> noise;
    if (use_map)
        map = get_displacement_map(cfg);
    else
        noise = get_noise_module(cfg);

    // A displacement map is sampled at an even spacing, so the same wall gives the same texture on every layer.
    const double min_dist_between_points = use_map ? double(cfg.point_distance) : cfg.point_distance * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const double range_random_point_dist = use_map ? 0. : cfg.point_distance / 2.;
    double dist_left_over = use_map ? 0. : random_value() * (min_dist_between_points / 2.); // the distance to be traversed on the line before making the first new point
    Point* p0 = &poly.back();
    Points out;
    out.reserve(poly.size());
    for (Point &p1 : poly)
    {
        if (!closed) {
            // Skip the first point for open path
            closed = true;
            p0 = &p1;
            continue;
        }
        // 'a' is the (next) new point between p0 and p1
        Vec2d  p0p1      = (p1 - *p0).cast<double>();
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size;
            p0pa_dist += min_dist_between_points + random_value() * range_random_point_dist)
        {
            Point pa = *p0 + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            if (use_map) {
                const Vec2d n = outward_normal(p0p1, material_on_left);
                const double r = displacement_map_value(*map, cfg, unscale(pa), n, slice_z) * cfg.thickness;
                out.emplace_back(pa + (n * r).cast<coord_t>());
                continue;
            }
            double r = noise->GetValue(unscale_(pa.x()), unscale_(pa.y()), slice_z) * cfg.thickness;
            out.emplace_back(pa + (perp(p0p1).cast<double>().normalized() * r).cast<coord_t>());
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0 = &p1;
    }
    while (out.size() < 3) {
        size_t point_idx = poly.size() - 2;
        out.emplace_back(poly[point_idx]);
        if (point_idx == 0)
            break;
        -- point_idx;
    }
    if (out.size() >= 3)
        poly = std::move(out);
}

// Thanks Cura developers for this function.
void fuzzy_extrusion_line(Arachne::ExtrusionJunctions& ext_lines, coordf_t slice_z, const FuzzySkinConfig& cfg, bool material_on_left)
{
    const bool use_map = cfg.noise_type == NoiseType::DisplacementMap;
    std::shared_ptr<const DisplacementMap> map;
    std::unique_ptr<noise::module::Module> noise;
    if (use_map)
        map = get_displacement_map(cfg);
    else
        noise = get_noise_module(cfg);

    const double min_dist_between_points = use_map ? double(cfg.point_distance) : cfg.point_distance * 3. / 4.; // hardcoded: the point distance may vary between 3/4 and 5/4 the supplied value
    const double range_random_point_dist = use_map ? 0. : cfg.point_distance / 2.;
    const double min_extrusion_width = 0.01; // workaround for many print options. Need overwrite formula with the layer height parameter. The width must more than >>> layer_height * (1 - 0.25 * PI) * 1.05 <<< (last num is the coeff of overlay error case)
    double dist_left_over = use_map ? 0. : random_value() * (min_dist_between_points / 2.); // the distance to be traversed on the line before making the first new point

    auto* p0 = &ext_lines.front();
    Arachne::ExtrusionJunctions out;
    out.reserve(ext_lines.size());
    for (auto& p1 : ext_lines) {
        if (p0->p == p1.p) { // Connect endpoints.
            out.emplace_back(p1.p, p1.w, p1.perimeter_index);
            continue;
        }

        // 'a' is the (next) new point between p0 and p1
        Vec2d  p0p1 = (p1.p - p0->p).cast<double>();
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + random_value() * range_random_point_dist) {
            Point pa = p0->p + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            // Direction a positive displacement moves the point. Noise is symmetric, so it keeps the historic left side;
            // a displacement map needs the side away from the material, so white is a bump and not a dent.
            const Vec2d dir = use_map ? outward_normal(p0p1, material_on_left) : Vec2d(perp(p0p1).cast<double>().normalized());
            double r = use_map ? displacement_map_value(*map, cfg, unscale(pa), dir, slice_z) * cfg.thickness
                               : noise->GetValue(unscale_(pa.x()), unscale_(pa.y()), slice_z) * cfg.thickness;
            switch (cfg.mode) { //the curly code for testing
                case FuzzySkinMode::Displacement :
                    out.emplace_back(pa + (dir * r).cast<coord_t>(), p1.w, p1.perimeter_index);
                    break;
                case FuzzySkinMode::Extrusion :
                    out.emplace_back(pa, std::max(p1.w + r + min_extrusion_width,  min_extrusion_width), p1.perimeter_index); 
                    break;
                case FuzzySkinMode::Combined :
                    double rad = std::max(p1.w + r + min_extrusion_width,  min_extrusion_width);
                    out.emplace_back(pa + (dir * ((rad  - p1.w) / 2)).cast<coord_t>(), rad, p1.perimeter_index); //0.05 - minimum width of extruded line
                    break;
            }
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0 = &p1;
    }

    while (out.size() < 3) {
        size_t point_idx = ext_lines.size() - 2;
        out.emplace_back(ext_lines[point_idx].p, ext_lines[point_idx].w, ext_lines[point_idx].perimeter_index);
        if (point_idx == 0)
            break;
        --point_idx;
    }

    if (ext_lines.back().p == ext_lines.front().p) { // Connect endpoints.
        out.front().p = out.back().p;
        out.front().w = out.back().w;
    }

    if (out.size() >= 3)
        ext_lines = std::move(out);
}

void group_region_by_fuzzify(PerimeterGenerator& g)
{
    g.regions_by_fuzzify.clear();
    g.has_fuzzy_skin = false;
    g.has_fuzzy_hole = false;

    std::unordered_map<FuzzySkinConfig, SurfacesPtr> regions;
    for (auto region : *g.compatible_regions) {
        const auto&           region_config = region->region().config();
        // A displacement map on the whole surface is already in the slices; the walls are not moved again.
        const FuzzySkinConfig cfg{uses_surface_displacement(region_config) ? FuzzySkinType::None : region_config.fuzzy_skin.value,
                                  scaled<coord_t>(region_config.fuzzy_skin_thickness.value),
                                  scaled<coord_t>(region_config.fuzzy_skin_point_distance.value),
                                  region_config.fuzzy_skin_first_layer,
                                  region_config.fuzzy_skin_noise_type,
                                  region_config.fuzzy_skin_scale,
                                  region_config.fuzzy_skin_octaves,
                                  region_config.fuzzy_skin_persistence,
                                  region_config.fuzzy_skin_mode,
                                  region_config.fuzzy_skin_texture.value,
                                  region_config.fuzzy_skin_texture_projection,
                                  region_config.fuzzy_skin_texture_size};
        auto&                 surfaces = regions[cfg];
        for (const auto& surface : region->slices.surfaces) {
            surfaces.push_back(&surface);
        }

        if (cfg.type != FuzzySkinType::None) {
            g.has_fuzzy_skin = true;
            if (cfg.type != FuzzySkinType::External) {
                g.has_fuzzy_hole = true;
            }
        }
    }

    if (regions.size() == 1) { // optimization
        g.regions_by_fuzzify[regions.begin()->first] = {};
        return;
    }

    for (auto& it : regions) {
        g.regions_by_fuzzify[it.first] = offset_ex(it.second, ClipperSafetyOffset);
    }
}

bool should_fuzzify(const FuzzySkinConfig& config, const int layer_id, const size_t loop_idx, const bool is_contour)
{
    const auto fuzziy_type = config.type;

    if (fuzziy_type == FuzzySkinType::None) {
        return false;
    }
    if (!config.fuzzy_first_layer && layer_id <= 0) {
        // Do not fuzzy first layer unless told to
        return false;
    }

    const bool fuzzify_contours = loop_idx == 0 || fuzziy_type == FuzzySkinType::AllWalls;
    const bool fuzzify_holes    = fuzzify_contours && (fuzziy_type == FuzzySkinType::All || fuzziy_type == FuzzySkinType::AllWalls);

    return is_contour ? fuzzify_contours : fuzzify_holes;
}

bool displaced_outline(const Layer& layer, ExPolygons& out)
{
    const PrintRegionConfig* found = nullptr;
    for (const LayerRegion* layerm : layer.regions()) {
        const PrintRegionConfig& rc = layerm->region().config();
        // With the whole surface displaced, the layer's slices already carry the texture.
        if (rc.fuzzy_skin.value != FuzzySkinType::None && rc.fuzzy_skin_noise_type.value == NoiseType::DisplacementMap &&
            !rc.fuzzy_skin_texture_surface.value && !layerm->slices.empty()) {
            found = &rc;
            break;
        }
    }
    if (found == nullptr)
        return false;
    const FuzzySkinConfig cfg{found->fuzzy_skin,
                              scaled<coord_t>(found->fuzzy_skin_thickness.value),
                              scaled<coord_t>(found->fuzzy_skin_point_distance.value),
                              found->fuzzy_skin_first_layer,
                              found->fuzzy_skin_noise_type,
                              found->fuzzy_skin_scale,
                              found->fuzzy_skin_octaves,
                              found->fuzzy_skin_persistence,
                              found->fuzzy_skin_mode,
                              found->fuzzy_skin_texture.value,
                              found->fuzzy_skin_texture_projection,
                              found->fuzzy_skin_texture_size};
    if (cfg.thickness <= 0 || cfg.point_distance <= 0)
        return false;

    const int layer_id = int(layer.id());
    auto      displace = [&](Polygon& polygon, bool is_contour) {
        if (polygon.points.size() < 3 || !should_fuzzify(cfg, layer_id, 0, is_contour))
            return;
        fuzzy_polyline(polygon.points, true, layer.slice_z, cfg, polygon.is_counter_clockwise() == is_contour);
    };
    out = layer.lslices;
    for (ExPolygon& expoly : out) {
        displace(expoly.contour, true);
        for (Polygon& hole : expoly.holes)
            displace(hole, false);
    }
    return true;
}

Polygon apply_fuzzy_skin(const Polygon& polygon, const PerimeterGenerator& perimeter_generator, const size_t loop_idx, const bool is_contour)
{
    Polygon fuzzified;

    const auto  slice_z = perimeter_generator.slice_z;
    // The material is inside a contour and outside a hole, whichever way the loop runs.
    const bool  material_on_left = polygon.is_counter_clockwise() == is_contour;
    const auto& regions = perimeter_generator.regions_by_fuzzify;
    if (regions.size() == 1) { // optimization
        const auto& config  = regions.begin()->first;
        const bool  fuzzify = should_fuzzify(config, perimeter_generator.layer_id, loop_idx, is_contour);
        if (!fuzzify) {
            return polygon;
        }

        fuzzified = polygon;
        fuzzy_polyline(fuzzified.points, true, slice_z, config, material_on_left);
        return fuzzified;
    }

    // Find all affective regions
    std::vector<std::pair<const FuzzySkinConfig&, const ExPolygons&>> fuzzified_regions;
    fuzzified_regions.reserve(regions.size());
    for (const auto& region : regions) {
        if (should_fuzzify(region.first, perimeter_generator.layer_id, loop_idx, is_contour)) {
            fuzzified_regions.emplace_back(region.first, region.second);
        }
    }
    if (fuzzified_regions.empty()) {
        return polygon;
    }

#ifdef DEBUG_FUZZY
    {
        int i = 0;
        for (const auto& r : fuzzified_regions) {
            BoundingBox bbox = get_extents(perimeter_generator.slices->surfaces);
            bbox.offset(scale_(1.));
            ::Slic3r::SVG svg(debug_out_path("fuzzy_traverse_loops_%d_%d_%d_region_%d.svg", perimeter_generator.layer_id,
                                             loop.is_contour ? 0 : 1, loop.depth, i)
                                  .c_str(),
                              bbox);
            svg.draw_outline(perimeter_generator.slices->surfaces);
            svg.draw_outline(loop.polygon, "green");
            svg.draw(r.second, "red", 0.5);
            svg.draw_outline(r.second, "red");
            svg.Close();
            i++;
        }
    }
#endif

    // Split the loops into lines with different config, and fuzzy them separately
    fuzzified = polygon;
    for (const auto& r : fuzzified_regions) {
        const auto splitted = Algorithm::split_line(fuzzified, r.second, true);
        if (splitted.empty()) {
            // No intersection, skip
            continue;
        }

        // Fuzzy splitted polygon
        if (std::all_of(splitted.begin(), splitted.end(), [](const Algorithm::SplitLineJunction& j) { return j.clipped; })) {
            // The entire polygon is fuzzified
            fuzzy_polyline(fuzzified.points, true, slice_z, r.first, material_on_left);
        } else {
            Points segment;
            segment.reserve(splitted.size());
            fuzzified.points.clear();

            const auto fuzzy_current_segment = [&segment, &fuzzified, &r, slice_z, material_on_left]() {
                fuzzified.points.push_back(segment.front());
                const auto back = segment.back();
                fuzzy_polyline(segment, false, slice_z, r.first, material_on_left);
                fuzzified.points.insert(fuzzified.points.end(), segment.begin(), segment.end());
                fuzzified.points.push_back(back);
                segment.clear();
            };

            for (const auto& p : splitted) {
                if (p.clipped) {
                    segment.push_back(p.p);
                } else {
                    if (segment.empty()) {
                        fuzzified.points.push_back(p.p);
                    } else {
                        segment.push_back(p.p);
                        fuzzy_current_segment();
                    }
                }
            }
            if (!segment.empty()) {
                // Close the loop
                segment.push_back(splitted.front().p);
                fuzzy_current_segment();
            }
        }
    }

    return fuzzified;
}

void apply_fuzzy_skin(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, const bool is_contour)
{
    const auto  slice_z = perimeter_generator.slice_z;
    // The material is inside a contour and outside a hole. An open line has material on both sides; keep the left.
    bool material_on_left = true;
    if (extrusion->is_closed && extrusion->junctions.size() >= 3) {
        double area2 = 0.;
        const auto& js = extrusion->junctions;
        for (size_t i = 0, j = js.size() - 1; i < js.size(); j = i++)
            area2 += double(js[j].p.x()) * double(js[i].p.y()) - double(js[i].p.x()) * double(js[j].p.y());
        material_on_left = (area2 > 0.) == is_contour;
    }
    const auto& regions = perimeter_generator.regions_by_fuzzify;
    if (regions.size() == 1) { // optimization
        const auto& config  = regions.begin()->first;
        const bool  fuzzify = should_fuzzify(config, perimeter_generator.layer_id, extrusion->inset_idx, is_contour);
        if (fuzzify)
            fuzzy_extrusion_line(extrusion->junctions, slice_z, config, material_on_left);
    } else {
        // Find all affective regions
        std::vector<std::pair<const FuzzySkinConfig&, const ExPolygons&>> fuzzified_regions;
        fuzzified_regions.reserve(regions.size());
        for (const auto& region : regions) {
            if (should_fuzzify(region.first, perimeter_generator.layer_id, extrusion->inset_idx, is_contour)) {
                fuzzified_regions.emplace_back(region.first, region.second);
            }
        }
        if (!fuzzified_regions.empty()) {
            // Split the loops into lines with different config, and fuzzy them separately
            for (const auto& r : fuzzified_regions) {
                const auto splitted = Algorithm::split_line(*extrusion, r.second, false);
                if (splitted.empty()) {
                    // No intersection, skip
                    continue;
                }

                // Fuzzy splitted extrusion
                if (std::all_of(splitted.begin(), splitted.end(), [](const Algorithm::SplitLineJunction& j) { return j.clipped; })) {
                    // The entire polygon is fuzzified
                    fuzzy_extrusion_line(extrusion->junctions, slice_z, r.first, material_on_left);
                } else {
                    const auto                              current_ext = extrusion->junctions;
                    std::vector<Arachne::ExtrusionJunction> segment;
                    segment.reserve(current_ext.size());
                    extrusion->junctions.clear();

                    const auto fuzzy_current_segment = [&segment, &extrusion, &r, slice_z, material_on_left]() {
                        extrusion->junctions.push_back(segment.front());
                        const auto back = segment.back();
                        fuzzy_extrusion_line(segment, slice_z, r.first, material_on_left);
                        extrusion->junctions.insert(extrusion->junctions.end(), segment.begin(), segment.end());
                        extrusion->junctions.push_back(back);
                        segment.clear();
                    };

                    const auto to_ex_junction = [&current_ext](const Algorithm::SplitLineJunction& j) -> Arachne::ExtrusionJunction {
                        Arachne::ExtrusionJunction res = current_ext[j.get_src_index()];
                        if (!j.is_src()) {
                            res.p = j.p;
                        }
                        return res;
                    };

                    for (const auto& p : splitted) {
                        if (p.clipped) {
                            segment.push_back(to_ex_junction(p));
                        } else {
                            if (segment.empty()) {
                                extrusion->junctions.push_back(to_ex_junction(p));
                            } else {
                                segment.push_back(to_ex_junction(p));
                                fuzzy_current_segment();
                            }
                        }
                    }
                    if (!segment.empty()) {
                        fuzzy_current_segment();
                    }
                }
            }
        }
    }
}

} // namespace Slic3r::Feature::FuzzySkin
