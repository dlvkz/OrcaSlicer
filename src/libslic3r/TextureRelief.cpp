#include "TextureRelief.hpp"

#include "Format/TexturedMesh.hpp"
#include "Model.hpp"
#include "Utils.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <numeric>
#include <tuple>
#include <unordered_map>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include <png.h>

namespace Slic3r {

// ---------------------------------------------------------------------------------------------------------------------
// Finding the texture of a volume
// ---------------------------------------------------------------------------------------------------------------------

std::shared_ptr<const TexturedMesh> textured_mesh_of_volume(const ModelObject& object, const ModelVolume& volume)
{
    const std::string& file = !volume.source.input_file.empty() ? volume.source.input_file : object.input_file;
    if (file.empty() || !is_textured_model_file(file))
        return nullptr;
    std::shared_ptr<const TexturedMesh> tm = load_textured_mesh(file);
    if (!tm)
        return nullptr;
    const indexed_triangle_set& its = volume.mesh().its;
    if (its.indices.size() != tm->its.indices.size() || its.vertices.empty() || tm->its.vertices.empty())
        return nullptr;
    // The importer only moves and scales the mesh: map the source's bounding box onto the volume's and compare corners.
    BoundingBoxf3 a, b;
    for (const stl_vertex& v : tm->its.vertices)
        a.merge(v.cast<double>());
    for (const stl_vertex& v : its.vertices)
        b.merge(v.cast<double>());
    const Vec3d sa = a.size(), sb = b.size();
    const double scale = sa.maxCoeff() > 0. ? sb.maxCoeff() / sa.maxCoeff() : 1.;
    const double tol   = 1e-3 * std::max(sb.maxCoeff(), 1e-6);
    const size_t n     = its.indices.size();
    for (size_t k = 0; k < 64; ++k) {
        const size_t f = (n * k) / 64;
        for (int c = 0; c < 3; ++c) {
            const Vec3d src = (tm->its.vertices[tm->its.indices[f](c)].cast<double>() - a.min) * scale + b.min;
            const Vec3d dst = its.vertices[its.indices[f](c)].cast<double>();
            if ((src - dst).norm() > tol)
                return nullptr;
        }
    }
    return tm;
}

bool has_normal_map(const ModelObject& object)
{
    for (const ModelVolume* v : object.volumes)
        if (v->is_model_part())
            if (auto tm = textured_mesh_of_volume(object, *v); tm && tm->has_normals())
                return true;
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Relief
// ---------------------------------------------------------------------------------------------------------------------


static void fft1d(std::complex<float>* a, size_t n, size_t stride, bool inverse)
{
    // bit reversal
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i * stride], a[j * stride]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double              ang = 2. * M_PI / double(len) * (inverse ? 1. : -1.);
        const std::complex<float> wl(float(std::cos(ang)), float(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1., 0.);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[(i + k) * stride];
                const std::complex<float> v = a[(i + k + len / 2) * stride] * std::complex<float>(w);
                a[(i + k) * stride]           = u + v;
                a[(i + k + len / 2) * stride] = u - v;
                w *= std::complex<double>(wl);
            }
        }
    }
    if (inverse)
        for (size_t i = 0; i < n; ++i)
            a[i * stride] /= float(n);
}

void fft2d(std::vector<std::complex<float>>& a, size_t w, size_t h, bool inverse)
{
    tbb::parallel_for(size_t(0), h, [&](size_t r) { fft1d(a.data() + r * w, w, 1, inverse); });
    tbb::parallel_for(size_t(0), w, [&](size_t c) { fft1d(a.data() + c, h, w, inverse); });
}

size_t next_pow2(size_t n)
{
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

// Box blur along rows or columns, edges clamped.
static void box_blur(std::vector<double>& v, size_t w, size_t h, int radius, bool rows)
{
    if (radius <= 0)
        return;
    const size_t n = rows ? w : h, lines = rows ? h : w;
    tbb::parallel_for(size_t(0), lines, [&](size_t line) {
        std::vector<double> src(n), prefix(n + 1, 0.);
        for (size_t i = 0; i < n; ++i)
            src[i] = rows ? v[line * w + i] : v[i * w + line];
        for (size_t i = 0; i < n; ++i)
            prefix[i + 1] = prefix[i] + src[i];
        for (size_t i = 0; i < n; ++i) {
            const long   a = std::max(0L, long(i) - radius), b = std::min(long(n) - 1, long(i) + radius);
            const double m = (prefix[size_t(b) + 1] - prefix[size_t(a)]) / double(b - a + 1);
            if (rows)
                v[line * w + i] = m;
            else
                v[i * w + line] = m;
        }
    });
}

// Approximate Gaussian blur (three box passes).
static void gaussian_blur(std::vector<double>& v, size_t w, size_t h, double sigma)
{
    const int radius = std::max(1, int(std::lround((std::sqrt(12. * sigma * sigma / 3. + 1.) - 1.) / 2.)));
    for (int pass = 0; pass < 3; ++pass) {
        box_blur(v, w, h, radius, true);
        box_blur(v, w, h, radius, false);
    }
}

bool write_png16(const std::string& path, const std::vector<uint16_t>& px, size_t w, size_t h, std::string& error)
{
    FILE* f = boost::nowide::fopen(path.c_str(), "wb");
    if (!f) {
        error = "cannot write \"" + path + "\"";
        return false;
    }
    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop   info = png ? png_create_info_struct(png) : nullptr;
    std::vector<png_byte> row(w * 2);
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        error = "PNG write error";
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, png_uint_32(w), png_uint_32(h), 16, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (size_t r = 0; r < h; ++r) {
        for (size_t c = 0; c < w; ++c) {
            const uint16_t v = px[r * w + c];
            row[2 * c]       = png_byte(v >> 8); // big endian
            row[2 * c + 1]   = png_byte(v & 0xFF);
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(f);
    return true;
}


std::string TextureReliefReport::summary() const
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "surface texture from the normal map, depth %.3f mm (%s)", depth_mm, height_map.c_str());
    return buf;
}

bool apply_normal_map_relief(ModelObject& object, const TextureReliefParams& params, TextureReliefReport& report, std::string& error)
{
    report = TextureReliefReport();
    const ModelVolume*                  volume = nullptr;
    std::shared_ptr<const TexturedMesh> tm;
    for (const ModelVolume* v : object.volumes)
        if (v->is_model_part())
            if (auto t = textured_mesh_of_volume(object, *v); t && t->has_normals()) {
                volume = v;
                tm     = t;
                break;
            }
    if (!volume) {
        error = "the object has no normal map";
        return false;
    }
    // The normal map used by most facets.
    std::map<int, size_t> votes;
    for (int i : tm->normal_image)
        if (i >= 0)
            ++votes[i];
    const int image = std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    const TextureImage& nimg = tm->images[size_t(image)];
    const size_t        W = size_t(nimg.width), H = size_t(nimg.height);
    if (W * H > size_t(8192) * 8192) {
        error = "the normal map is too large";
        return false;
    }

    // Rasterize the patches: per pixel the facet covering it and that facet's millimetres per pixel along x and y.
    const indexed_triangle_set& its   = volume->mesh().its;
    const Transform3d           trafo = object.instances.empty() ? volume->get_matrix() :
                                                                   object.instances.front()->get_matrix() * volume->get_matrix();
    std::vector<int>   tri(W * H, -1);
    std::vector<float> su(W * H, 0.f), sv(W * H, 0.f);
    for (size_t f = 0; f < its.indices.size(); ++f) {
        if (tm->normal_image[f] != image)
            continue;
        Vec2d px[3];
        Vec3d q[3];
        for (int c = 0; c < 3; ++c) {
            const Vec2f& uv = tm->uvs[f][c];
            px[c]           = Vec2d(double(uv.x()) * double(W) - 0.5, (1. - double(uv.y())) * double(H) - 0.5); // rows run down
            q[c]            = trafo * its.vertices[its.indices[f](c)].cast<double>();
        }
        Matrix2d M;
        M.col(0) = px[1] - px[0];
        M.col(1) = px[2] - px[0];
        if (std::abs(M.determinant()) < 1e-12)
            continue;
        const Matrix2d Mi = M.inverse();
        Eigen::Matrix<double, 3, 2> Q;
        Q.col(0) = q[1] - q[0];
        Q.col(1) = q[2] - q[0];
        const Eigen::Matrix<double, 3, 2> J = Q * Mi; // mm per pixel along x and y
        const float jx = float(J.col(0).norm()), jy = float(J.col(1).norm());
        const long  x0 = std::max(0L, long(std::floor(std::min({px[0].x(), px[1].x(), px[2].x()}))));
        const long  x1 = std::min(long(W) - 1, long(std::ceil(std::max({px[0].x(), px[1].x(), px[2].x()}))));
        const long  y0 = std::max(0L, long(std::floor(std::min({px[0].y(), px[1].y(), px[2].y()}))));
        const long  y1 = std::min(long(H) - 1, long(std::ceil(std::max({px[0].y(), px[1].y(), px[2].y()}))));
        for (long y = y0; y <= y1; ++y)
            for (long x = x0; x <= x1; ++x) {
                const Vec2d d = Mi * (Vec2d(double(x), double(y)) - px[0]);
                if (d.x() >= -1e-6 && d.y() >= -1e-6 && d.x() + d.y() <= 1. + 1e-6) {
                    const size_t i = size_t(y) * W + size_t(x);
                    tri[i]         = int(f);
                    su[i]          = jx;
                    sv[i]          = jy;
                }
            }
    }
    std::vector<uint8_t> mask(W * H), inner(W * H, 0);
    size_t               covered = 0;
    for (size_t i = 0; i < W * H; ++i) {
        mask[i] = tri[i] >= 0;
        covered += mask[i];
    }
    if (covered == 0) {
        error = "the normal map covers no part of the mesh";
        return false;
    }
    // Slopes only away from patch edges, where the next pixel belongs to another part of the model (4-neighbour erosion).
    for (size_t y = 1; y + 1 < H; ++y)
        for (size_t x = 1; x + 1 < W; ++x) {
            const size_t i = y * W + x;
            inner[i]       = mask[i] && mask[i - 1] && mask[i + 1] && mask[i - W] && mask[i + W];
        }

    // Frankot-Chellappa on a power of two grid.
    const size_t PW = next_pow2(W), PH = next_pow2(H);
    std::vector<std::complex<float>> GX(PW * PH, 0.f), GY(PW * PH, 0.f);
    std::vector<float> scales;
    scales.reserve(2 * covered);
    for (size_t y = 0; y < H; ++y)
        for (size_t x = 0; x < W; ++x) {
            const size_t i = y * W + x;
            if (mask[i]) {
                scales.push_back(su[i]);
                scales.push_back(sv[i]);
            }
            if (!inner[i])
                continue;
            const uint8_t* p  = &nimg.rgb[i * 3];
            const double   nx = p[0] / 255. * 2. - 1., ny = p[1] / 255. * 2. - 1.;
            const double   nz = std::clamp(p[2] / 255. * 2. - 1., 0.2, 1.);
            GX[y * PW + x]    = float(-nx / nz * su[i]); // mm per pixel step along columns
            GY[y * PW + x]    = float(ny / nz * sv[i]);  // rows run down the image, +Y is up
        }
    fft2d(GX, PW, PH, false);
    fft2d(GY, PW, PH, false);
    auto freq = [](size_t k, size_t n) { return 2. * M_PI * (k < (n + 1) / 2 ? double(k) : double(k) - double(n)) / double(n); };
    tbb::parallel_for(size_t(0), PH, [&](size_t r) {
        const double wy = freq(r, PH);
        for (size_t c = 0; c < PW; ++c) {
            const double wx  = freq(c, PW);
            const double den = wx * wx + wy * wy;
            const size_t i   = r * PW + c;
            if (den == 0.) {
                GX[i] = 0.f;
                continue;
            }
            const std::complex<double> z = (std::complex<double>(0., -wx) * std::complex<double>(GX[i]) +
                                            std::complex<double>(0., -wy) * std::complex<double>(GY[i])) / den;
            GX[i] = std::complex<float>(z);
        }
    });
    GY.clear();
    GY.shrink_to_fit();
    fft2d(GX, PW, PH, true);
    std::vector<double> h(W * H), hm(W * H), m(W * H);
    for (size_t y = 0; y < H; ++y)
        for (size_t x = 0; x < W; ++x) {
            const size_t i = y * W + x;
            h[i]           = GX[y * PW + x].real();
            m[i]           = mask[i] ? 1. : 0.;
            hm[i]          = h[i] * m[i];
        }
    GX.clear();
    GX.shrink_to_fit();

    // High pass: only relief narrower than a few millimetres; removes the offsets between patches.
    std::nth_element(scales.begin(), scales.begin() + scales.size() / 2, scales.end());
    const double median_scale = std::max(1e-6, double(scales[scales.size() / 2]));
    const double sigma        = params.highpass_mm / median_scale;
    gaussian_blur(hm, W, H, sigma);
    gaussian_blur(m, W, H, sigma);
    std::vector<double> vals;
    vals.reserve(covered);
    for (size_t i = 0; i < W * H; ++i) {
        if (mask[i]) {
            h[i] -= m[i] > 1e-6 ? hm[i] / m[i] : 0.;
            vals.push_back(std::abs(h[i]));
        } else
            h[i] = 0.;
    }
    const size_t k = std::min(vals.size() - 1, size_t(0.999 * double(vals.size())));
    std::nth_element(vals.begin(), vals.begin() + k, vals.end());
    const double depth = vals[k];
    if (!(depth > 1e-6)) {
        error = "the normal map is flat";
        return false;
    }

    // Pixels outside the patches take the nearest patch's height, so lookups at patch edges stay clean.
    {
        std::vector<int8_t> done(W * H, 0);
        std::deque<size_t>  queue;
        for (size_t i = 0; i < W * H; ++i)
            if (mask[i]) {
                done[i] = 1;
                queue.push_back(i);
            }
        while (!queue.empty()) {
            const size_t i = queue.front();
            queue.pop_front();
            const size_t x = i % W, y = i / W;
            const long   nb[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
            for (const auto& d : nb) {
                const long nx = long(x) + d[0], ny = long(y) + d[1];
                if (nx < 0 || ny < 0 || nx >= long(W) || ny >= long(H))
                    continue;
                const size_t j = size_t(ny) * W + size_t(nx);
                if (!done[j]) {
                    done[j] = 1;
                    h[j]    = h[i];
                    queue.push_back(j);
                }
            }
        }
    }
    std::vector<uint16_t> px(W * H);
    for (size_t i = 0; i < W * H; ++i)
        px[i] = uint16_t(std::lround(std::clamp(0.5 + h[i] / (2. * depth), 0., 1.) * 65535.));

    // Write the height map.
    namespace fs = boost::filesystem;
    fs::path dir = params.output_dir.empty() ? (data_dir().empty() ? fs::temp_directory_path() : fs::path(data_dir())) / "texture_relief" :
                                               fs::path(params.output_dir);
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    const std::string source = tm->source;
    const size_t      hash   = std::hash<std::string>()(source + "|" + std::to_string(fs::last_write_time(fs::path(source), ec)) + "|" +
                                                 std::to_string(trafo.linear().norm()));
    char tag[32];
    std::snprintf(tag, sizeof(tag), "%016llx", (unsigned long long) hash);
    const std::string out_path = (dir / (fs::path(source).stem().string() + "_relief_" + tag + ".png")).string();
    if (!write_png16(out_path, px, W, H, error))
        return false;

    // Settings: the displacement map over the whole surface, UV projection with the source file.
    ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Disable);
    auto                      set = [&](const char* key, const std::string& value) { object.config.set_deserialize(key, value, ctx); };
    std::string               texture = out_path, uv_mesh = source;
    std::replace(texture.begin(), texture.end(), '\\', '/');
    std::replace(uv_mesh.begin(), uv_mesh.end(), '\\', '/');
    char depth_text[32];
    std::snprintf(depth_text, sizeof(depth_text), "%.4f", depth);
    set("fuzzy_skin", "external");
    set("fuzzy_skin_noise_type", "displacementmap");
    set("fuzzy_skin_texture_pattern", "image");
    set("fuzzy_skin_texture", texture);
    set("fuzzy_skin_texture_projection", "uv");
    set("fuzzy_skin_texture_invert", "0");
    set("fuzzy_skin_texture_offset", "0.5");
    set("fuzzy_skin_texture_uv_mesh", uv_mesh);
    set("fuzzy_skin_thickness", depth_text);
    set("fuzzy_skin_point_distance", "0.2");
    set("fuzzy_skin_first_layer", "1");
    set("fuzzy_skin_texture_surface", "1");
    set("fuzzy_skin_texture_smooth_support", "1");
    report.height_map = out_path;
    report.depth_mm   = depth;
    return true;
}

} // namespace Slic3r
