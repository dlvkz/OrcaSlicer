#ifndef slic3r_TextureRelief_hpp_
#define slic3r_TextureRelief_hpp_

#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

class ModelObject;
class ModelVolume;
struct TexturedMesh;

// The textured mesh a volume was imported from: its source file (.glb / .obj) loaded with texture coordinates, with the
// same facets as the volume. nullptr when there is none or the volume's mesh no longer matches it.
std::shared_ptr<const TexturedMesh> textured_mesh_of_volume(const ModelObject& object, const ModelVolume& volume);

// Whether one of the object's parts has a normal map.
bool has_normal_map(const ModelObject& object);

// In-place 2-D FFT of a w x h row-major grid; w and h must be powers of two. The inverse is scaled by 1 / (w h).
void fft2d(std::vector<std::complex<float>>& a, size_t w, size_t h, bool inverse);
size_t next_pow2(size_t n);
// 16-bit greyscale PNG.
bool write_png16(const std::string& path, const std::vector<uint16_t>& px, size_t w, size_t h, std::string& error);

// ---------------------------------------------------------------------------------------------------------------------
// Normal map -> surface displacement
//
// The tangent space normal map is integrated into a height map in texture space (each slope scaled by the surface
// millimetres per texel of its triangle, Frankot-Chellappa), high-passed to the relief narrower than highpass_mm and
// written as a 16-bit PNG. The object then gets the fuzzy skin displacement map in the UV projection with the source
// file as the UV mesh, and a depth that covers 99.9% of the relief.
// ---------------------------------------------------------------------------------------------------------------------
struct TextureReliefParams
{
    double      highpass_mm = 3.0;
    std::string output_dir; // where the height map is written; empty: data_dir()/texture_relief or the temp folder
};

struct TextureReliefReport
{
    std::string height_map;
    double      depth_mm = 0.;
    std::string summary() const;
};

bool apply_normal_map_relief(ModelObject& object, const TextureReliefParams& params, TextureReliefReport& report, std::string& error);

} // namespace Slic3r

#endif // slic3r_TextureRelief_hpp_
