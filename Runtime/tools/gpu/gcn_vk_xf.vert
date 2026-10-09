#version 450
// GameCube GPU on Vulkan (Source/Gcn/gcn_vk.c): the transform unit (XF) on the GPU. A port of
// gcn_raster.c's transform() and light_channel(): the game's vertices as the vertex loader decoded
// them, the matrices, lights and texture coordinate generation of their primitive (an XF state),
// out as gcn_vk.vert's are: attributes multiplied by 1/w for the fragment shader's per-pixel divide.
// The GPU clips (near / far as the GameCube does, unless the game turned clipping off) and culls.

struct XfVertex
{
    vec4 pos_mtx;    // position, position matrix (uint bits; ~0 = from MATINDEX_A)
    vec4 nrm_flags;  // normal, has normal | has color0 << 1 | has color1 << 2
    vec4 bin_tlo;    // binormal, texture matrices 0-3 (a byte each, 0xFF = from MATINDEX)
    vec4 tan_thi;    // tangent, texture matrices 4-7
    vec4 col0;       // 0..1
    vec4 col1;
    vec4 tex[4];     // two coordinate pairs each
    uvec4 ids;       // XF state, pixel state
};

struct Xf
{
    uvec4 misc;       // MATINDEX_A, MATINDEX_B, projection type, textures | channels << 4 | dual texture << 8
    uvec4 texgen[2];  // XF 0x1040-0x1047
    uvec4 post[2];    // XF 0x1050-0x1057
    vec4 proj[2];     // XF 0x1020-0x1025
    vec4 vp[2];       // sx, sy, sz, ox | oy, oz, 2 * scale / frame buffer width, 2 * scale / height
    vec4 chan[4];     // channel 0 material, ambient; channel 1 material, ambient (0..255)
    uvec4 chanf;      // channel 0 color, alpha, channel 1 color, alpha: flags | light mask << 8
    vec4 light[32];   // the 8 lights: color RGBA, (position, a0), (direction, a1), (k0, k1, k2, a2)
    float mtx[272];   // XF memory 0x000-0x10F: position and texture matrices
    float nrm[104];   // 0x400-0x467: normal matrices
    float pmx[272];   // 0x500-0x60F: post (dual texture) matrices
};

layout(std430, set = 0, binding = 3) readonly buffer XfVerts { XfVertex xv[]; };
layout(std430, set = 0, binding = 4) readonly buffer XfStates { Xf xs[]; };

layout(location = 0) noperspective out float v_iw;
layout(location = 1) noperspective out vec4 v_col0;
layout(location = 2) noperspective out vec4 v_col1;
layout(location = 3) noperspective out vec3 v_tc[8];
layout(location = 11) flat out uint v_state;

uint X;

vec4 row_m(int a) { return vec4(xs[X].mtx[a], xs[X].mtx[a + 1], xs[X].mtx[a + 2], xs[X].mtx[a + 3]); }
vec4 row_p(int a) { return vec4(xs[X].pmx[a], xs[X].pmx[a + 1], xs[X].pmx[a + 2], xs[X].pmx[a + 3]); }
vec3 mul34(int a, vec4 p) { return vec3(dot(row_m(a), p), dot(row_m(a + 4), p), dot(row_m(a + 8), p)); }
vec3 mul34p(int a, vec4 p) { return vec3(dot(row_p(a), p), dot(row_p(a + 4), p), dot(row_p(a + 8), p)); }

uint bits(uint v, int lo, int n) { return (v >> uint(lo)) & ((1u << uint(n)) - 1u); }

// a light's weight at a vertex: the diffuse term times the attenuation (gcn_raster.c light_weight)
float light_weight(uint f, int i, vec3 wpos, vec3 wnrm)
{
    int diffuse = int((f >> 4) & 3u);
    bool atten = (f & 64u) != 0u, spot = (f & 128u) != 0u;
    vec4 l1 = xs[X].light[i * 4 + 1], l2 = xs[X].light[i * 4 + 2], l3 = xs[X].light[i * 4 + 3];
    vec3 d = l1.xyz - wpos;
    float len = sqrt(dot(d, d)), att = 1.0;
    if (len > 0.0) d *= 1.0 / len;
    float ndl = dot(d, wnrm);
    if (atten)
    {
        float cs, aa, dd;
        if (spot)
        {
            // the angle to the spot's axis (its direction is stored negated), the distance
            cs = max(dot(d, l2.xyz), 0.0);
            aa = l1.w + l2.w * cs + l3.w * cs * cs;
            dd = l3.x + l3.y * len + l3.z * len * len;
        }
        else
        {
            // specular: the normal against the half-angle vector, both polynomials in it
            cs = ndl >= 0.0 ? max(dot(wnrm, l2.xyz), 0.0) : 0.0;
            aa = l1.w + l2.w * cs + l3.w * cs * cs;
            dd = l3.x + l3.y * cs + l3.z * cs * cs;
        }
        att = dd != 0.0 ? max(aa, 0.0) / dd : 0.0;
    }
    if (diffuse == 0) ndl = 1.0;
    else if (diffuse == 2 && ndl < 0.0) ndl = 0.0;
    return ndl * att;
}

vec4 light_channel(int ch, bool has, vec4 vcol, vec3 wpos, vec3 wnrm)
{
    uint fc = xs[X].chanf[ch * 2], fa = xs[X].chanf[ch * 2 + 1];
    vec4 m = xs[X].chan[ch * 2], amb = xs[X].chan[ch * 2 + 1];
    vec4 col255 = vcol * 255.0;

    if ((fc & 1u) != 0u) m.rgb = has ? col255.rgb : vec3(255.0);
    if ((fa & 1u) != 0u) m.a = has ? col255.a : 255.0;
    vec4 o = m;
    if ((fc & 8u) != 0u)
    {
        vec3 acc = ((fc & 2u) != 0u && has) ? col255.rgb : amb.rgb;
        for (int i = 0; i < 8; i++)
            if ((fc & (256u << uint(i))) != 0u) acc += xs[X].light[i * 4].rgb * light_weight(fc, i, wpos, wnrm);
        o.rgb = m.rgb * clamp(acc, 0.0, 255.0) * (1.0 / 255.0);
    }
    if ((fa & 8u) != 0u)
    {
        float acc = ((fa & 2u) != 0u && has) ? col255.a : amb.a;
        for (int i = 0; i < 8; i++)
            if ((fa & (256u << uint(i))) != 0u) acc += xs[X].light[i * 4].a * light_weight(fa, i, wpos, wnrm);
        o.a = m.a * clamp(acc, 0.0, 255.0) * (1.0 / 255.0);
    }
    return o;
}

void main()
{
    XfVertex vin = xv[gl_VertexIndex];
    X = vin.ids.x;
    uvec4 misc = xs[X].misc;
    uint mia = misc.x, mib = misc.y;
    uint pmi = floatBitsToUint(vin.pos_mtx.w);
    int pm = int(pmi != 0xFFFFFFFFu ? pmi : bits(mia, 0, 6));
    uint flags = floatBitsToUint(vin.nrm_flags.w);
    vec3 v = mul34(pm * 4, vec4(vin.pos_mtx.xyz, 1.0));
    vec3 n = vec3(0.0, 0.0, 1.0);
    vec4 o;
    int ntex = int(misc.w & 15u), nchan = int((misc.w >> 4) & 15u);

    if ((flags & 1u) != 0u)
    {
        int na = (pm & 31) * 3;
        vec3 r = vin.nrm_flags.xyz;
        n = vec3(xs[X].nrm[na] * r.x + xs[X].nrm[na + 1] * r.y + xs[X].nrm[na + 2] * r.z,
                 xs[X].nrm[na + 3] * r.x + xs[X].nrm[na + 4] * r.y + xs[X].nrm[na + 5] * r.z,
                 xs[X].nrm[na + 6] * r.x + xs[X].nrm[na + 7] * r.y + xs[X].nrm[na + 8] * r.z);
        float len = sqrt(dot(n, n));
        if (len > 0.0) n /= len;
    }
    vec4 p0 = xs[X].proj[0], p1 = xs[X].proj[1];
    if (misc.z == 0u)
        o = vec4(p0.x * v.x + p0.y * v.z, p0.z * v.y + p0.w * v.z, p1.x * v.z + p1.y, -v.z);
    else
        o = vec4(p0.x * v.x + p0.y, p0.z * v.y + p0.w, p1.x * v.z + p1.y, 1.0);

    vec4 c0 = light_channel(0, (flags & 2u) != 0u, vin.col0, v, n);
    vec4 c1 = nchan >= 2 ? light_channel(1, (flags & 4u) != 0u, vin.col1, v, n) : vec4(0.0);

    vec3 tc[8];
    for (int i = 0; i < 8; i++)
    {
        tc[i] = vec3(0.0, 0.0, 1.0);
        if (i >= ntex) continue;
        uint tg = xs[X].texgen[i >> 2][i & 3];
        int type = int(bits(tg, 4, 3)), src = int(bits(tg, 7, 5));
        bool stq = bits(tg, 1, 1) != 0u, abc1 = bits(tg, 2, 1) != 0u;
        uint tsel = i < 4 ? floatBitsToUint(vin.bin_tlo.w) : floatBitsToUint(vin.tan_thi.w);
        uint tmb = (tsel >> uint((i & 3) * 8)) & 255u;
        int tm = int(tmb != 255u ? tmb : (i < 4 ? bits(mia, 6 + i * 6, 6) : bits(mib, (i - 4) * 6, 6)));
        vec4 s = vec4(0.0, 0.0, 1.0, 1.0);
        vec3 r;

        if (type == 2 || type == 3) // color channels as coordinates
        {
            tc[i] = vec3(c0.r / 255.0, c0.g / 255.0, 1.0);
            continue;
        }
        if (src == 0) s.xyz = vin.pos_mtx.xyz;
        else if (src == 1) s.xyz = vin.nrm_flags.xyz;
        else if (src == 3) s.xyz = vin.tan_thi.xyz;
        else if (src == 4) s.xyz = vin.bin_tlo.xyz;
        else if (src >= 5 && src <= 12)
        {
            vec4 tp = vin.tex[(src - 5) >> 1];
            s.xy = ((src - 5) & 1) != 0 ? tp.zw : tp.xy;
            s.z = 1.0;
        }
        if (!abc1) s.z = 1.0;
        if (type == 1) // emboss: not modelled
        {
            tc[i] = vec3(s.xy, 1.0);
            continue;
        }
        if (stq)
        {
            r = mul34(tm * 4, s);
        }
        else
        {
            int a = tm * 4;
            r = vec3(dot(row_m(a), vec4(s.xyz, 1.0)), dot(row_m(a + 4), vec4(s.xyz, 1.0)), 1.0);
        }
        if ((misc.w & 256u) != 0u) // dual-texture (post) transform
        {
            uint pti = xs[X].post[i >> 2][i & 3];
            vec4 q = vec4(r, 1.0);
            if (bits(pti, 8, 1) != 0u)
            {
                float len = sqrt(dot(q.xyz, q.xyz));
                if (len > 0.0) q.xyz /= len;
            }
            r = mul34p(int(bits(pti, 0, 6)) * 4, q);
        }
        tc[i] = r;
    }

    // to the frame buffer (gcn_raster.c to_screen), as clip coordinates: x, y, depth times w
    vec4 vp0 = xs[X].vp[0], vp1 = xs[X].vp[1];
    float w = o.w, iw = 1.0 / w;
    float sx = (vp0.w + vp0.x * o.x * iw - 342.0) * vp1.z - 1.0;
    float sy = (vp1.x + vp0.y * o.y * iw - 342.0) * vp1.w - 1.0;
    float sz = (vp1.y + vp0.z * o.z * iw) / 16777216.0;
    gl_Position = vec4(sx * w, sy * w, sz * w, w);
    v_iw = iw;
    v_col0 = c0 * iw;
    v_col1 = c1 * iw;
    for (int i = 0; i < 8; i++) v_tc[i] = tc[i] * iw;
    v_state = vin.ids.y;
}
