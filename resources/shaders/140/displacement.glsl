// Fuzzy skin displacement map preview, shared by gouraud.vs and gouraud.fs: the height the map gives a point of the
// surface, as the slicer computes it (Feature/FuzzySkin). gouraud_displace.tes moves the tessellated surface by it, the
// fragment shader turns it into a bumped normal and lights it per pixel.
uniform bool disp_active;
uniform sampler2D disp_tex;
uniform int disp_projection;   // 0 box, 1 cylindrical, 2 UV, 3 planar top, 4 planar front, 5 planar side, 6 spherical, 7 on the surface
uniform bool disp_walls_only;
uniform float disp_size;
uniform float disp_aspect;     // image rows / columns
uniform float disp_depth;
uniform vec2 disp_center;
uniform vec2 disp_rotation;    // cos and sin of the image's counter-clockwise turn
uniform bool disp_invert;
// The map value that leaves the surface in place: 1 - the offset setting (0 only in, 1 only out).
uniform float disp_midlevel;
uniform float disp_tiles_around; // cylindrical, spherical: tiles once around the object
uniform float disp_center_z;   // spherical: height of the object's middle
// On the surface (7): the pattern laid over the part (Feature/FuzzySkin/SurfacePattern), repeated here.
uniform int disp_pattern;      // FuzzySkinTexturePattern: 2 hexagons, 6 bumps, 7 stone, 8 noise; others laid by charts
uniform sampler2D disp_sites;  // per site: position, tag (charts: two more texels, see disp_charts); 1024 per row
uniform sampler2D disp_buckets; // per bucket: first site, count; 1024 per row
uniform int disp_bucket_mask;
uniform float disp_cell;
uniform vec3 disp_origin;
uniform vec3 disp_offset;      // the part's translation: sites are in world coordinates less it
uniform vec2 disp_noise_range;

#define DISP_PI 3.14159265358979

// u left to right, v bottom to top; the image's first row is at the top.
float disp_sample(vec2 uv) { return texture(disp_tex, vec2(uv.x, 1.0 - uv.y)).r; }

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

// The integer hash of the slicer's patterns, [0, 1).
float disp_hash01(uint a, uint b, uint c)
{
    uint h = a * 0x8da6b343u ^ b * 0xd8163841u ^ c * 0xcb1ab31fu;
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    h ^= h >> 16u;
    return float(h) / 4294967296.0;
}

uint disp_bucket(ivec3 c)
{
    uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u;
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    return h & uint(disp_bucket_mask);
}

float disp_lattice(ivec3 c, uint octave)
{
    return disp_hash01(uint(c.x), uint(c.y), uint(c.z) * 0x9e3779b1u + octave * 0x85ebca6bu);
}

// 3D value noise, six octaves, as SurfacePattern::noise_raw.
float disp_noise(vec3 p)
{
    float sum = 0.0;
    float amplitude = 1.0;
    for (int octave = 0; octave < 6; ++octave) {
        vec3 q = p * (float(4 << octave) / disp_size);
        ivec3 i = ivec3(floor(q));
        vec3 f = q - vec3(i);
        vec3 s = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
        uint o = uint(octave);
        float v = mix(mix(mix(disp_lattice(i, o), disp_lattice(i + ivec3(1, 0, 0), o), s.x),
                          mix(disp_lattice(i + ivec3(0, 1, 0), o), disp_lattice(i + ivec3(1, 1, 0), o), s.x), s.y),
                      mix(mix(disp_lattice(i + ivec3(0, 0, 1), o), disp_lattice(i + ivec3(1, 0, 1), o), s.x),
                          mix(disp_lattice(i + ivec3(0, 1, 1), o), disp_lattice(i + ivec3(1, 1, 1), o), s.x), s.y), s.z);
        sum += amplitude * v;
        amplitude *= 0.5;
    }
    return sum;
}

// An image or striped pattern laid on the surface by charts, as SurfacePattern::chart_value: the charts of the sites
// within reach, each projecting the image along its own surface direction, blended. Sites take three texels: (position,
// tag), (normal, offset x), (right, offset y). Sampled at the full resolution: the loop is not uniform control flow.
float disp_charts(vec3 p, vec3 n)
{
    float R = 0.7 * disp_cell;
    ivec3 c = ivec3(floor((p - disp_origin) / disp_cell));
    float sum = 0.0;
    float weight = 0.0;
    float nearest_d = 1e9;
    float nearest_h = 0.5;
    uint seen[27];
    int n_seen = 0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                uint b = disp_bucket(c + ivec3(dx, dy, dz));
                bool dup = false;
                for (int i = 0; i < n_seen; ++i)
                    dup = dup || seen[i] == b;
                if (dup)
                    continue;
                seen[n_seen++] = b;
                vec2 range = texelFetch(disp_buckets, ivec2(int(b % 1024u), int(b / 1024u)), 0).rg;
                int start = int(range.x);
                int count = min(int(range.y), 32);
                for (int k = 0; k < count; ++k) {
                    int j = 3 * (start + k);
                    vec4 a = texelFetch(disp_sites, ivec2(j % 1024, j / 1024), 0);
                    vec3 d = p - a.xyz;
                    float dist = length(d);
                    if (dist >= R)
                        continue;
                    vec4 nb = texelFetch(disp_sites, ivec2((j + 1) % 1024, (j + 1) / 1024), 0);
                    vec4 rb = texelFetch(disp_sites, ivec2((j + 2) % 1024, (j + 2) / 1024), 0);
                    float facing = dot(n, nb.xyz);
                    if (facing <= 0.0)
                        continue;
                    float q = 1.0 - (dist / R) * (dist / R);
                    float w = q * q * facing * facing;
                    vec3 up = cross(nb.xyz, rb.xyz);
                    float sa = dot(d, rb.xyz) + nb.w;
                    float sb = dot(d, up) + rb.w;
                    float ra = disp_rotation.x * sa + disp_rotation.y * sb;
                    float rr = -disp_rotation.y * sa + disp_rotation.x * sb;
                    vec2 uv = vec2(ra / disp_size + 0.5, rr / (disp_size * disp_aspect));
                    float h = textureLod(disp_tex, vec2(uv.x, 1.0 - uv.y), 0.0).r;
                    sum += w * h;
                    weight += w;
                    if (dist < nearest_d) {
                        nearest_d = dist;
                        nearest_h = h;
                    }
                }
            }
    return weight > 1e-12 ? sum / weight : nearest_h;
}

// The pattern laid on the surface at p (world coordinates less disp_offset), as SurfacePattern::value.
float disp_surface(vec3 p, vec3 n)
{
    if (!(disp_pattern == 2 || disp_pattern == 6 || disp_pattern == 7 || disp_pattern == 8))
        return disp_charts(p, n);
    if (disp_pattern == 8)
        return clamp((disp_noise(p) - disp_noise_range.x) / (disp_noise_range.y - disp_noise_range.x), 0.0, 1.0);
    ivec3 c = ivec3(floor((p - disp_origin) / disp_cell));
    float d1 = 1e9;
    float d2 = 1e9;
    int i1 = -1;
    int i2 = -1;
    vec3 s1 = vec3(0.0);
    vec3 s2 = vec3(0.0);
    float tag1 = 0.0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                uint b = disp_bucket(c + ivec3(dx, dy, dz));
                vec2 range = texelFetch(disp_buckets, ivec2(int(b % 1024u), int(b / 1024u)), 0).rg;
                int start = int(range.x);
                int count = min(int(range.y), 32);
                for (int k = 0; k < count; ++k) {
                    int idx = start + k;
                    if (idx == i1 || idx == i2)
                        continue;
                    vec4 site = texelFetch(disp_sites, ivec2(idx % 1024, idx / 1024), 0);
                    float d = distance(p, site.xyz);
                    if (d < d1) {
                        d2 = d1; i2 = i1; s2 = s1;
                        d1 = d; i1 = idx; s1 = site.xyz; tag1 = site.w;
                    } else if (d < d2) {
                        d2 = d; i2 = idx; s2 = site.xyz;
                    }
                }
            }
    if (i1 < 0)
        return 0.0;
    if (disp_pattern == 6) {
        float d = d1 / (0.38 * disp_size);
        return d < 1.0 ? sqrt(1.0 - d * d) : 0.0;
    }
    if (disp_pattern == 2) {
        if (i2 < 0)
            return 1.0;
        float gap = distance(s1, s2);
        float e = gap > 1e-9 ? (d2 * d2 - d1 * d1) / (2.0 * gap) : 0.0;
        return smoothstep(0.02, 0.09, e / disp_size);
    }
    if (disp_pattern == 7) {
        float t = i2 < 0 ? 1.0 : clamp(((d2 - d1) / disp_size - 0.015) / 0.12, 0.0, 1.0);
        return sqrt(1.0 - (1.0 - t) * (1.0 - t)) * (0.75 + 0.25 * tag1);
    }
    return 0.5;
}

// p: object coordinates (XY centred on the object), uv: texture coordinates, n: unit normal of the face.
float disp_height(vec3 p, vec2 uv, vec3 n)
{
    float value;
    if (disp_projection == 7) {
        value = disp_surface(vec3(p.xy + disp_center, p.z) - disp_offset, n);
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
        return 0.0;
    return (value - disp_midlevel) * 2.0 * disp_depth;
}
