#ifndef slic3r_GUI_DisplacementPreview_hpp_
#define slic3r_GUI_DisplacementPreview_hpp_

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Config.hpp"

#include "libslic3r/Point.hpp"
#include "admesh/stl.h"

namespace Slic3r {

class DynamicPrintConfig;
class GLVolumeCollection;
class Model;

namespace Feature::FuzzySkin {
struct DisplacementMap;
}

namespace GUI {

class GLCanvas3D;

// Shows a fuzzy skin displacement map on the models in the 3D editor, before slicing, live. With OpenGL 4.0 the
// tessellation shader (gouraud_displace) splits each part's triangles on the GPU, as finely as they show on screen, and
// moves the surface by the height the slicer computes; the detail between the points is shaded per pixel. Older OpenGL,
// and the UV projection, only shade the map on the surface (gouraud). A small panel holds the displacement settings of
// the selected object (or the process preset) and a switch to hide the preview.
class DisplacementPreview
{
public:
    DisplacementPreview() = default;
    DisplacementPreview(const DisplacementPreview&) = delete;
    DisplacementPreview& operator=(const DisplacementPreview&) = delete;
    ~DisplacementPreview();

    // Fills GLVolume::displacement_preview of every model part from the settings that apply to it (process, object,
    // part). Needs the canvas's OpenGL context. Returns true when the picture changed since the last call.
    bool update(GLVolumeCollection& volumes, const Model& model, const DynamicPrintConfig& print_config);

    // The panel with the displacement settings of the selected object (or of the only object, or else of the process
    // preset), shown in the 3D editor from the start. Call between ImGui new_frame() and render.
    void render_panel(GLCanvas3D& canvas);

    // A panel slider is being dragged: the canvas leaves the mouse alone until it is released.
    bool dragging() const { return m_drag.has_value(); }
    // The point (canvas pixels) is on the panel as last drawn. ImGui only knows the mouse is over its window after it has
    // drawn a frame with it there, so a click right after the mouse arrives could otherwise go to the scene below too.
    bool panel_contains(float x, float y) const
    {
        return x >= m_panel_rect[0] && x < m_panel_rect[2] && y >= m_panel_rect[1] && y < m_panel_rect[3];
    }
    // The panel is not drawn (a gizmo is open, or another view).
    void hide_panel() { m_panel_rect = { 0.f, 0.f, 0.f, 0.f }; }

    // Frees the OpenGL textures and stops the background work. Needs the OpenGL context.
    void release();

private:
    struct Texture
    {
        std::shared_ptr<const Feature::FuzzySkin::DisplacementMap> map;
        unsigned int                                               id{ 0 };
    };

    unsigned int texture_for(const std::shared_ptr<const Feature::FuzzySkin::DisplacementMap>& map);

    // A pattern laid on the surface of one part (Feature/FuzzySkin/SurfacePattern): its sites uploaded for the shader.
    // Built in the part's world coordinates less the translation, so moving the part keeps it.
    struct SurfaceSites
    {
        const void*  mesh{ nullptr };
        Matrix3d     linear{ Matrix3d::Identity() };
        int          pattern{ 0 };
        double       size{ 0. };
        unsigned int sites_texture{ 0 };
        unsigned int buckets_texture{ 0 };
        int          bucket_mask{ 0 };
        float        cell{ 1.f };
        Vec3f        origin{ Vec3f::Zero() };
        bool         used{ false };
    };
    const SurfaceSites* surface_sites_for(const void* mesh, const indexed_triangle_set& its, const Matrix3d& linear, int pattern, double size);

    // A value of the panel's settings: written to the object it edits (and its parts that set it themselves), so each
    // object can have its own; with no object, to the process preset (the defaults).
    using Values = std::vector<std::pair<std::string, std::unique_ptr<ConfigOption>>>;
    const ConfigOption* panel_option(const Model& model, const DynamicPrintConfig& process, const std::string& key) const;
    bool                writes_locally(const Model& model, int object, const std::string& key) const;
    // Whether the object or one of its parts sets the key itself.
    bool                has_own(const Model& model, int object, const std::string& key) const;
    void                commit(GLCanvas3D& canvas, int object, Values values);

    std::vector<Texture>      m_textures;
    std::vector<SurfaceSites> m_surface_sites;
    size_t               m_state_hash{ 0 };
    bool                 m_show{ true };
    // The object the panel edits, -1 for the process preset.
    int                  m_panel_object{ -1 };
    // Where the panel was last drawn: left, top, right, bottom in canvas pixels; empty when hidden.
    std::array<float, 4> m_panel_rect{ 0.f, 0.f, 0.f, 0.f };
    // A slider being dragged: its value is shown on the models where it will be written, and written when released.
    struct Drag
    {
        std::string       key;
        ConfigOptionFloat value;
        bool              local{ false }; // written to the object (or its parts), not the process preset
        int               object{ -1 };
    };
    std::optional<Drag>          m_drag;
    // The top of sliders whose range follows their value (the texture depth), kept while dragging.
    std::map<std::string, float> m_slider_top;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DisplacementPreview_hpp_
