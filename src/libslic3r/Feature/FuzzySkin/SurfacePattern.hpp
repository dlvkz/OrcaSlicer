#ifndef libslic3r_SurfacePattern_hpp_
#define libslic3r_SurfacePattern_hpp_

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "admesh/stl.h"

namespace Slic3r::Feature::FuzzySkin {

struct DisplacementMap;

// A pattern laid out on the surface of a part instead of projected onto it from an image, so it has no seams and is not
// stretched anywhere: over edges, round a vase's neck, across a sharp rim.
//
// Cells, bumps and stones sit around sites spread evenly over the mesh (dart throwing on points sampled over the
// triangles, then Lloyd relaxation for the regular ones, which turns the cells into a honeycomb that bends with the
// surface). The height at a point comes from its 3D distance to the nearest two sites, so a cell on an edge folds over
// it. Noise is 3D value noise, seamless by construction.
//
// Images and the striped patterns (knurl, ribs, waves, bricks) are laid by charts: sites spread evenly over the mesh
// (dart throwing, half a tile apart at most), each projecting the image along its own surface direction. The charts are
// chained over the surface from one site (shortest paths between neighbouring sites): a site takes its neighbour's frame,
// turned as little as possible into its own surface (on walls, the horizontal is kept), and its image offset is the
// neighbour's plus the step between them, so the image continues across sites, round a cylinder by its arc length.
// Points blend the charts of the sites around them, so where chains meet from two sides (the back of a cylinder, round a
// sphere) the image fades from one to the other instead of a seam.
//
// The sites only depend on the mesh's shape, not on where it is: the slicer and the 3D editor's preview, which get the
// same mesh in different places, lay the same pattern.
class SurfacePattern
{
public:
    // The patterns that can be laid on the surface (all of them).
    static bool supports(FuzzySkinTexturePattern pattern);
    // Laid by charts (an image projected per site, see above) rather than by cells around the sites.
    static bool uses_charts(FuzzySkinTexturePattern pattern);

    // vertices, faces: the part's mesh, in the coordinates value() is asked in. size: fuzzy_skin_texture_size, the
    // spacing of cells and bumps, the width a projected tile of the same pattern would have.
    SurfacePattern(FuzzySkinTexturePattern                         pattern,
                   double                                          size,
                   const std::vector<Vec3d>&                       vertices,
                   const std::vector<stl_triangle_vertex_indices>& faces);

    // For the charts: the image (a built-in pattern or a file) and its counter-clockwise turn, degrees.
    void set_map(std::shared_ptr<const DisplacementMap> map, double rotation_deg);

    // Height in [0, 1] at a point on the surface (or near it) whose outward unit normal is n (zero if unknown: charts of
    // sites facing away are then not left out).
    double value(const Vec3d& p, const Vec3d& n = Vec3d::Zero()) const;

    FuzzySkinTexturePattern pattern() const { return m_pattern; }
    double                  size() const { return m_size; }

    // The lookup, for drawing the pattern on the GPU (3D editor preview), which repeats value() in GLSL.
    // Sites are sorted by bucket: bucket b holds sites [bucket_start[b], bucket_start[b + 1]).
    struct Site
    {
        Vec3f position;
        float tag; // random in [0, 1), per site: a stone's height
        // Charts: the surface's direction, the image's right (its up is normal x right) and the image's position, mm.
        Vec3f normal{ Vec3f::Zero() };
        Vec3f right{ Vec3f::Zero() };
        Vec2f offset{ Vec2f::Zero() };
    };
    // Charts: a point blends the sites within this distance.
    double chart_radius() const { return 0.7 * m_cell; }
    const std::vector<Site>&     sites() const { return m_sites; }
    const std::vector<uint32_t>& bucket_start() const { return m_bucket_start; }
    double                       cell() const { return m_cell; }
    const Vec3d&                 origin() const { return m_origin; }
    // A grid cell's bucket. mask = number of buckets - 1.
    static uint32_t bucket(int x, int y, int z, uint32_t mask);
    // 3D value noise before normalisation (the GPU repeats it), and the range used to spread it over [0, 1].
    static double                    noise_raw(const Vec3d& p, double size);
    static std::pair<double, double> noise_range();

private:
    struct Nearest
    {
        double d1, d2;  // distances to the nearest site and the next
        int    i1, i2;  // their indices, -1 if none within reach
    };
    Nearest nearest(const Vec3d& p) const;
    void    build_charts(const std::vector<Vec3d>& vertices, const std::vector<stl_triangle_vertex_indices>& faces);
    double  chart_value(const Vec3d& p, const Vec3d& n) const;

    FuzzySkinTexturePattern m_pattern;
    double                  m_size;
    double                  m_cell{ 1. };
    Vec3d                   m_origin{ Vec3d::Zero() };
    std::vector<Site>       m_sites;
    std::vector<uint32_t>   m_bucket_start;
    std::shared_ptr<const DisplacementMap> m_map;
    double                                 m_rotation{ 0. };
};

} // namespace Slic3r::Feature::FuzzySkin

#endif // libslic3r_SurfacePattern_hpp_
