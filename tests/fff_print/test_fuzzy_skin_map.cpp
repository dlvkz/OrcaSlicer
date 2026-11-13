#include "catch_main.hpp"

#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Feature/FuzzySkin/FuzzySkin.hpp"
#include "libslic3r/Feature/FuzzySkin/SurfaceDisplacement.hpp"
#include "libslic3r/Feature/FuzzySkin/SurfacePattern.hpp"
#include "libslic3r/Feature/FuzzySkin/TexturePatterns.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Polygon.hpp"

using namespace Slic3r;
using namespace Slic3r::Feature::FuzzySkin;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::string temp_png()
{
    return (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("fuzzy-map-%%%%-%%%%.png")).string();
}

// Grey image, cols x rows, value per column.
std::string write_columns(const std::vector<uint8_t>& column_values, size_t rows)
{
    std::vector<uint8_t> px;
    for (size_t r = 0; r < rows; ++r)
        px.insert(px.end(), column_values.begin(), column_values.end());
    const std::string path = temp_png();
    REQUIRE(png::write_gray_to_file(path, column_values.size(), rows, px));
    return path;
}

FuzzySkinConfig map_config(const std::string& texture, FuzzySkinTextureProjection projection, double size, double radius = 0.)
{
    TextureMapping mapping;
    mapping.projection = projection;
    mapping.size       = size;
    mapping.radius     = radius;
    return FuzzySkinConfig{FuzzySkinType::External, scaled<coord_t>(0.5), scaled<coord_t>(0.2), true, NoiseType::DisplacementMap,
                           1., 4, 0.5, FuzzySkinMode::Displacement, texture, FuzzySkinTexturePattern::Image, mapping};
}

// A 20 mm square around the origin, counter-clockwise.
Points square() { return {Point::new_scale(-10, -10), Point::new_scale(10, -10), Point::new_scale(10, 10), Point::new_scale(-10, 10)}; }

// How far a point lies outside the 20 mm square, measured from the side it is closest to (negative inside).
double outside(const Point& p)
{
    const Vec2d q = unscale(p);
    return std::max(std::abs(q.x()), std::abs(q.y())) - 10.;
}

// Within the reach of a 20 mm square's corner: a corner moved along one side's normal by `reach` lands on the next side.
bool near_corner(const Point& p, double reach = 0.5)
{
    const Vec2d q = unscale(p);
    return std::abs(q.x()) > 9.9 - reach && std::abs(q.y()) > 9.9 - reach;
}

} // namespace

TEST_CASE("Fuzzy skin displacement map pushes white out and black in", "[FuzzySkin]")
{
    const std::string white = write_columns(std::vector<uint8_t>(8, 255), 8);
    const std::string black = write_columns(std::vector<uint8_t>(8, 0), 8);

    SECTION("white, material inside the loop: every point 0.5 mm outside") {
        Points pts = square();
        fuzzy_polyline(pts, true, 1., map_config(white, FuzzySkinTextureProjection::Box, 20.), true);
        REQUIRE(pts.size() > 300); // 80 mm at 0.2 mm
        for (const Point& p : pts)
            REQUIRE_THAT(outside(p), WithinAbs(0.5, 0.01));
    }
    SECTION("white, material outside the loop (a hole): every point 0.5 mm inside") {
        Points pts = square();
        fuzzy_polyline(pts, true, 1., map_config(white, FuzzySkinTextureProjection::Box, 20.), false);
        for (const Point& p : pts)
            if (!near_corner(p)) // a corner pulled in along one side's normal lands on the next side
                REQUIRE_THAT(outside(p), WithinAbs(-0.5, 0.01));
    }
    SECTION("black, material inside: every point 0.5 mm inside") {
        Points pts = square();
        fuzzy_polyline(pts, true, 1., map_config(black, FuzzySkinTextureProjection::Box, 20.), true);
        for (const Point& p : pts)
            if (!near_corner(p)) // a corner pulled in along one side's normal lands on the next side
                REQUIRE_THAT(outside(p), WithinAbs(-0.5, 0.01));
    }
    SECTION("offset 0 only cuts in: white stays on the surface, black goes twice the depth in") {
        for (const auto& [texture, expected] : { std::make_pair(white, 0.), std::make_pair(black, -1.) }) {
            FuzzySkinConfig cfg = map_config(texture, FuzzySkinTextureProjection::Box, 20.);
            cfg.texture_mapping.offset = 0.;
            Points pts = square();
            fuzzy_polyline(pts, true, 1., cfg, true);
            for (const Point& p : pts)
                if (!near_corner(p, 1.))
                    REQUIRE_THAT(outside(p), WithinAbs(expected, 0.01));
        }
    }
    SECTION("offset 1 only stands out: black stays on the surface, white goes twice the depth out") {
        for (const auto& [texture, expected] : { std::make_pair(black, 0.), std::make_pair(white, 1.) }) {
            FuzzySkinConfig cfg = map_config(texture, FuzzySkinTextureProjection::Box, 20.);
            cfg.texture_mapping.offset = 1.;
            Points pts = square();
            fuzzy_polyline(pts, true, 1., cfg, true);
            for (const Point& p : pts)
                REQUIRE_THAT(outside(p), WithinAbs(expected, 0.01));
        }
    }
    SECTION("the same loop gives the same points every time") {
        Points a = square(), b = square();
        fuzzy_polyline(a, true, 1., map_config(white, FuzzySkinTextureProjection::Box, 20.), true);
        fuzzy_polyline(b, true, 1., map_config(white, FuzzySkinTextureProjection::Box, 20.), true);
        REQUIRE(bool(a == b));
    }
    boost::filesystem::remove(white);
    boost::filesystem::remove(black);
}

TEST_CASE("Fuzzy skin box projection is not mirrored on any side", "[FuzzySkin]")
{
    // Left half of the image black, right half white. One tile is one side of the square, centred on it, so on every
    // side the half to the right of a viewer outside the square bulges and the left half is sunk.
    std::vector<uint8_t> cols(64, 0);
    std::fill(cols.begin() + 32, cols.end(), 255);
    const std::string path = write_columns(cols, 4);

    Points pts = square();
    fuzzy_polyline(pts, true, 1., map_config(path, FuzzySkinTextureProjection::Box, 20.), true);
    size_t checked = 0;
    for (const Point& p : pts) {
        const Vec2d q = unscale(p);
        double right; // the coordinate along the side, increasing to the viewer's right
        if (q.y() < -10. + 1. && std::abs(q.x()) < 8.)       right = q.x();  // front, seen from -Y
        else if (q.x() > 10. - 1. && std::abs(q.y()) < 8.)   right = q.y();  // seen from +X
        else if (q.y() > 10. - 1. && std::abs(q.x()) < 8.)   right = -q.x(); // seen from +Y
        else if (q.x() < -10. + 1. && std::abs(q.y()) < 8.)  right = -q.y(); // seen from -X
        else continue;
        if (std::abs(right) < 1.)
            continue; // the blend between the halves
        ++checked;
        if (right > 0.)
            REQUIRE(outside(p) > 0.4);
        else
            REQUIRE(outside(p) < -0.4);
    }
    REQUIRE(checked > 250);
    boost::filesystem::remove(path);
}

TEST_CASE("Fuzzy skin cylindrical projection centres the image at the front", "[FuzzySkin]")
{
    // A white band in the middle of the image, black elsewhere: only the front of a circle, around -Y, bulges.
    std::vector<uint8_t> cols(64, 0);
    std::fill(cols.begin() + 24, cols.begin() + 40, 255);
    const std::string path = write_columns(cols, 4);

    Points circle;
    for (int i = 0; i < 360; ++i)
        circle.emplace_back(Point::new_scale(15. * std::cos(i * M_PI / 180.), 15. * std::sin(i * M_PI / 180.)));
    fuzzy_polyline(circle, true, 1., map_config(path, FuzzySkinTextureProjection::Cylindrical, 2. * M_PI * 15., 15.), true);
    for (const Point& p : circle) {
        const Vec2d  q   = unscale(p);
        const double deg = std::atan2(q.y(), q.x()) * 180. / M_PI; // -90 is the front
        if (std::abs(deg + 90.) < 30.)
            REQUIRE(q.norm() > 15.4);
        else if (std::abs(deg + 90.) > 60.)
            REQUIRE(q.norm() < 14.6);
    }
    boost::filesystem::remove(path);
}

TEST_CASE("Fuzzy skin displacement map loading", "[FuzzySkin]")
{
    std::string error;
    SECTION("a missing file is an error, not a crash") {
        REQUIRE(load_displacement_map("does/not/exist.png", &error, true) == nullptr);
        REQUIRE(error.find("does/not/exist.png") != std::string::npos);
    }
    SECTION("no file set is an error") {
        REQUIRE(load_displacement_map("", &error, true) == nullptr);
        REQUIRE(!error.empty());
    }
    SECTION("a colour image is read as brightness") {
        const std::string path = temp_png();
        const std::vector<uint8_t> rgb{255, 255, 255, 0, 0, 0, 128, 128, 128, 255, 255, 255};
        REQUIRE(png::write_rgb_to_file(path, 2, 2, rgb));
        auto map = load_displacement_map(path, &error, true);
        REQUIRE(map != nullptr);
        REQUIRE(map->cols == 2);
        REQUIRE(map->rows == 2);
        REQUIRE_THAT(map->height[0], WithinAbs(1.f, 0.01));
        REQUIRE_THAT(map->height[1], WithinAbs(0.f, 0.01));
        REQUIRE_THAT(map->height[2], WithinAbs(128.f / 255.f, 0.01));
        boost::filesystem::remove(path);
    }
    SECTION("an edited file is read again") {
        const std::string path = write_columns({0, 0}, 2);
        auto first = load_displacement_map(path, &error, true);
        REQUIRE(first != nullptr);
        REQUIRE_THAT(first->height[0], WithinAbs(0.f, 1e-4));
        // make sure the modification time changes
        boost::filesystem::last_write_time(path, boost::filesystem::last_write_time(path) - 10);
        const std::vector<uint8_t> px(4, 255);
        REQUIRE(png::write_gray_to_file(path, 2, 2, px));
        boost::filesystem::last_write_time(path, boost::filesystem::last_write_time(path) + 20);
        auto second = load_displacement_map(path, &error, true);
        REQUIRE(second != nullptr);
        REQUIRE_THAT(second->height[0], WithinAbs(1.f, 1e-4));
        boost::filesystem::remove(path);
    }
}

TEST_CASE("Marching squares outlines where a field is positive", "[FuzzySkin]")
{
    // A disc of radius 5 and a ring from 2 to 5, on a 0.1 mm grid around the origin.
    const double step = 0.1, x0 = -6., y0 = -6.;
    const size_t n    = 121;
    std::vector<float> disc(n * n), ring(n * n);
    for (size_t r = 0; r < n; ++r)
        for (size_t c = 0; c < n; ++c) {
            const double x = x0 + double(c) * step, y = y0 + double(r) * step, d = std::hypot(x, y);
            const bool   border = r == 0 || c == 0 || r + 1 == n || c + 1 == n;
            disc[r * n + c] = border ? -1.f : float(5. - d);
            ring[r * n + c] = border ? -1.f : float(std::min(5. - d, d - 2.));
        }
    SECTION("a disc is one counter-clockwise contour of the right area") {
        const Polygons p = positive_region_contours(disc, n, n, x0, y0, step);
        REQUIRE(p.size() == 1);
        REQUIRE(p.front().is_counter_clockwise());
        REQUIRE_THAT(unscaled(unscaled(p.front().area())), WithinRel(M_PI * 25., 0.005));
    }
    SECTION("a ring is a contour and a clockwise hole") {
        const ExPolygons ex = union_ex(positive_region_contours(ring, n, n, x0, y0, step));
        REQUIRE(ex.size() == 1);
        REQUIRE(ex.front().holes.size() == 1);
        REQUIRE_THAT(unscaled(unscaled(ex.front().area())), WithinRel(M_PI * (25. - 4.), 0.01));
    }
}

TEST_CASE("Whole surface displacement moves walls, tops and leaves the bed face", "[FuzzySkin]")
{
    // A 20 mm cube, bottom on the bed, sliced at the first layer, the middle, just under and just over the top.
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const std::vector<float>   zs{0.1f, 10.f, 19.8f, 20.2f};
    MeshSlicingParamsEx        params;
    std::vector<ExPolygons>    layers = slice_mesh_ex(cube, zs, params);
    REQUIRE(layers[3].empty());
    auto area = [](const ExPolygons& ex) { double a = 0.; for (const ExPolygon& e : ex) a += e.area(); return unscaled(unscaled(a)); };

    auto run = [&](uint8_t grey) {
        const std::string path = write_columns(std::vector<uint8_t>(8, grey), 8);
        std::string       error;
        SurfaceDisplacement sd{load_displacement_map(path, &error, true), TextureMapping{FuzzySkinTextureProjection::Box, 20.}, 0.5, 0.2, true};
        REQUIRE(sd.map != nullptr);
        std::vector<ExPolygons> out = layers;
        displace_slices(cube, Transform3d::Identity(), zs, out, sd, [] {});
        boost::filesystem::remove(path);
        return out;
    };

    SECTION("white: the walls move out, the top rises into the layer above") {
        const std::vector<ExPolygons> out = run(255);
        REQUIRE_THAT(area(out[1]), WithinRel(21. * 21., 0.01));   // 0.5 mm out on every side
        REQUIRE_THAT(area(out[2]), WithinRel(21. * 21., 0.01));
        REQUIRE(area(out[3]) > 399.);                             // the top, now at 20.5, reaches this layer
        REQUIRE(area(out[3]) < 442.);
        REQUIRE_THAT(area(out[0]), WithinRel(21. * 21., 0.01));   // the first layer's walls too, but not below the bed
    }
    SECTION("black: the walls move in, the top sinks below the layer under it, the bed face stays") {
        const std::vector<ExPolygons> out = run(0);
        REQUIRE_THAT(area(out[1]), WithinRel(19. * 19., 0.01));
        REQUIRE(area(out[2]) < 1.);                               // the top, now at 19.5, is below 19.8
        REQUIRE(out[3].empty());
        REQUIRE_THAT(area(out[0]), WithinRel(19. * 19., 0.01));   // walls in, nothing cut from the bottom
    }
    SECTION("mid grey changes nothing") {
        const std::vector<ExPolygons> out = run(128);
        for (size_t i = 0; i < 3; ++i)
            REQUIRE_THAT(area(out[i]), WithinRel(400., 0.01));
        REQUIRE(area(out[3]) < 1.);
    }
}

namespace {

const std::vector<FuzzySkinTexturePattern> all_patterns{FuzzySkinTexturePattern::Knurl, FuzzySkinTexturePattern::Hexagons,
                                                        FuzzySkinTexturePattern::Bricks, FuzzySkinTexturePattern::Ribs,
                                                        FuzzySkinTexturePattern::Waves, FuzzySkinTexturePattern::Bumps,
                                                        FuzzySkinTexturePattern::Stone, FuzzySkinTexturePattern::Noise};

TextureMapping mapping(FuzzySkinTextureProjection projection, double size)
{
    TextureMapping m;
    m.projection = projection;
    m.size       = size;
    return m;
}

} // namespace

TEST_CASE("Built-in displacement patterns tile seamlessly over the full range", "[FuzzySkin]")
{
    for (FuzzySkinTexturePattern pattern : all_patterns) {
        DYNAMIC_SECTION("pattern " << int(pattern)) {
            const auto map = pattern_displacement_map(pattern);
            REQUIRE(map != nullptr);
            REQUIRE(map->cols >= 256);
            REQUIRE(map->rows >= 256);
            REQUIRE(pattern_displacement_map(pattern) == map); // generated once
            const auto [lo, hi] = std::minmax_element(map->height.begin(), map->height.end());
            REQUIRE_THAT(*lo, WithinAbs(0., 1e-6));
            REQUIRE_THAT(*hi, WithinAbs(1., 1e-6));
            // The step from the last column to the first (and the last row to the first) is no bigger than the steps
            // inside the image: no seam where the tiles meet.
            auto   h     = [&](size_t r, size_t c) { return double(map->height[r * map->cols + c]); };
            double inner = 0., seam = 0.;
            for (size_t r = 0; r < map->rows; ++r)
                for (size_t c = 0; c + 1 < map->cols; ++c)
                    inner = std::max(inner, std::abs(h(r, c + 1) - h(r, c)));
            for (size_t c = 0; c < map->cols; ++c)
                for (size_t r = 0; r + 1 < map->rows; ++r)
                    inner = std::max(inner, std::abs(h(r + 1, c) - h(r, c)));
            for (size_t r = 0; r < map->rows; ++r)
                seam = std::max(seam, std::abs(h(r, 0) - h(r, map->cols - 1)));
            for (size_t c = 0; c < map->cols; ++c)
                seam = std::max(seam, std::abs(h(0, c) - h(map->rows - 1, c)));
            REQUIRE(seam <= inner * 1.01 + 1e-6);
        }
    }
}

TEST_CASE("A built-in pattern needs no image file", "[FuzzySkin]")
{
    std::string error;
    REQUIRE(load_texture(FuzzySkinTexturePattern::Knurl, "", &error, true) == pattern_displacement_map(FuzzySkinTexturePattern::Knurl));
    REQUIRE(load_texture(FuzzySkinTexturePattern::Image, "", &error, true) == nullptr);
    REQUIRE(!error.empty());
}

TEST_CASE("Displacement map projections", "[FuzzySkin]")
{
    // Ribs vary across the image only: 1 in the middle of a tile, 0 at its edges.
    const auto             ribs = pattern_displacement_map(FuzzySkinTexturePattern::Ribs);
    const DisplacementMap& map  = *ribs;
    const Vec3d            front(0., -1., 0.), back(0., 1., 0.), up(0., 0., 1.), right(1., 0., 0.);

    SECTION("planar from the front goes straight through, whatever the depth or the face") {
        const TextureMapping m = mapping(FuzzySkinTextureProjection::PlanarFront, 10.);
        for (double x : {-3., 0., 2.5, 4.}) {
            const double v = displacement_map_value(map, m, Vec3d(x, -7., 3.), front);
            REQUIRE_THAT(displacement_map_value(map, m, Vec3d(x, 5., 8.), back), WithinAbs(v, 1e-9));
            REQUIRE_THAT(displacement_map_value(map, m, Vec3d(x, 0., 1.), up), WithinAbs(v, 1e-9));
        }
        REQUIRE_THAT(displacement_map_value(map, m, Vec3d(0., -7., 3.), front), WithinAbs(1., 1e-3));
        REQUIRE_THAT(displacement_map_value(map, m, Vec3d(5., -7., 3.), front), WithinAbs(-1., 2e-2));
    }
    SECTION("planar from the top and from the side") {
        const TextureMapping top = mapping(FuzzySkinTextureProjection::PlanarTop, 10.);
        REQUIRE_THAT(displacement_map_value(map, top, Vec3d(0., 3., 1.), right), WithinAbs(1., 1e-3));
        REQUIRE_THAT(displacement_map_value(map, top, Vec3d(5., -2., 9.), front), WithinAbs(-1., 2e-2));
        const TextureMapping side = mapping(FuzzySkinTextureProjection::PlanarSide, 10.);
        REQUIRE_THAT(displacement_map_value(map, side, Vec3d(4., 0., 2.), right), WithinAbs(1., 1e-3));
        REQUIRE_THAT(displacement_map_value(map, side, Vec3d(-4., 5., 2.), right), WithinAbs(-1., 2e-2));
    }
    SECTION("rotation turns the image: ribs at 90 degrees run across") {
        TextureMapping m = mapping(FuzzySkinTextureProjection::PlanarFront, 10.);
        m.rotation       = 90.;
        // The ribs now vary with height, not across: crests 10 and 20 mm up, grooves at 5 and 15 mm.
        for (double x : {-4., 0., 3.}) {
            REQUIRE_THAT(displacement_map_value(map, m, Vec3d(x, -5., 10.), front), WithinAbs(1., 1e-3));
            REQUIRE_THAT(displacement_map_value(map, m, Vec3d(x, -5., 20.), front), WithinAbs(1., 1e-3));
            REQUIRE_THAT(displacement_map_value(map, m, Vec3d(x, -5., 5.), front), WithinAbs(-1., 2e-2));
            REQUIRE_THAT(displacement_map_value(map, m, Vec3d(x, -5., 15.), front), WithinAbs(-1., 2e-2));
        }
    }
    SECTION("invert swaps bumps and dents") {
        TextureMapping m = mapping(FuzzySkinTextureProjection::Box, 10.);
        for (double x : {-4., 0., 1.5, 3.}) {
            m.invert       = false;
            const double v = displacement_map_value(map, m, Vec3d(x, -5., 2.), front);
            m.invert       = true;
            REQUIRE_THAT(displacement_map_value(map, m, Vec3d(x, -5., 2.), front), WithinAbs(-v, 1e-9));
        }
    }
    SECTION("cylindrical repeats a whole number of tiles around and closes at the back") {
        TextureMapping m = mapping(FuzzySkinTextureProjection::Cylindrical, 10.);
        m.radius         = 15.;
        const int n      = tiles_around(m);
        REQUIRE(n == 9); // 94.2 mm around / 10 mm
        auto at_angle = [&](double a) {
            const Vec3d p(15. * std::cos(a), 15. * std::sin(a), 4.);
            return displacement_map_value(map, m, p, Vec3d(std::cos(a), std::sin(a), 0.));
        };
        REQUIRE_THAT(at_angle(-0.5 * M_PI), WithinAbs(1., 1e-3)); // a crest faces the front
        for (double a = -M_PI; a < M_PI; a += 0.05)
            REQUIRE_THAT(at_angle(a + 2. * M_PI / n), WithinAbs(at_angle(a), 1e-6));
        // Across the seam at the back (+Y): no jump.
        REQUIRE_THAT(at_angle(0.5 * M_PI - 1e-4), WithinAbs(at_angle(0.5 * M_PI + 1e-4), 1e-2));
    }
    SECTION("spherical puts the middle of a tile on the equator at the front") {
        const auto     bumps = pattern_displacement_map(FuzzySkinTexturePattern::Bumps);
        TextureMapping m     = mapping(FuzzySkinTextureProjection::Spherical, 10.);
        m.radius             = 15.;
        m.center_z           = 15.;
        REQUIRE_THAT(displacement_map_value(*bumps, m, Vec3d(0., -15., 15.), front), WithinAbs(1., 1e-2));
        // Half a tile up the meridian is between bumps.
        const double r = tiles_around(m) * 10. / (2. * M_PI), lat = 5. / r;
        REQUIRE_THAT(displacement_map_value(*bumps, m, Vec3d(0., -15. * std::cos(lat), 15. + 15. * std::sin(lat)), front),
                     WithinAbs(-1., 1e-2));
    }
}

TEST_CASE("Whole surface displacement with a built-in pattern", "[FuzzySkin]")
{
    // Bumps over a cube's walls: on the layers through the middle of a row of bumps the walls move out where the bumps
    // are, never more than the depth. The cube spans 0..20 mm, so with 5 mm tiles the bumps sit at multiples of 5 mm
    // across and at 2.5, 7.5, ... mm up.
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const std::vector<float>   zs{2.5f, 7.5f};
    MeshSlicingParamsEx        params;
    std::vector<ExPolygons>    layers = slice_mesh_ex(cube, zs, params);
    TextureMapping             m;
    m.projection = FuzzySkinTextureProjection::Box;
    m.size       = 5.;
    SurfaceDisplacement     sd{pattern_displacement_map(FuzzySkinTexturePattern::Bumps), m, 0.4, 0.1, true};
    std::vector<ExPolygons> out = layers;
    displace_slices(cube, Transform3d::Identity(), zs, out, sd, [] {});
    for (const ExPolygons& layer : out) {
        REQUIRE(layer.size() == 1);
        const BoundingBox bb = get_extents(layer.front());
        REQUIRE(unscaled(bb.size().x()) > 20.5);
        REQUIRE(unscaled(bb.size().x()) < 20.81);
    }
}

namespace {
std::vector<Vec3d> to_vec3d(const indexed_triangle_set& its)
{
    std::vector<Vec3d> out;
    for (const stl_vertex& v : its.vertices)
        out.emplace_back(v.cast<double>());
    return out;
}
} // namespace

TEST_CASE("Patterns laid on the surface are even, seamless and do not depend on where the part is", "[FuzzySkin]")
{
    // A 26 x 16 mm cylinder, like the sampler coupons: cells must cover the side and the top at the same spacing.
    const indexed_triangle_set its   = its_make_cylinder(13., 16.);
    const std::vector<Vec3d>   verts = to_vec3d(its);
    const double               size  = 4.;
    const SurfacePattern       cells(FuzzySkinTexturePattern::Hexagons, size, verts, its.indices);
    const double               area     = 2. * M_PI * 13. * 16. + 2. * M_PI * 13. * 13.;
    const double               expected = area / (0.866 * size * size);
    const size_t               n        = cells.sites().size();
    REQUIRE(double(n) > 0.8 * expected);
    REQUIRE(double(n) < 1.25 * expected);

    SECTION("sites sit about one size apart everywhere, top and side alike")
    {
        double sum = 0., closest = 1e9;
        size_t on_caps = 0;
        for (size_t i = 0; i < n; ++i) {
            const Vec3d p    = cells.sites()[i].position.cast<double>();
            double      best = 1e9;
            for (size_t j = 0; j < n; ++j)
                if (j != i)
                    best = std::min(best, (cells.sites()[j].position.cast<double>() - p).norm());
            sum += best;
            closest = std::min(closest, best);
            if (p.z() > 16. - 1e-3 || p.z() < 1e-3)
                ++on_caps;
        }
        REQUIRE_THAT(sum / double(n), WithinRel(size, 0.15));
        REQUIRE(closest > 0.5 * size);
        // The top and the bottom hold their share of the cells (45% of the area).
        REQUIRE(double(on_caps) > 0.35 * double(n));
        REQUIRE(double(on_caps) < 0.55 * double(n));
    }

    SECTION("the height changes smoothly over the rim, where a projection would cut the pattern")
    {
        double worst = 0.;
        for (int k = 0; k < 360; ++k) {
            const double a = k * M_PI / 180.;
            const Vec3d  top(std::cos(a) * (13. - 0.02), std::sin(a) * (13. - 0.02), 16.);
            const Vec3d  side(std::cos(a) * 13., std::sin(a) * 13., 16. - 0.02);
            worst = std::max(worst, std::abs(cells.value(top) - cells.value(side)));
        }
        REQUIRE(worst < 0.2);
    }

    SECTION("the same mesh moved elsewhere gets the same pattern")
    {
        const Vec3d        shift(37.3, -12.1, 5.7);
        std::vector<Vec3d> moved = verts;
        for (Vec3d& v : moved)
            v += shift;
        const SurfacePattern other(FuzzySkinTexturePattern::Hexagons, size, moved, its.indices);
        REQUIRE(other.sites().size() == n);
        double worst = 0.;
        for (int k = 0; k < 500; ++k) {
            const double a = k * 0.731, z = std::fmod(k * 0.37, 16.);
            const Vec3d  p(std::cos(a) * 13., std::sin(a) * 13., z);
            worst = std::max(worst, std::abs(cells.value(p) - other.value(p + shift)));
        }
        REQUIRE(worst < 1e-4);
    }

    SECTION("bumps peak on their sites and are flat between them")
    {
        const SurfacePattern bumps(FuzzySkinTexturePattern::Bumps, size, verts, its.indices);
        REQUIRE(!bumps.sites().empty());
        for (const auto& s : bumps.sites())
            REQUIRE_THAT(bumps.value(s.position.cast<double>()), WithinAbs(1., 1e-6));
    }

    SECTION("noise spans the full range and needs no sites")
    {
        const SurfacePattern noise(FuzzySkinTexturePattern::Noise, 12., verts, its.indices);
        REQUIRE(noise.sites().empty());
        double lo = 1., hi = 0.;
        for (int k = 0; k < 20000; ++k) {
            const Vec3d  p(std::fmod(k * 0.6180339, 50.), std::fmod(k * 0.4142135, 50.), std::fmod(k * 0.7320508, 50.));
            const double v = noise.value(p);
            REQUIRE(v >= 0.);
            REQUIRE(v <= 1.);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        REQUIRE(lo < 0.15);
        REQUIRE(hi > 0.85);
    }

    SECTION("every pattern can be laid on the surface; images and stripes by charts")
    {
        REQUIRE(SurfacePattern::supports(FuzzySkinTexturePattern::Stone));
        REQUIRE(SurfacePattern::supports(FuzzySkinTexturePattern::Knurl));
        REQUIRE(SurfacePattern::supports(FuzzySkinTexturePattern::Image));
        REQUIRE_FALSE(SurfacePattern::uses_charts(FuzzySkinTexturePattern::Hexagons));
        REQUIRE_FALSE(SurfacePattern::uses_charts(FuzzySkinTexturePattern::Noise));
        REQUIRE(SurfacePattern::uses_charts(FuzzySkinTexturePattern::Bricks));
        REQUIRE(SurfacePattern::uses_charts(FuzzySkinTexturePattern::Image));
    }
}

TEST_CASE("Images laid on the surface by charts continue across the charts", "[FuzzySkin]")
{
    std::string error;
    const auto  map = load_texture(FuzzySkinTexturePattern::Bricks, "", &error, false);
    REQUIRE(map != nullptr);
    const double size = 10.;

    SECTION("on a flat face the charts agree: the image repeats every tile width along its own direction")
    {
        // The top of a 60 mm box: one plane, so every chart has the same frame and the offsets add up exactly.
        const indexed_triangle_set its = its_make_cube(60., 60., 10.);
        SurfacePattern             sp(FuzzySkinTexturePattern::Bricks, size, to_vec3d(its), its.indices);
        sp.set_map(map, 0.);
        REQUIRE(!sp.sites().empty());
        const Vec3d up = Vec3d::UnitZ();
        // The image's right on the top, from a site there.
        Vec3d right = Vec3d::Zero();
        for (const auto& s : sp.sites())
            if (s.position.z() > 9.99f && s.normal.z() > 0.99f) {
                right = s.right.cast<double>();
                break;
            }
        REQUIRE(right.norm() > 0.5);
        double worst = 0.;
        for (int k = 0; k < 400; ++k) {
            const Vec3d p(15. + std::fmod(k * 6.1803, 25.), 15. + std::fmod(k * 4.1421, 25.), 10.);
            worst = std::max(worst, std::abs(sp.value(p, up) - sp.value(p + size * right, up)));
        }
        REQUIRE(worst < 0.02);
    }

    SECTION("round a cylinder the image advances by the arc length, apart from where the chains meet")
    {
        // Along a circle round a 20 mm radius cylinder, the value one tile width further on (by arc length) matches
        // nearly everywhere: the charts carry the image round instead of restarting it.
        const double               r   = 20.;
        const indexed_triangle_set its = its_make_cylinder(r, 40., 2. * M_PI / 360.);
        SurfacePattern             sp(FuzzySkinTexturePattern::Bricks, size, to_vec3d(its), its.indices);
        sp.set_map(map, 0.);
        int       matching = 0;
        const int steps    = 360;
        for (int k = 0; k < steps; ++k) {
            const double a0 = 2. * M_PI * k / steps, a1 = a0 + size / r;
            const Vec3d  n0(std::cos(a0), std::sin(a0), 0.), n1(std::cos(a1), std::sin(a1), 0.);
            if (std::abs(sp.value(r * n0 + Vec3d(0., 0., 20.3), n0) - sp.value(r * n1 + Vec3d(0., 0., 20.3), n1)) < 0.05)
                ++matching;
        }
        REQUIRE(matching > int(0.8 * steps));
    }
}

TEST_CASE("Whole surface displacement with a pattern laid on the surface", "[FuzzySkin]")
{
    // Bumps on a cylinder: the walls move out where the bumps are, never more than the depth, and the top gets bumps too.
    // The layers step through a few rows of bumps (rows are about 3.5 mm apart), so some cross a row near its middle.
    const indexed_triangle_set its = its_make_cylinder(10., 20.);
    std::vector<float>         zs;
    for (int k = 0; k < 8; ++k)
        zs.push_back(6.f + 0.5f * float(k));
    zs.push_back(20.2f);
    MeshSlicingParamsEx        params;
    std::vector<ExPolygons>    layers = slice_mesh_ex(its, zs, params);
    REQUIRE(layers.back().empty());
    TextureMapping m;
    m.projection = FuzzySkinTextureProjection::OnSurface;
    m.size       = 4.;
    SurfaceDisplacement sd{pattern_displacement_map(FuzzySkinTexturePattern::Bumps), m, 0.4, 0.1, true, nullptr, false,
                           FuzzySkinTexturePattern::Bumps};
    std::vector<ExPolygons> out = layers;
    displace_slices(its, Transform3d::Identity(), zs, out, sd, [] {});
    double highest = 0.;
    for (size_t i = 0; i + 1 < zs.size(); ++i) {
        REQUIRE(out[i].size() == 1);
        double rmax = 0.;
        for (const Point& p : out[i].front().contour.points)
            rmax = std::max(rmax, unscale(p).norm());
        REQUIRE(rmax < 10.45);
        highest = std::max(highest, rmax);
    }
    REQUIRE(highest > 10.3);
    // Above the top, only the bumps' caps: islands.
    REQUIRE(out.back().size() > 3);
}

TEST_CASE("Per layer displacement moves outlines sideways only, with every projection", "[FuzzySkin]")
{
    // Bumps on a cylinder, per layer: the walls move out and in by at most the depth, the top stays flat (nothing above it),
    // and the projections that look the surface up (on the surface) work too.
    const indexed_triangle_set its = its_make_cylinder(10., 20.);
    std::vector<float>         zs;
    for (int k = 0; k < 8; ++k)
        zs.push_back(6.f + 0.5f * float(k));
    zs.push_back(20.2f);
    MeshSlicingParamsEx params;
    for (const auto projection : {FuzzySkinTextureProjection::Cylindrical, FuzzySkinTextureProjection::OnSurface}) {
        DYNAMIC_SECTION("projection " << int(projection))
        {
            std::vector<ExPolygons> layers = slice_mesh_ex(its, zs, params);
            TextureMapping          m;
            m.projection = projection;
            m.size       = 4.;
            m.radius     = 10.;
            SurfaceDisplacement sd{pattern_displacement_map(FuzzySkinTexturePattern::Bumps), m, 0.4, 0.1, true, nullptr, false,
                                   FuzzySkinTexturePattern::Bumps, true};
            displace_slices(its, Transform3d::Identity(), zs, layers, sd, [] {});
            double highest = 0., lowest = 1e9;
            for (size_t i = 0; i + 1 < zs.size(); ++i) {
                REQUIRE(layers[i].size() == 1);
                for (const Point& p : layers[i].front().contour.points) {
                    highest = std::max(highest, unscale(p).norm());
                    lowest  = std::min(lowest, unscale(p).norm());
                }
            }
            REQUIRE(highest < 10.41);
            REQUIRE(highest > 10.3);
            REQUIRE(lowest > 9.55);
            REQUIRE(layers.back().empty());
        }
    }
}
