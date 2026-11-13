#include "SurfacePattern.hpp"
#include "FuzzySkin.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <numeric>
#include <queue>
#include <unordered_map>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace Slic3r::Feature::FuzzySkin {

namespace {

// The same integer hash as the projected patterns (TexturePatterns.cpp) and the preview shader: [0, 1) on every platform.
double hash01(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x8da6b343u ^ b * 0xd8163841u ^ c * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return double(h) / 4294967296.;
}

double smoothstep(double e0, double e1, double x)
{
    const double t = std::clamp((x - e0) / (e1 - e0), 0., 1.);
    return t * t * (3. - 2. * t);
}

// Points sampled evenly over the mesh, from which the sites are picked and which the relaxation moves them between.
struct Candidates
{
    std::vector<Vec3d>    points;
    std::vector<uint32_t> key; // random, the order in which dart throwing tries them
    std::vector<float>    tag; // random, carried by a site picked here
    std::vector<uint32_t> face; // the triangle it lies on
};

Candidates sample_surface(const std::vector<Vec3d>& vertices, const std::vector<stl_triangle_vertex_indices>& faces, double density)
{
    double area = 0.;
    for (const stl_triangle_vertex_indices& f : faces)
        area += 0.5 * (vertices[f(1)] - vertices[f(0)]).cross(vertices[f(2)] - vertices[f(0)]).norm();
    // A huge part with a fine pattern: fewer candidates per site rather than running out of memory.
    constexpr double MAX_CANDIDATES = 4e6;
    if (area * density > MAX_CANDIDATES)
        density = MAX_CANDIDATES / area;
    Candidates c;
    c.points.reserve(size_t(area * density * 1.05) + 16);
    for (size_t i = 0; i < faces.size(); ++i) {
        const stl_triangle_vertex_indices& f = faces[i];
        const Vec3d &a = vertices[f(0)], &b = vertices[f(1)], &d = vertices[f(2)];
        const double expected = 0.5 * (b - a).cross(d - a).norm() * density;
        const auto   face     = uint32_t(i);
        const int    count    = int(expected) + (hash01(face, 0xffffffffu, 11) < expected - std::floor(expected) ? 1 : 0);
        for (int k = 0; k < count; ++k) {
            double u = hash01(face, uint32_t(k), 1), v = hash01(face, uint32_t(k), 2);
            if (u + v > 1.) {
                u = 1. - u;
                v = 1. - v;
            }
            c.points.emplace_back(a + (b - a) * u + (d - a) * v);
            c.key.emplace_back(uint32_t(hash01(face, uint32_t(k), 3) * 4294967296.));
            c.tag.emplace_back(float(hash01(face, uint32_t(k), 4)));
            c.face.emplace_back(face);
        }
    }
    return c;
}

// Points bucketed by the grid cell they fall in, with the same hash the GPU uses.
struct Grid
{
    double                cell{ 1. };
    Vec3d                 origin{ Vec3d::Zero() };
    uint32_t              mask{ 0 };
    std::vector<uint32_t> start; // bucket b holds order[start[b] .. start[b + 1])
    std::vector<uint32_t> order;

    Vec3i32 cell_of(const Vec3d& p) const
    {
        const Vec3d q = (p - origin) / cell;
        return { int(std::floor(q.x())), int(std::floor(q.y())), int(std::floor(q.z())) };
    }

    void build(const std::vector<Vec3d>& points)
    {
        size_t buckets = 64;
        while (buckets < 2 * points.size())
            buckets <<= 1;
        mask = uint32_t(buckets - 1);
        std::vector<uint32_t> of(points.size());
        start.assign(buckets + 1, 0);
        for (size_t i = 0; i < points.size(); ++i) {
            const Vec3i32 c = cell_of(points[i]);
            of[i]         = SurfacePattern::bucket(c.x(), c.y(), c.z(), mask);
            ++start[of[i] + 1];
        }
        std::partial_sum(start.begin(), start.end(), start.begin());
        std::vector<uint32_t> fill(start.begin(), start.end() - 1);
        order.resize(points.size());
        for (size_t i = 0; i < points.size(); ++i)
            order[fill[of[i]]++] = uint32_t(i);
    }

    // Calls f(index) for every point in the 27 cells around p (some more than once when buckets collide).
    template<class F> void visit(const Vec3d& p, F&& f) const
    {
        const Vec3i32 c = cell_of(p);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const uint32_t b = SurfacePattern::bucket(c.x() + dx, c.y() + dy, c.z() + dz, mask);
                    for (uint32_t k = start[b]; k < start[b + 1]; ++k)
                        f(order[k]);
                }
    }
};

// How far each candidate lies from the nearest centre of a honeycomb (cells s apart in rows, rows s * sqrt(3) / 2 apart,
// every other row shifted by half a cell) laid along the surface:
// - flat areas of at least a few cells (box sides, tops) get a lattice in their own plane;
// - other faces get rows around the vertical axis through the middle of the part, each row holding as many cells as fit
//   around it at its height, so the cells keep their size where the part narrows or widens (a row gains or loses a cell
//   where it must, as a honeycomb on a cone does); faces looking nearly straight up or down get a lattice seen from above.
// Dart throwing takes the candidates nearest to these centres first, so the sites start out as a honeycomb wherever the
// lattice fits, and only the joins between lattices are left to the relaxation.
std::vector<float> lattice_distances(const Candidates& cand, const std::vector<Vec3d>& vertices,
                                     const std::vector<stl_triangle_vertex_indices>& faces, double s)
{
    const double row_h = s * 0.8660254037844386;
    Vec3d        lo = vertices.front(), hi = lo;
    for (const Vec3d& v : vertices) {
        lo = lo.cwiseMin(v);
        hi = hi.cwiseMax(v);
    }
    const Vec2d axis = 0.5 * (lo.head<2>() + hi.head<2>());

    // Distance to the nearest centre of a lattice in plane coordinates (a, b).
    auto planar = [&](double a, double b) {
        double     best = std::numeric_limits<double>::max();
        const long j0   = std::lround(b / row_h);
        for (long j = j0 - 1; j <= j0 + 1; ++j) {
            const double shift = (j & 1) ? 0.5 : 0.;
            const long   i0    = std::lround(a / s - shift);
            for (long i = i0 - 1; i <= i0 + 1; ++i)
                best = std::min(best, std::hypot(a - (double(i) + shift) * s, b - double(j) * row_h));
        }
        return best;
    };

    // Faces of flat areas big enough to hold a few cells, and each area's frame.
    std::vector<int>                                       group(faces.size(), -1);
    std::vector<std::pair<Vec3d, Vec3d>>                   frames; // in-plane u, v
    {
        std::map<std::tuple<long, long, long, long>, std::pair<double, std::vector<size_t>>> planes;
        for (size_t i = 0; i < faces.size(); ++i) {
            const Vec3d &a = vertices[faces[i](0)], &b = vertices[faces[i](1)], &c = vertices[faces[i](2)];
            Vec3d        n = (b - a).cross(c - a);
            const double len = n.norm();
            if (len <= 0.)
                continue;
            n /= len;
            auto& e = planes[std::make_tuple(std::lround(n.x() * 1e4), std::lround(n.y() * 1e4), std::lround(n.z() * 1e4), std::lround(n.dot(a) * 1e3))];
            e.first += 0.5 * len;
            e.second.push_back(i);
        }
        for (const auto& [key, e] : planes) {
            if (e.first < 3. * s * s)
                continue;
            const Vec3d n(std::get<0>(key) * 1e-4, std::get<1>(key) * 1e-4, std::get<2>(key) * 1e-4);
            Vec3d       u = std::abs(n.z()) > 0.99 ? Vec3d(1., 0., 0.) : Vec3d(-n.y(), n.x(), 0.).normalized();
            Vec3d       v = n.normalized().cross(u);
            if (std::abs(n.z()) > 0.99)
                v = Vec3d(0., 1., 0.);
            for (size_t f : e.second)
                group[f] = int(frames.size());
            frames.emplace_back(u, v);
        }
    }

    // Around the axis: the part's profile (mean radius of the candidates on round faces, in thin slices of height), the
    // distance along the profile from the bottom, rows at equal distances along it, and how many cells each row holds.
    std::vector<char> on_round(cand.points.size(), 0);
    const double      slice = 0.25;
    const size_t      n_slices = size_t(std::ceil((hi.z() - lo.z()) / slice)) + 2;
    std::vector<double> sum(n_slices, 0.), radius(n_slices, -1.), along(n_slices, 0.);
    std::vector<size_t> count(n_slices, 0);
    auto slice_of = [&](double z) { return std::min(n_slices - 1, size_t(std::max(0., (z - lo.z()) / slice))); };
    for (size_t k = 0; k < cand.points.size(); ++k) {
        const uint32_t f = cand.face[k];
        if (group[f] >= 0)
            continue;
        const Vec3d &a = vertices[faces[f](0)], &b = vertices[faces[f](1)], &c = vertices[faces[f](2)];
        const Vec3d  n = (b - a).cross(c - a).normalized();
        if (std::abs(n.z()) > 0.8)
            continue;
        on_round[k]    = 1;
        const size_t i = slice_of(cand.points[k].z());
        sum[i] += (cand.points[k].head<2>() - axis).norm();
        ++count[i];
    }
    // Slices without round faces take the radius of the nearest ones.
    for (size_t i = 0; i < n_slices; ++i)
        if (count[i] > 0)
            radius[i] = sum[i] / double(count[i]);
    {
        double last = -1.;
        for (size_t i = 0; i < n_slices; ++i)
            if (radius[i] >= 0.)
                last = radius[i];
            else if (last >= 0.)
                radius[i] = last;
        last = -1.;
        for (size_t i = n_slices; i-- > 0;)
            if (count[i] > 0)
                last = radius[i];
            else if (radius[i] < 0. && last >= 0.)
                radius[i] = last;
        for (double& r : radius)
            r = std::max(r, 0.);
    }
    for (size_t i = 1; i < n_slices; ++i)
        along[i] = along[i - 1] + std::hypot(slice, radius[i] - radius[i - 1]);
    // Distance along the profile at height z, and the radius that far along.
    auto along_at = [&](double z) {
        const double x = std::clamp((z - lo.z()) / slice, 0., double(n_slices - 1));
        const size_t i = std::min(n_slices - 2, size_t(x));
        return along[i] + (along[i + 1] - along[i]) * (x - double(i));
    };
    auto radius_along = [&](double t) {
        const size_t i = std::min<size_t>(n_slices - 1, size_t(std::lower_bound(along.begin(), along.end(), t) - along.begin()));
        return radius[i];
    };
    // Cells per row: a row keeps the count of the row below while that makes its cells at most 12% wider or narrower, so
    // neighbouring rows interlock as a honeycomb; where the part narrows or widens more, a row gains or loses cells.
    const long          rows = long(std::ceil(along.back() / row_h)) + 2;
    std::vector<double> cells(size_t(rows), 1.);
    for (long j = 0; j < rows; ++j) {
        const double fit = 2. * M_PI * radius_along(double(j) * row_h) / s;
        cells[size_t(j)] = j > 0 && std::abs(fit / cells[size_t(j - 1)] - 1.) <= 0.12 ? cells[size_t(j - 1)] : std::max(1., std::round(fit));
    }

    std::vector<float> out(cand.points.size());
    for (size_t k = 0; k < cand.points.size(); ++k) {
        const Vec3d& p = cand.points[k];
        const int    g = group[cand.face[k]];
        double       d;
        if (g >= 0) {
            d = planar((p - lo).dot(frames[g].first), (p - lo).dot(frames[g].second));
        } else if (!on_round[k]) {
            d = planar(p.x() - lo.x(), p.y() - lo.y());
        } else {
            const Vec2d  q     = p.head<2>() - axis;
            const double r     = q.norm();
            const double theta = std::atan2(q.y(), q.x());
            const double t     = along_at(p.z());
            const long   j0    = std::lround(t / row_h);
            d                  = std::numeric_limits<double>::max();
            for (long j = std::max(0L, j0 - 1); j <= std::min(rows - 1, j0 + 1); ++j) {
                const double step  = 2. * M_PI / cells[size_t(j)];
                const double shift = (j & 1) ? 0.5 : 0.;
                const double i     = std::round(theta / step - shift);
                for (double di = -1.; di <= 1.; di += 1.) {
                    double dt = theta - (i + di + shift) * step;
                    dt -= 2. * M_PI * std::round(dt / (2. * M_PI));
                    d = std::min(d, std::hypot(dt * r, t - double(j) * row_h));
                }
            }
            if (d == std::numeric_limits<double>::max())
                d = s;
        }
        out[k] = float(d);
    }
    return out;
}

struct Layout
{
    double spacing;    // dart throwing radius, mm
    double cell;       // lookup grid cell: every site that matters is within one cell, mm
    double site_area;  // surface per site, mm^2
    int    relaxation; // Lloyd iterations
};

Layout layout(FuzzySkinTexturePattern pattern, double size)
{
    if (pattern == FuzzySkinTexturePattern::Stone) {
        // As many stones as in a tile of the projected pattern (11), scattered unevenly.
        const double r = 0.252 * size;
        return { r, 1.6 * r, r * r / 0.697, 1 };
    }
    // Cells and bumps one size apart, started from a honeycomb along the surface (lattice_distances) and relaxed where
    // its pieces meet. Dart throwing at 0.78 of the spacing keeps every lattice centre and nothing between them.
    return { 0.78 * size, size, 0.866 * size * size, 4 };
}

} // namespace

bool SurfacePattern::supports(FuzzySkinTexturePattern /* pattern */) { return true; }

bool SurfacePattern::uses_charts(FuzzySkinTexturePattern pattern)
{
    return !(pattern == FuzzySkinTexturePattern::Hexagons || pattern == FuzzySkinTexturePattern::Bumps ||
             pattern == FuzzySkinTexturePattern::Stone || pattern == FuzzySkinTexturePattern::Noise);
}

void SurfacePattern::set_map(std::shared_ptr<const DisplacementMap> map, double rotation_deg)
{
    m_map      = std::move(map);
    m_rotation = rotation_deg;
}

uint32_t SurfacePattern::bucket(int x, int y, int z, uint32_t mask)
{
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h & mask;
}

double SurfacePattern::noise_raw(const Vec3d& p, double size)
{
    double sum = 0., amplitude = 1.;
    for (int octave = 0; octave < 6; ++octave) {
        const Vec3d  q  = p * (double(4 << octave) / size);
        const int    ix = int(std::floor(q.x())), iy = int(std::floor(q.y())), iz = int(std::floor(q.z()));
        auto         at = [octave](int i, int j, int k) {
            return hash01(uint32_t(i), uint32_t(j), uint32_t(k) * 0x9e3779b1u + uint32_t(octave) * 0x85ebca6bu);
        };
        auto quintic = [](double t) { return t * t * t * (t * (t * 6. - 15.) + 10.); };
        const double sx = quintic(q.x() - ix), sy = quintic(q.y() - iy), sz = quintic(q.z() - iz);
        auto         lerp = [](double a, double b, double t) { return a + (b - a) * t; };
        const double v    = lerp(lerp(lerp(at(ix, iy, iz), at(ix + 1, iy, iz), sx), lerp(at(ix, iy + 1, iz), at(ix + 1, iy + 1, iz), sx), sy),
                                 lerp(lerp(at(ix, iy, iz + 1), at(ix + 1, iy, iz + 1), sx), lerp(at(ix, iy + 1, iz + 1), at(ix + 1, iy + 1, iz + 1), sx), sy),
                                 sz);
        sum += amplitude * v;
        amplitude *= 0.5;
    }
    return sum;
}

std::pair<double, double> SurfacePattern::noise_range()
{
    // The noise rarely reaches its theoretical bounds: spread what it really spans (0.1 and 99.9 percentiles of many
    // samples) over [0, 1], as the projected noise pattern is spread over its image.
    static const std::pair<double, double> range = [] {
        std::vector<double> v(200000);
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = noise_raw(Vec3d(hash01(uint32_t(i), 1, 21), hash01(uint32_t(i), 2, 21), hash01(uint32_t(i), 3, 21)) * 50., 1.);
        std::sort(v.begin(), v.end());
        return std::make_pair(v[v.size() / 1000], v[v.size() - 1 - v.size() / 1000]);
    }();
    return range;
}

SurfacePattern::SurfacePattern(FuzzySkinTexturePattern                         pattern,
                               double                                          size,
                               const std::vector<Vec3d>&                       vertices,
                               const std::vector<stl_triangle_vertex_indices>& faces)
    : m_pattern(pattern), m_size(std::max(size, 0.01))
{
    if (pattern == FuzzySkinTexturePattern::Noise || faces.empty())
        return;
    if (uses_charts(pattern)) {
        build_charts(vertices, faces);
        return;
    }
    const Layout lay = layout(pattern, m_size);
    m_cell           = lay.cell;
    Vec3d lo         = vertices.front();
    for (const Vec3d& v : vertices)
        lo = lo.cwiseMin(v);
    // Two cells of margin, so the grid coordinates of anything near the surface stay positive.
    m_origin = lo - Vec3d::Constant(2. * m_cell);

    // About 150 candidates per site, so one lies close to every lattice centre.
    const Candidates cand = sample_surface(vertices, faces, 150. / lay.site_area);
    if (cand.points.empty())
        return;

    // Dart throwing: take a candidate unless a site is closer than the spacing. Stones in random order; cells and bumps
    // nearest to a honeycomb centre first.
    std::vector<uint32_t> by_key(cand.points.size());
    std::iota(by_key.begin(), by_key.end(), 0u);
    if (pattern == FuzzySkinTexturePattern::Stone) {
        std::sort(by_key.begin(), by_key.end(), [&](uint32_t a, uint32_t b) { return cand.key[a] != cand.key[b] ? cand.key[a] < cand.key[b] : a < b; });
    } else {
        const std::vector<float> d = lattice_distances(cand, vertices, faces, m_size);
        std::sort(by_key.begin(), by_key.end(), [&](uint32_t a, uint32_t b) {
            return d[a] != d[b] ? d[a] < d[b] : (cand.key[a] != cand.key[b] ? cand.key[a] < cand.key[b] : a < b);
        });
    }
    std::vector<Vec3d> sites;
    std::vector<float> tags;
    {
        const double                                         r = lay.spacing, r2 = r * r;
        std::unordered_map<uint64_t, std::vector<uint32_t>> taken;
        auto key_of = [&](int x, int y, int z) { return (uint64_t(uint32_t(x) & 0x1fffff) << 42) | (uint64_t(uint32_t(y) & 0x1fffff) << 21) | uint64_t(uint32_t(z) & 0x1fffff); };
        for (uint32_t i : by_key) {
            const Vec3d& p = cand.points[i];
            const Vec3d  q = (p - m_origin) / r;
            const int    cx = int(std::floor(q.x())), cy = int(std::floor(q.y())), cz = int(std::floor(q.z()));
            bool         free = true;
            for (int dz = -1; dz <= 1 && free; ++dz)
                for (int dy = -1; dy <= 1 && free; ++dy)
                    for (int dx = -1; dx <= 1 && free; ++dx)
                        if (auto it = taken.find(key_of(cx + dx, cy + dy, cz + dz)); it != taken.end())
                            for (uint32_t s : it->second)
                                if ((sites[s] - p).squaredNorm() < r2) {
                                    free = false;
                                    break;
                                }
            if (!free)
                continue;
            taken[key_of(cx, cy, cz)].push_back(uint32_t(sites.size()));
            sites.push_back(p);
            tags.push_back(cand.tag[i]);
        }
    }

    // Lloyd relaxation over the candidates: every site moves to the candidate closest to the middle of the candidates
    // nearest to it, which keeps it on the surface.
    std::vector<uint32_t> owner(cand.points.size());
    for (int iteration = 0; iteration < lay.relaxation; ++iteration) {
        Grid grid{ m_cell, m_origin };
        grid.build(sites);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, cand.points.size()), [&](const tbb::blocked_range<size_t>& range) {
            for (size_t j = range.begin(); j < range.end(); ++j) {
                const Vec3d& p    = cand.points[j];
                double       best = std::numeric_limits<double>::max();
                uint32_t     who  = 0;
                grid.visit(p, [&](uint32_t s) {
                    const double d = (sites[s] - p).squaredNorm();
                    if (d < best || (d == best && s < who)) {
                        best = d;
                        who  = s;
                    }
                });
                if (best == std::numeric_limits<double>::max()) {
                    // Nothing within a cell: look at every site (rare, a lone corner of the mesh).
                    for (size_t s = 0; s < sites.size(); ++s)
                        if (const double d = (sites[s] - p).squaredNorm(); d < best) {
                            best = d;
                            who  = uint32_t(s);
                        }
                }
                owner[j] = who;
            }
        });
        std::vector<Vec3d>  centroid(sites.size(), Vec3d::Zero());
        std::vector<size_t> count(sites.size(), 0);
        for (size_t j = 0; j < cand.points.size(); ++j) {
            centroid[owner[j]] += cand.points[j];
            ++count[owner[j]];
        }
        for (size_t s = 0; s < sites.size(); ++s)
            if (count[s] > 0)
                centroid[s] /= double(count[s]);
        std::vector<double> best(sites.size(), std::numeric_limits<double>::max());
        for (size_t j = 0; j < cand.points.size(); ++j) {
            const uint32_t s = owner[j];
            if (const double d = (cand.points[j] - centroid[s]).squaredNorm(); d < best[s]) {
                best[s]  = d;
                sites[s] = cand.points[j];
            }
        }
    }

    // The final lookup: sites sorted by bucket.
    Grid grid{ m_cell, m_origin };
    grid.build(sites);
    m_sites.reserve(sites.size());
    for (uint32_t s : grid.order)
        m_sites.push_back({ sites[s].cast<float>(), tags[s] });
    m_bucket_start = std::move(grid.start);
}

void SurfacePattern::build_charts(const std::vector<Vec3d>& vertices, const std::vector<stl_triangle_vertex_indices>& faces)
{
    // Sites half a tile apart, but at least 1 mm and at most 5 mm: small enough that a chart's plane follows the surface.
    const double r = std::clamp(0.5 * m_size, 1., 5.);
    m_cell         = 2. * r;
    Vec3d lo       = vertices.front();
    for (const Vec3d& v : vertices)
        lo = lo.cwiseMin(v);
    m_origin = lo - Vec3d::Constant(2. * m_cell);

    const Candidates cand = sample_surface(vertices, faces, 30. / (0.866 * r * r));
    if (cand.points.empty())
        return;
    auto face_normal = [&](uint32_t f) {
        const stl_triangle_vertex_indices& t = faces[f];
        const Vec3d n = (vertices[t(1)] - vertices[t(0)]).cross(vertices[t(2)] - vertices[t(0)]);
        const double len = n.norm();
        return len > 0. ? Vec3d(n / len) : Vec3d(Vec3d::UnitZ());
    };

    // Dart throwing in random order, r apart.
    std::vector<uint32_t> by_key(cand.points.size());
    std::iota(by_key.begin(), by_key.end(), 0u);
    std::sort(by_key.begin(), by_key.end(), [&](uint32_t a, uint32_t b) { return cand.key[a] != cand.key[b] ? cand.key[a] < cand.key[b] : a < b; });
    std::vector<Vec3d>    sites;
    std::vector<float>    tags;
    std::vector<uint32_t> site_face;
    {
        const double                                         r2 = r * r;
        std::unordered_map<uint64_t, std::vector<uint32_t>> taken;
        auto key_of = [](int x, int y, int z) { return (uint64_t(uint32_t(x) & 0x1fffff) << 42) | (uint64_t(uint32_t(y) & 0x1fffff) << 21) | uint64_t(uint32_t(z) & 0x1fffff); };
        for (uint32_t i : by_key) {
            const Vec3d& p = cand.points[i];
            const Vec3d  q = (p - m_origin) / r;
            const int    cx = int(std::floor(q.x())), cy = int(std::floor(q.y())), cz = int(std::floor(q.z()));
            bool         free = true;
            for (int dz = -1; dz <= 1 && free; ++dz)
                for (int dy = -1; dy <= 1 && free; ++dy)
                    for (int dx = -1; dx <= 1 && free; ++dx)
                        if (auto it = taken.find(key_of(cx + dx, cy + dy, cz + dz)); it != taken.end())
                            for (uint32_t s : it->second)
                                if ((sites[s] - p).squaredNorm() < r2) {
                                    free = false;
                                    break;
                                }
            if (!free)
                continue;
            taken[key_of(cx, cy, cz)].push_back(uint32_t(sites.size()));
            sites.push_back(p);
            tags.push_back(cand.tag[i]);
            site_face.push_back(cand.face[i]);
        }
    }
    const size_t n_sites = sites.size();

    // Each site's surface direction: the faces under the candidates nearest to it, averaged (a smooth direction over a
    // coarse mesh, so the charts do not jump from facet to facet).
    Grid grid{ m_cell, m_origin };
    grid.build(sites);
    std::vector<Vec3d> normal(n_sites, Vec3d::Zero());
    for (size_t j = 0; j < cand.points.size(); ++j) {
        const Vec3d& p    = cand.points[j];
        const Vec3d  fn   = face_normal(cand.face[j]);
        double       best = std::numeric_limits<double>::max();
        int          who  = -1;
        grid.visit(p, [&](uint32_t s) {
            // Only sites on the same side of a thin wall.
            if (face_normal(site_face[s]).dot(fn) <= 0.)
                return;
            const double d = (sites[s] - p).squaredNorm();
            if (d < best) {
                best = d;
                who  = int(s);
            }
        });
        if (who >= 0)
            normal[who] += fn;
    }
    for (size_t s = 0; s < n_sites; ++s) {
        const double len = normal[s].norm();
        normal[s]        = len > 1e-9 ? Vec3d(normal[s] / len) : face_normal(site_face[s]);
    }

    // Neighbours: within 2 r, facing roughly the same way.
    std::vector<std::vector<uint32_t>> neighbours(n_sites);
    for (size_t s = 0; s < n_sites; ++s) {
        auto& nb = neighbours[s];
        grid.visit(sites[s], [&](uint32_t t) {
            if (t != s && (sites[t] - sites[s]).squaredNorm() <= 4. * r * r && normal[t].dot(normal[s]) > 0.3)
                nb.push_back(t);
        });
        std::sort(nb.begin(), nb.end());
        nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
    }

    // The image's right: on walls (not facing up or down) horizontal, as a projection from the side would have it;
    // elsewhere carried over from the neighbour.
    auto wall_right = [](const Vec3d& n, Vec3d& right) {
        if (std::abs(n.z()) >= 0.9)
            return false;
        right = Vec3d::UnitZ().cross(n).normalized();
        return true;
    };
    std::vector<Vec3d>  right(n_sites, Vec3d::UnitX()), up(n_sites, Vec3d::UnitY());
    std::vector<Vec2d>  offset(n_sites, Vec2d::Zero());
    std::vector<double> dist(n_sites, std::numeric_limits<double>::max());
    std::vector<char>   done(n_sites, 0);
    using Item = std::pair<double, uint32_t>;
    for (uint32_t root = 0; root < n_sites; ++root) {
        if (done[root])
            continue;
        // A new piece of surface: a frame of its own (from above or the side), the image placed by the site's position.
        const Vec3d& n = normal[root];
        if (!wall_right(n, right[root])) {
            Vec3d x = Vec3d::UnitX() - Vec3d::UnitX().dot(n) * n;
            if (x.norm() < 1e-6)
                x = Vec3d::UnitY() - Vec3d::UnitY().dot(n) * n;
            right[root] = x.normalized();
        }
        up[root]     = n.cross(right[root]);
        offset[root] = Vec2d(sites[root].dot(right[root]), sites[root].dot(up[root]));
        dist[root]   = 0.;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        queue.emplace(0., root);
        while (!queue.empty()) {
            const auto [d, s] = queue.top();
            queue.pop();
            if (done[s])
                continue;
            done[s] = 1;
            for (uint32_t t : neighbours[s]) {
                if (done[t])
                    continue;
                const Vec3d  step = sites[t] - sites[s];
                const double nd   = d + step.norm();
                if (nd >= dist[t])
                    continue;
                dist[t] = nd;
                if (!wall_right(normal[t], right[t])) {
                    // The neighbour's right turned into this site's surface.
                    Vec3d x = right[s] - right[s].dot(normal[t]) * normal[t];
                    if (x.norm() < 1e-6)
                        x = up[s] - up[s].dot(normal[t]) * normal[t];
                    right[t] = x.normalized();
                }
                up[t] = normal[t].cross(right[t]);
                // The step measured in the frame halfway between the two.
                Vec3d mr = right[s] + right[t], mu = up[s] + up[t];
                mr       = mr.norm() > 1e-9 ? Vec3d(mr.normalized()) : right[s];
                mu       = mu.norm() > 1e-9 ? Vec3d(mu.normalized()) : up[s];
                offset[t] = offset[s] + Vec2d(step.dot(mr), step.dot(mu));
                queue.emplace(nd, t);
            }
        }
    }

    // The final lookup: sites sorted by bucket.
    Grid lookup{ m_cell, m_origin };
    lookup.build(sites);
    m_sites.reserve(n_sites);
    for (uint32_t s : lookup.order)
        m_sites.push_back({ sites[s].cast<float>(), tags[s], normal[s].cast<float>(), right[s].cast<float>(), offset[s].cast<float>() });
    m_bucket_start = std::move(lookup.start);
}

double SurfacePattern::chart_value(const Vec3d& p, const Vec3d& n) const
{
    if (!m_map || m_sites.empty() || m_map->cols == 0 || m_map->rows == 0)
        return 0.5;
    const double R      = chart_radius();
    const double tile_w = m_size, tile_h = m_size * double(m_map->rows) / double(m_map->cols);
    const double angle = m_rotation * M_PI / 180., c = std::cos(angle), s = std::sin(angle);
    const bool   use_n = n.squaredNorm() > 0.5;
    const uint32_t mask = uint32_t(m_bucket_start.size() - 2);
    const Vec3d    q    = (p - m_origin) / m_cell;
    const int      cx = int(std::floor(q.x())), cy = int(std::floor(q.y())), cz = int(std::floor(q.z()));
    uint32_t       seen[27];
    int            n_seen = 0;
    double         sum = 0., weight = 0., nearest_d = std::numeric_limits<double>::max(), nearest_h = 0.5;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const uint32_t b = bucket(cx + dx, cy + dy, cz + dz, mask);
                // Two cells may share a bucket: visit it once.
                if (std::find(seen, seen + n_seen, b) != seen + n_seen)
                    continue;
                seen[n_seen++] = b;
                for (uint32_t k = m_bucket_start[b]; k < m_bucket_start[b + 1]; ++k) {
                    const Site&  site = m_sites[k];
                    const Vec3d  d    = p - site.position.cast<double>();
                    const double dist = d.norm();
                    if (dist >= R)
                        continue;
                    const Vec3d sn = site.normal.cast<double>();
                    double      w  = 1. - (dist / R) * (dist / R);
                    w *= w;
                    if (use_n) {
                        const double facing = n.dot(sn);
                        if (facing <= 0.)
                            continue;
                        w *= facing * facing;
                    }
                    const Vec3d  right = site.right.cast<double>(), up = sn.cross(right);
                    const double a = d.dot(right) + site.offset.x(), bb = d.dot(up) + site.offset.y();
                    const double ra = c * a + s * bb, rb = -s * a + c * bb;
                    const double h  = m_map->sample(ra / tile_w + 0.5, rb / tile_h);
                    sum += w * h;
                    weight += w;
                    if (dist < nearest_d) {
                        nearest_d = dist;
                        nearest_h = h;
                    }
                }
            }
    return weight > 1e-12 ? sum / weight : nearest_h;
}

SurfacePattern::Nearest SurfacePattern::nearest(const Vec3d& p) const
{
    Nearest n{ std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), -1, -1 };
    if (m_sites.empty())
        return n;
    const uint32_t mask = uint32_t(m_bucket_start.size() - 2);
    const Vec3d    q    = (p - m_origin) / m_cell;
    const int      cx = int(std::floor(q.x())), cy = int(std::floor(q.y())), cz = int(std::floor(q.z()));
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const uint32_t b = bucket(cx + dx, cy + dy, cz + dz, mask);
                for (uint32_t k = m_bucket_start[b]; k < m_bucket_start[b + 1]; ++k) {
                    if (int(k) == n.i1 || int(k) == n.i2)
                        continue;
                    const double d = (m_sites[k].position.cast<double>() - p).norm();
                    if (d < n.d1) {
                        n.d2 = n.d1;
                        n.i2 = n.i1;
                        n.d1 = d;
                        n.i1 = int(k);
                    } else if (d < n.d2) {
                        n.d2 = d;
                        n.i2 = int(k);
                    }
                }
            }
    return n;
}

double SurfacePattern::value(const Vec3d& p, const Vec3d& normal) const
{
    if (uses_charts(m_pattern))
        return chart_value(p, normal);
    if (m_pattern == FuzzySkinTexturePattern::Noise) {
        const auto [lo, hi] = noise_range();
        return std::clamp((noise_raw(p, m_size) - lo) / (hi - lo), 0., 1.);
    }
    if (m_sites.empty())
        return 0.5;
    const Nearest n = nearest(p);
    if (n.i1 < 0)
        return 0.;
    switch (m_pattern) {
    case FuzzySkinTexturePattern::Bumps: {
        // As the projected bumps: a dome 0.76 of the spacing across.
        const double d = n.d1 / (0.38 * m_size);
        return d < 1. ? std::sqrt(1. - d * d) : 0.;
    }
    case FuzzySkinTexturePattern::Hexagons: {
        if (n.i2 < 0)
            return 1.;
        // Distance to the cell's border: the plane halfway between the two sites. Grooves as the projected hexagons.
        const double gap = (m_sites[n.i2].position - m_sites[n.i1].position).cast<double>().norm();
        const double e   = gap > 1e-9 ? (n.d2 * n.d2 - n.d1 * n.d1) / (2. * gap) : 0.;
        return smoothstep(0.02, 0.09, e / m_size);
    }
    case FuzzySkinTexturePattern::Stone: {
        // As the projected stones: rounded off towards the gaps, each stone at its own height.
        const double t = n.i2 < 0 ? 1. : std::clamp(((n.d2 - n.d1) / m_size - 0.015) / 0.12, 0., 1.);
        return std::sqrt(1. - (1. - t) * (1. - t)) * (0.75 + 0.25 * double(m_sites[n.i1].tag));
    }
    default: return 0.5;
    }
}

} // namespace Slic3r::Feature::FuzzySkin
