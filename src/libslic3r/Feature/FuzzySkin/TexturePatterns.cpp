#include "TexturePatterns.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "FuzzySkin.hpp"

namespace Slic3r::Feature::FuzzySkin {

namespace {

constexpr double SQRT3 = 1.7320508075688772;

double fract(double x) { return x - std::floor(x); }

double smoothstep(double e0, double e1, double x)
{
    const double t = std::clamp((x - e0) / (e1 - e0), 0., 1.);
    return t * t * (3. - 2. * t);
}

// Integer hash to [0, 1), the same on every platform (std::uniform_real_distribution is not).
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

// Every pattern is a height over one tile: x across [0, 1), y up [0, 1), repeating seamlessly in both.

// Diamond knurl: square pyramids between two sets of diagonal grooves.
double knurl(double x, double y)
{
    auto tri = [](double t) { return 1. - std::abs(2. * fract(t) - 1.); };
    return std::min(tri(x + y), tri(x - y));
}

// Pointy-top hexagons one unit apart across; the tile is one hexagon wide and two rows (sqrt 3) high.
double hexagons(double x, double y)
{
    const double hx = x, hy = y * SQRT3;
    double       d  = 1e9;
    // Distance to the nearest centre in the hexagon's own metric: 0.5 on its edges.
    auto hexdist = [](double dx, double dy) { dx = std::abs(dx); dy = std::abs(dy); return std::max(dx, 0.5 * dx + 0.5 * SQRT3 * dy); };
    const int i0 = int(std::floor(hx)), j0 = int(std::floor(hy / SQRT3));
    for (int j = j0 - 1; j <= j0 + 1; ++j)
        for (int i = i0 - 1; i <= i0 + 1; ++i) {
            d = std::min(d, hexdist(hx - i, hy - j * SQRT3));
            d = std::min(d, hexdist(hx - (i + 0.5), hy - (j + 0.5) * SQRT3));
        }
    return smoothstep(0.02, 0.09, 0.5 - d);
}

// Running bond: bricks one tile wide and half a tile high, every other row shifted by half a brick.
double bricks(double x, double y)
{
    const double rows = 2.;
    const double row  = std::floor(y * rows);
    const double bx   = fract(x + 0.5 * row);
    const double by   = fract(y * rows);
    const double d    = std::min(std::min(bx, 1. - bx), std::min(by, 1. - by) / rows); // to the mortar, in tile widths
    return smoothstep(0.015, 0.045, d);
}

// Rounded ribs running up, meeting in sharp grooves.
double ribs(double x, double /*y*/) { return std::sin(M_PI * fract(x)); }

// Two wavy ridges across each tile.
double waves(double x, double y) { return 0.5 + 0.5 * std::cos(2. * M_PI * (2. * y + 0.25 * std::sin(2. * M_PI * x))); }

// A round bump in the middle of each tile.
double bumps(double x, double y)
{
    const double d = std::hypot(x - 0.5, y - 0.5) / 0.38;
    return d < 1. ? std::sqrt(1. - d * d) : 0.;
}

// Cobblestones: Voronoi cells around scattered points, rounded off towards the gaps between them.
constexpr int STONES = 11;
using Stones         = std::array<std::array<double, 3>, STONES>; // x, y, height of each stone

Stones make_stones()
{
    Stones p{};
    for (int k = 0; k < STONES; ++k)
        p[k] = {hash01(k, 1, 7), hash01(k, 2, 7), hash01(k, 3, 7)};
    return p;
}

double stone(double x, double y)
{
    constexpr int N = STONES;
    static const Stones points = make_stones();
    double f1 = 1e9, f2 = 1e9;
    int    nearest = 0;
    for (int k = 0; k < N; ++k)
        for (int oy = -1; oy <= 1; ++oy)
            for (int ox = -1; ox <= 1; ++ox) {
                const double d = std::hypot(x - points[k][0] - ox, y - points[k][1] - oy);
                if (d < f1) {
                    f2      = f1;
                    f1      = d;
                    nearest = k;
                } else if (d < f2)
                    f2 = d;
            }
    const double t = std::clamp((f2 - f1 - 0.015) / 0.12, 0., 1.);
    return std::sqrt(1. - (1. - t) * (1. - t)) * (0.75 + 0.25 * points[nearest][2]);
}

// Fractal value noise on lattices that divide the tile, so it repeats.
double noise(double x, double y)
{
    double sum = 0., amplitude = 1.;
    for (int octave = 0; octave < 6; ++octave) {
        const int    period = 4 << octave;
        const double px = x * period, py = y * period;
        const int    ix = int(std::floor(px)), iy = int(std::floor(py));
        const double fx = px - ix, fy = py - iy;
        auto         at = [&](int i, int j) {
            return hash01(uint32_t(((i % period) + period) % period), uint32_t(((j % period) + period) % period), uint32_t(octave));
        };
        auto quintic = [](double t) { return t * t * t * (t * (t * 6. - 15.) + 10.); };
        const double sx = quintic(fx), sy = quintic(fy);
        const double a = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * sx;
        const double b = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * sx;
        sum += amplitude * (a + (b - a) * sy);
        amplitude *= 0.5;
    }
    return sum;
}

std::shared_ptr<const DisplacementMap> generate(FuzzySkinTexturePattern pattern)
{
    std::function<double(double, double)> h;
    size_t                                 cols = 512, rows = 512;
    switch (pattern) {
    case FuzzySkinTexturePattern::Knurl: h = knurl; break;
    case FuzzySkinTexturePattern::Hexagons:
        h    = hexagons;
        rows = size_t(std::lround(cols * SQRT3));
        break;
    case FuzzySkinTexturePattern::Bricks: h = bricks; break;
    case FuzzySkinTexturePattern::Ribs: h = ribs; break;
    case FuzzySkinTexturePattern::Waves: h = waves; break;
    case FuzzySkinTexturePattern::Bumps: h = bumps; break;
    case FuzzySkinTexturePattern::Stone: h = stone; break;
    case FuzzySkinTexturePattern::Noise: h = noise; break;
    default: return nullptr;
    }
    auto map    = std::make_shared<DisplacementMap>();
    map->cols   = cols;
    map->rows   = rows;
    map->height.resize(cols * rows);
    // Row 0 is the top of the image; pixel centres sit at the middle of their cells, where DisplacementMap::sample
    // puts them.
    for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < cols; ++c)
            map->height[r * cols + c] = float(h((double(c) + 0.5) / double(cols), 1. - (double(r) + 0.5) / double(rows)));
    // Spread over [0, 1], so the skin thickness is the full height of every pattern.
    const auto [lo, hi] = std::minmax_element(map->height.begin(), map->height.end());
    const float min = *lo, range = *hi - *lo;
    if (range > 1e-6f)
        for (float& v : map->height)
            v = (v - min) / range;
    return map;
}

} // namespace

std::shared_ptr<const DisplacementMap> pattern_displacement_map(FuzzySkinTexturePattern pattern)
{
    static std::mutex                                                   mutex;
    static std::array<std::shared_ptr<const DisplacementMap>, 16>       cache;
    const size_t                                                        idx = size_t(pattern);
    if (pattern == FuzzySkinTexturePattern::Image || idx >= cache.size())
        return nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (!cache[idx])
        cache[idx] = generate(pattern);
    return cache[idx];
}

} // namespace Slic3r::Feature::FuzzySkin
