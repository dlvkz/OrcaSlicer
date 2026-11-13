#include "catch_main.hpp"

#include <algorithm>
#include <cmath>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A zero-thickness wavy sheet standing upright: x from -w/2 to w/2, y = amp * sin(...), z from 0 to h.
indexed_triangle_set wavy_sheet(float w, float h, float amp, int nx = 60, int nz = 10)
{
    indexed_triangle_set its;
    for (int j = 0; j <= nz; ++j)
        for (int i = 0; i <= nx; ++i) {
            const float x = -w / 2 + w * float(i) / float(nx);
            its.vertices.emplace_back(x, amp * std::sin(x / w * 2.f * float(M_PI)), h * float(j) / float(nz));
        }
    for (int j = 0; j < nz; ++j)
        for (int i = 0; i < nx; ++i) {
            const int a = j * (nx + 1) + i, b = a + 1, c = a + nx + 1, d = c + 1;
            its.indices.emplace_back(a, b, d);
            its.indices.emplace_back(a, d, c);
        }
    return its;
}

double area_mm2(const ExPolygons& layer)
{
    double a = 0.;
    for (const ExPolygon& e : layer)
        a += e.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

double slice_area(const indexed_triangle_set& its, float z, float open_width)
{
    MeshSlicingParamsEx params;
    params.closing_radius     = 0.049f;
    params.open_surface_width = open_width;
    return area_mm2(slice_mesh_ex(its, { z }, params).front());
}

} // namespace

TEST_CASE("Open surface thickness prints open and paper-thin surfaces", "[OpenSurface]")
{
    SECTION("A zero-thickness sheet is lost by default and printed as a band of the given width")
    {
        const indexed_triangle_set sheet  = wavy_sheet(40.f, 20.f, 3.f);
        // Length of the sheet's curve at any height.
        double length = 0.;
        for (int i = 0; i < 60; ++i) {
            const Vec3f& p = sheet.vertices[i];
            const Vec3f& q = sheet.vertices[i + 1];
            length += (q - p).head<2>().norm();
        }
        CHECK(slice_area(sheet, 10.f, 0.f) < 0.01);
        const double band = slice_area(sheet, 10.f, 0.8f);
        // Width times length, plus the rounded ends, less what the inside of the bends loses.
        CHECK(band > 0.9 * 0.8 * length);
        CHECK(band < 1.1 * 0.8 * length + M_PI * 0.4 * 0.4);
    }
    SECTION("A closed part thinner than the width is widened to it")
    {
        indexed_triangle_set wall = its_make_cube(30., 0.2, 20.);
        CHECK_THAT(slice_area(wall, 10.f, 0.f), WithinRel(30. * 0.2, 0.02));
        const double grown = slice_area(wall, 10.f, 0.8f);
        CHECK(grown > 30. * 0.75);
        CHECK(grown < 31. * 0.85);
    }
    SECTION("Parts thicker than the width are left alone")
    {
        indexed_triangle_set block = its_make_cube(30., 10., 20.);
        CHECK_THAT(slice_area(block, 10.f, 0.8f), WithinRel(slice_area(block, 10.f, 0.f), 1e-9));
    }
}

TEST_CASE("Slicing a broken mesh gives the same layers every time", "[OpenSurface]")
{
    // An open, self-touching mesh: two sheets crossing each other and a box with its top missing. The intersection lines
    // are collected in parallel; chaining them must not depend on the order they arrive in.
    indexed_triangle_set its = wavy_sheet(40.f, 20.f, 3.f);
    indexed_triangle_set other = wavy_sheet(40.f, 20.f, -3.f);
    for (auto& v : other.vertices)
        v = Vec3f(v.y(), v.x(), v.z());
    its_merge(its, other);
    indexed_triangle_set box = its_make_cube(20., 20., 20.);
    box.indices.erase(std::remove_if(box.indices.begin(), box.indices.end(),
                                     [&box](const stl_triangle_vertex_indices& f) {
                                         return box.vertices[f[0]].z() > 19.f && box.vertices[f[1]].z() > 19.f &&
                                                box.vertices[f[2]].z() > 19.f;
                                     }),
                      box.indices.end());
    its_merge(its, box);
    std::vector<float> zs;
    for (float z = 0.1f; z < 20.f; z += 0.2f)
        zs.push_back(z);
    MeshSlicingParamsEx params;
    params.closing_radius = 0.049f;
    const std::vector<ExPolygons> first = slice_mesh_ex(its, zs, params);
    for (int run = 0; run < 5; ++run) {
        const std::vector<ExPolygons> again = slice_mesh_ex(its, zs, params);
        for (size_t i = 0; i < zs.size(); ++i)
            REQUIRE_THAT(area_mm2(again[i]), WithinAbs(area_mm2(first[i]), 1e-9));
    }
}
