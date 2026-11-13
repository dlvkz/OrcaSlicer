// The gouraud fragment shader, shared by gouraud.fs (GLSL 1.40) and gouraud_displace.fs (4.00, after tessellation).

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

in vec3 clipping_planes_dots;
in float color_clip_plane_dot;

// x = diffuse, y = specular;
in vec2 intensity;

in vec4 world_pos;
in float world_normal_z;
in vec3 eye_normal;
in vec3 eye_position;
in vec3 world_normal;
in vec2 tex_coord;
// The surface before the displacement moved it: where the height is looked up and the shading frame.
in vec3 disp_base_pos;
in vec3 disp_base_eye;

#include "displacement.glsl"
#define INTENSITY_CORRECTION 0.6
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)
#define INTENSITY_AMBIENT    0.3

// Bump mapping without tangents (Mikkelsen 2010): the change of height over one pixel right (dhdx) and one pixel up
// (dhdy) tilts the normal.
vec3 disp_bumped_normal(vec3 N, float dhdx, float dhdy)
{
    vec3 dpdx = dFdx(disp_base_eye);
    vec3 dpdy = dFdy(disp_base_eye);
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

out vec4 out_color;

void main()
{
    vec2 light = intensity;
    if (disp_active) {
        vec3 N = normalize(eye_normal);
        if (!gl_FrontFacing)
            N = -N;
        vec3 p = vec3(disp_base_pos.xy - disp_center, disp_base_pos.z);
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
        out_color = vec4(0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + 0.8 * color.rgb * light.x, color.a);
    else
#endif
        out_color = vec4(vec3(light.y) + color.rgb * light.x, color.a);
}
