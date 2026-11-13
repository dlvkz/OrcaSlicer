#version 400

// Fuzzy skin displacement map preview, live: the part's own mesh goes to the tessellation stages unchanged
// (gouraud_displace.tcs splits it, gouraud_displace.tes moves it). v_normal is the smooth vertex normal.

in vec3 v_position;
in vec3 v_normal;
in vec2 v_tex_coord;

out vec3 tc_position;
out vec3 tc_normal;
out vec2 tc_tex_coord;

void main()
{
    tc_position  = v_position;
    tc_normal    = v_normal;
    tc_tex_coord = v_tex_coord;
}
