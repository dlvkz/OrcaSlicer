#version 110

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
//BBS: add grey and orange
//const vec3 GREY = vec3(0.9, 0.9, 0.9);
const vec3 ORANGE = vec3(0.8, 0.4, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
const float EPSILON = 0.0001;

struct PrintVolumeDetection
{
	// 0 = rectangle, 1 = circle, 2 = custom, 3 = invalid
	int type;
    // type = 0 (rectangle):
    // x = min.x, y = min.y, z = max.x, w = max.y
    // type = 1 (circle):
    // x = center.x, y = center.y, z = radius
	vec4 xy_data;
    // x = min z, y = max z
	vec2 z_data;
};

struct SlopeDetection
{
    bool actived;
	float normal_z;
    mat3 volume_world_normal_matrix;
};

uniform vec4 uniform_color;
uniform bool use_color_clip_plane;
uniform vec4 uniform_color_clip_plane_1;
uniform vec4 uniform_color_clip_plane_2;
uniform SlopeDetection slope;


#ifdef ENABLE_ENVIRONMENT_MAP
    uniform sampler2D environment_tex;
    uniform bool use_environment_tex;
#endif // ENABLE_ENVIRONMENT_MAP

uniform PrintVolumeDetection print_volume;

varying vec3 clipping_planes_dots;
varying float color_clip_plane_dot;

// x = diffuse, y = specular;
varying vec2 intensity;

varying vec4 world_pos;
varying float world_normal_z;
varying vec3 eye_normal;
varying vec3 eye_position;
varying vec3 world_normal;
varying vec2 tex_coord;

// Fuzzy skin displacement map preview: the height the map gives a point of the surface, as the slicer computes it
// (Feature/FuzzySkin), turned into a bumped normal with screen space derivatives and lit again per pixel.
uniform bool disp_active;
uniform sampler2D disp_tex;
uniform int disp_projection;   // 0 box, 1 cylindrical, 2 UV, 3 planar top, 4 planar front, 5 planar side, 6 spherical,
                              // 7 on the surface (not drawn by this shader: no integer arithmetic in GLSL 1.10)
uniform bool disp_walls_only;
uniform float disp_size;
uniform float disp_aspect;     // image rows / columns
uniform float disp_depth;
uniform vec2 disp_center;
uniform vec2 disp_rotation;    // cos and sin of the image's counter-clockwise turn
uniform bool disp_invert;
uniform float disp_tiles_around; // cylindrical, spherical: tiles once around the object
uniform float disp_center_z;   // spherical: height of the object's middle

#define DISP_PI 3.14159265358979
#define INTENSITY_CORRECTION 0.6
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)
#define INTENSITY_AMBIENT    0.3

// u left to right, v bottom to top; the image's first row is at the top.
float disp_sample(vec2 uv) { return texture2D(disp_tex, vec2(uv.x, 1.0 - uv.y)).r; }

// The image at (a, b) mm to the right and up in the plane it is projected from, turned by disp_rotation. a = 0 is the
// middle of a tile, b = 0 its bottom edge or, with centred_v, its middle.
float disp_at(float a, float b, bool centred_v)
{
    float ra = disp_rotation.x * a + disp_rotation.y * b;
    float rb = -disp_rotation.y * a + disp_rotation.x * b;
    return disp_sample(vec2(ra / disp_size + 0.5, rb / (disp_size * disp_aspect) + (centred_v ? 0.5 : 0.0)));
}

// Distance around the vertical axis, counter-clockwise seen from above, 0 at the front (-Y), a whole number of tiles
// once around.
float disp_around(vec3 p)
{
    float turn = (atan(p.y, p.x) + 0.5 * DISP_PI) / (2.0 * DISP_PI);
    return (turn - floor(turn + 0.5)) * disp_tiles_around * disp_size;
}

// p: object coordinates (XY centred on the object), uv: texture coordinates, n: unit normal of the face.
float disp_height(vec3 p, vec2 uv, vec3 n)
{
    float value;
    if (disp_projection == 7) {
        value = 0.5;
    } else if (disp_projection == 2) {
        value = disp_sample(uv);
    } else if (disp_projection == 1) {
        value = abs(n.z) > 0.70710678 ? disp_at(n.z >= 0.0 ? p.x : -p.x, p.y, true) : disp_at(disp_around(p), p.z, false);
    } else if (disp_projection == 6) {
        float equator_radius = disp_tiles_around * disp_size / (2.0 * DISP_PI);
        float latitude = atan(p.z - disp_center_z, length(p.xy));
        value = disp_at(disp_around(p), latitude * equator_radius, true);
    } else if (disp_projection == 3) {
        value = disp_at(p.x, p.y, true);
    } else if (disp_projection == 4) {
        value = disp_at(p.x, p.z, false);
    } else if (disp_projection == 5) {
        value = disp_at(p.y, p.z, false);
    } else {
        // Box: from the four sides, the top and the bottom, blended by the normal.
        vec3 w = n * n;
        w = w * w;
        w /= (w.x + w.y + w.z);
        float sx = disp_at(n.x >= 0.0 ? p.y : -p.y, p.z, false);
        float sy = disp_at(n.y >= 0.0 ? -p.x : p.x, p.z, false);
        float top = disp_at(n.z >= 0.0 ? p.x : -p.x, p.y, true);
        value = w.x * sx + w.y * sy + w.z * top;
    }
    if (disp_invert)
        value = 1.0 - value;
    if (disp_walls_only && abs(n.z) > 0.70710678)
        value = 0.5;
    return (value * 2.0 - 1.0) * disp_depth;
}

// Bump mapping without tangents (Mikkelsen 2010): the change of height over one pixel right (dhdx) and one pixel up
// (dhdy) tilts the normal.
vec3 disp_bumped_normal(vec3 N, float dhdx, float dhdy)
{
    vec3 dpdx = dFdx(eye_position);
    vec3 dpdy = dFdy(eye_position);
    vec3 r1 = cross(dpdy, N);
    vec3 r2 = cross(N, dpdx);
    float det = dot(dpdx, r1);
    vec3 grad = sign(det) * (dhdx * r1 + dhdy * r2);
    return normalize(abs(det) * N - grad);
}

// The vertex shader's lighting, per pixel.
vec2 disp_intensity(vec3 N)
{
    vec2 result;
    float NdotL = max(dot(N, LIGHT_TOP_DIR), 0.0);
    result.x = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;
    result.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(eye_position), reflect(-LIGHT_TOP_DIR, N)), 0.0), LIGHT_TOP_SHININESS);
    NdotL = max(dot(N, LIGHT_FRONT_DIR), 0.0);
    result.x += NdotL * LIGHT_FRONT_DIFFUSE;
    return result;
}

void main()
{
    vec2 light = intensity;
    if (disp_active) {
        vec3 N = normalize(eye_normal);
        if (!gl_FrontFacing)
            N = -N;
        vec3 p = vec3(world_pos.xy - disp_center, world_pos.z);
        vec3 n = normalize(world_normal);
        float h = disp_height(p, tex_coord, n);
        float hx = disp_height(p + dFdx(p), tex_coord + dFdx(tex_coord), n);
        float hy = disp_height(p + dFdy(p), tex_coord + dFdy(tex_coord), n);
        light = disp_intensity(disp_bumped_normal(N, hx - h, hy - h));
    }
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    vec4 color;
	if (use_color_clip_plane) {
		color.rgb = (color_clip_plane_dot < 0.0) ? uniform_color_clip_plane_1.rgb : uniform_color_clip_plane_2.rgb;
		color.a = uniform_color.a;
    }
    else
	    color = uniform_color;

    if (slope.actived) {
         if(world_pos.z<0.1&&world_pos.z>-0.1)
         {
                color.rgb = LightBlue;
                color.a = 0.8;
         }
         else if( world_normal_z < slope.normal_z - EPSILON)
         {
                color.rgb = color.rgb * 0.5 + LightRed * 0.5;
                color.a = 0.8;
         }
    }
    // if the fragment is outside the print volume -> use darker color
	vec3 pv_check_min = ZERO;
	vec3 pv_check_max = ZERO;
    if (print_volume.type == 0) {
		// rectangle
		pv_check_min = world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x);
		pv_check_max = world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y);
	}
	else if (print_volume.type == 1) {
		// circle
		float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
		pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x);
		pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y);
	}
	color.rgb = (any(lessThan(pv_check_min, ZERO)) || any(greaterThan(pv_check_max, ZERO))) ? mix(color.rgb, ZERO, 0.3333) : color.rgb;

#ifdef ENABLE_ENVIRONMENT_MAP
    if (use_environment_tex)
        gl_FragColor = vec4(0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + 0.8 * color.rgb * light.x, color.a);
    else
#endif
        gl_FragColor = vec4(vec3(light.y) + color.rgb * light.x, color.a);
}
