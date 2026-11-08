#include "SurfaceDisplacement.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <unordered_map>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"

#include "FuzzySkin.hpp"

namespace Slic3r::Feature::FuzzySkin {

// A face counts as steep (a wall, handled by moving the outline sideways) when its normal is within 45 degrees of
// horizontal, and as shallow (a top or bottom, handled by adding and cutting regions) otherwise.
static constexpr double STEEP_HORIZONTAL_SQ = 0.5;

bool uses_surface_displacement(const PrintRegionConfig& config)
{
    return config.fuzzy_skin.value != FuzzySkinType::None && config.fuzzy_skin_noise_type.value == NoiseType::DisplacementMap &&
           config.fuzzy_skin_texture_surface.value && config.fuzzy_skin_thickness.value > 0.;
}

std::optional<SurfaceDisplacement> surface_displacement(const PrintRegionConfig& config)
{
    if (!uses_surface_displacement(config))
        return std::nullopt;
    std::string error;
    auto        map = load_displacement_map(config.fuzzy_skin_texture.value, &error, false);
    if (!map)
        throw Slic3r::SlicingError("Fuzzy skin displacement map: " + error);
    return SurfaceDisplacement{std::move(map),
                               config.fuzzy_skin_texture_projection.value,
                               config.fuzzy_skin_texture_size.value,
                               config.fuzzy_skin_thickness.value,
                               std::max(0.02, config.fuzzy_skin_point_distance.value),
                               config.fuzzy_skin_first_layer.value};
}

double surface_displacement_height_margin(const PrintRegionConfig& config)
{
    return uses_surface_displacement(config) ? config.fuzzy_skin_thickness.value : 0.;
}

// ---------------------------------------------------------------------------------------------------------------------
// Marching squares
// ---------------------------------------------------------------------------------------------------------------------

Polygons positive_region_contours(const std::vector<float>& values, size_t rows, size_t cols, double x0, double y0, double step)
{
    Polygons out;
    if (rows < 2 || cols < 2)
        return out;
    auto value  = [&](size_t r, size_t c) { return values[r * cols + c]; };
    auto inside = [](float v) { return v > 0.f; };
    // Grid edges: 2 * node for the edge to the right of a node, 2 * node + 1 for the edge above it.
    auto right_edge = [cols](size_t r, size_t c) { return 2 * (r * cols + c); };
    auto up_edge    = [cols](size_t r, size_t c) { return 2 * (r * cols + c) + 1; };
    auto edge_point = [&](size_t edge) {
        const size_t node = edge / 2, r = node / cols, c = node % cols;
        const bool   horizontal = (edge % 2) == 0;
        const double a = value(r, c), b = horizontal ? value(r, c + 1) : value(r + 1, c);
        const double t = a / (a - b);
        return Point::new_scale(x0 + (double(c) + (horizontal ? t : 0.)) * step, y0 + (double(r) + (horizontal ? 0. : t)) * step);
    };

    // Each cell contributes segments with the positive side on their left, from the edge where its boundary, walked
    // counter-clockwise, leaves the positive region to the edge where it enters it again. An edge is left through in
    // one of its two cells and entered through in the other, so the segments chain by their edges.
    std::unordered_map<size_t, size_t> next;
    for (size_t r = 0; r + 1 < rows; ++r)
        for (size_t c = 0; c + 1 < cols; ++c) {
            // corners counter-clockwise from the bottom left, edge k runs from corner k to corner k + 1
            const float  v[4] = {value(r, c), value(r, c + 1), value(r + 1, c + 1), value(r + 1, c)};
            const size_t e[4] = {right_edge(r, c), up_edge(r, c + 1), right_edge(r + 1, c), up_edge(r, c)};
            struct Crossing { size_t edge; bool leaving; };
            Crossing crossings[4];
            int      n = 0;
            for (int k = 0; k < 4; ++k) {
                const bool in0 = inside(v[k]), in1 = inside(v[(k + 1) % 4]);
                if (in0 != in1)
                    crossings[n++] = {e[k], in0};
            }
            if (n == 2) {
                const Crossing& leave = crossings[0].leaving ? crossings[0] : crossings[1];
                const Crossing& enter = crossings[0].leaving ? crossings[1] : crossings[0];
                next[leave.edge]      = enter.edge;
            } else if (n == 4) {
                // Saddle: the centre decides whether the two positive corners are connected through the cell.
                const bool connected = inside(0.25f * (v[0] + v[1] + v[2] + v[3]));
                for (int i = 0; i < 4; ++i)
                    if (crossings[i].leaving)
                        next[crossings[i].edge] = crossings[(i + (connected ? 1 : 3)) % 4].edge;
            }
        }

    while (!next.empty()) {
        const size_t start = next.begin()->first;
        Polygon      poly;
        size_t       edge = start;
        for (;;) {
            auto it = next.find(edge);
            if (it == next.end())
                break;
            poly.points.emplace_back(edge_point(edge));
            edge = it->second;
            next.erase(it);
            if (edge == start)
                break;
        }
        if (poly.points.size() >= 3)
            out.emplace_back(std::move(poly));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Displacing slices
// ---------------------------------------------------------------------------------------------------------------------

namespace {

struct Mesh
{
    std::vector<Vec3d>                       vertices;
    std::vector<stl_triangle_vertex_indices> faces;
    std::vector<Vec3d>                       normals;
    AABBTreeIndirect::Tree<3, double>        tree;
    double                                   zmin = 0.;
};

// Coplanar flat or shallow faces, processed together.
struct ShallowPlane
{
    Vec3d      normal;      // outward, unit
    double     offset;      // normal . p + offset = 0 on the plane
    double     zmin, zmax;  // of its faces
    ExPolygons footprint;   // its faces seen from above, scaled
    BoundingBox bbox;
    bool       flat;
    double     height(double x, double y) const { return -(normal.x() * x + normal.y() * y + offset) / normal.z(); }
};

Mesh transformed_mesh(const indexed_triangle_set& its, const Transform3d& trafo)
{
    Mesh mesh;
    mesh.vertices.reserve(its.vertices.size());
    for (const stl_vertex& v : its.vertices)
        mesh.vertices.emplace_back(trafo * v.cast<double>());
    mesh.faces = its.indices;
    mesh.normals.reserve(mesh.faces.size());
    for (const stl_triangle_vertex_indices& f : mesh.faces) {
        const Vec3d n = (mesh.vertices[f(1)] - mesh.vertices[f(0)]).cross(mesh.vertices[f(2)] - mesh.vertices[f(0)]);
        const double len = n.norm();
        mesh.normals.emplace_back(len > 0. ? Vec3d(n / len) : Vec3d(0., 0., 0.));
    }
    mesh.zmin = std::numeric_limits<double>::max();
    for (const Vec3d& v : mesh.vertices)
        mesh.zmin = std::min(mesh.zmin, v.z());
    mesh.tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.vertices, mesh.faces);
    return mesh;
}

std::vector<ShallowPlane> shallow_planes(const Mesh& mesh)
{
    std::vector<ShallowPlane>                              planes;
    std::map<std::tuple<long, long, long, long>, size_t>   by_plane;
    std::vector<Polygons>                                  triangles;
    for (size_t i = 0; i < mesh.faces.size(); ++i) {
        const Vec3d& n = mesh.normals[i];
        if (n.squaredNorm() == 0. || n.x() * n.x() + n.y() * n.y() >= STEEP_HORIZONTAL_SQ)
            continue;
        const stl_triangle_vertex_indices& f = mesh.faces[i];
        const Vec3d &a = mesh.vertices[f(0)], &b = mesh.vertices[f(1)], &c = mesh.vertices[f(2)];
        const double fzmax = std::max({a.z(), b.z(), c.z()}), fzmin = std::min({a.z(), b.z(), c.z()});
        // The face the object stands on stays flat.
        if (n.z() < 0. && fzmax <= mesh.zmin + 1e-3)
            continue;
        const double offset = -n.dot(a);
        const auto   key    = std::make_tuple(std::lround(n.x() * 1e5), std::lround(n.y() * 1e5), std::lround(n.z() * 1e5), std::lround(offset * 1e4));
        auto [it, added]    = by_plane.emplace(key, planes.size());
        if (added) {
            planes.push_back({n, offset, fzmin, fzmax, {}, {}, std::abs(n.x()) < 1e-6 && std::abs(n.y()) < 1e-6});
            triangles.emplace_back();
        }
        ShallowPlane& plane = planes[it->second];
        plane.zmin          = std::min(plane.zmin, fzmin);
        plane.zmax          = std::max(plane.zmax, fzmax);
        Polygon tri({Point::new_scale(a.x(), a.y()), Point::new_scale(b.x(), b.y()), Point::new_scale(c.x(), c.y())});
        if (tri.area() < 0.)
            tri.reverse();
        triangles[it->second].emplace_back(std::move(tri));
    }
    for (size_t i = 0; i < planes.size(); ++i) {
        // A tiny overlap so that faces sharing an edge merge into one footprint.
        planes[i].footprint = offset_ex(union_ex(triangles[i]), float(SCALED_EPSILON));
        planes[i].bbox      = get_extents(planes[i].footprint);
    }
    return planes;
}

// The outline of one layer, moved sideways where it lies on a steep face.
Polygon displace_ring(const Polygon& ring, double z, const Mesh& mesh, const SurfaceDisplacement& sd)
{
    const double step = scale_(sd.spacing);
    Points       out;
    out.reserve(size_t(ring.length() / step) + ring.points.size());
    const size_t n = ring.points.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2d  a   = ring.points[i].cast<double>();
        const Vec2d  b   = ring.points[(i + 1) % n].cast<double>();
        const double len = (b - a).norm();
        if (len < 1.)
            continue;
        // Samples in the middle of equal steps, never on a corner, where the face a point belongs to is ambiguous.
        const int k = std::max(1, int(std::ceil(len / step)));
        for (int j = 0; j < k; ++j) {
            const Vec2d q = a + (b - a) * ((double(j) + 0.5) / double(k));
            const Vec3d p(unscale<double>(q.x()), unscale<double>(q.y()), z);
            size_t      hit = 0;
            Vec3d       hit_point;
            if (AABBTreeIndirect::squared_distance_to_indexed_triangle_set(mesh.vertices, mesh.faces, mesh.tree, p, hit, hit_point) < 0.) {
                out.emplace_back(q.cast<coord_t>());
                continue;
            }
            const Vec3d& nrm = mesh.normals[hit];
            const double nh2 = nrm.x() * nrm.x() + nrm.y() * nrm.y();
            if (nh2 < STEEP_HORIZONTAL_SQ) {
                // A shallow face: displaced up or down by the shallow plane pass.
                out.emplace_back(q.cast<coord_t>());
                continue;
            }
            // The surface point that, moved by d along its normal, lands in this layer sits d * nz lower.
            double d = displacement_map_value(*sd.map, sd.projection, sd.size, p, nrm) * sd.depth;
            d        = displacement_map_value(*sd.map, sd.projection, sd.size, Vec3d(p.x(), p.y(), z - d * nrm.z()), nrm) * sd.depth;
            // A surface offset by d along its normal crosses the layer d / sin(slope) further out.
            const Vec2d shift = Vec2d(nrm.x(), nrm.y()) * (d / nh2) / SCALING_FACTOR;
            out.emplace_back((q + shift).cast<coord_t>());
        }
    }
    return Polygon(std::move(out));
}

// The parts of a flat or shallow plane's footprint that the displacement adds to or cuts from the layer at z.
void displace_plane(const ShallowPlane& plane, double z, const SurfaceDisplacement& sd, Polygons& add, Polygons& cut)
{
    const double nz    = std::abs(plane.normal.z());
    const double reach = sd.depth / nz;
    if (z < plane.zmin - reach || z > plane.zmax + reach)
        return;
    const bool up = plane.normal.z() > 0.;

    // A grid over the footprint with a border of nodes outside, so the contours close.
    const double x_min = unscale<double>(plane.bbox.min.x()), y_min = unscale<double>(plane.bbox.min.y());
    const double w = unscale<double>(plane.bbox.max.x()) - x_min, h = unscale<double>(plane.bbox.max.y()) - y_min;
    double       step = sd.spacing;
    // Keep a huge flat top from taking a huge grid: at most about 4M nodes.
    step              = std::max(step, std::sqrt(std::max(w * h, 1e-6) / 4e6));
    const size_t cols = size_t(std::ceil(w / step)) + 3, rows = size_t(std::ceil(h / step)) + 3;
    const double x0 = x_min - step, y0 = y_min - step;

    // displaced: positive where the displaced surface puts material in this layer; original: where the face does.
    std::vector<float> displaced(rows * cols, -1.f), original;
    if (!plane.flat)
        original.assign(rows * cols, -1.f);
    for (size_t r = 1; r + 1 < rows; ++r)
        for (size_t c = 1; c + 1 < cols; ++c) {
            const double x = x0 + double(c) * step, y = y0 + double(r) * step;
            const double hz = plane.height(x, y);
            const double v  = displacement_map_value(*sd.map, sd.projection, sd.size, Vec3d(x, y, hz), plane.normal) * sd.depth / nz;
            // A top holds material below it, raised by v; a bottom holds material above it, lowered by v.
            displaced[r * cols + c] = float(up ? hz + v - z : z - (hz - v));
            if (!plane.flat)
                original[r * cols + c] = float(up ? hz - z : z - hz);
        }
    const ExPolygons with_map = intersection_ex(union_ex(positive_region_contours(displaced, rows, cols, x0, y0, step)), plane.footprint);
    ExPolygons       without;
    if (plane.flat) {
        if (up ? plane.zmax > z : plane.zmax < z)
            without = plane.footprint;
    } else {
        without = intersection_ex(union_ex(positive_region_contours(original, rows, cols, x0, y0, step)), plane.footprint);
    }
    append(add, to_polygons(diff_ex(with_map, without)));
    append(cut, to_polygons(diff_ex(without, with_map)));
}

} // namespace

void displace_slices(const indexed_triangle_set&  its,
                     const Transform3d&           trafo,
                     const std::vector<float>&    zs,
                     std::vector<ExPolygons>&     layers,
                     const SurfaceDisplacement&   sd,
                     const std::function<void()>& throw_on_cancel)
{
    if (its.indices.empty() || zs.empty() || layers.size() != zs.size())
        return;
    const Mesh                      mesh   = transformed_mesh(its, trafo);
    const std::vector<ShallowPlane> planes = shallow_planes(mesh);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, zs.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            throw_on_cancel();
            const double z = zs[i];
            Polygons     add, cut;
            for (const ShallowPlane& plane : planes)
                displace_plane(plane, z, sd, add, cut);
            ExPolygons& layer = layers[i];
            if (layer.empty() && add.empty())
                continue;

            Polygons rings;
            const bool walls = sd.first_layer || i > 0;
            for (const ExPolygon& expoly : layer) {
                rings.emplace_back(walls ? displace_ring(expoly.contour, z, mesh, sd) : expoly.contour);
                for (const Polygon& hole : expoly.holes)
                    rings.emplace_back(walls ? displace_ring(hole, z, mesh, sd) : hole);
            }
            // Contours counter-clockwise and holes clockwise: the positive union restores the islands and their holes, and
            // drops the small reversed loops where outlines moved inwards cross at a corner.
            ExPolygons result = union_ex(rings, ClipperLib::pftPositive);
            if (!add.empty())
                result = union_ex(result, add);
            if (!cut.empty())
                result = diff_ex(result, cut);
            layer = std::move(result);
        }
    });
}

} // namespace Slic3r::Feature::FuzzySkin
