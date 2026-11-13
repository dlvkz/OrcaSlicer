#include "TexturedMesh.hpp"

#include "objparser.hpp"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"

#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <unordered_map>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include <png.h>
#include <jpeglib.h>
#include <jerror.h>

#include "nlohmann/json.hpp"

namespace Slic3r {

// ---------------------------------------------------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------------------------------------------------

Vec3f TextureImage::sample(double u, double v) const
{
    if (empty())
        return Vec3f(255.f, 255.f, 255.f);
    const double x  = (u - std::floor(u)) * double(width) - 0.5;
    const double y  = (1. - (v - std::floor(v))) * double(height) - 0.5;
    const double fx = std::floor(x), fy = std::floor(y);
    const float  tx = float(x - fx), ty = float(y - fy);
    auto         wrap = [](long i, int n) { return int(((i % long(n)) + long(n)) % long(n)); };
    const int    c0 = wrap(long(fx), width), c1 = wrap(long(fx) + 1, width);
    const int    r0 = wrap(long(fy), height), r1 = wrap(long(fy) + 1, height);
    auto         px = [this](int r, int c) {
        const uint8_t* p = &rgb[(size_t(r) * size_t(width) + size_t(c)) * 3];
        return Vec3f(p[0], p[1], p[2]);
    };
    const Vec3f top    = px(r0, c0) * (1.f - tx) + px(r0, c1) * tx;
    const Vec3f bottom = px(r1, c0) * (1.f - tx) + px(r1, c1) * tx;
    return top * (1.f - ty) + bottom * ty;
}

namespace {

struct PngMemory
{
    const uint8_t* data;
    size_t         size;
    size_t         pos;
};

void png_read_memory(png_structp png, png_bytep out, png_size_t n)
{
    auto* m = static_cast<PngMemory*>(png_get_io_ptr(png));
    if (m->pos + n > m->size)
        png_error(png, "unexpected end of PNG data");
    std::memcpy(out, m->data + m->pos, n);
    m->pos += n;
}

// Kept free of C++ objects with destructors between setjmp and the libpng calls that may longjmp.
bool decode_png_rgb(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgb, const char*& why)
{
    png_structp png  = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop   info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info) {
        png_destroy_read_struct(&png, &info, nullptr);
        why = "out of memory";
        return false;
    }
    PngMemory              mem{data, size, 0};
    std::vector<png_bytep> rows;
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        why = "not a readable PNG image";
        return false;
    }
    png_set_read_fn(png, &mem, png_read_memory);
    png_read_info(png, info);
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_strip_alpha(png);
    png_set_gray_to_rgb(png);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    width  = int(png_get_image_width(png, info));
    height = int(png_get_image_height(png, info));
    if (png_get_channels(png, info) != 3 || width <= 0 || height <= 0) {
        png_destroy_read_struct(&png, &info, nullptr);
        why = "unsupported PNG pixel format";
        return false;
    }
    rgb.assign(size_t(width) * size_t(height) * 3, 0);
    rows.resize(size_t(height));
    for (int r = 0; r < height; ++r)
        rows[size_t(r)] = rgb.data() + size_t(r) * size_t(width) * 3;
    png_read_image(png, rows.data());
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);
    return true;
}

struct JpegError
{
    jpeg_error_mgr pub;
    jmp_buf        jump;
    char           message[JMSG_LENGTH_MAX];
};

void jpeg_error_exit(j_common_ptr cinfo)
{
    auto* err = reinterpret_cast<JpegError*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->jump, 1);
}

bool decode_jpeg_rgb(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgb, std::string& why)
{
    jpeg_decompress_struct cinfo;
    JpegError              err;
    cinfo.err           = jpeg_std_error(&err.pub);
    err.pub.error_exit  = jpeg_error_exit;
    err.message[0]      = 0;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&cinfo);
        why = std::string("not a readable JPEG image: ") + err.message;
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, const_cast<unsigned char*>(data), (unsigned long) size);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    width  = int(cinfo.output_width);
    height = int(cinfo.output_height);
    rgb.assign(size_t(width) * size_t(height) * 3, 0);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = rgb.data() + size_t(cinfo.output_scanline) * size_t(width) * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0)
        return false;
    f.seekg(0, std::ios::beg);
    out.resize(size_t(n));
    if (n > 0)
        f.read(reinterpret_cast<char*>(out.data()), n);
    return bool(f);
}

std::string resolve_relative(const std::string& base_file, const std::string& name)
{
    boost::filesystem::path p(name);
    if (p.is_absolute())
        return p.string();
    return (boost::filesystem::path(base_file).parent_path() / p).string();
}

} // namespace

bool decode_texture_image(const uint8_t* data, size_t size, TextureImage& out, std::string& error)
{
    out.width = out.height = 0;
    out.rgb.clear();
    if (size >= 8 && png_sig_cmp(const_cast<png_bytep>(data), 0, 8) == 0) {
        const char* why = nullptr;
        if (!decode_png_rgb(data, size, out.width, out.height, out.rgb, why)) {
            error = why ? why : "PNG error";
            return false;
        }
        return true;
    }
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return decode_jpeg_rgb(data, size, out.width, out.height, out.rgb, error);
    error = "only PNG and JPEG images are supported";
    return false;
}

bool load_texture_image(const std::string& path, TextureImage& out, std::string& error)
{
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) {
        error = "cannot open \"" + path + "\"";
        return false;
    }
    if (!decode_texture_image(bytes.data(), bytes.size(), out, error)) {
        error = "\"" + path + "\": " + error;
        return false;
    }
    out.name = boost::filesystem::path(path).filename().string();
    return true;
}

bool TexturedMesh::has_color() const
{
    return !uvs.empty() && std::any_of(color_image.begin(), color_image.end(), [](int i) { return i >= 0; });
}

bool TexturedMesh::has_normals() const
{
    return !uvs.empty() && std::any_of(normal_image.begin(), normal_image.end(), [](int i) { return i >= 0; });
}

// Flips every facet with negative total volume, as the importers do, keeping the texture coordinates with their corners.
static void orient_outwards(TexturedMesh& m)
{
    if (its_volume(m.its) >= 0.f)
        return;
    for (size_t i = 0; i < m.its.indices.size(); ++i) {
        std::swap(m.its.indices[i](1), m.its.indices[i](2));
        if (i < m.uvs.size())
            std::swap(m.uvs[i][1], m.uvs[i][2]);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// glTF binary
// ---------------------------------------------------------------------------------------------------------------------

namespace {

std::vector<uint8_t> base64_decode(const std::string& s)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    int                  acc = 0, bits = 0;
    for (char c : s) {
        const int v = value(c);
        if (v < 0)
            continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((acc >> bits) & 0xFF));
        }
    }
    return out;
}

class Gltf
{
public:
    Gltf(const std::string& path) : m_path(path) {}

    bool load(std::string& error)
    {
        std::vector<uint8_t> file;
        if (!read_file(m_path, file)) {
            error = "cannot open \"" + m_path + "\"";
            return false;
        }
        auto u32 = [&](size_t at) {
            uint32_t v;
            std::memcpy(&v, file.data() + at, 4);
            return v;
        };
        std::string json_text;
        if (file.size() >= 20 && u32(0) == 0x46546C67u) { // "glTF"
            if (u32(4) != 2) {
                error = "only glTF 2.0 is supported";
                return false;
            }
            size_t at = 12;
            while (at + 8 <= file.size()) {
                const uint32_t len = u32(at), type = u32(at + 4);
                if (at + 8 + len > file.size())
                    break;
                if (type == 0x4E4F534Au) // JSON
                    json_text.assign(reinterpret_cast<const char*>(file.data() + at + 8), len);
                else if (type == 0x004E4942u) // BIN
                    m_bin.assign(file.data() + at + 8, file.data() + at + 8 + len);
                at += 8 + ((len + 3) & ~3u);
            }
        } else
            json_text.assign(file.begin(), file.end()); // .gltf text
        if (json_text.empty()) {
            error = "no glTF JSON found";
            return false;
        }
        try {
            m_j = nlohmann::json::parse(json_text);
        } catch (const std::exception& ex) {
            error = std::string("glTF JSON: ") + ex.what();
            return false;
        }
        // Compressed geometry would be read as raw bytes and come out as garbage: refuse it with a clear message.
        // KHR_mesh_quantization only stores attributes as normalized integers, which accessor() reads.
        for (const auto& ext : m_j.value("extensionsRequired", nlohmann::json::array())) {
            const std::string name = ext.is_string() ? ext.get<std::string>() : std::string();
            if (name == "EXT_meshopt_compression" || name == "KHR_meshopt_compression" || name == "KHR_draco_mesh_compression") {
                error = "the file's geometry is compressed (" + name +
                        "), which is not supported yet. Export it again without mesh compression, for example from Blender.";
                return false;
            }
        }
        return true;
    }

    const nlohmann::json& json() const { return m_j; }

    // Bytes of a buffer view.
    bool view(size_t index, const uint8_t*& data, size_t& size, size_t& stride, std::string& error)
    {
        const auto& views = m_j.value("bufferViews", nlohmann::json::array());
        if (index >= views.size()) {
            error = "bad buffer view";
            return false;
        }
        const auto&  v      = views[index];
        const size_t buffer = v.value("buffer", size_t(0));
        const std::vector<uint8_t>* buf = this->buffer(buffer, error);
        if (!buf)
            return false;
        const size_t offset = v.value("byteOffset", size_t(0));
        size                = v.value("byteLength", size_t(0));
        stride              = v.value("byteStride", size_t(0));
        if (offset + size > buf->size()) {
            error = "buffer view out of range";
            return false;
        }
        data = buf->data() + offset;
        return true;
    }

    // Accessor values as floats, count * components (normalized integers scaled to [0, 1]).
    bool accessor(size_t index, std::vector<float>& out, int& components, std::string& error)
    {
        const auto& accs = m_j.value("accessors", nlohmann::json::array());
        if (index >= accs.size()) {
            error = "bad accessor";
            return false;
        }
        const auto& a     = accs[index];
        const std::string type = a.value("type", std::string("SCALAR"));
        components             = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : 0;
        const int  ctype       = a.value("componentType", 5126);
        const bool normalized  = a.value("normalized", false);
        const size_t count     = a.value("count", size_t(0));
        const int  csize       = (ctype == 5120 || ctype == 5121) ? 1 : (ctype == 5122 || ctype == 5123) ? 2 : 4;
        if (components == 0 || a.contains("sparse")) {
            error = "unsupported accessor";
            return false;
        }
        out.assign(count * size_t(components), 0.f);
        if (!a.contains("bufferView"))
            return true; // all zeros
        const uint8_t* data   = nullptr;
        size_t         size   = 0, stride = 0;
        if (!view(a["bufferView"].get<size_t>(), data, size, stride, error))
            return false;
        const size_t offset = a.value("byteOffset", size_t(0));
        if (stride == 0)
            stride = size_t(csize * components);
        if (count > 0 && offset + (count - 1) * stride + size_t(csize * components) > size) {
            error = "accessor out of range";
            return false;
        }
        for (size_t i = 0; i < count; ++i)
            for (int c = 0; c < components; ++c) {
                const uint8_t* p = data + offset + i * stride + size_t(c * csize);
                float          v = 0.f;
                switch (ctype) {
                case 5126: std::memcpy(&v, p, 4); break;
                case 5121: v = normalized ? float(*p) / 255.f : float(*p); break;
                case 5120: v = normalized ? std::max(float(int8_t(*p)) / 127.f, -1.f) : float(int8_t(*p)); break;
                case 5123: { uint16_t t; std::memcpy(&t, p, 2); v = normalized ? float(t) / 65535.f : float(t); break; }
                case 5122: { int16_t t; std::memcpy(&t, p, 2); v = normalized ? std::max(float(t) / 32767.f, -1.f) : float(t); break; }
                case 5125: { uint32_t t; std::memcpy(&t, p, 4); v = float(t); break; }
                default: error = "unsupported component type"; return false;
                }
                out[i * size_t(components) + size_t(c)] = v;
            }
        return true;
    }

    // Indices need all 32 bits.
    bool indices(size_t index, std::vector<uint32_t>& out, std::string& error)
    {
        const auto& accs = m_j.value("accessors", nlohmann::json::array());
        if (index >= accs.size()) {
            error = "bad accessor";
            return false;
        }
        const auto&  a     = accs[index];
        const int    ctype = a.value("componentType", 5125);
        const size_t count = a.value("count", size_t(0));
        const int    csize = ctype == 5121 ? 1 : ctype == 5123 ? 2 : 4;
        const uint8_t* data = nullptr;
        size_t         size = 0, stride = 0;
        if (!a.contains("bufferView") || !view(a["bufferView"].get<size_t>(), data, size, stride, error))
            return false;
        const size_t offset = a.value("byteOffset", size_t(0));
        if (stride == 0)
            stride = size_t(csize);
        if (count > 0 && offset + (count - 1) * stride + size_t(csize) > size) {
            error = "index accessor out of range";
            return false;
        }
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* p = data + offset + i * stride;
            if (ctype == 5121)
                out[i] = *p;
            else if (ctype == 5123) {
                uint16_t t;
                std::memcpy(&t, p, 2);
                out[i] = t;
            } else
                std::memcpy(&out[i], p, 4);
        }
        return true;
    }

    // Decoded image of a texture index, cached; -1 when missing or unreadable.
    int texture_image(const nlohmann::json& texture_info, std::vector<TextureImage>& images)
    {
        if (!texture_info.is_object() || !texture_info.contains("index") || texture_info.value("texCoord", 0) != 0)
            return -1;
        const auto&  textures = m_j.value("textures", nlohmann::json::array());
        const size_t t        = texture_info["index"].get<size_t>();
        if (t >= textures.size() || !textures[t].contains("source"))
            return -1;
        const size_t source = textures[t]["source"].get<size_t>();
        if (auto it = m_image_index.find(source); it != m_image_index.end())
            return it->second;
        int         result = -1;
        std::string error;
        const auto& imgs   = m_j.value("images", nlohmann::json::array());
        if (source < imgs.size()) {
            const auto&  im = imgs[source];
            TextureImage image;
            bool         ok = false;
            if (im.contains("bufferView")) {
                const uint8_t* data = nullptr;
                size_t         size = 0, stride = 0;
                ok = view(im["bufferView"].get<size_t>(), data, size, stride, error) && decode_texture_image(data, size, image, error);
            } else if (im.contains("uri")) {
                const std::string uri = im["uri"].get<std::string>();
                if (boost::starts_with(uri, "data:")) {
                    const std::vector<uint8_t> bytes = base64_decode(uri.substr(uri.find(',') + 1));
                    ok = decode_texture_image(bytes.data(), bytes.size(), image, error);
                } else
                    ok = load_texture_image(resolve_relative(m_path, uri), image, error);
            }
            if (ok) {
                image.name = im.value("name", std::string("image ") + std::to_string(source));
                images.emplace_back(std::move(image));
                result = int(images.size()) - 1;
            } else
                BOOST_LOG_TRIVIAL(warning) << "glTF image " << source << " of " << m_path << ": " << error;
        }
        m_image_index[source] = result;
        return result;
    }

private:
    const std::vector<uint8_t>* buffer(size_t index, std::string& error)
    {
        if (auto it = m_buffers.find(index); it != m_buffers.end())
            return &it->second;
        const auto& bufs = m_j.value("buffers", nlohmann::json::array());
        if (index >= bufs.size()) {
            error = "bad buffer";
            return nullptr;
        }
        std::vector<uint8_t> data;
        if (!bufs[index].contains("uri")) {
            data = m_bin;
        } else {
            const std::string uri = bufs[index]["uri"].get<std::string>();
            if (boost::starts_with(uri, "data:"))
                data = base64_decode(uri.substr(uri.find(',') + 1));
            else if (!read_file(resolve_relative(m_path, uri), data)) {
                error = "cannot open buffer \"" + uri + "\"";
                return nullptr;
            }
        }
        return &(m_buffers[index] = std::move(data));
    }

    std::string                                   m_path;
    nlohmann::json                                m_j;
    std::vector<uint8_t>                          m_bin;
    std::map<size_t, std::vector<uint8_t>>        m_buffers;
    std::map<size_t, int>                         m_image_index;
};

Transform3d node_transform(const nlohmann::json& node)
{
    Transform3d t = Transform3d::Identity();
    if (node.contains("matrix") && node["matrix"].size() == 16) {
        Matrix4d m;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m(r, c) = node["matrix"][size_t(c * 4 + r)].get<double>(); // column major
        t.matrix() = m;
        return t;
    }
    if (node.contains("translation") && node["translation"].size() == 3)
        t.translate(Vec3d(node["translation"][0].get<double>(), node["translation"][1].get<double>(), node["translation"][2].get<double>()));
    if (node.contains("rotation") && node["rotation"].size() == 4)
        t.rotate(Eigen::Quaterniond(node["rotation"][3].get<double>(), node["rotation"][0].get<double>(), node["rotation"][1].get<double>(),
                                    node["rotation"][2].get<double>()));
    if (node.contains("scale") && node["scale"].size() == 3)
        t.scale(Vec3d(node["scale"][0].get<double>(), node["scale"][1].get<double>(), node["scale"][2].get<double>()));
    return t;
}

struct PositionHash
{
    size_t operator()(const std::array<float, 3>& p) const
    {
        uint32_t b[3];
        std::memcpy(b, p.data(), sizeof(b));
        return (size_t(b[0]) * 73856093u) ^ (size_t(b[1]) * 19349663u) ^ (size_t(b[2]) * 83492791u);
    }
};

} // namespace

bool load_glb(const std::string& path, TexturedMesh& out, std::string& error)
{
    out = TexturedMesh();
    out.source = path;
    Gltf gltf(path);
    if (!gltf.load(error))
        return false;
    const nlohmann::json& j = gltf.json();

    const auto& nodes  = j.value("nodes", nlohmann::json::array());
    const auto& meshes = j.value("meshes", nlohmann::json::array());
    const auto& mats   = j.value("materials", nlohmann::json::array());

    std::vector<size_t> roots;
    const auto&         scenes = j.value("scenes", nlohmann::json::array());
    const size_t        scene  = j.value("scene", size_t(0));
    if (scene < scenes.size() && scenes[scene].contains("nodes"))
        for (const auto& n : scenes[scene]["nodes"])
            roots.push_back(n.get<size_t>());
    else {
        // no scene: every node that is nobody's child
        std::vector<bool> child(nodes.size(), false);
        for (const auto& n : nodes)
            if (n.contains("children"))
                for (const auto& c : n["children"])
                    if (c.get<size_t>() < child.size())
                        child[c.get<size_t>()] = true;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (!child[i])
                roots.push_back(i);
    }

    // glTF is Y up, the bed is Z up: (x, y, z) -> (x, -z, y), front (+Z in glTF) towards -Y.
    Transform3d y_up_to_z_up = Transform3d::Identity();
    y_up_to_z_up.matrix().block<3, 3>(0, 0) << 1, 0, 0, 0, 0, -1, 0, 1, 0;

    std::unordered_map<std::array<float, 3>, int, PositionHash> welded;
    std::vector<std::pair<size_t, Transform3d>>                 stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
        stack.emplace_back(*it, y_up_to_z_up);
    size_t guard = 0;
    while (!stack.empty() && ++guard < 100000) {
        const auto [ni, parent] = stack.back();
        stack.pop_back();
        if (ni >= nodes.size())
            continue;
        const auto&       node  = nodes[ni];
        const Transform3d world = parent * node_transform(node);
        if (node.contains("children"))
            for (auto it = node["children"].rbegin(); it != node["children"].rend(); ++it)
                stack.emplace_back(it->get<size_t>(), world);
        if (!node.contains("mesh") || node["mesh"].get<size_t>() >= meshes.size())
            continue;
        const bool mirrored = world.linear().determinant() < 0.;
        for (const auto& prim : meshes[node["mesh"].get<size_t>()].value("primitives", nlohmann::json::array())) {
            if (prim.value("mode", 4) != 4 || !prim.contains("attributes") || !prim["attributes"].contains("POSITION"))
                continue;
            std::vector<float> pos, uv;
            int                pc = 0, uc = 0;
            if (!gltf.accessor(prim["attributes"]["POSITION"].get<size_t>(), pos, pc, error) || pc != 3)
                return false;
            const size_t nv = pos.size() / 3;
            if (prim["attributes"].contains("TEXCOORD_0") &&
                (!gltf.accessor(prim["attributes"]["TEXCOORD_0"].get<size_t>(), uv, uc, error) || uc != 2 || uv.size() / 2 != nv))
                uv.clear();
            std::vector<uint32_t> idx;
            if (prim.contains("indices")) {
                if (!gltf.indices(prim["indices"].get<size_t>(), idx, error))
                    return false;
            } else {
                idx.resize(nv);
                for (size_t i = 0; i < nv; ++i)
                    idx[i] = uint32_t(i);
            }
            int color = -1, normal = -1;
            if (prim.contains("material") && prim["material"].get<size_t>() < mats.size()) {
                const auto& mat = mats[prim["material"].get<size_t>()];
                if (mat.contains("pbrMetallicRoughness") && mat["pbrMetallicRoughness"].contains("baseColorTexture"))
                    color = gltf.texture_image(mat["pbrMetallicRoughness"]["baseColorTexture"], out.images);
                if (mat.contains("normalTexture"))
                    normal = gltf.texture_image(mat["normalTexture"], out.images);
            }
            std::vector<int> vmap(nv, -1);
            auto             vertex = [&](uint32_t i) {
                if (vmap[i] >= 0)
                    return vmap[i];
                const Vec3d                 p = world * Vec3d(pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]);
                const std::array<float, 3>  key{float(p.x()), float(p.y()), float(p.z())};
                auto [it, inserted] = welded.emplace(key, int(out.its.vertices.size()));
                if (inserted)
                    out.its.vertices.emplace_back(key[0], key[1], key[2]);
                return vmap[i] = it->second;
            };
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                uint32_t c[3] = {idx[t], idx[t + 1], idx[t + 2]};
                if (c[0] >= nv || c[1] >= nv || c[2] >= nv)
                    continue;
                if (mirrored)
                    std::swap(c[1], c[2]);
                const int a = vertex(c[0]), b = vertex(c[1]), d = vertex(c[2]);
                if (a == b || b == d || a == d)
                    continue; // degenerate after welding
                out.its.indices.emplace_back(a, b, d);
                std::array<Vec2f, 3> f;
                for (int k = 0; k < 3; ++k)
                    f[k] = uv.empty() ? Vec2f(0.f, 0.f) : Vec2f(uv[2 * c[k]], 1.f - uv[2 * c[k] + 1]); // glTF v runs down
                out.uvs.push_back(f);
                out.color_image.push_back(uv.empty() ? -1 : color);
                out.normal_image.push_back(uv.empty() ? -1 : normal);
            }
        }
    }
    if (out.its.indices.empty()) {
        error = "the file has no triangles";
        return false;
    }
    orient_outwards(out);
    return true;
}

bool load_glb(const char* path, Model* model, std::string& message)
{
    TexturedMesh mesh;
    if (!load_glb(std::string(path), mesh, message))
        return false;
    TriangleMesh tm(std::move(mesh.its));
    if (tm.empty()) {
        message = "the file has no triangles";
        return false;
    }
    model->add_object(boost::filesystem::path(path).filename().string().c_str(), path, std::move(tm));
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// OBJ
// ---------------------------------------------------------------------------------------------------------------------

bool load_obj_textured(const std::string& path, TexturedMesh& out, std::string& error)
{
    out        = TexturedMesh();
    out.source = path;
    ObjParser::ObjData data;
    ObjParser::MtlData mtl;
    if (!ObjParser::objparse(path.c_str(), data)) {
        error = "\"" + path + "\" is not a readable OBJ file";
        return false;
    }
    bool has_mtl = false;
    for (const std::string& lib : data.mtllibs) {
        if (lib.empty())
            continue;
        const std::string mtl_path = resolve_relative(path, lib);
        if (boost::filesystem::exists(mtl_path) && ObjParser::mtlparse(mtl_path.c_str(), mtl))
            has_mtl = true;
    }
    const size_t n_coords = data.coordinates.size() / OBJ_VERTEX_LENGTH;
    const size_t n_uvs    = data.textureCoordinates.size() / 2;
    for (size_t i = 0; i < n_coords; ++i)
        out.its.vertices.emplace_back(data.coordinates[i * OBJ_VERTEX_LENGTH], data.coordinates[i * OBJ_VERTEX_LENGTH + 1],
                                      data.coordinates[i * OBJ_VERTEX_LENGTH + 2]);

    std::map<std::string, int> image_of_file;
    auto image = [&](const std::string& file) -> int {
        if (file.empty())
            return -1;
        const std::string full = resolve_relative(path, file);
        if (auto it = image_of_file.find(full); it != image_of_file.end())
            return it->second;
        TextureImage img;
        std::string  why;
        int          result = -1;
        if (load_texture_image(full, img, why)) {
            out.images.emplace_back(std::move(img));
            result = int(out.images.size()) - 1;
        } else
            BOOST_LOG_TRIVIAL(warning) << "OBJ texture: " << why;
        return image_of_file[full] = result;
    };
    // Material of a triangle, as load_obj() finds it.
    auto material = [&](int face_index) -> const ObjParser::ObjNewMtl* {
        if (!has_mtl || data.usemtls.empty())
            return nullptr;
        const ObjParser::ObjUseMtl* use = nullptr;
        if (data.usemtls.size() == 1)
            use = &data.usemtls.front();
        else
            for (const ObjParser::ObjUseMtl& u : data.usemtls)
                if (face_index >= u.face_start && face_index <= u.face_end) {
                    use = &u;
                    break;
                }
        if (use == nullptr)
            return nullptr;
        auto it = mtl.new_mtl_unmap.find(use->name);
        return it == mtl.new_mtl_unmap.end() ? nullptr : it->second.get();
    };

    int  corners[4], tex[4];
    auto add = [&](int a, int b, int c) {
        out.its.indices.emplace_back(corners[a], corners[b], corners[c]);
        const int face = int(out.its.indices.size()) - 1;
        std::array<Vec2f, 3> f;
        const int            t[3] = {tex[a], tex[b], tex[c]};
        bool                 has_uv = true;
        for (int k = 0; k < 3; ++k) {
            if (t[k] >= 0 && size_t(t[k]) < n_uvs)
                f[k] = Vec2f(data.textureCoordinates[2 * t[k]], data.textureCoordinates[2 * t[k] + 1]);
            else {
                f[k]   = Vec2f(0.f, 0.f);
                has_uv = false;
            }
        }
        out.uvs.push_back(f);
        const ObjParser::ObjNewMtl* m = material(face);
        out.color_image.push_back(has_uv && m ? image(m->map_Kd) : -1);
        out.normal_image.push_back(has_uv && m ? image(m->map_Bump) : -1);
    };
    for (size_t i = 0; i < data.vertices.size();) {
        if (data.vertices[i].coordIdx == -1) {
            ++i;
            continue;
        }
        int cnt = 0;
        while (i < data.vertices.size()) {
            const ObjParser::ObjVertex& v = data.vertices[i++];
            if (v.coordIdx == -1)
                break;
            if (cnt >= 4) {
                error = "the file contains polygons with more than 4 vertices";
                return false;
            }
            if (v.coordIdx < 0 || size_t(v.coordIdx) >= n_coords) {
                error = "the file contains an invalid vertex index";
                return false;
            }
            corners[cnt] = v.coordIdx;
            tex[cnt]     = v.textureCoordIdx;
            ++cnt;
        }
        if (cnt < 3) {
            error = "the file contains polygons with less than 3 vertices";
            return false;
        }
        add(0, 1, 2);
        if (cnt == 4)
            add(0, 2, 3);
    }
    if (out.its.indices.empty()) {
        error = "the file has no triangles";
        return false;
    }
    if (n_uvs == 0) {
        out.uvs.clear();
        out.color_image.clear();
        out.normal_image.clear();
    }
    orient_outwards(out);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------------------------------------------------

bool is_textured_model_file(const std::string& path)
{
    return boost::iends_with(path, ".glb") || boost::iends_with(path, ".gltf") || boost::iends_with(path, ".obj");
}

namespace {
struct CacheEntry
{
    std::time_t                         mtime = 0;
    std::shared_ptr<const TexturedMesh> mesh;
    std::string                         error;
};
std::mutex                        g_cache_mutex;
std::map<std::string, CacheEntry> g_cache;
} // namespace

std::shared_ptr<const TexturedMesh> load_textured_mesh(const std::string& path, std::string* error)
{
    auto fail = [error](const std::string& msg) -> std::shared_ptr<const TexturedMesh> {
        if (error)
            *error = msg;
        return nullptr;
    };
    if (path.empty() || !is_textured_model_file(path))
        return fail("not a .glb or .obj file");
    boost::system::error_code ec;
    const std::time_t         mtime = boost::filesystem::last_write_time(boost::filesystem::path(path), ec);
    if (ec)
        return fail("cannot open \"" + path + "\"");
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (auto it = g_cache.find(path); it != g_cache.end() && it->second.mtime == mtime) {
        if (!it->second.mesh)
            return fail(it->second.error);
        return it->second.mesh;
    }
    auto        mesh = std::make_shared<TexturedMesh>();
    std::string why;
    const bool  ok = boost::iends_with(path, ".obj") ? load_obj_textured(path, *mesh, why) : load_glb(path, *mesh, why);
    if (ok && mesh->uvs.empty()) {
        why = "\"" + path + "\" has no texture coordinates";
    }
    CacheEntry& entry = g_cache[path];
    entry.mtime       = mtime;
    entry.mesh        = ok && !mesh->uvs.empty() ? std::shared_ptr<const TexturedMesh>(std::move(mesh)) : nullptr;
    entry.error       = why;
    if (!entry.mesh)
        return fail(why);
    return entry.mesh;
}

} // namespace Slic3r
