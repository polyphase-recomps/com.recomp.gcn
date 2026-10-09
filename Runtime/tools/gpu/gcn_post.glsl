// Shared by the display post-processing passes (gcn_post_*.comp, Source/Gcn/gcn_vk.c
// gcn_vk_present): one pixel per invocation, inputs as combined samplers (clamped), the output a
// storage image. The push constants:
//   rt     1 / width, 1 / height, width, height of the pass's input (SMAA_RT_METRICS)
//   size   input width, height, output width, height
//   param  x: RCAS sharpness in stops (0 = most)

layout(local_size_x = 8, local_size_y = 8) in;

layout(push_constant) uniform Post
{
    vec4 rt;
    uvec4 size;
    vec4 param;
} pc;

layout(set = 0, binding = 0) uniform sampler2D in0;
layout(set = 0, binding = 1) uniform sampler2D in1;
layout(set = 0, binding = 2) uniform sampler2D in2;
layout(set = 0, binding = 3, rgba8) uniform writeonly image2D outImg;
