#ifndef slic3r_Format_TexturedMesh_hpp_
#define slic3r_Format_TexturedMesh_hpp_

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Point.hpp"
#include "admesh/stl.h"

namespace Slic3r {

class Model;

// An image decoded to 8-bit RGB, row 0 at the top.
struct TextureImage
{
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgb;
    std::string          name;

    bool empty() const { return width <= 0 || height <= 0; }
    // Bilinear lookup with wrap-around at texture coordinates (u right, v up, one image per unit), 0..255 per channel.
    Vec3f sample(double u, double v) const;
};

// Decodes a PNG or JPEG image held in memory.
bool decode_texture_image(const uint8_t* data, size_t size, TextureImage& out, std::string& error);
bool load_texture_image(const std::string& path, TextureImage& out, std::string& error);

// A mesh with texture coordinates and the colour and normal maps of its faces. The triangles are in the order the
// model importer creates them, so they match the facets of the imported volume one to one.
struct TexturedMesh
{
    indexed_triangle_set              its;          // positions as imported (Z up)
    std::vector<std::array<Vec2f, 3>> uvs;          // per facet, OBJ convention: u right, v up
    std::vector<int>                  color_image;  // per facet, index into images or -1
    std::vector<int>                  normal_image; // per facet, index into images or -1
    std::vector<TextureImage>         images;
    std::string                       source;

    bool has_color() const;
    bool has_normals() const;
};

// glTF binary (.glb): every triangle primitive of the default scene with its node transforms, converted from glTF's
// Y up to Z up and welded by position (glTF splits vertices at UV seams). Images embedded in the file or next to it.
bool load_glb(const std::string& path, TexturedMesh& out, std::string& error);
// A .glb file as a model object.
bool load_glb(const char* path, Model* model, std::string& message);

// OBJ with texture coordinates and an MTL with map_Kd (colour) and norm / bump / map_Bump (normal map). The triangles
// follow load_obj().
bool load_obj_textured(const std::string& path, TexturedMesh& out, std::string& error);

// .glb or .obj through a process-wide cache keyed by path and modification time. nullptr when the file has no texture
// coordinates or can't be read.
std::shared_ptr<const TexturedMesh> load_textured_mesh(const std::string& path, std::string* error = nullptr);

bool is_textured_model_file(const std::string& path);

} // namespace Slic3r

#endif // slic3r_Format_TexturedMesh_hpp_
