#include "catch_main.hpp"

#include "libslic3r/TextToHeight.hpp"

#include <boost/filesystem.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// Fixtures in tests/data/text_to_height, written by the Python reference (text2height: colour_to_height.py,
// export_deepbump.py):
//   normals_64.f32 / height_64.f32           3x64x64 normals encoded 0..1 -> 64x64 Frankot-Chellappa height
//   deepbump_in_64.f32 / deepbump_out_64.f32 64x64 grey tile -> 3x64x64 DeepBump output

static std::vector<float> read_f32(const std::string& name)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/text_to_height/" + name;
    std::vector<float> out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        out.resize(size_t(std::ftell(f)) / sizeof(float));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(out.data(), sizeof(float), out.size(), f) != out.size())
            out.clear();
        std::fclose(f);
    }
    return out;
}

static double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b)
{
    double m = 0.;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::abs(double(a[i]) - double(b[i])));
    return m;
}

TEST_CASE("Normals to height matches the reference", "[TextToHeight]")
{
    const std::vector<float> n = read_f32("normals_64.f32"), ref = read_f32("height_64.f32");
    REQUIRE(n.size() == 3 * 64 * 64);
    REQUIRE(ref.size() == 64 * 64);
    const std::vector<float> h = TextToHeight::normals_to_height(n, 64, 64);
    REQUIRE(h.size() == ref.size());
    double scale = 0.;
    for (float v : ref)
        scale = std::max(scale, double(std::abs(v)));
    CAPTURE(scale);
    REQUIRE(max_abs_diff(h, ref) < 1e-4 * scale);
}

TEST_CASE("Normalise maps the 0.5 and 99.5 percentiles to 0 and 1", "[TextToHeight]")
{
    std::vector<float> v(1001);
    std::iota(v.begin(), v.end(), 0.f); // 0..1000: percentiles 5 and 995
    TextToHeight::normalise(v);
    REQUIRE_THAT(v[5], WithinAbs(0., 1e-6));
    REQUIRE_THAT(v[995], WithinAbs(1., 1e-6));
    REQUIRE_THAT(v[500], WithinAbs(0.5, 1e-6));
    REQUIRE(v[0] == 0.f);
    REQUIRE(v[1000] == 1.f);
}

TEST_CASE("Prompt slug", "[TextToHeight]")
{
    REQUIRE(TextToHeight::slug("Old red-brick wall!") == "old_red_brick_wall");
    REQUIRE(TextToHeight::slug("  ***  ") == "texture");
    REQUIRE(TextToHeight::slug(std::string(60, 'a')).size() == 40);
}

TEST_CASE("Model files in a folder", "[TextToHeight]")
{
    const auto files = TextToHeight::ModelFiles::in_folder("does/not/exist");
    REQUIRE(files.missing().size() == 3);
}

// Needs the model files: set TEXT2HEIGHT_MODELS to the folder that holds deepbump256.gguf (and the other two).
TEST_CASE("DeepBump on ggml matches the reference", "[TextToHeight]")
{
    if (!TextToHeight::available())
        SKIP("built without stable-diffusion.cpp");
    const char* dir = std::getenv("TEXT2HEIGHT_MODELS");
    if (!dir)
        SKIP("TEXT2HEIGHT_MODELS not set");
    const auto files = TextToHeight::ModelFiles::in_folder(dir);
    if (!boost::filesystem::exists(files.deepbump))
        SKIP("no deepbump256.gguf in TEXT2HEIGHT_MODELS");
    const std::vector<float> in = read_f32("deepbump_in_64.f32"), ref = read_f32("deepbump_out_64.f32");
    REQUIRE(in.size() == 64 * 64);
    REQUIRE(ref.size() == 3 * 64 * 64);
    SECTION("on the processor, exact")
    {
        TextToHeight::NormalEstimator net;
        std::string                   error;
        REQUIRE(net.load(files.deepbump, 0, error, false));
        REQUIRE(net.device() == "processor");
        std::vector<float> out;
        REQUIRE(net.run_tile(in, 64, 64, out, error));
        REQUIRE(out.size() == ref.size());
        CHECK(max_abs_diff(out, ref) < 1e-4);
    }
    SECTION("on the graphics card, within half precision")
    {
        if (TextToHeight::compute_device_description() == "processor")
            SKIP("no usable graphics card");
        TextToHeight::NormalEstimator net;
        std::string                   error;
        REQUIRE(net.load(files.deepbump, 0, error));
        REQUIRE(net.device() != "processor");
        std::vector<float> out;
        REQUIRE(net.run_tile(in, 64, 64, out, error));
        REQUIRE(out.size() == ref.size());
        CHECK(max_abs_diff(out, ref) < 1e-2);
    }
}

// The whole pipeline on one prompt (hidden: about 10 s). Needs TEXT2HEIGHT_MODELS; writes colour (raw RGB) and height (raw
// float) to TEXT2HEIGHT_OUT if set, for comparing with the Python reference on the same colour image.
TEST_CASE("Text to height, whole pipeline", "[TextToHeight][.]")
{
    const char* dir = std::getenv("TEXT2HEIGHT_MODELS");
    if (!TextToHeight::available() || !dir)
        SKIP("needs stable-diffusion.cpp and TEXT2HEIGHT_MODELS");
    const auto files = TextToHeight::ModelFiles::in_folder(dir);
    REQUIRE(files.missing().empty());
    std::string                   error;
    TextToHeight::ColourGenerator gen;
    TextToHeight::NormalEstimator net;
    auto                          t0 = std::chrono::steady_clock::now();
    REQUIRE(gen.load(files, 0, error));
    REQUIRE(net.load(files.deepbump, 0, error));
    auto                   t1 = std::chrono::steady_clock::now();
    TextToHeight::ImageRGB colour;
    REQUIRE(gen.generate("cobblestone pavement", 1000, colour, error));
    auto t2 = std::chrono::steady_clock::now();
    REQUIRE(colour.width == 512);
    REQUIRE(colour.height == 512);
    std::vector<float> normals;
    REQUIRE(net.colour_to_normals(colour, normals, error));
    auto               t3 = std::chrono::steady_clock::now();
    std::vector<float> height = TextToHeight::normals_to_height(normals, 512, 512);
    TextToHeight::normalise(height);
    auto t4 = std::chrono::steady_clock::now();
    auto s  = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    WARN("load " << s(t0, t1) << " s, colour " << s(t1, t2) << " s, normals " << s(t2, t3) << " s, height " << s(t3, t4) << " s");
    if (const char* out = std::getenv("TEXT2HEIGHT_OUT")) {
        if (FILE* f = std::fopen((std::string(out) + "/colour.rgb").c_str(), "wb")) {
            std::fwrite(colour.rgb.data(), 1, colour.rgb.size(), f);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((std::string(out) + "/height.f32").c_str(), "wb")) {
            std::fwrite(height.data(), sizeof(float), height.size(), f);
            std::fclose(f);
        }
    }
}
