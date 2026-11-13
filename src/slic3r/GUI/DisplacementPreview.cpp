#include "DisplacementPreview.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_map>

#include <GL/glew.h>
#include <imgui/imgui.h>

#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Feature/FuzzySkin/FuzzySkin.hpp"
#include "libslic3r/Feature/FuzzySkin/SurfacePattern.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "GLModel.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "ImGuiWrapper.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "TextToHeightDialog.hpp"
#include "libslic3r/TextToHeight.hpp"

#include <boost/filesystem/path.hpp>
#include <wx/filedlg.h>

namespace Slic3r::GUI {

using Feature::FuzzySkin::DisplacementMap;
using Feature::FuzzySkin::UVMesh;

namespace {

// The part's triangles, one vertex per corner, with the texture coordinates of the UV mesh. The UV mesh is fitted to
// the part by bounding box like the slicer does (SurfaceDisplacement), and each triangle takes its corners' coordinates
// from the UV triangle closest to its centre, so seams in the texture layout stay sharp.
void build_uv_model(GLModel& out, const indexed_triangle_set& its, const UVMesh& uv)
{
    out.reset();
    if (its.indices.empty() || its.vertices.empty() || uv.faces.empty())
        return;
    Vec3d its_min = its.vertices.front().cast<double>(), its_max = its_min;
    for (const stl_vertex& v : its.vertices) {
        its_min = its_min.cwiseMin(v.cast<double>());
        its_max = its_max.cwiseMax(v.cast<double>());
    }
    const Vec3d uv_size = uv.max - uv.min, its_size = its_max - its_min;
    Vec3d       scale;
    for (int k = 0; k < 3; ++k)
        scale(k) = uv_size(k) > 1e-9 ? its_size(k) / uv_size(k) : 1.;
    std::vector<Vec3d> verts;
    verts.reserve(uv.vertices.size());
    for (const Vec3d& v : uv.vertices)
        verts.emplace_back(its_min + (v - uv.min).cwiseProduct(scale));
    const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(verts, uv.faces);

    GLModel::Geometry geometry;
    geometry.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2 };
    geometry.reserve_vertices(3 * its.indices.size());
    geometry.reserve_indices(3 * its.indices.size());
    unsigned int count = 0;
    for (const stl_triangle_vertex_indices& f : its.indices) {
        const Vec3d corner[3] = { its.vertices[f(0)].cast<double>(), its.vertices[f(1)].cast<double>(), its.vertices[f(2)].cast<double>() };
        const Vec3d centre    = (corner[0] + corner[1] + corner[2]) / 3.;
        size_t      hit       = 0;
        Vec3d       closest;
        Vec2d       tex[3]    = { Vec2d::Zero(), Vec2d::Zero(), Vec2d::Zero() };
        if (AABBTreeIndirect::squared_distance_to_indexed_triangle_set(verts, uv.faces, tree, centre, hit, closest) >= 0.) {
            const stl_triangle_vertex_indices& g = uv.faces[hit];
            const Vec3d &a = verts[g(0)], &b = verts[g(1)], &c = verts[g(2)];
            const Vec3d  e0 = b - a, e1 = c - a;
            const double d00 = e0.dot(e0), d01 = e0.dot(e1), d11 = e1.dot(e1), den = d00 * d11 - d01 * d01;
            for (int k = 0; k < 3; ++k) {
                const Vec3d  e2  = corner[k] - a;
                const double d20 = e2.dot(e0), d21 = e2.dot(e1);
                // Not clamped: a corner just off the UV triangle extrapolates within the same patch of the texture.
                const double wb = std::abs(den) > 1e-18 ? (d11 * d20 - d01 * d21) / den : 0.;
                const double wc = std::abs(den) > 1e-18 ? (d00 * d21 - d01 * d20) / den : 0.;
                tex[k]          = uv.uvs[3 * hit] * (1. - wb - wc) + uv.uvs[3 * hit + 1] * wb + uv.uvs[3 * hit + 2] * wc;
            }
        }
        Vec3f n = (corner[1] - corner[0]).cross(corner[2] - corner[0]).cast<float>();
        if (n.squaredNorm() > 0.f)
            n.normalize();
        for (int k = 0; k < 3; ++k)
            geometry.add_vertex(Vec3f(corner[k].cast<float>()), n, Vec2f(tex[k].cast<float>()));
        count += 3;
        geometry.add_triangle(count - 3, count - 2, count - 1);
    }
    out.init_from(std::move(geometry));
}

std::string string_option(const ConfigOption* opt)
{
    const auto* s = dynamic_cast<const ConfigOptionString*>(opt);
    return s ? s->value : std::string();
}

} // namespace

namespace {
// The part's mesh as patches of three for the tessellation shader (gouraud_displace), which splits them on the GPU and
// moves the surface. The normals are smooth (weighted by the faces' areas): the shader moves the points along a
// triangle's edges by them, the same for the triangles on both sides, so the moved surface stays closed.
void build_patch_model(GLModel& out, const indexed_triangle_set& its)
{
    std::vector<Vec3f> normals(its.vertices.size(), Vec3f::Zero());
    for (const stl_triangle_vertex_indices& f : its.indices) {
        const Vec3f n = (its.vertices[f(1)] - its.vertices[f(0)]).cross(its.vertices[f(2)] - its.vertices[f(0)]);
        for (int k = 0; k < 3; ++k)
            normals[f(k)] += n;
    }
    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Patches, GLModel::Geometry::EVertexLayout::P3N3 };
    g.reserve_vertices(its.vertices.size());
    g.reserve_indices(3 * its.indices.size());
    for (size_t i = 0; i < its.vertices.size(); ++i) {
        const float len = normals[i].norm();
        g.add_vertex(its.vertices[i], len > 0.f ? Vec3f(normals[i] / len) : Vec3f(Vec3f::UnitZ()));
    }
    for (const stl_triangle_vertex_indices& f : its.indices)
        g.add_triangle(unsigned(f(0)), unsigned(f(1)), unsigned(f(2)));
    out.reset();
    out.init_from(std::move(g));
}
} // namespace

DisplacementPreview::~DisplacementPreview()
{
    // The textures belong to the canvas's OpenGL context, which may be gone by now; GLCanvas3D calls release().
}

void DisplacementPreview::release()
{
    for (Texture& t : m_textures)
        if (t.id != 0)
            glsafe(::glDeleteTextures(1, &t.id));
    m_textures.clear();
    for (SurfaceSites& s : m_surface_sites) {
        glsafe(::glDeleteTextures(1, &s.sites_texture));
        glsafe(::glDeleteTextures(1, &s.buckets_texture));
    }
    m_surface_sites.clear();
}

namespace {
// Texels per row of the lookup textures (the shader divides by the same number).
constexpr size_t SITES_ROW = 1024;

unsigned int upload_float_rows(const std::vector<float>& data, int channels)
{
    const size_t texels = std::max<size_t>(1, data.size() / size_t(channels));
    const size_t rows   = (texels + SITES_ROW - 1) / SITES_ROW;
    std::vector<float> padded(rows * SITES_ROW * size_t(channels), 0.f);
    std::copy(data.begin(), data.end(), padded.begin());
    unsigned int id = 0;
    glsafe(::glGenTextures(1, &id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, id));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, channels == 4 ? GL_RGBA32F : GL_RG32F, GLsizei(SITES_ROW), GLsizei(rows), 0,
                          channels == 4 ? GL_RGBA : GL_RG, GL_FLOAT, padded.data()));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 4));
    return id;
}
} // namespace

const DisplacementPreview::SurfaceSites* DisplacementPreview::surface_sites_for(
    const void* mesh, const indexed_triangle_set& its, const Matrix3d& linear, int pattern, double size)
{
    for (SurfaceSites& s : m_surface_sites)
        if (s.mesh == mesh && s.pattern == pattern && s.size == size && s.linear.isApprox(linear, 1e-9)) {
            s.used = true;
            return &s;
        }
    if (!wxGetApp().is_gl_version_greater_or_equal_to(3, 0) || its.indices.empty())
        return nullptr;
    std::vector<Vec3d> vertices;
    vertices.reserve(its.vertices.size());
    for (const stl_vertex& v : its.vertices)
        vertices.emplace_back(linear * v.cast<double>());
    const Feature::FuzzySkin::SurfacePattern sp(FuzzySkinTexturePattern(pattern), size, vertices, its.indices);
    // Per site one texel (position, tag), or with charts three: (position, tag), (normal, offset x), (right, offset y).
    const bool         charts = Feature::FuzzySkin::SurfacePattern::uses_charts(FuzzySkinTexturePattern(pattern));
    std::vector<float> sites;
    sites.reserve((charts ? 12 : 4) * sp.sites().size());
    for (const auto& site : sp.sites()) {
        sites.insert(sites.end(), { site.position.x(), site.position.y(), site.position.z(), site.tag });
        if (charts)
            sites.insert(sites.end(), { site.normal.x(), site.normal.y(), site.normal.z(), site.offset.x(),
                                        site.right.x(), site.right.y(), site.right.z(), site.offset.y() });
    }
    std::vector<float> buckets;
    const auto&        start = sp.bucket_start();
    buckets.reserve(2 * start.size());
    for (size_t b = 0; b + 1 < start.size(); ++b) {
        buckets.push_back(float(start[b]));
        buckets.push_back(float(start[b + 1] - start[b]));
    }
    SurfaceSites s;
    s.mesh            = mesh;
    s.linear          = linear;
    s.pattern         = pattern;
    s.size            = size;
    s.sites_texture   = upload_float_rows(sites, 4);
    s.buckets_texture = upload_float_rows(buckets, 2);
    s.bucket_mask     = start.size() >= 2 ? int(start.size() - 2) : 0;
    s.cell            = float(sp.cell());
    s.origin          = sp.origin().cast<float>();
    s.used            = true;
    m_surface_sites.push_back(s);
    return &m_surface_sites.back();
}

unsigned int DisplacementPreview::texture_for(const std::shared_ptr<const DisplacementMap>& map)
{
    for (const Texture& t : m_textures)
        if (t.map == map)
            return t.id;
    // A float texture with mipmaps, so a fine map seen from afar averages out instead of sparkling.
    if (!wxGetApp().is_gl_version_greater_or_equal_to(3, 0) || map->cols == 0 || map->rows == 0)
        return 0;
    unsigned int id = 0;
    glsafe(::glGenTextures(1, &id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, id));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, GLsizei(map->cols), GLsizei(map->rows), 0, GL_RED, GL_FLOAT, map->height.data()));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glGenerateMipmap(GL_TEXTURE_2D));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 4));
    m_textures.push_back({ map, id });
    return id;
}

bool DisplacementPreview::update(GLVolumeCollection& volumes, const Model& model, const DynamicPrintConfig& print_config)
{
    size_t hash = std::hash<bool>()(m_show);
    auto   mix  = [&hash](size_t v) { hash ^= v + size_t(0x9e3779b97f4a7c15ULL) + (hash << 6) + (hash >> 2); };
    auto   mixf = [&mix](float f) { mix(std::hash<float>()(f)); };

    // The projections are centred on the XY middle of each object instance's bounding box, as the slicer centres them.
    std::map<std::pair<int, int>, BoundingBoxf3> boxes;
    for (const GLVolume* v : volumes.volumes)
        if (v != nullptr && !v->is_modifier && !v->is_wipe_tower && v->volume_idx() >= 0)
            boxes[{ v->object_idx(), v->instance_idx() }].merge(v->transformed_convex_hull_bounding_box());

    std::vector<const DisplacementMap*> used;
    // Without the tessellation shader (OpenGL older than 4.0) the map is only shaded on the surface.
    const bool can_tessellate = wxGetApp().get_shader("gouraud_displace") != nullptr;
    for (SurfaceSites& s : m_surface_sites)
        s.used = false;
    for (GLVolume* v : volumes.volumes) {
        if (v == nullptr)
            continue;
        GLVolume::DisplacementPreview& dp = v->displacement_preview;
        dp.active                         = false;
        dp.displaced                      = false;
        if (!m_show || v->is_modifier || v->is_wipe_tower || v->volume_idx() < 0 || v->object_idx() < 0 ||
            size_t(v->object_idx()) >= model.objects.size())
            continue;
        const ModelObject* mo = model.objects[v->object_idx()];
        if (size_t(v->volume_idx()) >= mo->volumes.size())
            continue;
        const ModelVolume* mv = mo->volumes[v->volume_idx()];
        if (!mv->is_model_part())
            continue;
        // A part's setting overrides its object's, which overrides the process preset's.
        auto opt = [&](const char* key) -> const ConfigOption* {
            // A slider of the panel being dragged, on the parts its value will be written to: the whole object it edits,
            // or, editing the defaults, the objects without a value of their own.
            if (m_drag && m_drag->key == key) {
                const bool own = mv->config.has(key) || mo->config.has(key);
                if (m_drag->local ? v->object_idx() == m_drag->object : !own)
                    return &m_drag->value;
            }
            if (mv->config.has(key))
                return mv->config.option(key);
            if (mo->config.has(key))
                return mo->config.option(key);
            return print_config.option(key);
        };
        auto as_int = [&](const char* key) { const ConfigOption* o = opt(key); return o ? o->getInt() : 0; };
        if (as_int("fuzzy_skin") == int(FuzzySkinType::None) || as_int("fuzzy_skin_noise_type") != int(NoiseType::DisplacementMap))
            continue;
        const ConfigOption* depth_opt = opt("fuzzy_skin_thickness");
        float               depth     = depth_opt ? float(depth_opt->getFloat()) : 0.f;
        std::string error;
        const auto  map = Feature::FuzzySkin::load_texture(FuzzySkinTexturePattern(as_int("fuzzy_skin_texture_pattern")),
                                                           string_option(opt("fuzzy_skin_texture")), &error, false);
        if (!map)
            continue;
        const auto projection = FuzzySkinTextureProjection(as_int("fuzzy_skin_texture_projection"));
        std::shared_ptr<const UVMesh> uv;
        if (projection == FuzzySkinTextureProjection::UV) {
            // The part's own model file unless a UV mesh is set, as the slicer does.
            uv = Feature::FuzzySkin::load_uv_mesh(Feature::FuzzySkin::uv_mesh_source(string_option(opt("fuzzy_skin_texture_uv_mesh")),
                                                                                     mv->source.input_file, mo->input_file),
                                                  &error, false);
            if (!uv)
                continue;
            if (v->displacement_uv_source != &mv->mesh() || v->displacement_uv_mesh_source != uv.get() ||
                !v->displacement_uv_model.is_initialized()) {
                build_uv_model(v->displacement_uv_model, mv->mesh().its, *uv);
                v->displacement_uv_source      = &mv->mesh();
                v->displacement_uv_mesh_source = uv.get();
            }
        }
        dp.texture = texture_for(map);
        if (dp.texture == 0)
            continue;
        used.push_back(map.get());
        const ConfigOption* size_opt    = opt("fuzzy_skin_texture_size");
        const ConfigOption* surface_opt = opt("fuzzy_skin_texture_surface");
        const ConfigOption* angle_opt   = opt("fuzzy_skin_texture_rotation");
        const ConfigOption* invert_opt  = opt("fuzzy_skin_texture_invert");
        const ConfigOption* offset_opt  = opt("fuzzy_skin_texture_offset");
        dp.active                       = true;
        dp.projection                   = int(projection);
        dp.walls_only                   = surface_opt != nullptr && !surface_opt->getBool();
        dp.size                         = size_opt ? float(size_opt->getFloat()) : 20.f;
        dp.aspect                       = float(map->rows) / float(map->cols);
        dp.depth                        = depth;
        dp.rotation                     = angle_opt ? float(angle_opt->getFloat()) : 0.f;
        dp.invert                       = invert_opt != nullptr && invert_opt->getBool();
        dp.midlevel                     = 1.f - std::clamp(offset_opt ? float(offset_opt->getFloat()) : 0.5f, 0.f, 1.f);
        const BoundingBoxf3& box        = boxes[{ v->object_idx(), v->instance_idx() }];
        dp.center                       = box.defined ? Vec2f(box.center().head<2>().cast<float>()) : Vec2f::Zero();
        // As the slicer maps it (Feature/FuzzySkin/TextureMapping.hpp), from the object's size.
        Feature::FuzzySkin::TextureMapping mapping;
        mapping.size                    = dp.size;
        mapping.radius                  = box.defined ? 0.5 * std::max(box.size().x(), box.size().y()) : 0.;
        dp.tiles_around                 = float(Feature::FuzzySkin::tiles_around(mapping));
        dp.center_z                     = box.defined ? float(0.5 * box.size().z()) : 0.f;
        dp.pattern                      = as_int("fuzzy_skin_texture_pattern");
        const auto noise_range          = Feature::FuzzySkin::SurfacePattern::noise_range();
        dp.noise_range                  = Vec2f(float(noise_range.first), float(noise_range.second));
        if (projection == FuzzySkinTextureProjection::OnSurface) {
            const Transform3d world = v->world_matrix();
            dp.offset               = world.translation().cast<float>();
            if (FuzzySkinTexturePattern(dp.pattern) != FuzzySkinTexturePattern::Noise) {
                const SurfaceSites* sites = surface_sites_for(&mv->mesh(), mv->mesh().its, world.linear(), dp.pattern, dp.size);
                if (sites == nullptr) {
                    dp.active = false;
                    continue;
                }
                dp.sites_texture   = sites->sites_texture;
                dp.buckets_texture = sites->buckets_texture;
                dp.bucket_mask     = sites->bucket_mask;
                dp.cell            = sites->cell;
                dp.origin          = sites->origin;
            }
            mix(dp.sites_texture);
            mixf(dp.offset.x());
            mixf(dp.offset.y());
            mixf(dp.offset.z());
        }
        mix(size_t(dp.pattern));

        mix(size_t(v));
        mix(dp.texture);
        mix(size_t(dp.projection) * 2 + size_t(dp.walls_only));
        mixf(dp.size);
        mixf(dp.rotation);
        mix(size_t(dp.invert));
        mixf(dp.midlevel);
        mixf(dp.tiles_around);
        mixf(dp.center_z);
        mixf(dp.depth);
        mixf(dp.center.x());
        mixf(dp.center.y());
        mix(size_t(v->displacement_uv_mesh_source));

        // The surface moved live by the tessellation shader. Not for UV (the patch model has no texture coordinates):
        // there the shading alone shows the map.
        if (can_tessellate && projection != FuzzySkinTextureProjection::UV) {
            const size_t key = size_t(mv->mesh_ptr().get()) ^ (mv->mesh().its.indices.size() << 1);
            if (v->displacement_mesh_key != key || !v->displacement_mesh_model.is_initialized()) {
                build_patch_model(v->displacement_mesh_model, mv->mesh().its);
                v->displacement_mesh_key = key;
            }
            dp.displaced = true;
        }
        mix(size_t(dp.displaced));
    }

    // Free the patch models of parts that no longer show a map.
    for (GLVolume* v : volumes.volumes)
        if (v != nullptr && !v->displacement_preview.displaced && v->displacement_mesh_model.is_initialized()) {
            v->displacement_mesh_model.reset();
            v->displacement_mesh_key = 0;
        }
    // Drop the sites of parts that no longer show a pattern laid on them (or were scaled or turned, which changes it).
    for (auto it = m_surface_sites.begin(); it != m_surface_sites.end();) {
        if (!it->used) {
            glsafe(::glDeleteTextures(1, &it->sites_texture));
            glsafe(::glDeleteTextures(1, &it->buckets_texture));
            it = m_surface_sites.erase(it);
        } else
            ++it;
    }

    // Drop textures no part shows any more (an edited image is a new map).
    for (auto it = m_textures.begin(); it != m_textures.end();) {
        if (std::find(used.begin(), used.end(), it->map.get()) == used.end()) {
            glsafe(::glDeleteTextures(1, &it->id));
            it = m_textures.erase(it);
        } else
            ++it;
    }

    const bool changed = hash != m_state_hash;
    m_state_hash       = hash;
    return changed;
}

const ConfigOption* DisplacementPreview::panel_option(const Model& model, const DynamicPrintConfig& process, const std::string& key) const
{
    if (m_drag && m_drag->key == key)
        return &m_drag->value;
    if (m_panel_object >= 0 && size_t(m_panel_object) < model.objects.size()) {
        const ModelObject* mo = model.objects[m_panel_object];
        for (const ModelVolume* mv : mo->volumes)
            if (mv->is_model_part() && mv->config.has(key))
                return mv->config.option(key);
        if (mo->config.has(key))
            return mo->config.option(key);
    }
    return process.option(key);
}

bool DisplacementPreview::writes_locally(const Model& model, int object, const std::string& /* key */) const
{
    // Settings changed for an object are its own (each object can have its own map); with no object, the process
    // preset's defaults change.
    return object >= 0 && size_t(object) < model.objects.size();
}

bool DisplacementPreview::has_own(const Model& model, int object, const std::string& key) const
{
    if (object < 0 || size_t(object) >= model.objects.size())
        return false;
    const ModelObject* mo = model.objects[object];
    if (mo->config.has(key))
        return true;
    for (const ModelVolume* mv : mo->volumes)
        if (mv->is_model_part() && mv->config.has(key))
            return true;
    return false;
}

void DisplacementPreview::commit(GLCanvas3D& canvas, int object, Values values)
{
    if (values.empty())
        return;
    Model& model         = wxGetApp().model();
    bool   object_change = false;
    Tab*   tab           = wxGetApp().get_tab(Preset::TYPE_PRINT);
    bool   tab_change    = false;
    for (auto& [key, value] : values) {
        if (writes_locally(model, object, key)) {
            // The object's value, and the parts' that have one of their own (they would hide it).
            ModelObject* mo = model.objects[object];
            for (ModelVolume* mv : mo->volumes)
                if (mv->is_model_part() && mv->config.has(key))
                    mv->config.set_key_value(key, value->clone());
            mo->config.set_key_value(key, value->clone());
            object_change = true;
        } else if (tab != nullptr) {
            tab->get_config()->set_key_value(key, value->clone());
            tab_change = true;
        }
    }
    if (object_change) {
        wxGetApp().obj_list()->changed_object(object);
        wxGetApp().obj_list()->update_and_show_object_settings_item();
    }
    if (tab_change) {
        DynamicPrintConfig* config = tab->get_config();
        tab->update_dirty();
        tab->reload_config();
        wxGetApp().mainframe->on_config_changed(config);
    }
    canvas.set_as_dirty();
    canvas.request_extra_frame();
}

void DisplacementPreview::render_panel(GLCanvas3D& canvas)
{
    const Model* model = canvas.get_model();
    if (model == nullptr)
        return;
    const DynamicPrintConfig& process = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    // The selected object; with nothing selected the only object, or else the process preset (all objects).
    int target = canvas.get_selection().get_object_idx();
    if (target < 0 && model->objects.size() == 1)
        target = 0;
    if (target >= int(model->objects.size()))
        target = -1;
    if (target != m_panel_object)
        m_drag.reset();
    m_panel_object = target;

    ImGuiWrapper& imgui   = *wxGetApp().imgui();
    const float   scale   = canvas.get_scale();
    const float   label_w = imgui.scaled(8.5f);
    // Wide enough for the longest choice, "On the surface (seamless)".
    const float   item_w  = std::max(imgui.scaled(11.f), ImGui::CalcTextSize(_u8L("On the surface (seamless)").c_str()).x +
                                                         ImGui::GetFrameHeight() + 2.f * ImGui::GetStyle().FramePadding.x);
    Values        pending;
    // Changes made after a file dialog: the dialog can't run inside the ImGui frame.
    auto later = [](std::function<void()> f) { wxGetApp().CallAfter(std::move(f)); };

    auto row = [&](const ConfigOptionDef& def) {
        ImGui::AlignTextToFramePadding();
        imgui.text(_(def.label));
        if (ImGui::IsItemHovered() && !def.tooltip.empty())
            imgui.tooltip(_(def.tooltip), imgui.scaled(22.f));
        ImGui::SameLine(label_w);
        ImGui::PushItemWidth(item_w);
    };
    auto combo = [&](const std::string& key) {
        const ConfigOptionDef* def = print_config_def.get(key);
        const ConfigOption*    o   = panel_option(*model, process, key);
        if (def == nullptr || o == nullptr || def->enum_keys_map == nullptr)
            return;
        std::vector<std::string> labels;
        std::vector<int>         ids;
        int                      sel = -1;
        for (size_t i = 0; i < def->enum_values.size(); ++i) {
            const auto it = def->enum_keys_map->find(def->enum_values[i]);
            if (it == def->enum_keys_map->end())
                continue;
            labels.push_back(into_u8(i < def->enum_labels.size() ? _(def->enum_labels[i]) : from_u8(def->enum_values[i])));
            ids.push_back(it->second);
            if (it->second == o->getInt())
                sel = int(ids.size()) - 1;
        }
        row(*def);
        if (ImGui::BeginCombo(("##" + key).c_str(), sel >= 0 ? labels[sel].c_str() : "")) {
            for (size_t i = 0; i < labels.size(); ++i)
                if (ImGui::Selectable(labels[i].c_str(), int(i) == sel) && int(i) != sel) {
                    std::unique_ptr<ConfigOption> value(o->clone());
                    value->setInt(ids[i]);
                    pending.emplace_back(key, std::move(value));
                }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
    };
    // top 0: twice the value, at least 1, kept while dragging.
    auto slider = [&](const std::string& key, float bottom, float top, const char* format) {
        const ConfigOptionDef* def = print_config_def.get(key);
        const ConfigOption*    o   = panel_option(*model, process, key);
        if (def == nullptr || o == nullptr)
            return;
        float value = float(o->getFloat());
        if (top <= 0.f) {
            if (!m_drag || m_drag->key != key)
                m_slider_top[key] = std::max(1.f, 2.f * value);
            top = m_slider_top[key];
        }
        row(*def);
        if (ImGui::SliderFloat(("##" + key).c_str(), &value, bottom, top, format)) {
            // Typed in (ctrl + click) it may leave the slider's range, not the setting's.
            value = std::clamp(value, float(def->min), float(def->max));
            if (!m_drag || m_drag->key != key)
                m_drag = Drag{ key, ConfigOptionFloat(value), writes_locally(*model, target, key), target };
            m_drag->value.value = value;
            canvas.set_as_dirty();
            canvas.request_extra_frame();
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_drag && m_drag->key == key) {
            pending.emplace_back(key, std::make_unique<ConfigOptionFloat>(m_drag->value.value));
            m_drag.reset();
        }
        ImGui::PopItemWidth();
    };
    auto checkbox = [&](const std::string& key) {
        const ConfigOptionDef* def = print_config_def.get(key);
        const ConfigOption*    o   = panel_option(*model, process, key);
        if (def == nullptr || o == nullptr)
            return;
        bool value = o->getBool();
        if (imgui.bbl_checkbox(_(def->label), value))
            pending.emplace_back(key, std::make_unique<ConfigOptionBool>(value));
        if (ImGui::IsItemHovered() && !def->tooltip.empty())
            imgui.tooltip(_(def->tooltip), imgui.scaled(22.f));
    };
    // A file setting: a button for the file, then its name.
    auto file = [&](const std::string& key, const std::function<void()>& on_button, const wxString& empty_text) {
        const ConfigOptionDef* def = print_config_def.get(key);
        const ConfigOption*    o   = panel_option(*model, process, key);
        if (def == nullptr || o == nullptr)
            return;
        row(*def);
        ImGui::PopItemWidth();
        if (ImGui::Button(("...##" + key).c_str()))
            on_button();
        const std::string path = string_option(o);
        ImGui::SameLine();
        if (path.empty())
            imgui.text(empty_text);
        else {
            std::string name = boost::filesystem::path(path).filename().string();
            if (name.size() > 28)
                name = name.substr(0, 25) + "...";
            imgui.text(from_u8(name));
            if (ImGui::IsItemHovered())
                imgui.tooltip(from_u8(path), imgui.scaled(30.f));
        }
    };
    const int object  = target;
    auto      set_file = [this, &canvas, object](const char* key, std::string path) {
        // Forward slashes: string options un-escape backslashes when a project is loaded again.
        std::replace(path.begin(), path.end(), '\\', '/');
        Values values;
        values.emplace_back(key, std::make_unique<ConfigOptionString>(path));
        commit(canvas, object, std::move(values));
    };
    auto browse_image = [set_file]() {
        wxFileDialog dlg(wxGetApp().mainframe, _L("Choose a displacement map image"), "", "",
                         _L("Images") + " (*.png;*.jpg;*.jpeg)|*.png;*.PNG;*.jpg;*.JPG;*.jpeg;*.JPEG", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK)
            set_file("fuzzy_skin_texture", into_u8(dlg.GetPath()));
    };
    // A map made from a description: the pattern becomes an image, the image the new map.
    auto image_from_text = [this, &canvas, object]() {
        TextToHeightDialog dlg(wxGetApp().mainframe);
        if (dlg.ShowModal() != wxID_OK || dlg.height_map_path().empty())
            return;
        std::string path = dlg.height_map_path();
        std::replace(path.begin(), path.end(), '\\', '/');
        Values values;
        values.emplace_back("fuzzy_skin_texture_pattern",
                            std::make_unique<ConfigOptionEnum<FuzzySkinTexturePattern>>(FuzzySkinTexturePattern::Image));
        values.emplace_back("fuzzy_skin_texture", std::make_unique<ConfigOptionString>(path));
        commit(canvas, object, std::move(values));
    };
    auto browse_uv_mesh = [set_file]() {
        wxFileDialog dlg(wxGetApp().mainframe, _L("Choose the textured model"), "", "",
                         _L("Models") + " (*.obj;*.glb;*.gltf)|*.obj;*.OBJ;*.glb;*.GLB;*.gltf;*.GLTF", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK)
            set_file("fuzzy_skin_texture_uv_mesh", into_u8(dlg.GetPath()));
    };

    // Top left, below the button that folds the sidebar (the navigator cube is at the bottom left).
    imgui.set_next_window_pos(10.f * scale, 50.f * scale, ImGuiCond_Always, 0.f, 0.f);
    imgui.begin(_L("Displacement map"), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);

    if (target >= 0) {
        imgui.text(from_u8(model->objects[target]->name));
        bool any_own = false;
        for (const char* key : { "fuzzy_skin", "fuzzy_skin_noise_type", "fuzzy_skin_texture", "fuzzy_skin_texture_pattern", "fuzzy_skin_thickness" })
            any_own = any_own || has_own(*model, target, key);
        imgui.text_colored(ImGuiWrapper::COL_GREY_LIGHT, any_own ? _L("This object's own settings") :
                                                                   _L("The defaults; a change applies to this object only"));
    } else {
        imgui.text(_L("Defaults for all objects"));
        imgui.text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Select an object to give it its own settings"));
    }

    bool show = m_show;
    if (imgui.bbl_checkbox(_L("Show on model"), show)) {
        m_show = show;
        canvas.set_as_dirty();
        canvas.request_extra_frame();
    }
    ImGui::Separator();

    const ConfigOption* type  = panel_option(*model, process, "fuzzy_skin");
    const ConfigOption* noise = panel_option(*model, process, "fuzzy_skin_noise_type");
    const bool on     = type != nullptr && type->getInt() != int(FuzzySkinType::None);
    const bool is_map = on && noise != nullptr && noise->getInt() == int(NoiseType::DisplacementMap);
    combo("fuzzy_skin");
    if (on)
        combo("fuzzy_skin_noise_type");
    if (!is_map) {
        if (imgui.button(_L("Use a displacement map"))) {
            if (!on)
                pending.emplace_back("fuzzy_skin", std::make_unique<ConfigOptionEnum<FuzzySkinType>>(FuzzySkinType::External));
            pending.emplace_back("fuzzy_skin_noise_type", std::make_unique<ConfigOptionEnum<NoiseType>>(NoiseType::DisplacementMap));
        }
    } else {
        combo("fuzzy_skin_texture_pattern");
        // Its own button, in sight whatever the pattern: it switches the pattern to the image it makes.
        if (TextToHeight::available()) {
            ImGui::Dummy(ImVec2(0.f, 0.f));
            ImGui::SameLine(label_w);
            if (ImGui::Button(into_u8(_L("Generate from text") + dots).c_str(), ImVec2(item_w, 0.f)))
                later(image_from_text);
            if (ImGui::IsItemHovered())
                imgui.tooltip(_L("Generate a seamless displacement map from a description, locally"), imgui.scaled(22.f));
        }
        const ConfigOption* pattern = panel_option(*model, process, "fuzzy_skin_texture_pattern");
        if (pattern != nullptr && pattern->getInt() == int(FuzzySkinTexturePattern::Image))
            file("fuzzy_skin_texture", [&]() { later(browse_image); }, _L("(none)"));
        combo("fuzzy_skin_texture_projection");
        const ConfigOption* proj_opt   = panel_option(*model, process, "fuzzy_skin_texture_projection");
        const auto          projection = FuzzySkinTextureProjection(proj_opt ? proj_opt->getInt() : 0);
        if (projection == FuzzySkinTextureProjection::UV)
        {
            // Empty: the texture coordinates of the file the part was loaded from, when it has them.
            wxString from_model = _L("(none: set one)");
            if (target >= 0)
                for (const ModelVolume* mv : model->objects[target]->volumes)
                    if (mv->is_model_part()) {
                        const std::string src = Feature::FuzzySkin::uv_mesh_source({}, mv->source.input_file, model->objects[target]->input_file);
                        if (!src.empty()) {
                            from_model = _L("from the model file") + " (" + from_u8(boost::filesystem::path(src).filename().string()) + ")";
                            break;
                        }
                    }
            file("fuzzy_skin_texture_uv_mesh", [&]() { later(browse_uv_mesh); }, from_model);
        }
        else
            slider("fuzzy_skin_texture_size", 1.f, 100.f, "%.1f mm");
        const bool cells = pattern != nullptr && !Feature::FuzzySkin::SurfacePattern::uses_charts(FuzzySkinTexturePattern(pattern->getInt()));
        if (projection != FuzzySkinTextureProjection::UV && !(projection == FuzzySkinTextureProjection::OnSurface && cells))
            slider("fuzzy_skin_texture_rotation", -180.f, 180.f, "%.0f\xC2\xB0");
        slider("fuzzy_skin_thickness", 0.f, 0.f, "%.2f mm");
        slider("fuzzy_skin_texture_offset", 0.f, 1.f, "%.2f");
        slider("fuzzy_skin_point_distance", 0.1f, 2.f, "%.2f mm");
        checkbox("fuzzy_skin_texture_invert");
        checkbox("fuzzy_skin_texture_surface");
    }
    const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    m_panel_rect = { pos.x, pos.y, pos.x + size.x, pos.y + size.y };
    imgui.end();

    commit(canvas, target, std::move(pending));
}

} // namespace Slic3r::GUI
