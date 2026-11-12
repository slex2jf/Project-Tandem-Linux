#version 450
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(push_constant) uniform Push {
    mat4 projection;
    uvec4 tev[4];
} pc;
layout(location = 0) out vec4 out_color;
int sx11(uint x) {
    int v = int(x & 0x7ffu);
    return (v & 0x400) != 0 ? v - 2048 : v;
}

vec4 decode_reg(uint ra, uint bg) {
    return vec4(float(sx11(ra)), float(sx11(bg >> 12)),
                float(sx11(bg)), float(sx11(ra >> 12))) / 255.0;
}

vec4 decode_konst(uint ka, uint kb) {
    return vec4(float(ka & 0xffu), float((kb >> 12) & 0xffu),
                float(kb & 0xffu), float((ka >> 12) & 0xffu)) / 255.0;
}

vec3 konst_rgb(uint sel, vec4 k0, vec4 k1) {
    if (sel < 8u) return vec3(float(8u - sel) / 8.0);
    if (sel == 12u) return k0.rgb;
    if (sel == 13u) return k1.rgb;
    if (sel == 14u || sel == 15u) return vec3(0.0);
    uint k = (sel - 16u) & 3u;
    uint c = (sel - 16u) >> 2;
    vec4 v = k == 0u ? k0 : k == 1u ? k1 : vec4(0.0);
    return vec3(v[int(c)]);
}

float konst_a(uint sel, vec4 k0, vec4 k1) {
    if (sel < 8u) return float(8u - sel) / 8.0;
    if (sel < 16u) return 1.0;
    uint k = (sel - 16u) & 3u;
    uint c = (sel - 16u) >> 2;
    vec4 v = k == 0u ? k0 : k == 1u ? k1 : vec4(0.0);
    return v[int(c)];
}

vec3 color_arg(uint arg, vec4 prev, vec4 r0, vec4 r1, vec4 r2,
               vec4 tex, vec4 ras, vec4 kon) {
    if (arg == 0u) return prev.rgb;
    if (arg == 1u) return vec3(prev.a);
    if (arg == 2u) return r0.rgb;
    if (arg == 3u) return vec3(r0.a);
    if (arg == 4u) return r1.rgb;
    if (arg == 5u) return vec3(r1.a);
    if (arg == 6u) return r2.rgb;
    if (arg == 7u) return vec3(r2.a);
    if (arg == 8u) return tex.rgb;
    if (arg == 9u) return vec3(tex.a);
    if (arg == 10u) return ras.rgb;
    if (arg == 11u) return vec3(ras.a);
    if (arg == 12u) return vec3(1.0);
    if (arg == 13u) return vec3(0.5);
    if (arg == 14u) return kon.rgb;
    return vec3(0.0);
}

float alpha_arg(uint arg, vec4 prev, vec4 r0, vec4 r1, vec4 r2,
                vec4 tex, vec4 ras, vec4 kon) {
    if (arg == 0u) return prev.a;
    if (arg == 1u) return r0.a;
    if (arg == 2u) return r1.a;
    if (arg == 3u) return r2.a;
    if (arg == 4u) return tex.a;
    if (arg == 5u) return ras.a;
    if (arg == 6u) return kon.a;
    return 0.0;
}

ivec3 tev_color(ivec3 a, ivec3 b, ivec3 c, ivec3 d, uint env) {
    uint bias = (env >> 16) & 3u;
    uint op = (env >> 18) & 1u;
    bool clamp_result = ((env >> 19) & 1u) != 0u;
    uint scale = (env >> 20) & 3u;
    ivec3 r;
    if (bias == 3u) {
        uint mode = (scale << 1) | op;
        bool test = mode == 0u ? any(greaterThan(a,b)) : mode == 1u ? any(equal(a,b)) :
                    mode == 2u ? (a.r > b.r) : mode == 3u ? (a.r == b.r) :
                    mode == 4u ? ((a.b << 16 | a.g << 8 | a.r) > (b.b << 16 | b.g << 8 | b.r)) :
                    mode == 5u ? ((a.b << 16 | a.g << 8 | a.r) == (b.b << 16 | b.g << 8 | b.r)) :
                    mode == 6u ? any(greaterThan(a,b)) : all(equal(a,b));
        r = d + (test ? c : ivec3(0));
    } else {
        ivec3 ax = a;
        ivec3 bx = b;
        if (bias == 1u) d += ivec3(128);
        else if (bias == 2u) d -= ivec3(128);
        ivec3 cx = c + (c >> 7);
        r = d + (op != 0u ? -1 : 1) * ((ax * (ivec3(256) - cx) + bx * cx) >> 8);
        if (scale == 1u) r <<= 1;
        else if (scale == 2u) r <<= 2;
        else if (scale == 3u) r /= 2;
    }
    return clamp_result ? clamp(r, ivec3(0), ivec3(255)) : clamp(r, ivec3(-1024), ivec3(1023));
}

int tev_alpha(int a, int b, int c, int d, uint env) {
    uint bias = (env >> 16) & 3u;
    uint op = (env >> 18) & 1u;
    bool clamp_result = ((env >> 19) & 1u) != 0u;
    uint scale = (env >> 20) & 3u;
    int r;
    if (bias == 3u) {
        uint mode = (scale << 1) | op;
        bool test = mode == 0u ? a > b : mode == 1u ? a == b : mode == 2u ? a > b : mode == 3u ? a == b : mode == 4u ? a > b : mode == 5u ? a == b : mode == 6u ? a > b : a == b;
        r = d + (test ? c : 0);
    } else {
        if (bias == 1u) d += 128;
        else if (bias == 2u) d -= 128;
        int cx = c + (c >> 7);
        r = d + (op != 0u ? -1 : 1) * ((a * (256 - cx) + b * cx) >> 8);
        if (scale == 1u) r <<= 1;
        else if (scale == 2u) r <<= 2;
        else if (scale == 3u) r /= 2;
    }
    return clamp_result ? clamp(r, 0, 255) : clamp(r, -1024, 1023);
}

void main() {
    // GX register 0 is TEVPREV; register 1 is TEVREG0.  Keep that numbering
    // aligned with the existing GX shader generator instead of treating the
    // first saved register as C0.
    vec4 prev = decode_reg(pc.tev[2].x, pc.tev[2].y);
    vec4 r0 = decode_reg(pc.tev[2].z, pc.tev[2].w);
    vec4 r1 = vec4(0.0);
    vec4 r2 = vec4(0.0);
    vec4 k0 = decode_konst(pc.tev[3].x, pc.tev[3].y);
    vec4 k1 = decode_konst(pc.tev[3].z, pc.tev[3].w);
    vec4 tex = texture(u_tex, v_uv);
    vec4 ras = v_color;
    for (int stage = 0; stage < 2; ++stage) {
        uint cenv = stage == 0 ? pc.tev[0].x : pc.tev[1].x;
        uint aenv = stage == 0 ? pc.tev[0].y : pc.tev[1].y;
        uint order = stage == 0 ? pc.tev[0].z : pc.tev[1].z;
        uint ksel = stage == 0 ? pc.tev[0].w : pc.tev[1].w;
        if (stage == 1 && cenv == 0u && aenv == 0u) break;
        bool tex_enabled = ((order >> 6) & 1u) != 0u;
        vec4 t = tex_enabled ? tex : vec4(1.0);
        uint kc = stage == 0 ? (ksel >> 4) & 31u : (ksel >> 14) & 31u;
        uint ka = stage == 0 ? (ksel >> 9) & 31u : (ksel >> 19) & 31u;
        vec4 kon = vec4(konst_rgb(kc, k0, k1), konst_a(ka, k0, k1));
        uint ca = (cenv >> 12) & 15u, cb = (cenv >> 8) & 15u;
        uint cc = (cenv >> 4) & 15u, cd = cenv & 15u;
        uint aa = (aenv >> 13) & 7u, ab = (aenv >> 10) & 7u;
        uint ac = (aenv >> 7) & 7u, ad = (aenv >> 4) & 7u;
        ivec3 cr = tev_color(ivec3(round(color_arg(ca,prev,r0,r1,r2,t,ras,kon)*255.0)),
                             ivec3(round(color_arg(cb,prev,r0,r1,r2,t,ras,kon)*255.0)),
                             ivec3(round(color_arg(cc,prev,r0,r1,r2,t,ras,kon)*255.0)),
                             ivec3(round(color_arg(cd,prev,r0,r1,r2,t,ras,kon)*255.0)), cenv);
        int ar = tev_alpha(int(round(alpha_arg(aa,prev,r0,r1,r2,t,ras,kon)*255.0)),
                           int(round(alpha_arg(ab,prev,r0,r1,r2,t,ras,kon)*255.0)),
                           int(round(alpha_arg(ac,prev,r0,r1,r2,t,ras,kon)*255.0)),
                           int(round(alpha_arg(ad,prev,r0,r1,r2,t,ras,kon)*255.0)), aenv);
        vec4 result = vec4(vec3(cr) / 255.0, float(ar) / 255.0);
        uint dest = (cenv >> 22) & 3u;
        if (dest == 0u) prev.rgb = result.rgb;
        else if (dest == 1u) r0.rgb = result.rgb;
        else if (dest == 2u) r1.rgb = result.rgb;
        else r2.rgb = result.rgb;
        dest = (aenv >> 22) & 3u;
        if (dest == 0u) prev.a = result.a;
        else if (dest == 1u) r0.a = result.a;
        else if (dest == 2u) r1.a = result.a;
        else r2.a = result.a;
    }
    out_color = clamp(prev, 0.0, 1.0);
}
