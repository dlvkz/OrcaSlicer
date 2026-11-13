#include "TextToHeight.hpp"

#include "TextureRelief.hpp"
#include "Utils.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>

#ifdef SLIC3R_TEXT2HEIGHT
#include <ggml.h>
#include <ggml-alloc.h>
#include <ggml-backend.h>
#include <gguf.h>
#include <stable-diffusion.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Slic3r {
namespace TextToHeight {

const char* const STYLE = ", seamless tileable texture, top-down orthographic view, flat even lighting, highly detailed relief";

bool available()
{
#ifdef SLIC3R_TEXT2HEIGHT
    return true;
#else
    return false;
#endif
}

ModelFiles ModelFiles::in_folder(const std::string& dir)
{
    const boost::filesystem::path d(dir);
    return {(d / "sd-turbo-gptq.gguf").string(), (d / "taesd.safetensors").string(), (d / "deepbump256.gguf").string()};
}

std::vector<std::string> ModelFiles::missing() const
{
    std::vector<std::string> out;
    for (const std::string* p : {&diffusion, &taesd, &deepbump}) {
        boost::system::error_code ec;
        if (p->empty() || !boost::filesystem::is_regular_file(*p, ec))
            out.push_back(boost::filesystem::path(*p).filename().string());
    }
    return out;
}

std::string default_model_folder() { return (boost::filesystem::path(data_dir()) / "models" / "text_to_height").string(); }

std::string slug(const std::string& prompt)
{
    std::string s;
    bool        gap = false;
    for (char ch : prompt) {
        const char c = char(std::tolower(static_cast<unsigned char>(ch)));
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (gap && !s.empty())
                s += '_';
            s += c;
            gap = false;
        } else
            gap = true;
    }
    if (s.size() > 40)
        s.resize(40);
    while (!s.empty() && s.back() == '_')
        s.pop_back();
    return s.empty() ? std::string("texture") : s;
}

// ---------------------------------------------------------------------------------------------------------------------
// Normals -> height
// ---------------------------------------------------------------------------------------------------------------------

std::vector<float> normals_to_height(const std::vector<float>& n, int w, int h)
{
    const size_t W = size_t(w), H = size_t(h), N = W * H;
    std::vector<std::complex<float>> FX(N), FY(N);
    for (size_t i = 0; i < N; ++i) {
        FX[i] = -(n[i] - 0.5f) * 2.f;     // DeepBump flips the rows twice (image, then gradients); that cancels out
        FY[i] = (n[N + i] - 0.5f) * 2.f;
    }
    fft2d(FX, W, H, false);
    fft2d(FY, W, H, false);
    // DeepBump's frequency grid: (k - (n // 2 + 1)) / n, ifftshifted. It is one bin off (no zero at k = 0), which is kept
    // so the result is the same as DeepBump's.
    auto freq = [](size_t k, size_t len) {
        const size_t shifted = (k + len / 2) % len;
        return (double(shifted) - double(len / 2 + 1)) / double(len - len % 2);
    };
    for (size_t r = 0; r < H; ++r) {
        const double v = freq(r, H);
        for (size_t c = 0; c < W; ++c) {
            const double               u = freq(c, W);
            const size_t               i = r * W + c;
            const std::complex<double> z = (std::complex<double>(0., -u) * std::complex<double>(FX[i]) +
                                            std::complex<double>(0., -v) * std::complex<double>(FY[i])) /
                                           (u * u + v * v + 1e-16);
            FX[i] = std::complex<float>(z);
        }
    }
    FX[0] = 0.f;
    fft2d(FX, W, H, true);
    std::vector<float> out(N);
    for (size_t i = 0; i < N; ++i)
        out[i] = FX[i].real();
    return out;
}

void normalise(std::vector<float>& v)
{
    if (v.empty())
        return;
    std::vector<float> s(v);
    std::sort(s.begin(), s.end());
    auto pct = [&s](double q) { // numpy's default (linear) percentile
        const double x  = q / 100. * double(s.size() - 1);
        const size_t i  = size_t(std::floor(x));
        const size_t j  = std::min(i + 1, s.size() - 1);
        return double(s[i]) + (double(s[j]) - double(s[i])) * (x - double(i));
    };
    const double lo = pct(0.5), hi = pct(99.5), span = std::max(hi - lo, 1e-9);
    for (float& x : v)
        x = float(std::clamp((double(x) - lo) / span, 0., 1.));
}

bool save_height_png16(const std::string& path, const std::vector<float>& height, int w, int h, std::string& error)
{
    std::vector<uint16_t> px(height.size());
    for (size_t i = 0; i < height.size(); ++i)
        px[i] = uint16_t(std::lround(std::clamp(height[i], 0.f, 1.f) * 65535.f));
    return write_png16(path, px, size_t(w), size_t(h), error);
}

#ifdef SLIC3R_TEXT2HEIGHT

// ---------------------------------------------------------------------------------------------------------------------
// Compute device
// ---------------------------------------------------------------------------------------------------------------------

// ggml's backends may be separate libraries loaded at run time (GGML_BACKEND_DL): the processor's always, Vulkan's when
// it was built and the system has a Vulkan driver. A build with the backends linked in has them registered already.
static void load_backends()
{
    static std::once_flag once;
    std::call_once(once, []() {
        // stable-diffusion.cpp's messages to the program's log: which device, timings, errors.
        sd_set_log_callback(
            [](enum sd_log_level_t level, const char* text, void*) {
                std::string t(text ? text : "");
                while (!t.empty() && (t.back() == '\n' || t.back() == '\r'))
                    t.pop_back();
                if (t.empty())
                    return;
                if (level >= SD_LOG_WARN)
                    BOOST_LOG_TRIVIAL(warning) << "stable-diffusion.cpp: " << t;
                else if (level == SD_LOG_INFO)
                    BOOST_LOG_TRIVIAL(info) << "stable-diffusion.cpp: " << t;
                else
                    BOOST_LOG_TRIVIAL(debug) << "stable-diffusion.cpp: " << t;
            },
            nullptr);
        // NVIDIA's cooperative matrix 2 kernels take about 25 s to compile on the first texture of every run (the
        // driver's shader cache is often full and then does not keep them), and save about 0.1 s per texture after
        // that: measured on an RTX 3080 Ti, 27 s -> 6 s for the first texture. Unless the user chose otherwise.
        if (std::getenv("GGML_VK_DISABLE_COOPMAT2") == nullptr) {
#ifdef _WIN32
            _putenv_s("GGML_VK_DISABLE_COOPMAT2", "1");
#else
            setenv("GGML_VK_DISABLE_COOPMAT2", "1", 0);
#endif
        }
        if (ggml_backend_reg_by_name("CPU") == nullptr)
            ggml_backend_load_all();
    });
}

// The graphics card (a discrete one first), or the processor.
static ggml_backend_dev_t pick_device(bool use_gpu)
{
    load_backends();
    if (use_gpu)
        for (auto type : {GGML_BACKEND_DEVICE_TYPE_GPU, GGML_BACKEND_DEVICE_TYPE_IGPU})
            if (ggml_backend_dev_t dev = ggml_backend_dev_by_type(type))
                return dev;
    return ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
}

static bool is_cpu(ggml_backend_dev_t dev) { return dev == nullptr || ggml_backend_dev_type(dev) == GGML_BACKEND_DEVICE_TYPE_CPU; }

static std::string describe(ggml_backend_dev_t dev)
{
    if (is_cpu(dev))
        return "processor";
    const char* d = ggml_backend_dev_description(dev);
    return d != nullptr && *d ? std::string(d) : std::string(ggml_backend_dev_name(dev));
}

std::string compute_device_description(bool use_gpu) { return describe(pick_device(use_gpu)); }

static void set_threads(ggml_backend_t backend, int threads)
{
    ggml_backend_dev_t dev = ggml_backend_get_device(backend);
    ggml_backend_reg_t reg = dev ? ggml_backend_dev_backend_reg(dev) : nullptr;
    auto set = reg ? (ggml_backend_set_n_threads_t) ggml_backend_reg_get_proc_address(reg, "ggml_backend_set_n_threads") : nullptr;
    if (set)
        set(backend, threads);
}

// ---------------------------------------------------------------------------------------------------------------------
// SD-Turbo through stable-diffusion.cpp
// ---------------------------------------------------------------------------------------------------------------------

struct ColourGenerator::Impl
{
    std::atomic<sd_ctx_t*> ctx{nullptr}; // atomic: cancel() may run on another thread while load() runs
    std::string            diffusion, taesd, backend; // the context keeps pointers to these
    ggml_backend_dev_t     device = nullptr;
    int                    threads = 0;
    std::atomic<bool>      cancelled{false};
    ~Impl()
    {
        if (sd_ctx_t* c = ctx.exchange(nullptr))
            free_sd_ctx(c);
    }
    bool create(ggml_backend_dev_t dev)
    {
        if (sd_ctx_t* c = ctx.exchange(nullptr))
            free_sd_ctx(c);
        device  = dev;
        backend = dev ? ggml_backend_dev_name(dev) : "";
        sd_ctx_params_t p;
        sd_ctx_params_init(&p);
        p.model_path = diffusion.c_str();
        p.taesd_path = taesd.c_str();
        p.n_threads  = threads;
        p.prediction = EPS_PRED;
        p.backend    = backend.empty() ? nullptr : backend.c_str();
        ctx.store(new_sd_ctx(&p));
        return ctx.load() != nullptr;
    }
};

ColourGenerator::ColourGenerator() : m_impl(new Impl) {}
ColourGenerator::~ColourGenerator() = default;
bool ColourGenerator::loaded() const { return m_impl->ctx.load() != nullptr; }
std::string ColourGenerator::device() const { return describe(m_impl->device); }

bool ColourGenerator::load(const ModelFiles& files, int threads, std::string& error, bool use_gpu)
{
    m_impl->diffusion = files.diffusion;
    m_impl->taesd     = files.taesd;
    m_impl->threads   = threads > 0 ? threads : sd_get_num_physical_cores();
    ggml_backend_dev_t dev = pick_device(use_gpu);
    // A graphics card without enough memory, or a driver that fails: the processor still works.
    if (!m_impl->create(dev) && !(is_cpu(dev) || m_impl->create(pick_device(false)))) {
        error = "could not load the text to texture model (" + files.diffusion + ")";
        return false;
    }
    return true;
}

static bool generate_once(sd_ctx_t* ctx, const std::string& prompt, int64_t seed, ImageRGB& out)
{
    const std::string full = prompt + STYLE;
    sd_img_gen_params_t g;
    sd_img_gen_params_init(&g);
    g.prompt                         = full.c_str();
    g.negative_prompt                = "";
    g.width                          = 512;
    g.height                         = 512;
    g.seed                           = seed;
    g.batch_count                    = 1;
    g.circular_x                     = true;
    g.circular_y                     = true;
    g.sample_params.sample_method    = EULER_SAMPLE_METHOD;
    g.sample_params.sample_steps     = 1;
    g.sample_params.guidance.txt_cfg = 1.f;
    sd_image_generation_result_t* res = nullptr;
    if (!generate_image(ctx, &g, &res) || !res || res->count < 1 || !res->items[0].image.data) {
        if (res)
            free_sd_image_generation_result(res);
        return false;
    }
    const sd_image_t& im = res->items[0].image;
    out.width            = int(im.width);
    out.height           = int(im.height);
    out.rgb.resize(size_t(im.width) * im.height * 3);
    for (size_t i = 0; i < size_t(im.width) * im.height; ++i)
        for (int c = 0; c < 3; ++c)
            out.rgb[i * 3 + c] = im.data[i * im.channel + std::min<uint32_t>(c, im.channel - 1)];
    free_sd_image_generation_result(res);
    return true;
}

bool ColourGenerator::generate(const std::string& prompt, int64_t seed, ImageRGB& out, std::string& error)
{
    sd_ctx_t* ctx = m_impl->ctx.load();
    if (!ctx) {
        error = "the model is not loaded";
        return false;
    }
    m_impl->cancelled = false;
    bool ok = generate_once(ctx, prompt, seed, out);
    if (!ok && !m_impl->cancelled && !is_cpu(m_impl->device) && m_impl->create(pick_device(false))) {
        // It failed on the graphics card: once more on the processor, which then stays in use.
        ctx = m_impl->ctx.load();
        ok  = generate_once(ctx, prompt, seed, out);
    }
    if (!ok)
        error = m_impl->cancelled ? "cancelled" : "the texture could not be generated";
    return ok;
}

void ColourGenerator::cancel()
{
    m_impl->cancelled = true;
    if (sd_ctx_t* ctx = m_impl->ctx.load())
        sd_cancel_generation(ctx, SD_CANCEL_ALL);
}

// ---------------------------------------------------------------------------------------------------------------------
// DeepBump on ggml
// ---------------------------------------------------------------------------------------------------------------------

// MobileNetV2 inverted residual blocks 1..17: expansion, output channels, stride.
static const int BLOCKS[17][3] = {{1, 16, 1}, {6, 24, 2}, {6, 24, 1}, {6, 32, 2}, {6, 32, 1}, {6, 32, 1},
                                  {6, 64, 2}, {6, 64, 1}, {6, 64, 1}, {6, 64, 1}, {6, 96, 1}, {6, 96, 1},
                                  {6, 96, 1}, {6, 160, 2}, {6, 160, 1}, {6, 160, 1}, {6, 320, 1}};

struct NormalEstimator::Impl
{
    ggml_backend_t        backend = nullptr;
    ggml_backend_dev_t    device  = nullptr;
    std::string           path;
    int                   threads = 0;
    ggml_context*         wctx    = nullptr;
    ggml_backend_buffer_t wbuf    = nullptr;
    // the graph for one tile size, kept between tiles
    int                   gw = 0, gh = 0;
    ggml_context*         gctx  = nullptr;
    ggml_cgraph*          graph = nullptr;
    ggml_gallocr_t        alloc = nullptr;
    ggml_tensor*          input = nullptr;
    ggml_tensor*          output = nullptr;

    ~Impl() { release(); }
    // The weights from the GGUF file at `path` into a buffer of the device.
    bool load_weights(ggml_backend_dev_t dev, std::string& error);
    void release()
    {
        free_graph();
        if (wbuf)
            ggml_backend_buffer_free(wbuf);
        if (wctx)
            ggml_free(wctx);
        if (backend)
            ggml_backend_free(backend);
        wbuf    = nullptr;
        wctx    = nullptr;
        backend = nullptr;
        device  = nullptr;
    }
    void free_graph()
    {
        if (alloc)
            ggml_gallocr_free(alloc);
        if (gctx)
            ggml_free(gctx);
        alloc = nullptr;
        gctx  = nullptr;
        graph = nullptr;
        gw = gh = 0;
    }
    ggml_tensor* weight(const std::string& name) const { return ggml_get_tensor(wctx, name.c_str()); }

    ggml_tensor* conv(ggml_context* ctx, ggml_tensor* x, const std::string& name, int stride, int act) const
    {
        ggml_tensor* w   = weight(name + ".weight");
        ggml_tensor* b   = weight(name + ".bias");
        const int    pad = int(w->ne[0] / 2);
        ggml_tensor* y   = nullptr;
        if (w->ne[2] == 1 && x->ne[2] > 1) {
            y = ggml_conv_2d_dw_direct(ctx, w, x, stride, stride, pad, pad, 1, 1);
        } else {
            // im2col in f32: ggml_conv_2d rounds the activations to f16 first, which moves the output by up to 2e-3
            ggml_tensor* cols = ggml_im2col(ctx, w, x, stride, stride, pad, pad, 1, 1, true, GGML_TYPE_F32); // [N, OH, OW, K]
            const int64_t K = cols->ne[0], OW = cols->ne[1], OH = cols->ne[2], N = cols->ne[3], OC = w->ne[3];
            y = ggml_mul_mat(ctx, ggml_reshape_2d(ctx, w, K, OC), ggml_reshape_2d(ctx, cols, K, OW * OH * N)); // [N*OH*OW, OC]
            y = ggml_cont(ctx, ggml_permute(ctx, ggml_reshape_4d(ctx, y, OC, OW, OH, N), 2, 0, 1, 3));    // [N, OC, OH, OW]
        }
        y                = ggml_add(ctx, y, ggml_reshape_4d(ctx, b, 1, 1, b->ne[0], 1));
        if (act == 6)
            y = ggml_clamp(ctx, y, 0.f, 6.f);
        else if (act == 1)
            y = ggml_relu(ctx, y);
        return y;
    }

    ggml_tensor* forward(ggml_context* ctx, ggml_tensor* x) const
    {
        std::vector<ggml_tensor*> skips;
        x       = conv(ctx, x, "encoder.features.0.0", 2, 6);
        int cin = 32;
        for (int i = 1; i <= 17; ++i) {
            const int         t = BLOCKS[i - 1][0], c = BLOCKS[i - 1][1], s = BLOCKS[i - 1][2];
            const std::string p = "encoder.features." + std::to_string(i) + ".conv";
            ggml_tensor*      h = x;
            if (t != 1) {
                h = conv(ctx, h, p + ".0.0", 1, 6);
                h = conv(ctx, h, p + ".1.0", s, 6);
                h = conv(ctx, h, p + ".2", 1, 0);
            } else {
                h = conv(ctx, h, p + ".0.0", s, 6);
                h = conv(ctx, h, p + ".1", 1, 0);
            }
            x   = (s == 1 && cin == c) ? ggml_add(ctx, x, h) : h;
            cin = c;
            if (i == 1 || i == 3 || i == 6 || i == 13)
                skips.push_back(x);
        }
        x = conv(ctx, x, "encoder.features.18.0", 1, 6);
        for (int k = 0; k < 5; ++k) {
            x = ggml_upscale(ctx, x, 2, GGML_SCALE_MODE_NEAREST);
            if (k < 4)
                x = ggml_concat(ctx, x, skips[3 - k], 2);
            const std::string p = "decoder.blocks." + std::to_string(k);
            x                   = conv(ctx, x, p + ".conv1.0", 1, 1);
            x                   = conv(ctx, x, p + ".conv2.0", 1, 1);
        }
        return ggml_sigmoid(ctx, conv(ctx, x, "segmentation_head.0", 1, 0));
    }

    bool build(int w, int h, std::string& error)
    {
        if (gctx && w == gw && h == gh)
            return true;
        free_graph();
        const size_t   nodes = 2048;
        ggml_init_params ip{ggml_tensor_overhead() * nodes + ggml_graph_overhead_custom(nodes, false), nullptr, true};
        gctx  = ggml_init(ip);
        input = ggml_new_tensor_4d(gctx, GGML_TYPE_F32, w, h, 1, 1);
        ggml_set_input(input);
        output = forward(gctx, input);
        ggml_set_output(output);
        graph = ggml_new_graph_custom(gctx, nodes, false);
        ggml_build_forward_expand(graph, output);
        alloc = ggml_gallocr_new(ggml_backend_get_default_buffer_type(backend));
        if (!ggml_gallocr_alloc_graph(alloc, graph)) {
            free_graph();
            error = "not enough memory for the normal map network";
            return false;
        }
        gw = w;
        gh = h;
        return true;
    }
};

NormalEstimator::NormalEstimator() : m_impl(new Impl) {}
NormalEstimator::~NormalEstimator() = default;
bool NormalEstimator::loaded() const { return m_impl->wbuf != nullptr; }

static bool seek(FILE* f, int64_t pos)
{
#ifdef _WIN32
    return _fseeki64(f, pos, SEEK_SET) == 0;
#else
    return fseeko(f, off_t(pos), SEEK_SET) == 0;
#endif
}

std::string NormalEstimator::device() const { return describe(m_impl->device); }

bool NormalEstimator::load(const std::string& path, int threads, std::string& error, bool use_gpu)
{
    Impl& m   = *m_impl;
    m.path    = path;
    m.threads = threads > 0 ? threads : sd_get_num_physical_cores();
    ggml_backend_dev_t dev = pick_device(use_gpu);
    if (m.load_weights(dev, error))
        return true;
    // A graphics card that cannot take the weights: the processor.
    return !is_cpu(dev) && m.load_weights(pick_device(false), error);
}

bool NormalEstimator::Impl::load_weights(ggml_backend_dev_t dev, std::string& error)
{
    Impl& m = *this;
    m.release();
    const std::string& path = m.path;
    m.backend = dev ? ggml_backend_dev_init(dev, nullptr) : nullptr;
    if (!m.backend) {
        error = "no device to run the normal map network on";
        return false;
    }
    m.device = dev;
    set_threads(m.backend, m.threads);
    FILE* f = boost::nowide::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open \"" + path + "\"";
        return false;
    }
    ggml_context*   meta = nullptr;
    gguf_init_params gp{true, &meta};
    gguf_context*   g = gguf_init_from_file_ptr(f, gp);
    if (!g) {
        fclose(f);
        error = "\"" + path + "\" is not a GGUF file";
        return false;
    }
    m.wctx = meta;
    m.wbuf = ggml_backend_alloc_ctx_tensors(m.wctx, m.backend);
    bool ok = m.wbuf != nullptr;
    std::vector<uint8_t> buf;
    for (int64_t i = 0; ok && i < gguf_get_n_tensors(g); ++i) {
        const char*  name = gguf_get_tensor_name(g, i);
        ggml_tensor* t    = ggml_get_tensor(m.wctx, name);
        if (!t) {
            ok = false;
            break;
        }
        buf.resize(ggml_nbytes(t));
        ok = seek(f, int64_t(gguf_get_data_offset(g) + gguf_get_tensor_offset(g, i))) && fread(buf.data(), 1, buf.size(), f) == buf.size();
        if (ok)
            ggml_backend_tensor_set(t, buf.data(), 0, buf.size());
    }
    gguf_free(g);
    fclose(f);
    if (!ok || !m.weight("segmentation_head.0.weight")) {
        error = m.wbuf ? "\"" + path + "\" is not the DeepBump model" : "not enough memory for the normal map network";
        m.release();
        return false;
    }
    return true;
}

bool NormalEstimator::run_tile(const std::vector<float>& grey, int w, int h, std::vector<float>& normals, std::string& error)
{
    Impl& m = *m_impl;
    if (!loaded()) {
        error = "the normal map model is not loaded";
        return false;
    }
    if (w % 32 || h % 32 || grey.size() != size_t(w) * h) {
        error = "tile size must be a multiple of 32";
        return false;
    }
    auto run = [&]() {
        if (!m.build(w, h, error))
            return false;
        ggml_backend_tensor_set(m.input, grey.data(), 0, grey.size() * sizeof(float));
        if (ggml_backend_graph_compute(m.backend, m.graph) != GGML_STATUS_SUCCESS) {
            error = "the normal map network failed";
            return false;
        }
        return true;
    };
    // It failed on the graphics card: once more on the processor, which then stays in use.
    if (!run() && (is_cpu(m.device) || !m.load_weights(pick_device(false), error) || !run()))
        return false;
    normals.resize(size_t(3) * w * h);
    ggml_backend_tensor_get(m.output, normals.data(), 0, normals.size() * sizeof(float));
    return true;
}

bool NormalEstimator::colour_to_normals(const ImageRGB& img, std::vector<float>& normals, std::string& error,
                                        const std::function<bool(float)>& progress)
{
    const int T = 256, S = 128;
    const int W = img.width, H = img.height;
    if (W < T || H < T) {
        error = "the texture is smaller than 256 pixels";
        return false;
    }
    std::vector<float> grey(size_t(W) * H);
    for (size_t i = 0; i < grey.size(); ++i)
        grey[i] = (float(img.rgb[i * 3]) + float(img.rgb[i * 3 + 1]) + float(img.rgb[i * 3 + 2])) / (3.f * 255.f);
    std::vector<double> win1(T);
    for (int i = 0; i < T; ++i) {
        const double s = std::sin(M_PI * double(i) / T + M_PI / (2. * T));
        win1[i]        = s * s;
    }
    // Tiles on the torus: the same as DeepBump's wrap-padded image cropped back, with a quarter of the tiles.
    std::vector<double> acc(size_t(3) * W * H, 0.), wsum(size_t(W) * H, 0.);
    std::vector<float>  tile(size_t(T) * T), pred;
    const int           ny = (H + S - 1) / S, nx = (W + S - 1) / S;
    for (int ty = 0; ty < ny; ++ty)
        for (int tx = 0; tx < nx; ++tx) {
            const int y0 = ty * S, x0 = tx * S;
            for (int y = 0; y < T; ++y)
                for (int x = 0; x < T; ++x)
                    tile[size_t(y) * T + x] = grey[size_t((y0 + y) % H) * W + (x0 + x) % W];
            if (!run_tile(tile, T, T, pred, error))
                return false;
            for (int y = 0; y < T; ++y)
                for (int x = 0; x < T; ++x) {
                    const double wgt = win1[y] * win1[x];
                    const size_t p   = size_t((y0 + y) % H) * W + (x0 + x) % W;
                    for (int c = 0; c < 3; ++c)
                        acc[size_t(c) * W * H + p] += pred[size_t(c) * T * T + size_t(y) * T + x] * wgt;
                    wsum[p] += wgt;
                }
            if (progress && !progress(float(ty * nx + tx + 1) / float(nx * ny))) {
                error = "cancelled";
                return false;
            }
        }
    const size_t N = size_t(W) * H;
    normals.assign(3 * N, 0.f);
    for (size_t p = 0; p < N; ++p) {
        double v[3], len = 0.;
        for (int c = 0; c < 3; ++c) {
            v[c] = acc[c * N + p] / std::max(wsum[p], 1e-9) - 0.5;
            len += v[c] * v[c];
        }
        len = std::sqrt(len);
        for (int c = 0; c < 3; ++c)
            normals[c * N + p] = float(v[c] / (len > 0. ? len : 1.) * 0.5 + 0.5);
    }
    return true;
}

#else // no stable-diffusion.cpp

struct ColourGenerator::Impl {};
ColourGenerator::ColourGenerator() : m_impl(new Impl) {}
ColourGenerator::~ColourGenerator() = default;
bool ColourGenerator::loaded() const { return false; }
std::string compute_device_description(bool) { return "processor"; }
bool ColourGenerator::load(const ModelFiles&, int, std::string& error, bool) { error = "built without text to texture support"; return false; }
std::string ColourGenerator::device() const { return {}; }
bool ColourGenerator::generate(const std::string&, int64_t, ImageRGB&, std::string& error) { error = "built without text to texture support"; return false; }
void ColourGenerator::cancel() {}

struct NormalEstimator::Impl {};
NormalEstimator::NormalEstimator() : m_impl(new Impl) {}
NormalEstimator::~NormalEstimator() = default;
bool NormalEstimator::loaded() const { return false; }
bool NormalEstimator::load(const std::string&, int, std::string& error, bool) { error = "built without text to texture support"; return false; }
std::string NormalEstimator::device() const { return {}; }
bool NormalEstimator::run_tile(const std::vector<float>&, int, int, std::vector<float>&, std::string& error) { error = "built without text to texture support"; return false; }
bool NormalEstimator::colour_to_normals(const ImageRGB&, std::vector<float>&, std::string& error, const std::function<bool(float)>&) { error = "built without text to texture support"; return false; }

#endif

} // namespace TextToHeight
} // namespace Slic3r
