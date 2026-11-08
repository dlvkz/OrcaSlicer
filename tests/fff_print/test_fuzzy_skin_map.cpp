#include "catch_main.hpp"

#include <boost/filesystem.hpp>

#include "libslic3r/Feature/FuzzySkin/FuzzySkin.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Polygon.hpp"

using namespace Slic3r;
using namespace Slic3r::Feature::FuzzySkin;
using Catch::Matchers::WithinAbs;

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

FuzzySkinConfig map_config(const std::string& texture, FuzzySkinTextureProjection projection, double size)
{
    return FuzzySkinConfig{FuzzySkinType::External, scaled<coord_t>(0.5), scaled<coord_t>(0.2), true, NoiseType::DisplacementMap,
                           1., 4, 0.5, FuzzySkinMode::Displacement, texture, projection, size};
}

// A 20 mm square around the origin, counter-clockwise.
Points square() { return {Point::new_scale(-10, -10), Point::new_scale(10, -10), Point::new_scale(10, 10), Point::new_scale(-10, 10)}; }

// How far a point lies outside the 20 mm square, measured from the side it is closest to (negative inside).
double outside(const Point& p)
{
    const Vec2d q = unscale(p);
    return std::max(std::abs(q.x()), std::abs(q.y())) - 10.;
}

bool near_corner(const Point& p)
{
    const Vec2d q = unscale(p);
    return std::abs(q.x()) > 9.4 && std::abs(q.y()) > 9.4;
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
    fuzzy_polyline(circle, true, 1., map_config(path, FuzzySkinTextureProjection::Cylindrical, 10.), true);
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
