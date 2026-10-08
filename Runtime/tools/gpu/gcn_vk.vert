#version 450
// GameCube GPU on Vulkan (Source/Gcn/gcn_vk.c): screen-space vertices from the CPU (gcn_raster.c),
// attributes already multiplied by 1/w. They are interpolated linearly in screen space
// (noperspective) and divided by the interpolated 1/w per pixel, as the software rasteriser does.

layout(location = 0) in vec4 a_pos;   // x, y (NDC), depth 0..1, 1/w
layout(location = 1) in vec4 a_col0;
layout(location = 2) in vec4 a_col1;
layout(location = 3) in vec3 a_tc[8];
layout(location = 11) in uint a_state;

layout(location = 0) noperspective out float v_iw;
layout(location = 1) noperspective out vec4 v_col0;
layout(location = 2) noperspective out vec4 v_col1;
layout(location = 3) noperspective out vec3 v_tc[8];
layout(location = 11) flat out uint v_state;

void main()
{
    gl_Position = vec4(a_pos.xy, a_pos.z, 1.0);
    v_iw = a_pos.w;
    v_col0 = a_col0;
    v_col1 = a_col1;
    for (int i = 0; i < 8; i++) v_tc[i] = a_tc[i];
    v_state = a_state;
}
