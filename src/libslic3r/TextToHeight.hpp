#ifndef slic3r_TextToHeight_hpp_
#define slic3r_TextToHeight_hpp_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
namespace TextToHeight {

// ---------------------------------------------------------------------------------------------------------------------
// Text -> seamless height map for the fuzzy skin displacement map, on the user's computer:
//   1. prompt -> seamless colour texture: SD-Turbo (Stability AI), one step, circular padding, through
//      stable-diffusion.cpp, with 4-bit GPTQ weights (8-bit for the most sensitive 30% of the UNet) and the TAESD decoder;
//   2. colour -> normal map: DeepBump (MobileNetV2 U-Net), 256 px tiles blended with a smooth window, on ggml;
//   3. normals -> height: Frankot-Chellappa, periodic.
// The model files are not part of the program: see ModelFiles. Built only with stable-diffusion.cpp
// (SLIC3R_TEXT2HEIGHT); without it available() is false.
// ---------------------------------------------------------------------------------------------------------------------

bool available();

struct ModelFiles
{
    std::string diffusion; // SD-Turbo, GGUF (sd-turbo-gptq.gguf)
    std::string taesd;     // TAESD decoder (taesd.safetensors)
    std::string deepbump;  // DeepBump, GGUF with batch norm folded in (deepbump256.gguf)

    static ModelFiles in_folder(const std::string& dir);
    // File names that are not there; empty when all are.
    std::vector<std::string> missing() const;
};

// data_dir()/models/text_to_height
std::string default_model_folder();

struct ImageRGB
{
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgb;
};

// Appended to every prompt.
extern const char* const STYLE;

// Both networks run on the graphics card when one is usable (Vulkan), otherwise on the processor. With use_gpu false,
// or when the graphics card fails to load or run them, they run on the processor.
// The device they will run on, for the user: the graphics card's name, or "processor".
std::string compute_device_description(bool use_gpu = true);

// Prompt -> colour texture (512 x 512). Holds the model in memory (about 0.9 GB) until destroyed.
class ColourGenerator
{
public:
    ColourGenerator();
    ~ColourGenerator();
    bool load(const ModelFiles& files, int threads, std::string& error, bool use_gpu = true);
    bool loaded() const;
    // Where the loaded model runs: the graphics card's name, or "processor".
    std::string device() const;
    // The same prompt and seed give the same texture.
    bool generate(const std::string& prompt, int64_t seed, ImageRGB& out, std::string& error);
    // From another thread: stop the running generate().
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Colour texture -> normal map (DeepBump). Holds 27 MB of weights.
class NormalEstimator
{
public:
    NormalEstimator();
    ~NormalEstimator();
    bool load(const std::string& gguf_path, int threads, std::string& error, bool use_gpu = true);
    bool loaded() const;
    std::string device() const;
    // One tile through the network: grey (h x w, h and w multiples of 32, values 0..1) -> 3 x h x w normals, 0..1.
    bool run_tile(const std::vector<float>& grey, int w, int h, std::vector<float>& normals, std::string& error);
    // A seamless texture: 256 px tiles at a 128 px stride with wrap-around, blended with a sin^2 window, normalised.
    // Returns 3 x h x w normals encoded 0..1 (DeepBump's convention).
    bool colour_to_normals(const ImageRGB& img, std::vector<float>& normals, std::string& error,
                           const std::function<bool(float)>& progress = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Frankot-Chellappa on a periodic grid (w, h powers of two), as DeepBump's normals_to_height(seamless=True).
std::vector<float> normals_to_height(const std::vector<float>& normals, int w, int h);
// To 0..1 between the 0.5th and 99.5th percentiles, clipped.
void normalise(std::vector<float>& height);

bool save_height_png16(const std::string& path, const std::vector<float>& height, int w, int h, std::string& error);
// Lower case letters and digits joined by '_', at most 40 characters.
std::string slug(const std::string& prompt);

} // namespace TextToHeight
} // namespace Slic3r

#endif
