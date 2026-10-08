#version 450
#extension GL_EXT_nonuniform_qualifier : require
// GameCube GPU on Vulkan (Source/Gcn/gcn_vk.c): the TEV, alpha test and fog of one pixel, for
// every configuration (the state comes per primitive from a buffer). It follows gcn_raster.c's
// shade(): integer TEV math, so both draw the same pixels; textures are filtered by the GPU's
// samplers (the same wrap modes and texel centres, the hardware's own filter weights).
// Blending, the depth test and write masks are the pipeline's (fixed function).
//
// Written to stay in registers: the TEV's values are named variables picked by switches (the
// selectors are the same for every pixel of a primitive), not an indexed array.

struct State
{
    ivec4 hdr;         // stages, chanused | tcproj << 8, alpha test f0 | f1 << 3 | op << 6 | always << 8, r0 | r1 << 8
    ivec4 reg[4];      // PREV, REG0..2 rgba
    ivec4 fog[2];      // type, ortho, bmag, bshift | r, g, b, -
    vec4 fogf;         // a, c
    ivec4 smp[8];      // per map: slot, w, h, wrap s | wrap t << 2 | linear << 4
    ivec4 stage[64];   // 4 per stage, see gcn_vk.h
};

layout(std430, set = 0, binding = 0) readonly buffer States { State st[]; };
layout(set = 0, binding = 1) uniform texture2D textures[4096];
layout(set = 0, binding = 2) uniform sampler samplers[18]; // wrap s + wrap t * 3 + linear * 9

layout(location = 0) noperspective in float v_iw;
layout(location = 1) noperspective in vec4 v_col0;
layout(location = 2) noperspective in vec4 v_col1;
layout(location = 3) noperspective in vec3 v_tc[8];
layout(location = 11) flat in uint v_state;

layout(location = 0) out vec4 o_color;

// TEV values (gcn_raster.c SL_*): PREV, REG0-2, texture, rasterised color, 1, 0.5, konst, 0
ivec4 v_prev, v_r0, v_r1, v_r2, v_tex, v_ras, v_konst;

ivec4 pick(int i)
{
    switch (i)
    {
    case 0: return v_prev;
    case 1: return v_r0;
    case 2: return v_r1;
    case 3: return v_r2;
    case 4: return v_tex;
    case 5: return v_ras;
    case 6: return ivec4(255);
    case 7: return ivec4(128);
    case 8: return v_konst;
    default: return ivec4(0);
    }
}

void put_rgb(int i, ivec3 c)
{
    switch (i)
    {
    case 0: v_prev.rgb = c; break;
    case 1: v_r0.rgb = c; break;
    case 2: v_r1.rgb = c; break;
    default: v_r2.rgb = c; break;
    }
}

void put_a(int i, int a)
{
    switch (i)
    {
    case 0: v_prev.a = a; break;
    case 1: v_r0.a = a; break;
    case 2: v_r1.a = a; break;
    default: v_r2.a = a; break;
    }
}

ivec4 sample_map(uint s, int map, vec2 uv)
{
    ivec4 m = st[s].smp[map];
    if (m.x < 0) return ivec4(255);
    if (!(abs(uv.x) < 1e6)) uv.x = 0.0; // also NaN
    if (!(abs(uv.y) < 1e6)) uv.y = 0.0;
    // 0 clamp, 1 repeat, 2 mirror (3, not a mode, repeats as in gcn_raster.c's wrap())
    int ws = m.w & 3, wt = (m.w >> 2) & 3, lin = (m.w >> 4) & 1;
    if (ws == 3) ws = 1;
    if (wt == 3) wt = 1;
    int si = ws + wt * 3 + lin * 9;
    vec4 c = textureLod(sampler2D(textures[nonuniformEXT(m.x)], samplers[nonuniformEXT(si)]), uv, 0.0);
    return ivec4(c * 255.0 + 0.5);
}

ivec3 swz(ivec4 c, int sw)
{
    return ivec3(c[sw & 3], c[(sw >> 2) & 3], c[(sw >> 4) & 3]);
}

int tev_op(int a, int b, int c, int d, int op)
{
    int bias = op & 3, sub = (op >> 2) & 1, scale = (op >> 3) & 3, clampv = (op >> 5) & 1;
    c = c + (c >> 7);
    if (bias == 3) return d + ((sub == 0 ? (a > b) : (a == b)) ? c : 0);
    int v = (a * (256 - c) + b * c) >> 8;
    if (sub != 0) v = -v;
    v += d;
    if (bias == 1) v += 128;
    else if (bias == 2) v -= 128;
    if (scale == 1) v *= 2;
    else if (scale == 2) v *= 4;
    else if (scale == 3) v /= 2;
    if (clampv != 0) return clamp(v, 0, 255);
    return clamp(v, -1024, 1023);
}

ivec3 tev_op3(ivec3 a, ivec3 b, ivec3 c, ivec3 d, int op)
{
    return ivec3(tev_op(a.r, b.r, c.r, d.r, op), tev_op(a.g, b.g, c.g, d.g, op), tev_op(a.b, b.b, c.b, d.b, op));
}

bool cmp(int f, int a, int r)
{
    switch (f)
    {
    case 0: return false;
    case 1: return a < r;
    case 2: return a == r;
    case 3: return a <= r;
    case 4: return a > r;
    case 5: return a != r;
    case 6: return a >= r;
    default: return true;
    }
}

// a color input: rgb of the slot, or its alpha three times
ivec3 cin(int sel)
{
    ivec4 v = pick(sel & 15);
    return (sel & 16) != 0 ? ivec3(v.a) : v.rgb;
}

void main()
{
    uint s = v_state;
    ivec4 hdr = st[s].hdr;
    int nstages = hdr.x, used = hdr.y;
    float w = 1.0 / v_iw;
    ivec4 c0 = ivec4(0), c1 = ivec4(0);

    if ((used & 1) != 0) c0 = clamp(ivec4(v_col0 * w), 0, 255);
    if ((used & 2) != 0) c1 = clamp(ivec4(v_col1 * w), 0, 255);

    v_prev = st[s].reg[0];
    v_r0 = st[s].reg[1];
    v_r1 = st[s].reg[2];
    v_r2 = st[s].reg[3];
    v_tex = ivec4(255);
    v_ras = ivec4(0);
    v_konst = ivec4(0);

    for (int k = 0; k < nstages; k++)
    {
        // texon, texmap, texcoord, chan | swaps, konst rgb.. | konst a, inputs 0-2 | input 3, ops, dests
        ivec4 q0 = st[s].stage[k * 4], q1 = st[s].stage[k * 4 + 1], q2 = st[s].stage[k * 4 + 2], q3 = st[s].stage[k * 4 + 3];
        int swaps = q1.x;

        if (q0.x != 0)
        {
            int tci = q0.z;
            vec3 a = v_tc[tci];
            vec2 uv = a.xy * w;
            if ((used & (256 << tci)) != 0)
            {
                float qq = a.z * w;
                if (qq != 0.0) uv *= 1.0 / qq;
            }
            ivec4 t = sample_map(s, q0.y, uv);
            v_tex = ivec4(t[(swaps >> 8) & 3], t[(swaps >> 10) & 3], t[(swaps >> 12) & 3], t[(swaps >> 14) & 3]);
        }
        else
        {
            v_tex = ivec4(255);
        }
        if (q0.w == 0)
            v_ras = ivec4(c0[swaps & 3], c0[(swaps >> 2) & 3], c0[(swaps >> 4) & 3], c0[(swaps >> 6) & 3]);
        else if (q0.w == 1)
            v_ras = ivec4(c1[swaps & 3], c1[(swaps >> 2) & 3], c1[(swaps >> 4) & 3], c1[(swaps >> 6) & 3]);
        else
            v_ras = ivec4(0);
        v_konst = ivec4(q1.y, q1.z, q1.w, q2.x);

        // inputs: color slot | alpha << 4 | alpha slot << 8
        int i0 = q2.y, i1 = q2.z, i2 = q2.w, i3 = q3.x;
        ivec3 rc = tev_op3(cin(i0) & 255, cin(i1) & 255, cin(i2) & 255, cin(i3), q3.y);
        int ra = tev_op(pick((i0 >> 8) & 15).a & 255, pick((i1 >> 8) & 15).a & 255, pick((i2 >> 8) & 15).a & 255,
                        pick((i3 >> 8) & 15).a, q3.z);
        put_rgb(q3.w & 15, rc);
        put_a((q3.w >> 4) & 15, ra);
    }
    ivec4 o = clamp(v_prev, 0, 255);

    // alpha test
    int at = hdr.z, aref = hdr.w;
    if (((at >> 8) & 1) == 0)
    {
        bool t0 = cmp(at & 7, o.a, aref & 255), t1 = cmp((at >> 3) & 7, o.a, (aref >> 8) & 255);
        int op = (at >> 6) & 3;
        bool pass = op == 0 ? (t0 && t1) : op == 1 ? (t0 || t1) : op == 2 ? (t0 != t1) : (t0 == t1);
        if (!pass) discard;
    }

    // fog
    ivec4 f0 = st[s].fog[0];
    if (f0.x != 0)
    {
        ivec4 f1 = st[s].fog[1];
        uint zi = uint(clamp(gl_FragCoord.z * 16777216.0, 0.0, 16777215.0));
        float fa = st[s].fogf.x, fc = st[s].fogf.y, ze, f;
        if (f0.y == 0)
        {
            int denom = f0.z - int(zi >> uint(f0.w));
            ze = denom != 0 ? fa * 16777215.0 / float(denom) : 1e30;
        }
        else
        {
            ze = fa * (float(zi) / 16777215.0);
        }
        f = clamp(ze - fc, 0.0, 1.0);
        if (f0.x == 4) f = 1.0 - exp2(-8.0 * f);
        else if (f0.x == 5) f = 1.0 - exp2(-8.0 * f * f);
        else if (f0.x == 6) f = exp2(-8.0 * (1.0 - f));
        else if (f0.x == 7) f = exp2(-8.0 * (1.0 - f) * (1.0 - f));
        int fi = int(f * 256.0);
        o.rgb = (o.rgb * (256 - fi) + f1.xyz * fi) >> 8;
    }
    o_color = vec4(o) / 255.0;
}
