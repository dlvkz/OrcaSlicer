#include "catch_main.hpp"

#include <cmath>
#include <fstream>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/TexturedMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/TextureRelief.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

fs::path temp_folder()
{
    fs::path dir = fs::temp_directory_path() / fs::unique_path("texture-relief-%%%%-%%%%");
    fs::create_directories(dir);
    return dir;
}

// A 20 mm cube whose six sides each take one constant texture coordinate (one colour band of the texture).
void write_cube_obj(const fs::path& dir, const std::vector<double>& side_u, bool normal_map)
{
    std::ofstream obj((dir / "cube.obj").string());
    obj << "mtllib cube.mtl\n";
    const double v[8][3] = {{0, 0, 0}, {20, 0, 0}, {20, 20, 0}, {0, 20, 0}, {0, 0, 20}, {20, 0, 20}, {20, 20, 20}, {0, 20, 20}};
    for (const auto& p : v)
        obj << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
    for (double u : side_u)
        obj << "vt " << u << " 0.5\n";
    // the top: the whole texture, for the normal map
    obj << "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n";
    obj << "usemtl m\n";
    const int sides[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    for (int s = 0; s < 6; ++s) {
        obj << "f";
        for (int k = 0; k < 4; ++k) {
            const int t = (normal_map && s == 1) ? int(side_u.size()) + k + 1 : s + 1;
            obj << ' ' << sides[s][k] + 1 << '/' << t;
        }
        obj << '\n';
    }
    std::ofstream mtl((dir / "cube.mtl").string());
    mtl << "newmtl m\nKd 1 1 1\nmap_Kd colour.png\n";
    if (normal_map)
        mtl << "norm -bm 1.0 normal.png\n";
}

} // namespace

TEST_CASE("Textured OBJ loads with one texture coordinate triple per imported facet", "[TextureRelief]")
{
    const fs::path dir = temp_folder();
    write_cube_obj(dir, {0.1, 0.1, 0.3, 0.3, 0.6, 0.9}, false);
    std::vector<uint8_t> px(64 * 8 * 3, 255);
    REQUIRE(png::write_rgb_to_file((dir / "colour.png").string(), 64, 8, px));

    TexturedMesh tm;
    std::string  error;
    REQUIRE(load_obj_textured((dir / "cube.obj").string(), tm, error));
    Model model = Model::read_from_file((dir / "cube.obj").string());
    REQUIRE(model.objects.size() == 1);
    REQUIRE(tm.its.indices.size() == model.objects[0]->volumes[0]->mesh().its.indices.size());
    REQUIRE(tm.uvs.size() == tm.its.indices.size());
    REQUIRE(tm.has_color());
    REQUIRE_FALSE(tm.has_normals());
    fs::remove_all(dir);
}

TEST_CASE("Normal map becomes a surface displacement map", "[TextureRelief]")
{
    const fs::path dir = temp_folder();
    std::vector<uint8_t> colour(64 * 8 * 3, 200);
    REQUIRE(png::write_rgb_to_file((dir / "colour.png").string(), 64, 8, colour));
    // ridges across the top: the normal tilts along x as a sine
    std::vector<uint8_t> normal;
    for (int r = 0; r < 64; ++r)
        for (int c = 0; c < 64; ++c) {
            const double nx = 0.4 * std::sin(2. * M_PI * c / 16.), nz = std::sqrt(1. - nx * nx);
            normal.push_back(uint8_t(std::lround((nx + 1.) / 2. * 255.)));
            normal.push_back(128);
            normal.push_back(uint8_t(std::lround((nz + 1.) / 2. * 255.)));
        }
    REQUIRE(png::write_rgb_to_file((dir / "normal.png").string(), 64, 64, normal));
    write_cube_obj(dir, {0.5, 0.5, 0.5, 0.5, 0.5, 0.5}, true);

    Model        model  = Model::read_from_file((dir / "cube.obj").string());
    ModelObject* object = model.objects[0];
    object->add_instance();
    REQUIRE(has_normal_map(*object));

    TextureReliefParams params;
    params.output_dir = dir.string();
    TextureReliefReport report;
    std::string         error;
    REQUIRE(apply_normal_map_relief(*object, params, report, error));
    INFO(report.summary());
    REQUIRE(fs::exists(report.height_map));
    REQUIRE(report.depth_mm > 0.05);
    REQUIRE(report.depth_mm < 5.);
    REQUIRE(object->config.get().opt_string("fuzzy_skin_texture_uv_mesh") == (dir / "cube.obj").generic_string());
    REQUIRE(object->config.has("fuzzy_skin_thickness"));
    fs::remove_all(dir);
}

TEST_CASE("A glTF with compressed geometry is refused with a clear message, not read as garbage", "[TextureRelief]")
{
    // A GLB whose JSON says its geometry needs a decoder (Tripo's current exports use EXT_meshopt_compression).
    const fs::path    dir  = temp_folder();
    const std::string json = R"({"asset":{"version":"2.0"},"extensionsUsed":["EXT_meshopt_compression"],)"
                             R"("extensionsRequired":["EXT_meshopt_compression"],"scenes":[{"nodes":[]}],"scene":0})";
    std::string padded = json;
    while (padded.size() % 4)
        padded += ' ';
    auto u32 = [](std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    {
        std::ofstream f((dir / "compressed.glb").string(), std::ios::binary);
        u32(f, 0x46546C67u);
        u32(f, 2);
        u32(f, uint32_t(12 + 8 + padded.size()));
        u32(f, uint32_t(padded.size()));
        u32(f, 0x4E4F534Au);
        f.write(padded.data(), std::streamsize(padded.size()));
    }
    TexturedMesh mesh;
    std::string  error;
    REQUIRE_FALSE(load_glb((dir / "compressed.glb").string(), mesh, error));
    REQUIRE_THAT(error, Catch::Matchers::ContainsSubstring("EXT_meshopt_compression"));
    fs::remove_all(dir);
}
