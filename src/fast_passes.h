#pragma once
// fast_passes.h -- Passes (fast) on the neural GPU worker (Extrapolation / Feedback / Hybrid / Edit strength / Interleaved)
// Ported from mochizuki0323/DLSSNR-AMD (nr_fast_passes.hpp, st_*.comp, st_math.glsl, il0.comp, il_pp.comp, nr_style_grade.glsl)
// All shaders target DX12 cs_5_0 HLSL with float4/uint4 aligned root constants and NaN/zero-safe pow math.

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace FastPasses {

using Microsoft::WRL::ComPtr;

inline constexpr UINT kMaxIlSlots = 8;

enum Method : int {
    kInterleaved   = 0,
    kExtrapolation = 1,
    kFeedback      = 2,
    kHybrid        = 3,
    kEditStrength  = 4
};

struct Settings {
    bool     enabled      = false;
    int      method       = kInterleaved;
    float    passes       = 3.0f;   // 1..8 for Extrapolation/Feedback/Hybrid
    int      ilPasses     = 3;      // 2..8 for Interleaved
    bool     haloLimit    = false;
    float    fine         = 1.0f;   // 0..2
    float    editStrength = 2.0f;   // 0..4
    unsigned style        = 0;      // active NR style (0=Standard,1=Natural,2=Cinematic)
    float    intensity    = 1.0f;   // active NR intensity (0..2)
};

struct Strength {
    float k = 1.0f;
    float w = 0.0f;
    float n = 1.0f;
};

inline Strength ComputeStrength(float passes) {
    const float n = std::clamp(passes, 1.0f, 8.0f);
    const uint32_t seg = std::min((uint32_t)n, 3u) - 1u;
    const float kt[] = { 1.0f, 1.77f, 2.36f, 2.94f };
    const float wt[] = { 0.0f, 0.50f, 0.67f, 0.75f };
    const float part = n - float(seg + 1u);
    Strength s;
    s.k = (n > 4.0f) ? (2.94f + 0.58f * (n - 4.0f)) : (kt[seg] + part * (kt[seg + 1] - kt[seg]));
    s.w = (n > 4.0f) ? (std::min)(0.80f, 1.0f - 1.0f / n) : (wt[seg] + part * (wt[seg + 1] - wt[seg]));
    s.n = n;
    return s;
}

inline const char* MethodName(int m) {
    switch (m) {
    case kInterleaved:   return "Interleaved";
    case kExtrapolation: return "Extrapolation";
    case kFeedback:      return "Feedback";
    case kHybrid:        return "Hybrid";
    case kEditStrength:  return "EditStrength";
    default:             return "Unknown";
    }
}

inline bool NeedsMotion(int m) {
    return m == kInterleaved || m == kFeedback || m == kHybrid;
}

inline bool NeedsFeedback(int m) {
    return m == kFeedback || m == kHybrid;
}

inline int EffectiveMethod(const Settings& s, bool hasMv) {
    if (!s.enabled) return -1;
    if (NeedsMotion(s.method) && !hasMv) {
        // Interleaved falls back to single pass; Feedback/Hybrid fall back to Extrapolation
        return (s.method == kInterleaved) ? -1 : kExtrapolation;
    }
    return s.method;
}

// =========================================================================
// Embedded cs_5_0 HLSL Shaders (unified b0: uint4 gU[4]; float4 gF[4];)
// =========================================================================

static const char* kFastMathHlsl = R"HLSL(
cbuffer FastCB : register(b0) {
    uint4  gU[4];
    float4 gF[4];
};
SamplerState gSmp : register(s0);

float3 FastClean3(float3 c) {
    if ((asuint(c.r) & 0x7F800000u) == 0x7F800000u) c.r = 0.0;
    if ((asuint(c.g) & 0x7F800000u) == 0x7F800000u) c.g = 0.0;
    if ((asuint(c.b) & 0x7F800000u) == 0x7F800000u) c.b = 0.0;
    return c;
}
float3 st_dec(float3 c) {
    c = saturate(FastClean3(c));
    return lerp(c / 12.92, pow(max((c + 0.055) / 1.055, 1e-6), 2.4), step(0.04045, c));
}
float3 st_enc(float3 l) {
    l = saturate(FastClean3(l));
    float3 hi = 1.055 * pow(max(l, 1e-7), 1.0 / 2.4) - 0.055;
    return lerp(l * 12.92, hi, step(0.0031308, l));
}
float3 st_cbrt3(float3 v) {
    float3 a = abs(v);
    float3 p = pow(max(a, 1e-7), 1.0 / 3.0);
    p = lerp(float3(0.0, 0.0, 0.0), p, step(1e-7, a));
    return sign(v) * p;
}
float3 st_lab(float3 s) {
    float3 l = st_dec(max(FastClean3(s), 0.0));
    float3 lms = float3(
        dot(l, float3(0.4122214708, 0.5363325363, 0.0514459929)),
        dot(l, float3(0.2119034982, 0.6806995451, 0.1073969566)),
        dot(l, float3(0.0883024619, 0.2817188376, 0.6299787005))
    );
    float3 q = st_cbrt3(lms);
    return float3(
        dot(q, float3(0.2104542553,  0.7936177850, -0.0040720468)),
        dot(q, float3(1.9779984951, -2.4285922050,  0.4505937099)),
        dot(q, float3(0.0259040371,  0.7827717662, -0.8086757660))
    );
}
float3 st_lin(float3 lab) {
    float3 q = float3(
        lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
        lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
        lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z
    );
    float3 lms = q * q * q;
    return float3(
        dot(lms, float3( 4.0767416621, -3.3077115913,  0.2309699292)),
        dot(lms, float3(-1.2684380046,  2.6097574011, -0.3413193965)),
        dot(lms, float3(-0.0041960863, -0.7034186147,  1.7076147010))
    );
}
bool st_in(float3 l) {
    return all(l >= float3(-1e-6, -1e-6, -1e-6)) && all(l <= float3(1.0 + 1e-6, 1.0 + 1e-6, 1.0 + 1e-6));
}
float3 st_at(float3 X, float3 O, float2 h, float t) {
    float lx = clamp(X.x, 1e-4, 1.0 - 1e-4);
    float lo = clamp(O.x, 1e-4, 1.0 - 1e-4);
    float L = (lo < lx)
        ? (lx * pow(max(lo / lx, 1e-6), t))
        : (1.0 - (1.0 - lx) * pow(max((1.0 - lo) / (1.0 - lx), 1e-6), t));
    float c = max(length(X.yz) + t * (length(O.yz) - length(X.yz)), 0.0);
    return float3(L, c * h);
}
float3 st_gfq(Texture2D<float4> CA, Texture2D<float4> CB, float2 pos, float I, uint2 low, out float mI, out float vI) {
    float2 q = clamp(pos / 4.0 - 0.5, float2(0.0, 0.0), float2(low) - 1.0);
    int2 a = int2(floor(q));
    int2 b = min(a + 1, int2(low) - 1);
    float2 f = q - float2(a);
    float4 A = lerp(lerp(CA.Load(int3(a, 0)), CA.Load(int3(b.x, a.y, 0)), f.x),
                    lerp(CA.Load(int3(a.x, b.y, 0)), CA.Load(int3(b, 0)), f.x), f.y);
    float4 B = lerp(lerp(CB.Load(int3(a, 0)), CB.Load(int3(b.x, a.y, 0)), f.x),
                    lerp(CB.Load(int3(a.x, b.y, 0)), CB.Load(int3(b, 0)), f.x), f.y);
    mI = A.w; vI = B.w;
    return A.rgb * I + B.rgb;
}
float st_soft(float anchor, float extra) {
    float a = clamp(anchor, 1e-4, 1.0 - 1e-4);
    return (extra < 0.0) ? (a * exp(clamp(extra / a, -16.0, 16.0)))
                         : (1.0 - (1.0 - a) * exp(clamp(-extra / (1.0 - a), -16.0, 16.0)));
}
float3 st_relight(float3 lab, float Lb, float extra) {
    float3 l = st_lin(float3(st_soft(Lb, extra), lab.yz));
    if (st_in(l)) return st_enc(l);
    float lo = 0.0, hi = 1.0;
    for (int i = 0; i < 12; ++i) {
        float m = 0.5 * (lo + hi);
        if (st_in(st_lin(float3(st_soft(Lb, m * extra), lab.yz)))) lo = m;
        else hi = m;
    }
    return st_enc(st_lin(float3(st_soft(Lb, lo * extra), lab.yz)));
}
)HLSL";

static const char* kFastFbInHlsl = R"HLSL(
Texture2D<float4>   gBaseIn : register(t0);
Texture2D<float4>   gCA     : register(t1);
Texture2D<float4>   gCB     : register(t2);
Texture2D<float4>   gMotion : register(t3);
RWTexture2D<float4> gFastIn : register(u0);
RWTexture2D<float4> gFbAdd  : register(u1);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint2 sz = gU[0].xy;
    if (id.x >= sz.x || id.y >= sz.y) return;
    float4 p = gBaseIn.Load(int3(int2(id.xy), 0));
    p.rgb = saturate(FastClean3(p.rgb));
    uint mode = gU[2].z;
    if (mode == 0u) {
        gFastIn[id.xy] = float4(p.rgb, 1.0);
        gFbAdd[id.xy]  = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float3 np = p.rgb;
    float conf = 0.0;
    uint2 low   = gU[0].zw;
    uint2 mbase = gU[1].xy;
    uint2 mgrid = max(gU[1].zw, uint2(1u, 1u));
    uint  flags = gU[2].x;
    float2 mscale = gF[0].xy;
    float  w      = gF[0].z;
    float  darkc  = gF[0].w;
    if ((flags & 1u) != 0u) {
        float2 prev = float2(id.xy) + 0.5;
        if ((flags & 2u) != 0u) {
            uint2 t = mbase + min(uint2((float2(id.xy) + 0.5) * float2(mgrid) / float2(sz)), mgrid - 1u);
            float2 mv = gMotion.Load(int3(int2(t), 0)).xy;
            if ((asuint(mv.x) & 0x7F800000u) == 0x7F800000u) mv.x = 0.0;
            if ((asuint(mv.y) & 0x7F800000u) == 0x7F800000u) mv.y = 0.0;
            prev += mv * mscale;
        }
        if ((flags & 16u) != 0u && all(prev >= 0.0) && all(prev <= float2(sz))) {
            float I = st_lab(p.rgb).x;
            float mI = 0.0, vI = 0.0;
            float3 q = st_gfq(gCA, gCB, prev, I, low, mI, vI);
            float z = (I - mI) * (I - mI) / (vI + 0.0004);
            conf = exp(-0.125 * z);
            float3 add = w * conf * q;
            if (darkc > 0.0) {
                float3 X = st_lab(p.rgb);
                float3 Y = st_lab(saturate(p.rgb + add));
                add = st_enc(st_lin(float3(Y.x, X.yz + smoothstep(0.0, darkc, X.x) * (Y.yz - X.yz)))) - p.rgb;
            }
            float s = 1.0;
            if (add.r > 1e-7) s = min(s, max(1.0 - p.r, 0.0) / add.r); else if (add.r < -1e-7) s = min(s, max(p.r, 0.0) / -add.r);
            if (add.g > 1e-7) s = min(s, max(1.0 - p.g, 0.0) / add.g); else if (add.g < -1e-7) s = min(s, max(p.g, 0.0) / -add.g);
            if (add.b > 1e-7) s = min(s, max(1.0 - p.b, 0.0) / add.b); else if (add.b < -1e-7) s = min(s, max(p.b, 0.0) / -add.b);
            np = saturate(p.rgb + s * add);
        }
    }
    gFastIn[id.xy] = float4(np, 1.0);
    gFbAdd[id.xy]  = float4(np - p.rgb, conf);
}
)HLSL";

static const char* kFastPrepHlsl = R"HLSL(
Texture2D<float4>   gP     : register(t0);
Texture2D<float4>   gN     : register(t1);
RWTexture2D<float2> gFineH : register(u0);
RWTexture2D<float4> gOutA  : register(u1);
RWTexture2D<float4> gOutB  : register(u2);

groupshared float  s_dl[20][20], s_ln[20][20], s_lp[20][20];
groupshared float3 s_v[16][16];

[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 lid3 : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    uint2 sz  = gU[0].xy;
    uint2 low = gU[0].zw;
    uint srcMode = gU[1].x;
    int2 org = int2(gid.xy) * 16;
    int2 lid = int2(lid3.xy);
    for (uint i = gi; i < 400u; i += 256u) {
        int2 t = clamp(org - 2 + int2(int(i % 20u), int(i / 20u)), int2(0, 0), int2(sz) - 1);
        float ln = st_lab(gN.Load(int3(t, 0)).rgb).x;
        float lp = st_lab(gP.Load(int3(t, 0)).rgb).x;
        s_dl[i / 20u][i % 20u] = ln - lp;
        s_ln[i / 20u][i % 20u] = ln;
        s_lp[i / 20u][i % 20u] = lp;
    }
    int2 px = org + lid;
    bool inside = (px.x < int(sz.x) && px.y < int(sz.y));
    if (srcMode != 0u && inside) {
        float3 n3 = saturate(FastClean3(gN.Load(int3(px, 0)).rgb));
        float3 p3 = saturate(FastClean3(gP.Load(int3(px, 0)).rgb));
        s_v[lid.y][lid.x] = n3 - p3;
    }
    GroupMemoryBarrierWithGroupSync();
    if (inside) {
        const float w[5] = { 1.0, 4.0, 6.0, 4.0, 1.0 };
        float s = 0.0;
        [unroll] for (int y = -2; y <= 2; ++y)
            [unroll] for (int x = -2; x <= 2; ++x)
                s += w[x + 2] * w[y + 2] * s_dl[lid.y + 2 + y][lid.x + 2 + x];
        gFineH[uint2(px)] = float2(s_dl[lid.y + 2][lid.x + 2] - s / 256.0, s_ln[lid.y + 2][lid.x + 2]);
    }
    if (lid.x < 4 && lid.y < 4) {
        uint2 q = gid.xy * 4u + uint2(lid);
        if (q.x < low.x && q.y < low.y) {
            float4 A = float4(0, 0, 0, 0), B = float4(0, 0, 0, 0);
            float c = 0.0;
            [unroll] for (uint y = 0u; y < 4u; ++y) {
                [unroll] for (uint x = 0u; x < 4u; ++x) {
                    uint2 p = q * 4u + uint2(x, y);
                    if (p.x >= sz.x || p.y >= sz.y) continue;
                    uint2 l = p - uint2(org);
                    float I = s_lp[l.y + 2u][l.x + 2u];
                    float3 v = (srcMode == 0u) ? float3(s_dl[l.y + 2u][l.x + 2u], 0.0, 0.0) : s_v[l.y][l.x];
                    A += float4(I, I * I, v.r, v.g);
                    B += float4(v.b, I * v);
                    c += 1.0;
                }
            }
            float inv = 1.0 / max(c, 1.0);
            gOutA[q] = A * inv;
            gOutB[q] = B * inv;
        }
    }
}
)HLSL";

static const char* kFastBoxHlsl = R"HLSL(
cbuffer FastCB : register(b0) {
    uint4  gU[4];
    float4 gF[4];
};
Texture2D<float4>   gInA  : register(t0);
Texture2D<float4>   gInB  : register(t1);
RWTexture2D<float4> gOutA : register(u0);
RWTexture2D<float4> gOutB : register(u1);

groupshared float4 s_a[24][16], s_b[24][16];

[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 lid3 : SV_GroupThreadID) {
    uint2 low   = gU[0].xy;
    int   r     = int(gU[0].z);
    uint  coef  = gU[0].w;
    float eps   = gF[0].x;
    int2 org = int2(gid.xy) * 16;
    int2 lid = int2(lid3.xy);
    int col = org.x + lid.x;
    for (int k = lid.y; k < 16 + 2 * r; k += 16) {
        int row = org.y - r + k;
        if (row < 0 || row >= int(low.y) || col >= int(low.x)) continue;
        float4 A = float4(0, 0, 0, 0), B = float4(0, 0, 0, 0);
        float c = 0.0;
        for (int i = -r; i <= r; ++i) {
            int cx = col + i;
            if (cx < 0 || cx >= int(low.x)) continue;
            A += gInA.Load(int3(cx, row, 0));
            B += gInB.Load(int3(cx, row, 0));
            c += 1.0;
        }
        float inv = 1.0 / max(c, 1.0);
        s_a[k][lid.x] = A * inv;
        s_b[k][lid.x] = B * inv;
    }
    GroupMemoryBarrierWithGroupSync();
    int2 q = org + lid;
    if (q.x >= int(low.x) || q.y >= int(low.y)) return;
    float4 sa = float4(0, 0, 0, 0), sb = float4(0, 0, 0, 0);
    float c = 0.0;
    for (int i = -r; i <= r; ++i) {
        int row = q.y + i;
        if (row < 0 || row >= int(low.y)) continue;
        sa += s_a[lid.y + r + i][lid.x];
        sb += s_b[lid.y + r + i][lid.x];
        c += 1.0;
    }
    float inv2 = 1.0 / max(c, 1.0);
    float4 A = sa * inv2;
    float4 B = sb * inv2;
    if (coef != 0u) {
        float mI = A.x;
        float vI = max(A.y - mI * mI, 0.0);
        float3 mp = float3(A.z, A.w, B.x);
        float3 mIp = B.yzw;
        float3 a = (mIp - mI * mp) / (vI + eps);
        gOutA[uint2(q)] = float4(a, mI);
        gOutB[uint2(q)] = float4(mp - a * mI, vI);
    } else {
        gOutA[uint2(q)] = A;
        gOutB[uint2(q)] = B;
    }
}
)HLSL";

static const char* kFastApplyHlsl = R"HLSL(
Texture2D<float4>   gP      : register(t0);
Texture2D<float4>   gFbAdd  : register(t1);
Texture2D<float4>   gCA     : register(t2);
Texture2D<float4>   gCB     : register(t3);
Texture2D<float4>   gN      : register(t4);
Texture2D<float2>   gFineH  : register(t5);
RWTexture2D<float4> gOut    : register(u0);
RWTexture2D<float4> gOutA   : register(u1);
RWTexture2D<float4> gOutB   : register(u2);

struct Ans { float3 O; float co; float2 uo; };
Ans make_ans(float3 O) { Ans a; a.O = O; a.co = length(O.yz); a.uo = (a.co > 0.0) ? (O.yz / a.co) : float2(0.0, 0.0); return a; }

float3 at7(Ans a, float amp, float2 dab, float f, float t, uint mode, float satkeep, float ccap, float rho) {
    float3 O = a.O;
    float L = st_soft(O.x, (t - 1.0) * amp);
    float g = (mode >= 7u) ? pow(max(L / max(O.x, 1e-4), 1e-6), satkeep) : 1.0;
    if (mode >= 8u) g = min(g, 1.0);
    float tc = t;
    if (ccap > 1.0) {
        float e = (t - 1.0) / (ccap - 1.0);
        tc = 1.0 + (t - 1.0) / pow(1.0 + e * e * e * e, 0.25);
    }
    float2 ab = (O.yz + (tc - 1.0) * f * dab) * g;
    float c = length(ab);
    if (rho > 0.0 && a.co > 0.005 && c > 1e-6) {
        float2 u = ab / c;
        float d = atan2(u.x * a.uo.y - u.y * a.uo.x, dot(u, a.uo));
        float r = rho * d;
        float sr = sin(r), cr = cos(r);
        ab = c * float2(u.x * cr - u.y * sr, u.x * sr + u.y * cr);
    }
    return float3(L, ab);
}

static const uint kQueue = 128u, kInline = 0xFFFFFFFFu, kDone = 0xFFFFFFFEu;
groupshared float4 s_qa[128], s_qb[128];
groupshared float2 s_qc[128];
groupshared uint   s_qn;
groupshared float4 s_iv[16][16];

struct Ov { float2 dab; float f, amp; float3 l; uint slot; };

Ov ov_begin(float3 X, Ans A, float amp_in) {
    float k = gF[0].x, chroma = gF[0].z, satkeep = gF[0].w;
    float rho = gF[1].z, cfloor = gF[2].x, ccap = gF[2].z;
    uint mode = gU[1].x;
    Ov v;
    float3 O = A.O;
    v.dab = O.yz - X.yz;
    float rel = length(v.dab) / (length(X.yz) + 0.01) / max(chroma, 1e-4);
    float fl = cfloor * smoothstep(0.4, 0.65, O.x);
    v.f = fl + (1.0 - fl) / (1.0 + rel * rel);
    v.amp = (amp_in > -1e8) ? amp_in : (O.x - X.x);
    v.l = st_lin(at7(A, v.amp, v.dab, v.f, k, mode, satkeep, ccap, rho));
    v.slot = kDone;
    if (!st_in(v.l)) {
        uint idx = 0;
        InterlockedAdd(s_qn, 1u, idx);
        if (idx < kQueue) {
            v.slot = idx;
            s_qa[idx] = float4(O, v.amp);
            s_qb[idx] = float4(A.uo, v.dab);
            s_qc[idx] = float2(v.f, A.co);
        } else {
            v.slot = kInline;
        }
    }
    return v;
}

float3 ov_search(Ans A, float amp, float2 dab, float f) {
    float k = gF[0].x, satkeep = gF[0].w, rho = gF[1].z, ccap = gF[2].z;
    uint mode = gU[1].x;
    float lo = 1.0, hi = k;
    for (int i = 0; i < 12; ++i) {
        float m = 0.5 * (lo + hi);
        if (st_in(st_lin(at7(A, amp, dab, f, m, mode, satkeep, ccap, rho)))) lo = m;
        else hi = m;
    }
    return st_lin(at7(A, amp, dab, f, lo, mode, satkeep, ccap, rho));
}

float3 ov_end(Ov v, Ans A, float3 o) {
    float3 l = v.l;
    if (v.slot == kInline) l = ov_search(A, v.amp, v.dab, v.f);
    else if (v.slot != kDone) l = s_qa[v.slot].xyz;
    return st_in(l) ? st_enc(l) : o;
}

float3 st_dadd(float3 n, float3 Ln, float3 add) {
    float dcol = gF[1].w, darkcol = gF[3].y;
    if (darkcol > 0.0) {
        float3 Y0 = st_lab(saturate(n + add));
        float cs = ((dcol >= 1.0) ? 1.0 : max(dcol, 0.0)) * smoothstep(0.0, darkcol, Ln.x);
        return st_enc(st_lin(float3(Y0.x, Ln.yz + cs * (Y0.yz - Ln.yz)))) - n;
    }
    if (dcol >= 1.0) return add;
    float3 Y = st_lab(saturate(n + add));
    if (dcol < 0.0) {
        float2 ab = Ln.yz * (Y.x / max(Ln.x, 1e-3)) - dcol * (Y.yz - Ln.yz);
        return st_enc(st_lin(float3(Y.x, ab))) - n;
    }
    return st_enc(st_lin(float3(Y.x, Ln.yz + dcol * (Y.yz - Ln.yz)))) - n;
}

float st_fit(float3 base, float3 add) {
    float s = 1.0;
    if (add.r > 1e-7) s = min(s, max(1.0 - base.r, 0.0) / add.r); else if (add.r < -1e-7) s = min(s, max(base.r, 0.0) / -add.r);
    if (add.g > 1e-7) s = min(s, max(1.0 - base.g, 0.0) / add.g); else if (add.g < -1e-7) s = min(s, max(base.g, 0.0) / -add.g);
    if (add.b > 1e-7) s = min(s, max(1.0 - base.b, 0.0) / add.b); else if (add.b < -1e-7) s = min(s, max(base.b, 0.0) / -add.b);
    return s;
}

float3 st_nohalo(float3 o, float3 alt) {
    float nohalo = gF[2].w;
    if (nohalo > 1.5) return (st_lab(o).x > st_lab(alt).x) ? alt : o;
    return lerp(o, alt, saturate(nohalo));
}

float3 st_fb_ring(float3 o, float3 Ln, int2 px, uint2 sz) {
    float r0 = 1e9;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            r0 = min(r0, gFineH.Load(int3(clamp(px + int2(x, y), int2(0, 0), int2(sz) - 1), 0)).y);
    float3 O = st_lab(o);
    float floor_l = min(Ln.x, r0);
    if (O.x >= floor_l) return o;
    float3 l = st_lin(float3(floor_l, O.yz));
    return st_in(l) ? st_enc(l) : o;
}

float3 ocap_tail(uint2 px, float3 p, float3 o) {
    uint mode = gU[1].x;
    float ocap = gF[3].x;
    if (ocap != 0.0 && (mode == 6u || mode == 8u)) {
        float3 own = p - gFbAdd.Load(int3(int2(px), 0)).rgb;
        float3 Xo = st_lab(own), Y = st_lab(o);
        float2 dab = Y.yz - Xo.yz;
        float c0 = length(Xo.yz);
        float2 u0 = (c0 > 1e-4) ? (Xo.yz / c0) : float2(0.0, 0.0);
        float2 desat = (ocap < 0.0) ? (min(dot(dab, u0), 0.0) * u0) : float2(0.0, 0.0);
        float2 rest = dab - desat;
        float lim = abs(ocap) * (c0 + 0.01), m = length(rest);
        if (m > 1e-6) {
            float e = m / max(lim, 1e-5);
            float mo = m / pow(1.0 + e * e * e * e, 0.25);
            float3 l = st_lin(float3(Y.x, Xo.yz + desat + rest * (mo / m)));
            if (st_in(l)) o = st_enc(l);
        }
    }
    return o;
}

float3 st_edit(float3 nE, float3 pE, float k) {
    float3 p = st_dec(saturate(pE)), n = st_dec(saturate(nE));
    float3 e = log2(max(n, 1.0 / 1024.0)) - log2(max(p, 1.0 / 1024.0));
    float pm = max(p.r, max(p.g, p.b));
    float3 t = smoothstep(1.0 / 1024.0, 1.0 / 64.0, pm) * smoothstep(float3(pm / 256.0, pm / 256.0, pm / 256.0), float3(pm / 64.0, pm / 64.0, pm / 64.0), p);
    return st_enc(n * exp2(clamp(t * (k - 1.0) * e, -12.0, 12.0)));
}

float3 run_strengthened(uint2 px, float3 n3, float3 p3) {
    uint2 sz = gU[0].xy, low = gU[0].zw;
    uint mode = gU[1].x;
    float k = gF[0].x, halo = gF[0].y, chroma = gF[0].z, satkeep = gF[0].w;
    float fine = gF[1].x, base = gF[1].y, rho = gF[1].z;
    float cfloor = gF[2].x, ring = gF[2].y, ccap = gF[2].z, nohalo = gF[2].w;
    float3 X = st_lab(p3);
    Ans A = make_ans(st_lab(n3));
    float mI = 0.0, vI = 0.0;
    float3 o = n3;
    if (mode == 9u) {
        float3 O = A.O;
        float dL = O.x - X.x;
        float B = st_gfq(gCA, gCB, float2(px) + 0.5, X.x, low, mI, vI).r;
        float F = gFineH.Load(int3(int2(px), 0)).x;
        float amp = base * B + fine * F + ((nohalo > 0.5) ? 0.0 : halo) * (dL - B - F);
        float2 dab = O.yz - X.yz;
        float rel = length(dab) / (length(X.yz) + 0.01) / max(chroma, 1e-4);
        float fl = cfloor * smoothstep(0.4, 0.65, O.x);
        float f = fl + (1.0 - fl) / (1.0 + rel * rel);
        float tt = k;
        float3 lab = at7(A, amp, dab, f, k, mode, satkeep, ccap, rho);
        float3 l = st_lin(lab);
        if (!st_in(l)) {
            float lo = 1.0, hi = k;
            for (int i = 0; i < 12; ++i) {
                float m = 0.5 * (lo + hi);
                if (st_in(st_lin(at7(A, amp, dab, f, m, mode, satkeep, ccap, rho)))) lo = m;
                else hi = m;
            }
            tt = lo;
            lab = at7(A, amp, dab, f, lo, mode, satkeep, ccap, rho);
            l = st_lin(lab);
        }
        if (ring > 0.0 && fine != 1.0) {
            float r0 = 1e9, r1 = -1e9;
            [unroll] for (int y = -1; y <= 1; ++y)
                [unroll] for (int x = -1; x <= 1; ++x) {
                    float v = gFineH.Load(int3(clamp(int2(px) + int2(x, y), int2(0, 0), int2(sz) - 1), 0)).y;
                    r0 = min(r0, v); r1 = max(r1, v);
                }
            float lnf = st_soft(O.x, (tt - 1.0) * (amp - (fine - 1.0) * F));
            lab.x = (ring > 1.5) ? max(lab.x, min(lnf, r0)) : clamp(lab.x, min(lnf, r0), max(lnf, r1));
            float3 l2 = st_lin(lab);
            if (st_in(l2)) l = l2;
        }
        if (st_in(l)) o = st_enc(l);
    } else if (mode == 6u) {
        float3 add = st_dadd(n3, A.O, k * ((n3 - p3) - st_gfq(gCA, gCB, float2(px) + 0.5, X.x, low, mI, vI)));
        o = n3 + st_fit(n3, add) * add;
        if (nohalo > 0.0) {
            float fF = k * gFineH.Load(int3(int2(px), 0)).x;
            o = st_nohalo(o, (fF != 0.0) ? st_relight(A.O, A.O.x, fF) : n3);
        }
        if (nohalo > 0.0 && ring > 0.0) o = st_fb_ring(o, A.O, int2(px), sz);
    }
    return ocap_tail(px, p3, o);
}

[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 lid3 : SV_GroupThreadID, uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    uint2 sz = gU[0].xy, low = gU[0].zw;
    uint mode = gU[1].x, restored = gU[1].y;
    bool inside = (id.x < sz.x && id.y < sz.y);
    bool hybrid = (mode == 8u);
    if (hybrid) {
        if (gi == 0u) s_qn = 0u;
        GroupMemoryBarrierWithGroupSync();
    }
    float4 n = float4(0, 0, 0, 1), p = float4(0, 0, 0, 1), fa = float4(0, 0, 0, 0);
    float3 o = float3(0, 0, 0), ofb = float3(0, 0, 0);
    Ans ans = make_ans(float3(0, 0, 0));
    Ov v1; v1.dab = 0; v1.f = 0; v1.amp = 0; v1.l = 0; v1.slot = kDone;
    Ov v2; v2.dab = 0; v2.f = 0; v2.amp = 0; v2.l = 0; v2.slot = kDone;
    if (inside) {
        n = gN.Load(int3(int2(id.xy), 0)); n.rgb = saturate(FastClean3(n.rgb));
        p = gP.Load(int3(int2(id.xy), 0)); p.rgb = saturate(FastClean3(p.rgb));
        if (hybrid) {
            float3 X = st_lab(p.rgb);
            ans = make_ans(st_lab(n.rgb));
            float mI = 0.0, vI = 0.0;
            fa = gFbAdd.Load(int3(int2(id.xy), 0));
            float3 q = st_gfq(gCA, gCB, float2(id.xy) + 0.5, X.x, low, mI, vI);
            float3 pp = saturate(p.rgb - fa.rgb), Xp = st_lab(pp);
            float halo = gF[0].y, fine = gF[1].x, ring = gF[2].y, nohalo = gF[2].w;
            float3 add = st_dadd(n.rgb, ans.O, halo * ((n.rgb - p.rgb) - q));
            ofb = n.rgb + st_fit(n.rgb, add) * add;
            v1 = ov_begin(Xp, ans, -1e9);
            if (nohalo > 0.0) {
                float F = gFineH.Load(int3(int2(id.xy), 0)).x, fF = halo * F;
                ofb = st_nohalo(ofb, (fF != 0.0) ? st_relight(ans.O, ans.O.x, fF) : n.rgb);
                if (ring > 0.0) ofb = st_fb_ring(ofb, ans.O, int2(id.xy), sz);
                float Bl = st_lab(saturate(pp + q)).x - Xp.x;
                v2 = ov_begin(Xp, ans, Bl + fine * F);
            }
        } else {
            o = (mode == 10u) ? st_edit(n.rgb, p.rgb, gF[0].x) : run_strengthened(id.xy, n.rgb, p.rgb);
        }
    }
    if (hybrid) {
        GroupMemoryBarrierWithGroupSync();
        uint qn = min(s_qn, kQueue);
        for (uint i = gi; i < qn; i += 256u) {
            float4 a = s_qa[i], b = s_qb[i]; float2 c = s_qc[i];
            Ans qA; qA.O = a.xyz; qA.co = c.y; qA.uo = b.xy;
            s_qa[i].xyz = ov_search(qA, a.w, b.zw, c.x);
        }
        GroupMemoryBarrierWithGroupSync();
        if (inside) {
            float3 oov = ov_end(v1, ans, n.rgb);
            if (gF[2].w > 0.0) oov = st_nohalo(oov, ov_end(v2, ans, n.rgb));
            o = ocap_tail(id.xy, p.rgb, lerp(oov, ofb, saturate(fa.a)));
        }
    }
    if (inside) {
        gOut[id.xy] = float4(saturate(o), n.a);
        if (restored != 0u) {
            float3 back = saturate(p.rgb - gFbAdd.Load(int3(int2(id.xy), 0)).rgb);
            s_iv[lid3.y][lid3.x] = float4(st_lab(back).x, saturate(o) - back);
        }
    }
    if (restored == 0u) return;
    GroupMemoryBarrierWithGroupSync();
    if (lid3.x >= 4u || lid3.y >= 4u) return;
    uint2 q = gid.xy * 4u + lid3.xy;
    if (q.x >= low.x || q.y >= low.y) return;
    float4 A = float4(0, 0, 0, 0), B = float4(0, 0, 0, 0);
    float c = 0.0;
    [unroll] for (uint y = 0u; y < 4u; ++y) {
        [unroll] for (uint x = 0u; x < 4u; ++x) {
            uint2 t = q * 4u + uint2(x, y);
            if (t.x >= sz.x || t.y >= sz.y) continue;
            float4 iv = s_iv[t.y - gid.y * 16u][t.x - gid.x * 16u];
            float I = iv.x; float3 v = iv.yzw;
            A += float4(I, I * I, v.r, v.g);
            B += float4(v.b, I * v);
            c += 1.0;
        }
    }
    float inv = 1.0 / max(c, 1.0);
    gOutA[q] = A * inv;
    gOutB[q] = B * inv;
}
)HLSL";

// ---- Interleaved shaders: nr_style_grade.glsl + il_pp.comp + il0.comp ----

static const char* kFastIlGradeHlsl = R"HLSL(
cbuffer FastCB : register(b0) {
    uint4  gU[4];
    float4 gF[4];
};
SamplerState gSmp : register(s0);

struct NrGrade {
    float black, white, exposure, gamma, contrast, saturation, sat_power, tint1, tint2;
    float band0, band1, band2, band3, band4;
};
NrGrade nr_grade_of(uint style) {
    NrGrade g = { 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    if (style == 1u) { g.exposure = -0.1; g.contrast = -0.25; g.saturation = -0.1; }
    if (style == 2u) { g.saturation = -0.15; }
    return g;
}
float nr_grade_smooth(float t) { t = saturate(t); return t * t * (3.0 - (t + t)); }
float3 nr_grade_tint(float3 c, float a, float hue_pos, float hue_neg, bool warm_first) {
    float m = abs(a);
    if (m < 1e-6) return c;
    float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b);
    float L = (mx + mn) * 0.5;
    float hi = (L < 0.5) ? (L + L) : ((L + 1.0) - L);
    float lo = (L + L) - hi;
    float3 t;
    if (warm_first) {
        t = (a > 0.0) ? float3(hi, (hi - lo) * 6.0 * hue_pos + lo, lo)
                      : float3(lo, (hi - lo) * hue_neg * 6.0 + lo, hi);
    } else {
        t = (a > 0.0) ? float3((hi - lo) * hue_pos * 6.0 + lo, hi, lo)
                      : float3((hi - lo) * 6.0 * hue_neg + lo, lo, hi);
    }
    return saturate(m * (t - c) + c);
}
float3 nr_grade_hsl(float3 c) {
    float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b);
    float sum = mx + mn, L = sum * 0.5;
    float h = 0.0, s = 0.0;
    if (mx > mn) {
        float d = mx - mn;
        s = (L > 0.5) ? (d / max(2.0 - mx - mn, 1e-6)) : (d / max(sum, 1e-6));
        if (mx == c.r) h = ((c.g < c.b ? 6.0 : 0.0) + (c.g - c.b) / d) / 6.0;
        else if (mx == c.g) h = ((c.b - c.r) / d + 2.0) / 6.0;
        else h = ((c.r - c.g) / d + 4.0) / 6.0;
    }
    return float3(h, s, L);
}
float nr_grade_hue(float t, float hi, float lo) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return t * (hi - lo) * 6.0 + lo;
    if (t < 0.5) return hi;
    if (t < 2.0 / 3.0) return (2.0 / 3.0 - t) * (hi - lo) * 6.0 + lo;
    return lo;
}
float3 nr_grade_rgb(float h, float s, float L) {
    if (s <= 0.0) return float3(L, L, L);
    float hi = (L < 0.5) ? (L * (s + 1.0)) : ((L + s) - L * s);
    float lo = (L + L) - hi;
    return float3(nr_grade_hue(h + 1.0 / 3.0, hi, lo), nr_grade_hue(h, hi, lo), nr_grade_hue(h - 1.0 / 3.0, hi, lo));
}
float nr_grade_contrast(float x, float k) {
    x = saturate(x);
    return k * (x * x * (3.0 - (x + x)) - x) + x;
}
float nr_grade_bands(float y, NrGrade g) {
    float x = saturate(y);
    float w0 = nr_grade_smooth((x - 0.25) / -0.25);
    float w1 = nr_grade_smooth(x / 0.25) * nr_grade_smooth((x - 0.5) / -0.25);
    float w2 = nr_grade_smooth((x - 0.25) / 0.25) * nr_grade_smooth((x - 0.75) / -0.25);
    float w3 = nr_grade_smooth((x - 0.5) / 0.25) * nr_grade_smooth((x - 1.0) / -0.25);
    float w4 = nr_grade_smooth((x - 0.75) / 0.25);
    float tot = w4 * g.band4 + w3 * g.band3 + w2 * g.band2 + w0 * g.band0 + w1 * g.band1;
    float p = pow(max(x, 1e-7), exp2(-tot));
    return (x <= 1e-7) ? 0.0 : p;
}
float3 nr_style_grade(uint style, float3 base, float3 rgb, float intensity) {
    NrGrade g = nr_grade_of(style);
    float3 c = saturate(rgb);
    float span = g.white - g.black;
    if (abs(span) > 1e-6) {
        c = saturate((c - g.black) / span);
    } else {
        c = step(g.black, c);
    }
    float gain = exp2(g.exposure);
    c = saturate(c * gain);
    if (abs(g.gamma) > 1e-6) {
        float ig = 1.0 / max(g.gamma + 1.0, 1e-4);
        c = float3(
            (c.r <= 1e-7) ? 0.0 : pow(c.r, ig),
            (c.g <= 1e-7) ? 0.0 : pow(c.g, ig),
            (c.b <= 1e-7) ? 0.0 : pow(c.b, ig)
        );
    }
    c = saturate(float3(
        nr_grade_contrast(c.r, g.contrast),
        nr_grade_contrast(c.g, g.contrast),
        nr_grade_contrast(c.b, g.contrast)
    ));
    if (abs(g.saturation) > 1e-6 || abs(g.sat_power) > 1e-6) {
        float3 hsl = nr_grade_hsl(c);
        float s = saturate(hsl.y * (1.0 + g.saturation));
        if (abs(g.sat_power) > 1e-6 && s > 1e-7) {
            s = saturate(pow(s, exp2(-g.sat_power)));
        }
        c = saturate(nr_grade_rgb(hsl.x, s, hsl.z));
    }
    c = nr_grade_tint(c, g.tint1, 0.22222222, 0.41666667, true);
    c = nr_grade_tint(c, g.tint2, 0.58333333, 0.08333333, false);
    c = saturate(float3(
        nr_grade_bands(c.r, g),
        nr_grade_bands(c.g, g),
        nr_grade_bands(c.b, g)
    ));
    return saturate(base + saturate(intensity) * (c - base));
}
)HLSL";

static const char* kFastIlPpHlsl = R"HLSL(
cbuffer FastCB : register(b0) {
    uint4  gU[4];
    float4 gF[4];
};
Texture2D<float4>      gTrust : register(t0);
Texture2DArray<float4> gSInc  : register(t1);
Texture2D<float4>      gSrc   : register(t2);
Texture2D<float>       gNear  : register(t3);
Texture2D<float2>      gDSrc  : register(t4);
RWTexture2D<float4>    gDst   : register(u0);
RWTexture2D<float2>    gDDst  : register(u1);

[numthreads(16, 16, 1)]
void main(uint3 g : SV_DispatchThreadID) {
    uint2 ssize = gU[0].xy, dsize = gU[0].zw;
    uint mode = gU[1].x, n = gU[1].y, dflag = gU[1].z;
    if (g.x >= dsize.x * n || g.y >= dsize.y) return;
    uint i = g.x / dsize.x;
    int2 d = int2(int(g.x - i * dsize.x), int(g.y));
    float4 s = float4(0, 0, 0, 0);
    float2 dn = float2(0, 0);
    bool doDepth = (dflag != 0u) && (i == 0u);
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            int2 c = 2 * d + int2(x, y);
            if (uint(c.x) >= ssize.x || uint(c.y) >= ssize.y) continue;
            if (mode == 0u) {
                float w = gTrust.Load(int3(c, 0)).r;
                float3 inc = gSInc.Load(int4(c, int(i), 0)).rgb;
                s += float4(inc * w, w);
                if (doDepth) dn += float2(gNear.Load(int3(c, 0)).r * w, w);
            } else {
                s += gSrc.Load(int3(c.x + int(i * ssize.x), c.y, 0));
                if (doDepth) dn += gDSrc.Load(int3(c, 0)).rg;
            }
        }
    }
    gDst[g.xy] = s * 0.25;
    if (doDepth) gDDst[d] = dn * 0.25;
}
)HLSL";

static const char* kFastIlHlsl = R"HLSL(
Texture2D<float4>        gProxy    : register(t0);
Texture2D<float4>        gBasePrev : register(t1);
Texture2DArray<float4>   gSInc     : register(t2);
Texture2D<float4>        gMotion   : register(t3);
Texture2D<float4>        gAnswer   : register(t4);
Texture2D<float4>        gTrustIn  : register(t5);
Texture2D<float4>        gL1       : register(t6);
Texture2D<float4>        gL2       : register(t7);
Texture2D<float4>        gL3       : register(t8);
Texture2D<float4>        gL4       : register(t9);
Texture2D<float4>        gL5       : register(t10);
Texture2D<float4>        gL6       : register(t11);
Texture2D<float>         gDepthIn  : register(t12);
Texture2D<float>         gDepthCur : register(t13);
Texture2D<float>         gDepthPrev: register(t14);
Texture2D<float2>        gDP1      : register(t15);
Texture2D<float2>        gDP2      : register(t16);
Texture2D<float2>        gDP3      : register(t17);
Texture2D<float2>        gDP4      : register(t18);
Texture2D<float2>        gDP5      : register(t19);
Texture2D<float2>        gDP6      : register(t20);

RWTexture2D<float4>      gOut      : register(u0); // Pre: FastIn; Post: FastOut
RWTexture2DArray<float4> gDInc     : register(u1);
RWTexture2D<float4>      gBaseCur  : register(u2);
RWTexture2D<float4>      gTrustOut : register(u3);
RWTexture2D<float>       gDepthOut : register(u4);

float3 IlClean3(float3 c) {
    if ((asuint(c.r) & 0x7F800000u) == 0x7F800000u) c.r = 0.0;
    if ((asuint(c.g) & 0x7F800000u) == 0x7F800000u) c.g = 0.0;
    if ((asuint(c.b) & 0x7F800000u) == 0x7F800000u) c.b = 0.0;
    return c;
}
float2 IlClean2(float2 v) {
    if ((asuint(v.x) & 0x7F800000u) == 0x7F800000u) v.x = 0.0;
    if ((asuint(v.y) & 0x7F800000u) == 0x7F800000u) v.y = 0.0;
    return v;
}
float3 post_slot_linear(uint k, float3 base, float3 rgb, uint flags, uint styles, float4 inten) {
    if ((flags & 2048u) == 0u) return rgb;
    uint kk = min(k, 3u);
    uint st = (styles >> (2u * kk)) & 3u;
    float I = inten[kk];
    rgb = saturate(min(I, 1.0) * (rgb - base) + base);
    if (st != 0u) rgb = nr_style_grade(st, base, rgb, I);
    return rgb;
}
float3 post_overdrive_final(float3 base, float3 rgb, float I) {
    if (I <= 1.0) return rgb;
    float fl = 1.0 / 512.0, guard = 2.0;
    float3 lw = float3(0.2126, 0.7152, 0.0722);
    float ratio = (dot(rgb, lw) + fl) / (dot(base, lw) + fl);
    float amp = clamp(pow(max(ratio, 1e-6), I), 1.0 / guard, guard);
    return saturate(rgb * (amp / max(ratio, 1e-6)));
}
float3 bil_inc(uint i, float2 p, uint2 sz) {
    float2 f = p - 0.5, b0 = floor(f), w = f - b0;
    int2 i0 = int2(b0), hi = int2(sz) - 1;
    float3 a = gSInc.Load(int4(clamp(i0, int2(0,0), hi), int(i), 0)).rgb;
    float3 b = gSInc.Load(int4(clamp(i0 + int2(1,0), int2(0,0), hi), int(i), 0)).rgb;
    float3 c = gSInc.Load(int4(clamp(i0 + int2(0,1), int2(0,0), hi), int(i), 0)).rgb;
    float3 d = gSInc.Load(int4(clamp(i0 + int2(1,1), int2(0,0), hi), int(i), 0)).rgb;
    return lerp(lerp(a, b, w.x), lerp(c, d, w.x), w.y);
}
float3 prev_base(float2 p, uint2 sz) {
    float2 f = p - 0.5, b0 = floor(f), w = f - b0;
    int2 i0 = int2(b0), hi = int2(sz) - 1;
    float3 a = gBasePrev.Load(int3(clamp(i0, int2(0,0), hi), 0)).rgb;
    float3 b = gBasePrev.Load(int3(clamp(i0 + int2(1,0), int2(0,0), hi), 0)).rgb;
    float3 c = gBasePrev.Load(int3(clamp(i0 + int2(0,1), int2(0,0), hi), 0)).rgb;
    float3 d = gBasePrev.Load(int3(clamp(i0 + int2(1,1), int2(0,0), hi), 0)).rgb;
    return lerp(lerp(a, b, w.x), lerp(c, d, w.x), w.y);
}
float near_at(uint2 mt, uint2 mbase, uint2 mgrid, uint2 dgrid, uint flags) {
    uint2 dt = min(((mt - mbase) * dgrid) / max(mgrid, uint2(1u, 1u)), max(dgrid, uint2(1u, 1u)) - 1u);
    float d = gDepthIn.Load(int3(int2(dt), 0)).r;
    return ((flags & 1024u) != 0u) ? d : (1.0 - d);
}
float2 dlv_read(uint l, int2 c) {
    if (l == 1u) return gDP1.Load(int3(c, 0)).rg;
    if (l == 2u) return gDP2.Load(int3(c, 0)).rg;
    if (l == 3u) return gDP3.Load(int3(c, 0)).rg;
    if (l == 4u) return gDP4.Load(int3(c, 0)).rg;
    if (l == 5u) return gDP5.Load(int3(c, 0)).rg;
    return gDP6.Load(int3(c, 0)).rg;
}
float4 lv_read(uint l, int2 c) {
    if (l == 1u) return gL1.Load(int3(c, 0));
    if (l == 2u) return gL2.Load(int3(c, 0));
    if (l == 3u) return gL3.Load(int3(c, 0));
    if (l == 4u) return gL4.Load(int3(c, 0));
    if (l == 5u) return gL5.Load(int3(c, 0));
    return gL6.Load(int3(c, 0));
}
void lvgeo(uint l, float2 p, uint2 sz, out int2 c[4], out int wx, out float2 t) {
    uint2 w = sz;
    for (uint k = 0u; k < l; ++k) w = (w + 1u) / 2u;
    float2 f = p / float(1u << l) - 0.5, b0 = floor(f);
    t = f - b0;
    int2 i0 = int2(b0), hi = int2(w) - 1;
    c[0] = clamp(i0, int2(0, 0), hi); c[1] = clamp(i0 + int2(1, 0), int2(0, 0), hi);
    c[2] = clamp(i0 + int2(0, 1), int2(0, 0), hi); c[3] = clamp(i0 + int2(1, 1), int2(0, 0), hi);
    wx = int(w.x);
}
float4 lvw(uint l, float2 p, uint2 sz, float nearVal) {
    int2 c[4]; int wx; float2 t;
    lvgeo(l, p, sz, c, wx, t);
    float4 wb = float4((1.0 - t.x) * (1.0 - t.y), t.x * (1.0 - t.y), (1.0 - t.x) * t.y, t.x * t.y);
    [unroll] for (int k = 0; k < 4; ++k) {
        float2 dn = dlv_read(l, c[k]);
        float nk = (dn.y > 1e-6) ? (dn.x / dn.y) : -1.0;
        float rel = (nk < 0.0) ? 1.0 : (abs(nk - nearVal) / max(max(nk, nearVal), 1e-7));
        wb[k] *= 1.0 - smoothstep(0.05, 0.2, rel);
    }
    return wb;
}
float4 lvb_ex(uint l, uint i, float2 p, uint2 sz, bool bil, float4 wb) {
    int2 c[4]; int wx; float2 t;
    lvgeo(l, p, sz, c, wx, t);
    int2 off = int2(int(i) * wx, 0);
    float4 a = lv_read(l, c[0] + off), b = lv_read(l, c[1] + off);
    float4 cc = lv_read(l, c[2] + off), d = lv_read(l, c[3] + off);
    if (!bil) return lerp(lerp(a, b, t.x), lerp(cc, d, t.x), t.y);
    float sw = wb.x + wb.y + wb.z + wb.w;
    return (sw > 1e-4) ? ((a * wb.x + b * wb.y + cc * wb.z + d * wb.w) / sw * min(sw * 4.0, 1.0)) : float4(0, 0, 0, 0);
}
float3 pull_inc(uint i, float2 p, uint2 sz, bool bil, float4 wl[6]) {
    float4 top0 = lvb_ex(6u, i, p, sz, false, float4(0, 0, 0, 0));
    float4 topd = bil ? lvb_ex(6u, i, p, sz, true, wl[5]) : top0;
    float4 top = (topd.a > 1e-4) ? topd : top0;
    float3 e = (top.a > 1e-4) ? (top.rgb / top.a) : float3(0, 0, 0);
    for (uint l = 5u; l >= 1u; --l) {
        float4 m0 = lvb_ex(l, i, p, sz, false, float4(0, 0, 0, 0));
        float4 md = bil ? lvb_ex(l, i, p, sz, true, wl[l - 1u]) : m0;
        float4 m = (md.a > 1e-4) ? md : m0;
        e = m.rgb + (1.0 - m.a) * e;
    }
    return e;
}
float2 grad_v(int2 c, float2 d, uint2 sz, uint2 mbase, uint2 mgrid, float2 mscale) {
    int2 lo = int2(mbase), hi = int2(mbase + mgrid) - 1;
    float2 stp = 2.0 * float2(sz) / float2(mgrid);
    float2 mx = (IlClean2(gMotion.Load(int3(clamp(c + int2(2, 0), lo, hi), 0)).xy) -
                 IlClean2(gMotion.Load(int3(clamp(c - int2(2, 0), lo, hi), 0)).xy)) * mscale / (2.0 * stp.x);
    float2 my = (IlClean2(gMotion.Load(int3(clamp(c + int2(0, 2), lo, hi), 0)).xy) -
                 IlClean2(gMotion.Load(int3(clamp(c - int2(0, 2), lo, hi), 0)).xy)) * mscale / (2.0 * stp.y);
    return mx * d.x + my * d.y;
}
float2 clampl(float2 x, float m) { float l = length(x); return (l > m && l > 1e-6) ? x * (m / l) : x; }

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint2 sz    = gU[0].xy;
    if (id.x >= sz.x || id.y >= sz.y) return;
    int2  ip    = int2(id.xy);
    uint2 mbase = gU[0].zw;
    uint2 mgrid = max(gU[1].xy, uint2(1u, 1u));
    uint  mode  = gU[1].z;
    uint  slot  = gU[1].w;
    uint  n     = gU[2].x;
    uint  flags = gU[2].y;
    uint  styles = gU[2].z;
    uint  warmMask = gU[3].y;
    uint2 dgrid = max(gU[3].zw, uint2(1u, 1u));
    float2 mscale = gF[0].xy;
    float  k0 = gF[0].z, k1 = gF[0].w;
    float4 inten = gF[1];

    if (mode == 3u) {
        // Store clean frame into BaseCur + copy raw to gOut when needed
        float4 p = gProxy.Load(int3(ip, 0));
        p.rgb = saturate(IlClean3(p.rgb));
        gBaseCur[id.xy] = float4(p.rgb, 1.0);
        gOut[id.xy]     = float4(p.rgb, 1.0);
        return;
    }

    if (mode == 5u) {
        uint2 mt = mbase + min(uint2((float2(id.xy) + 0.5) * float2(mgrid) / float2(sz)), mgrid - 1u);
        gDepthOut[id.xy] = near_at(mt, mbase, mgrid, dgrid, flags);
        return;
    }

    if (mode == 4u) {
        // Hole fill from 6-level pyramid (pure SRV gSInc -> pure UAV gDInc)
        float trust = gTrustIn.Load(int3(ip, 0)).r;
        if (trust >= 1.0) {
            for (uint i = 0u; i < n; ++i) {
                gDInc[uint3(id.xy, i)] = gSInc.Load(int4(ip, int(i), 0));
            }
            return;
        }
        bool bil = (flags & 67108864u) != 0u;
        float4 wl[6];
        if (bil) {
            float nearVal = gDepthCur.Load(int3(ip, 0)).r;
            [unroll] for (uint l = 1u; l <= 6u; ++l) {
                wl[l - 1u] = lvw(l, float2(id.xy) + 0.5, sz, nearVal);
            }
        } else {
            [unroll] for (uint l = 0u; l < 6u; ++l) wl[l] = float4(0, 0, 0, 0);
        }
        float3 z = saturate(IlClean3(gProxy.Load(int3(ip, 0)).rgb));
        for (uint i = 0u; i < n; ++i) {
            float3 curD = gSInc.Load(int4(ip, int(i), 0)).rgb;
            float3 fill = pull_inc(i, float2(id.xy) + 0.5, sz, bil, wl);
            float3 v = lerp(fill, curD, trust);
            gDInc[uint3(id.xy, i)] = float4(v, 0.0);
            if (i < slot && ((warmMask & (1u << i)) != 0u)) {
                z = post_slot_linear(i, z, z + v, flags, styles, inten);
            }
        }
        gOut[id.xy] = float4(saturate(z), 1.0);
        return;
    }

    if (mode == 0u) {
        // Pre: snapshot clean current frame to BaseCur, move all slots by MV + Anti-Ghost, compose slots < slot -> gOut (FastIn)
        float3 b = saturate(IlClean3(gProxy.Load(int3(ip, 0)).rgb));
        gBaseCur[id.xy] = float4(b, 1.0);
        float2 q = float2(id.xy) + 0.5;
        bool inside = true;
        if ((flags & 1u) != 0u) {
            uint2 t = mbase + min(uint2((float2(id.xy) + 0.5) * float2(mgrid) / float2(sz)), mgrid - 1u);
            if ((flags & 256u) != 0u) {
                uint2 best = t;
                float bn = near_at(t, mbase, mgrid, dgrid, flags);
                [unroll] for (int dy = -1; dy <= 1; ++dy) {
                    [unroll] for (int dx = -1; dx <= 1; ++dx) {
                        uint2 c = uint2(clamp(int2(t) + int2(dx, dy), int2(mbase), int2(mbase + mgrid) - 1));
                        float nn = near_at(c, mbase, mgrid, dgrid, flags);
                        if (nn > bn) { bn = nn; best = c; }
                    }
                }
                t = best;
            }
            q += IlClean2(gMotion.Load(int3(int2(t), 0)).xy) * mscale;
            inside = all(q >= 0.0) && all(q <= float2(sz));
        }
        float stay = 0.0, trust = 1.0;
        float3 lo = float3(1e9, 1e9, 1e9), hi = float3(-1e9, -1e9, -1e9), pq = float3(0, 0, 0);
        if ((flags & 1u) != 0u) {
            const float3 lw = float3(0.2126, 0.7152, 0.0722);
            float l = 0.0, em = 0.0, es = 0.0;
            [unroll] for (int y = -1; y <= 1; ++y) {
                [unroll] for (int x = -1; x <= 1; ++x) {
                    int2 c = clamp(ip + int2(x, y), int2(0, 0), int2(sz) - 1);
                    float3 cb = saturate(IlClean3(gProxy.Load(int3(c, 0)).rgb));
                    float3 pm = prev_base(q + float2(x, y), sz);
                    l += dot(cb, lw);
                    em += dot(pm, lw);
                    es += dot(gBasePrev.Load(int3(c, 0)).rgb, lw);
                    lo = min(lo, cb); hi = max(hi, cb);
                    if (x == 0 && y == 0) pq = pm;
                }
            }
            em = abs(l - em) / 9.0; es = abs(l - es) / 9.0;
            stay = smoothstep(0.0, 0.03, em - es);
            if (stay > 0.0) inside = true;
            if ((flags & 4u) != 0u) trust = 1.0 - smoothstep(0.015, 0.05, min(em, es));
            if ((flags & 8u) != 0u) {
                float2 v = q - (float2(id.xy) + 0.5);
                float2 qc = clamp(q, float2(0, 0), float2(sz) - 0.001);
                uint2 t2 = mbase + min(uint2(qc * float2(mgrid) / float2(sz)), mgrid - 1u);
                float2 vq = IlClean2(gMotion.Load(int3(int2(t2), 0)).xy) * mscale;
                float dv = length(vq - v), lv = length(v);
                if ((flags & 524288u) != 0u) {
                    int2 tc = int2(mbase + min(uint2((float2(id.xy) + 0.5) * float2(mgrid) / float2(sz)), mgrid - 1u));
                    float2 gp = grad_v(tc, v, sz, mbase, mgrid, mscale);
                    float cmax = 0.5 * lv;
                    dv = min(dv, length(vq - v - clampl(gp, cmax)));
                    if ((flags & 1048576u) != 0u) {
                        float2 gq = grad_v(int2(t2), v, sz, mbase, mgrid, mscale);
                        dv = min(dv, min(length(vq - v - clampl(gq, cmax)), length(vq - v - clampl(0.5 * (gp + gq), cmax))));
                    }
                }
                float t_v = lerp(1.0 - smoothstep(0.5 + 0.05 * lv, 2.0 + 0.15 * lv, dv), 1.0, stay);
                trust = min(trust, t_v);
            }
            if ((flags & 129u) == 129u) {
                float3 pv = lerp(pq, gBasePrev.Load(int3(ip, 0)).rgb, stay);
                float3 ov = max(pv - hi, lo - pv);
                float t_b = 1.0 - smoothstep(k0, k1, max(ov.r, max(ov.g, ov.b)));
                trust = min(trust, t_b);
            }
            if ((flags & 512u) != 0u && inside) {
                int2 b0 = int2(floor(q - 0.5)), hiD = int2(sz) - 1;
                float np = 0.0;
                [unroll] for (int dy = 0; dy <= 1; ++dy) {
                    [unroll] for (int dx = 0; dx <= 1; ++dx) {
                        np = max(np, gDepthPrev.Load(int3(clamp(b0 + int2(dx, dy), int2(0, 0), hiD), 0)).r);
                    }
                }
                float nc = gDepthCur.Load(int3(ip, 0)).r;
                float r = (np - nc) / max(nc, 1e-7);
                float t_d = lerp(1.0 - smoothstep(0.05, 0.2, r), 1.0, stay);
                trust = min(trust, t_d);
            }
        }
        if ((flags & 33554433u) == 33554433u && !inside) trust = 0.0;
        gTrustOut[id.xy] = float4(trust, trust, trust, 1.0);
        float3 z = b;
        for (uint i = 0u; i < n; ++i) {
            bool validSlot = (warmMask & (1u << i)) != 0u;
            float3 v = (!validSlot || !inside) ? float3(0, 0, 0) : ((flags & 1u) != 0u ? bil_inc(i, q, sz) : gSInc.Load(int4(ip, int(i), 0)).rgb);
            if (validSlot && stay > 0.0) v = lerp(v, gSInc.Load(int4(ip, int(i), 0)).rgb, stay);
            gDInc[uint3(id.xy, i)] = float4(v, 0.0);
            if (i < slot && validSlot) {
                z = post_slot_linear(i, z, z + trust * v, flags, styles, inten);
            }
        }
        gOut[id.xy] = float4(saturate(z), 1.0);
        return;
    }

    // mode == 1u: Post -- store fresh = answer - zin, accumulate all warmed slots onto b -> gOut (FastOut)
    float3 a   = saturate(IlClean3(gAnswer.Load(int3(ip, 0)).rgb));
    float3 zin = saturate(IlClean3(gBasePrev.Load(int3(ip, 0)).rgb)); // slot's input (gFastIn)
    float3 b   = saturate(IlClean3(gProxy.Load(int3(ip, 0)).rgb));    // original clean base (gBaseCur)
    float3 fresh = a - zin;
    float trust = ((flags & 65536u) != 0u && (flags & 64u) == 0u) ? gTrustIn.Load(int3(ip, 0)).r : 1.0;
    float3 o = b;
    for (uint i = 0u; i < n; ++i) {
        if ((warmMask & (1u << i)) == 0u && i != slot) {
            gDInc[uint3(id.xy, i)] = float4(0, 0, 0, 0);
            continue;
        }
        float3 v = (i == slot) ? fresh : gSInc.Load(int4(ip, int(i), 0)).rgb;
        if (i != slot && trust < 1.0) {
            v = lerp(fresh, v, trust);
        }
        gDInc[uint3(id.xy, i)] = float4(v, 0.0);
        o = post_slot_linear(i, o, o + v, flags, styles, inten);
    }
    if ((flags & 2048u) != 0u && inten.x > 1.0) o = post_overdrive_final(b, o, inten.x);
    gOut[id.xy] = float4(saturate(o), 1.0);
}
)HLSL";

static const char* kFastTo8Hlsl = R"HLSL(
cbuffer FastCB : register(b0) {
    uint4  gU[4];
    float4 gF[4];
};
Texture2D<float4>   gSrc : register(t0);
RWTexture2D<float4> gDst : register(u0);
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gU[0].x || id.y >= gU[0].y) return;
    float4 c = gSrc.Load(int3(int2(id.xy), 0));
    gDst[id.xy] = float4(saturate(c.rgb), 1.0);
}
)HLSL";

class Engine {
public:
    static constexpr UINT kSrvCount    = 22;
    static constexpr UINT kUavCount    = 10;
    static constexpr UINT kPageStrides = kSrvCount + kUavCount; // 32
    static constexpr UINT kPagesPerFrm = 24;
    static constexpr UINT kTotalPages  = kPagesPerFrm * 2;       // 48 pages double-buffered

    void Reset() {
        ready_ = false;
        failed_ = false;
        histValid_ = false;
        histCur_ = 0;
        ilValid_ = false;
        ilDepthValid_ = false;
        ilCur_ = 0;
        ilBcur_ = 0;
        ilDcur_ = 0;
        ilFrame_ = 0;
        ilWarmMask_ = 0;
        ilSlotsBuilt_ = 0;
        pageCursor_ = 0;
        frameParity_ = 0;
        lastPreMethod_ = -1;
        didPreThisFrame_ = false;
        lastPreSlot_ = 0;
        lastPreFlags_ = 0;
        lastPreStyles_ = 0;
        lastPreInten_[0] = lastPreInten_[1] = lastPreInten_[2] = lastPreInten_[3] = 1.0f;
        lastHasMv_ = false;

        w_ = h_ = lowW_ = lowH_ = 0;
        dev_.Reset();
        rs_.Reset();
        heap_.Reset();
        psoFbIn_.Reset();
        psoPrep_.Reset();
        psoBox_.Reset();
        psoApply_.Reset();
        psoIl_.Reset();
        psoIlPp_.Reset();
        psoTo8_.Reset();

        fastIn_.Reset(); fastInShared_ = nullptr; fastInState_ = D3D12_RESOURCE_STATE_COMMON;
        fedFastIn8To16_ = false;
        fastOut_.Reset(); fastOut8_.Reset();
        fbAdd_.Reset(); fineH_.Reset();
        for (int i = 0; i < 2; ++i) {
            D_[i].Reset(); T_[i].Reset(); G_[i].Reset();
            F0_[i].Reset(); F1_[i].Reset();
            ilBase_[i].Reset(); ilInc_[i].Reset(); ilDepth_[i].Reset();
        }
        ilTrust_.Reset();
        for (int l = 0; l < 6; ++l) { ilPyr_[l].Reset(); ilDPyr_[l].Reset(); }
    }

    void InvalidateHistory() {
        histValid_ = false;
        ilValid_ = false;
        ilDepthValid_ = false;
        ilWarmMask_ = 0;
        ilFrame_ = 0;
        didPreThisFrame_ = false;
        fedFastIn8To16_ = false;
    }

    void NotifyEvalFedFastIn(bool evalOk) {
        if (evalOk) fastInState_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    ID3D12Resource* FastInTex()  const { return fastIn_.Get(); }
    ID3D12Resource* FastOutTex() const { return fastOut_.Get(); }
    ID3D12Resource* FastOut8Tex() const { return fastOut8_.Get(); }
    bool IsReady() const { return ready_ && !failed_; }
    bool DidPreThisFrame() const { return didPreThisFrame_; }
    int  LastEffectiveMethod() const { return lastPreMethod_; }
    uint32_t LastPreSlot() const { return lastPreSlot_; }
    bool LastUsedNeutralCore() const { return lastPreMethod_ == kInterleaved && (lastPreFlags_ & 2048u) != 0u; }

    bool Ensure(ID3D12Device* dev, UINT w, UINT h, const Settings& s, std::string& logErr) {
        if (!s.enabled) return false;
        if (failed_) return false;
        if (!dev || w == 0 || h == 0) return false;

        if (dev_.Get() != dev || w_ != w || h_ != h) {
            Reset();
            dev_ = dev;
            w_ = w;
            h_ = h;
            lowW_ = (w + 3u) / 4u;
            lowH_ = (h + 3u) / 4u;
        }

        if (!rs_) {
            if (!BuildRootSigAndHeap(logErr)) { failed_ = true; return false; }
        }
        if (!CompileCorePipelines(logErr)) { failed_ = true; return false; }
        if (!EnsureBaseResources(logErr)) { failed_ = true; return false; }

        if (s.method == kInterleaved) {
            if (!EnsureInterleavedPipelinesAndResources(logErr)) {
                return false;
            }
        }
        ready_ = true;
        return true;
    }

    // Recorded INSIDE NgxEval batch BEFORE NgxBackendEval.
    // Returns the texture handle that NgxBackendEval MUST feed as DLSSNR.Color (either fastIn_ if staged, or srcColor if pass-through).
    ID3D12Resource* RecordPre(ID3D12GraphicsCommandList* cl,
                              ID3D12Resource* srcColor, D3D12_RESOURCE_STATES srcState,
                              ID3D12Resource* mvTex, D3D12_RESOURCE_STATES mvState,
                              float mvScaleX, float mvScaleY, bool reset,
                              const Settings& s, int& outEffMethod, bool& outNeutralCore,
                              ID3D12Resource* depthTex = nullptr,
                              D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON,
                              bool depthInverted = true) {
        didPreThisFrame_ = false;
        outNeutralCore = false;
        if (!ready_ || !cl || !srcColor) {
            outEffMethod = -1;
            return srcColor;
        }

        const bool hasMv = (mvTex != nullptr);
        const bool hasDepth = (depthTex != nullptr);
        const int eff = EffectiveMethod(s, hasMv);
        outEffMethod = eff;

        if (reset || eff != lastPreMethod_ || (eff == kInterleaved && (uint32_t)std::clamp(s.ilPasses, 2, 8) != ilSlotsBuilt_)) {
            if (reset || eff != lastPreMethod_) {
                histValid_ = false;
                ilValid_ = false;
                ilDepthValid_ = false;
                ilWarmMask_ = 0;
                ilFrame_ = 0;
            }
        }
        lastPreMethod_ = eff;
        lastHasMv_ = hasMv;
        frameParity_ ^= 1u;
        pageCursor_ = frameParity_ * kPagesPerFrm;

        if (eff < 0) return srcColor;

        // Track setting changes that alter history meaning
        if (lastSettingsMethod_ != eff ||
            std::fabs(lastPasses_ - s.passes) > 1e-4f ||
            lastHalo_ != s.haloLimit ||
            std::fabs(lastFine_ - s.fine) > 1e-4f) {
            histValid_ = false;
            lastSettingsMethod_ = eff;
            lastPasses_ = s.passes;
            lastHalo_ = s.haloLimit;
            lastFine_ = s.fine;
        }

        fedFastIn8To16_ = false;
        if (eff == kExtrapolation || eff == kEditStrength) {
            if (srcColor->GetDesc().Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
                Barrier(cl, srcColor, srcState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cl, fastIn_.Get(), fastInState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Barrier(cl, fbAdd_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                UINT cb[32]{};
                cb[0] = w_; cb[1] = h_; cb[2] = lowW_; cb[3] = lowH_;
                cb[10] = 0u;
                ID3D12Resource* srvs[12] = { srcColor };
                ID3D12Resource* uavs[10] = { fastIn_.Get(), fbAdd_.Get() };
                Dispatch(cl, psoFbIn_.Get(), srvs, 1, uavs, 2, cb, (w_ + 15u) / 16u, (h_ + 15u) / 16u);
                Barrier(cl, fbAdd_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, fastIn_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, srcColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, srcState);
                fastInState_ = D3D12_RESOURCE_STATE_COMMON;
                fedFastIn8To16_ = true;
                didPreThisFrame_ = true;
                return fastIn_.Get();
            }
            didPreThisFrame_ = true;
            return srcColor;
        }

        if (eff == kFeedback || eff == kHybrid) {
            // Inject moved guided coefficients from F0/F1 into fastIn_ (or clean copy when !histValid_)
            const uint32_t fbActive = (histValid_ && hasMv && !reset) ? 1u : 0u;
            const Strength st = ComputeStrength(s.passes);

            Barrier(cl, srcColor, srcState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (mvTex) Barrier(cl, mvTex, mvState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, fastIn_.Get(), fastInState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cl, fbAdd_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (fbActive) {
                Barrier(cl, F0_[histCur_].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cl, F1_[histCur_].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }

            UINT cb[32]{};
            cb[0] = w_; cb[1] = h_; cb[2] = lowW_; cb[3] = lowH_;
            cb[4] = 0;  cb[5] = 0;  cb[6] = w_;    cb[7] = h_;
            cb[8] = 16u | 2u | fbActive;
            cb[9] = fbActive;
            cb[10] = fbActive ? 1u : 0u;
            float f0 = mvScaleX, f1 = mvScaleY, f2 = st.w, f3 = 0.45f;
            memcpy(&cb[16], &f0, 4); memcpy(&cb[17], &f1, 4); memcpy(&cb[18], &f2, 4); memcpy(&cb[19], &f3, 4);

            ID3D12Resource* srvs[12] = { srcColor, fbActive ? F0_[histCur_].Get() : srcColor,
                                         fbActive ? F1_[histCur_].Get() : srcColor, mvTex ? mvTex : srcColor };
            ID3D12Resource* uavs[10] = { fastIn_.Get(), fbAdd_.Get() };
            Dispatch(cl, psoFbIn_.Get(), srvs, 4, uavs, 2, cb, (w_ + 15u) / 16u, (h_ + 15u) / 16u);

            if (fbActive) {
                Barrier(cl, F0_[histCur_].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, F1_[histCur_].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
            Barrier(cl, fbAdd_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, fastIn_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            fastInState_ = D3D12_RESOURCE_STATE_COMMON;
            if (mvTex) Barrier(cl, mvTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mvState);
            Barrier(cl, srcColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, srcState);

            didPreThisFrame_ = true;
            return fastIn_.Get();
        }

        // eff == kInterleaved (N = 2..8 slots)
        const uint32_t N = (uint32_t)std::clamp(s.ilPasses, 2, 8);
        ilSlotsBuilt_ = N;
        const bool graded = (s.style != 0u || std::fabs(s.intensity - 1.0f) > 1e-4f);
        if (graded != ilGradedLast_) {
            ilValid_ = false;
            ilWarmMask_ = 0;
            ilFrame_ = 0;
            ilGradedLast_ = graded;
        }
        outNeutralCore = graded;

        lastPreSlot_ = ilFrame_ % N;
        const bool isMoving = hasMv && ilValid_ && (ilWarmMask_ == ((1u << N) - 1u));
        const bool canDepthDisocc = hasDepth && ilDepthValid_;
        const uint32_t baseNext = ilBcur_ ^ 1u;
        const uint32_t incNext  = ilCur_  ^ 1u;
        const uint32_t depCur   = ilDcur_ ^ 1u;

        uint32_t stylesPack = 0u;
        for (uint32_t k = 0; k < 4u; ++k) {
            stylesPack |= (std::min(s.style, 2u) & 3u) << (2u * k);
            lastPreInten_[k] = s.intensity;
        }
        lastPreStyles_ = stylesPack;

        const uint32_t gx = (w_ + 15u) / 16u, gy = (h_ + 15u) / 16u;
        const uint32_t baseFlags =
            (isMoving ? (1u | 8u | 128u | 524288u | 1048576u | 33554432u | 65536u |
                         (hasDepth ? (256u | 67108864u) : 0u) | (canDepthDisocc ? 512u : 0u))
                      : 0u) |
            (depthInverted ? 1024u : 0u) |
            (graded ? 2048u : 0u);

        uint32_t dGridW = w_, dGridH = h_;
        if (hasDepth) {
            const D3D12_RESOURCE_DESC dd = depthTex->GetDesc();
            dGridW = (uint32_t)dd.Width;
            dGridH = (uint32_t)dd.Height;
        }

        Barrier(cl, srcColor, srcState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (mvTex) Barrier(cl, mvTex, mvState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (hasDepth) {
            Barrier(cl, depthTex, depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, ilDepth_[ilDcur_].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, ilDepth_[depCur].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        Barrier(cl, ilBase_[ilBcur_].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, ilInc_[ilCur_].Get(),   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, ilBase_[baseNext].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cl, ilInc_[incNext].Get(),   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cl, ilTrust_.Get(),          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cl, fastIn_.Get(),           fastInState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        UINT cb[32]{};
        cb[0] = w_; cb[1] = h_; cb[2] = 0; cb[3] = 0;
        cb[4] = w_; cb[5] = h_; cb[6] = 0u; cb[7] = lastPreSlot_;
        cb[8] = N;  cb[9] = baseFlags; cb[10] = stylesPack; cb[11] = baseNext;
        cb[12] = incNext; cb[13] = ilWarmMask_; cb[14] = dGridW; cb[15] = dGridH;
        float k0 = 0.02f, k1 = 0.06f;
        memcpy(&cb[16], &mvScaleX, 4); memcpy(&cb[17], &mvScaleY, 4);
        memcpy(&cb[18], &k0, 4);       memcpy(&cb[19], &k1, 4);
        memcpy(&cb[20], &lastPreInten_[0], 16);

        if (hasDepth) {
            // Mode 5: store current frame's nearness into ilDepth_[depCur] before mode 0 / mode 4 read it
            cb[6] = 5u;
            ID3D12Resource* srvs5[22] = {};
            srvs5[12] = depthTex;
            ID3D12Resource* uavs5[10] = {};
            uavs5[4] = ilDepth_[depCur].Get();
            Dispatch(cl, psoIl_.Get(), srvs5, 13, uavs5, 5, cb, gx, gy);
            Barrier(cl, ilDepth_[depCur].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cb[6] = 0u;
        }

        // Mode 0: warp slots + write BaseCur & Trust & FastIn
        ID3D12Resource* srvs0[22] = {};
        srvs0[0]  = srcColor;
        srvs0[1]  = ilBase_[ilBcur_].Get();
        srvs0[2]  = ilInc_[ilCur_].Get();
        srvs0[3]  = mvTex ? mvTex : srcColor;
        srvs0[12] = hasDepth ? depthTex : srcColor;
        srvs0[13] = ilDepth_[depCur].Get();
        srvs0[14] = ilDepth_[ilDcur_].Get();
        ID3D12Resource* uavs0[10] = {
            fastIn_.Get(), ilInc_[incNext].Get(), ilBase_[baseNext].Get(), ilTrust_.Get()
        };
        Dispatch(cl, psoIl_.Get(), srvs0, hasDepth ? 15u : 4u, uavs0, 4, cb, gx, gy);

        uint32_t latestInc = incNext;
        if (isMoving) {
            // Build 6-level Push-Pull pyramid + fill holes (mode 4: reads incNext -> writes ilCur_)
            Barrier(cl, ilTrust_.Get(),        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, ilInc_[incNext].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, ilInc_[ilCur_].Get(),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            uint32_t sw = w_, sh = h_;
            for (int l = 0; l < 6; ++l) {
                uint32_t dw = (sw + 1u) / 2u, dh = (sh + 1u) / 2u;
                Barrier(cl, ilPyr_[l].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (hasDepth) Barrier(cl, ilDPyr_[l].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                UINT pcb[32]{};
                pcb[0] = sw; pcb[1] = sh; pcb[2] = dw; pcb[3] = dh;
                pcb[4] = (l == 0) ? 0u : 1u;
                pcb[5] = N;
                pcb[6] = hasDepth ? 1u : 0u;
                ID3D12Resource* ps[22] = {};
                ps[0] = ilTrust_.Get();
                ps[1] = ilInc_[incNext].Get();
                ps[2] = (l == 0) ? ilInc_[incNext].Get() : ilPyr_[l - 1].Get();
                ps[3] = ilDepth_[depCur].Get();
                ps[4] = (l == 0) ? ilDepth_[depCur].Get() : ilDPyr_[l - 1].Get();
                ID3D12Resource* pu[10] = { ilPyr_[l].Get(), hasDepth ? ilDPyr_[l].Get() : nullptr };
                Dispatch(cl, psoIlPp_.Get(), ps, hasDepth ? 5u : 3u, pu, hasDepth ? 2u : 1u, pcb, (dw * N + 15u) / 16u, (dh + 15u) / 16u);
                Barrier(cl, ilPyr_[l].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (hasDepth) Barrier(cl, ilDPyr_[l].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                sw = dw; sh = dh;
            }

            // Fill holes & compose slot input into fastIn_ (pure SRV incNext -> pure UAV ilCur_)
            cb[6] = 4u; // mode 4
            cb[9] = baseFlags | 64u;
            ID3D12Resource* srvs4[22] = {};
            srvs4[0] = srcColor; srvs4[1] = ilBase_[ilBcur_].Get(); srvs4[2] = ilInc_[incNext].Get();
            srvs4[3] = mvTex ? mvTex : srcColor; srvs4[4] = srcColor; srvs4[5] = ilTrust_.Get();
            for (int l = 0; l < 6; ++l) srvs4[6 + l] = ilPyr_[l].Get();
            if (hasDepth) {
                srvs4[12] = depthTex;
                srvs4[13] = ilDepth_[depCur].Get();
                srvs4[14] = ilDepth_[ilDcur_].Get();
                for (int l = 0; l < 6; ++l) srvs4[15 + l] = ilDPyr_[l].Get();
            }
            ID3D12Resource* uavs4[10] = { fastIn_.Get(), ilInc_[ilCur_].Get(), ilBase_[baseNext].Get(), ilTrust_.Get() };
            Dispatch(cl, psoIl_.Get(), srvs4, hasDepth ? 21u : 12u, uavs4, 4, cb, gx, gy);

            for (int l = 0; l < 6; ++l) {
                Barrier(cl, ilPyr_[l].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                if (hasDepth) Barrier(cl, ilDPyr_[l].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
            Barrier(cl, ilTrust_.Get(),        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, ilInc_[incNext].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, ilInc_[ilCur_].Get(),  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            latestInc = ilCur_;
            lastPreFlags_ = baseFlags | 64u;
        } else {
            Barrier(cl, ilTrust_.Get(),        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, ilInc_[incNext].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, ilInc_[ilCur_].Get(),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            lastPreFlags_ = baseFlags;
        }

        if (hasDepth) {
            Barrier(cl, ilDepth_[depCur].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, ilDepth_[ilDcur_].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, depthTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depthState);
            ilDcur_ = depCur;
            ilDepthValid_ = true;
        }
        Barrier(cl, fastIn_.Get(),           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        fastInState_ = D3D12_RESOURCE_STATE_COMMON;
        Barrier(cl, ilBase_[baseNext].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, ilBase_[ilBcur_].Get(),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (mvTex) Barrier(cl, mvTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mvState);
        Barrier(cl, srcColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, srcState);

        // Advance active ping-pong buffers (post will read ilInc_[ilCur_] -> write ilInc_[ilCur_^1])
        ilBcur_ = baseNext;
        ilCur_  = latestInc;
        didPreThisFrame_ = true;
        return fastIn_.Get();
    }

    // Recorded AFTER NgxEval submission, BEFORE A-form readback / B-form BformBCompose.
    // Input `srcColor`: original clean input (W.color / W.colorFp16); `netOut`: po (in UNORDERED_ACCESS).
    // Leaves fastOut_ in D3D12_RESOURCE_STATE_COMMON, restores srcColor & netOut to their original states.
    bool RecordPost(ID3D12GraphicsCommandList* cl,
                    ID3D12Resource* srcColor, D3D12_RESOURCE_STATES srcState,
                    ID3D12Resource* netOut,   D3D12_RESOURCE_STATES netOutState,
                    const Settings& s, bool alsoMake8Bit) {
        if (!ready_ || !didPreThisFrame_ || !cl || !srcColor || !netOut) return false;
        const int eff = lastPreMethod_;
        if (eff < 0) return false;

        const uint32_t gx = (w_ + 15u) / 16u, gy = (h_ + 15u) / 16u;
        const uint32_t ldx = (lowW_ + 15u) / 16u, ldy = (lowH_ + 15u) / 16u;

        if (eff == kEditStrength) {
            ID3D12Resource* fedTex = fedFastIn8To16_ ? fastIn_.Get() : srcColor;
            D3D12_RESOURCE_STATES fedState = fedFastIn8To16_ ? fastInState_ : srcState;
            Barrier(cl, fedTex, fedState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, netOut, netOutState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, fastOut_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            UINT cb[32]{};
            cb[0] = w_; cb[1] = h_; cb[2] = lowW_; cb[3] = lowH_;
            cb[4] = 10u; cb[5] = 0u;
            float k = std::clamp(s.editStrength, 0.0f, 4.0f);
            memcpy(&cb[16], &k, 4);

            ID3D12Resource* srvs[12] = { fedTex, fbAdd_.Get(), G_[0].Get(), G_[1].Get(), netOut, fineH_.Get() };
            ID3D12Resource* uavs[10] = { fastOut_.Get(), D_[0].Get(), D_[1].Get() };
            Dispatch(cl, psoApply_.Get(), srvs, 6, uavs, 3, cb, gx, gy);

            Barrier(cl, fastOut_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, netOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, netOutState);
            Barrier(cl, fedTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, fedFastIn8To16_ ? D3D12_RESOURCE_STATE_COMMON : srcState);
            if (fedFastIn8To16_) fastInState_ = D3D12_RESOURCE_STATE_COMMON;
            didPreThisFrame_ = false;
            fedFastIn8To16_ = false;
            if (alsoMake8Bit) ConvertTo8(cl);
            return true;
        }

        if (eff == kExtrapolation || eff == kFeedback || eff == kHybrid) {
            const bool fb = (eff == kFeedback || eff == kHybrid) && lastHasMv_;
            const bool useFastIn = fb || fedFastIn8To16_;
            const Strength st = ComputeStrength(s.passes);
            ID3D12Resource* fedTex = useFastIn ? fastIn_.Get() : srcColor;
            D3D12_RESOURCE_STATES fedState = useFastIn ? fastInState_ : srcState;

            Barrier(cl, fedTex, fedState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, netOut, netOutState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, fineH_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cl, D_[0].Get(),  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cl, D_[1].Get(),  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            // 1. st_prep
            UINT prepCb[32]{};
            prepCb[0] = w_; prepCb[1] = h_; prepCb[2] = lowW_; prepCb[3] = lowH_;
            prepCb[4] = fb ? 1u : 0u;
            ID3D12Resource* ps[12] = { fedTex, netOut };
            ID3D12Resource* pu[10] = { fineH_.Get(), D_[0].Get(), D_[1].Get() };
            Dispatch(cl, psoPrep_.Get(), ps, 2, pu, 3, prepCb, gx, gy);

            Barrier(cl, fineH_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, D_[0].Get(),  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, D_[1].Get(),  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            // 2. guided filter box 1 (D -> T, coef=1) & box 2 (T -> G, coef=0)
            const uint32_t rad = fb ? 2u : 4u;
            const float    eps = fb ? 1e9f : 1e-3f;
            Barrier(cl, T_[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cl, T_[1].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            UINT boxCb[32]{};
            boxCb[0] = lowW_; boxCb[1] = lowH_; boxCb[2] = rad; boxCb[3] = 1u;
            memcpy(&boxCb[16], &eps, 4);
            ID3D12Resource* bs1[12] = { D_[0].Get(), D_[1].Get() };
            ID3D12Resource* bu1[10] = { T_[0].Get(), T_[1].Get() };
            Dispatch(cl, psoBox_.Get(), bs1, 2, bu1, 2, boxCb, ldx, ldy);

            Barrier(cl, T_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, T_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, G_[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cl, G_[1].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            boxCb[3] = 0u;
            ID3D12Resource* bs2[12] = { T_[0].Get(), T_[1].Get() };
            ID3D12Resource* bu2[10] = { G_[0].Get(), G_[1].Get() };
            Dispatch(cl, psoBox_.Get(), bs2, 2, bu2, 2, boxCb, ldx, ldy);

            Barrier(cl, G_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, G_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, T_[0].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, T_[1].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, D_[0].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, D_[1].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

            // 3. st_apply
            const uint32_t nextHist = histCur_ ^ 1u;
            if (fb) {
                Barrier(cl, fbAdd_.Get(),      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cl, D_[0].Get(),       D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Barrier(cl, D_[1].Get(),       D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Barrier(cl, fastOut_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            const uint32_t applyMode = fb ? ((eff == kHybrid) ? 8u : 6u) : 9u;
            const float kVal    = (eff == kFeedback) ? (st.n - 1.0f) : st.k;
            const float haloVal = fb ? (st.n - 1.0f) : (s.haloLimit ? 0.0f : 1.0f);
            float fineVal = fb ? 1.0f : 2.0f;
            float haloAdjusted = haloVal;
            float kAdjusted = kVal;
            if (std::fabs(s.fine - 1.0f) > 1e-4f) {
                if (!fb) fineVal *= s.fine;
                else if (eff == kFeedback) kAdjusted *= s.fine;
                else { haloAdjusted *= s.fine; fineVal = s.fine; }
            }
            float apf[16] = {
                kAdjusted, haloAdjusted, fb ? 0.6f : 0.3f, 0.5f,
                fineVal,   1.0f,         0.3f,           0.5f,
                fb ? 0.0f : 0.6f, 2.0f,  fb ? 0.0f : 2.5f, fb ? (s.haloLimit ? 1.0f : 0.5f) : 0.0f,
                fb ? -0.5f : 0.0f, fb ? 0.45f : 0.0f, 0.0f, 0.0f
            };
            UINT appCb[32]{};
            appCb[0] = w_; appCb[1] = h_; appCb[2] = lowW_; appCb[3] = lowH_;
            appCb[4] = applyMode; appCb[5] = fb ? 1u : 0u;
            memcpy(&appCb[16], apf, sizeof(apf));

            ID3D12Resource* as[12] = { fedTex, fbAdd_.Get(), G_[0].Get(), G_[1].Get(), netOut, fineH_.Get() };
            ID3D12Resource* au[10] = { fastOut_.Get(), D_[0].Get(), D_[1].Get() };
            Dispatch(cl, psoApply_.Get(), as, 6, au, 3, appCb, gx, gy);

            Barrier(cl, fastOut_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, G_[0].Get(),    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, G_[1].Get(),    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, fineH_.Get(),   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, netOut,         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, netOutState);
            Barrier(cl, fedTex,         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, useFastIn ? D3D12_RESOURCE_STATE_COMMON : fedState);
            if (useFastIn) fastInState_ = D3D12_RESOURCE_STATE_COMMON;
            if (fb) Barrier(cl, fbAdd_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

            if (fb) {
                // 4. Filter restored cell means D -> T -> F[nextHist]
                Barrier(cl, D_[0].Get(),       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cl, D_[1].Get(),       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cl, T_[0].Get(),       D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Barrier(cl, T_[1].Get(),       D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                boxCb[2] = 2u; boxCb[3] = 1u;
                ID3D12Resource* hs1[12] = { D_[0].Get(), D_[1].Get() };
                ID3D12Resource* hu1[10] = { T_[0].Get(), T_[1].Get() };
                Dispatch(cl, psoBox_.Get(), hs1, 2, hu1, 2, boxCb, ldx, ldy);

                Barrier(cl, T_[0].Get(),       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cl, T_[1].Get(),       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cl, F0_[nextHist].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Barrier(cl, F1_[nextHist].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                boxCb[3] = 0u;
                ID3D12Resource* hs2[12] = { T_[0].Get(), T_[1].Get() };
                ID3D12Resource* hu2[10] = { F0_[nextHist].Get(), F1_[nextHist].Get() };
                Dispatch(cl, psoBox_.Get(), hs2, 2, hu2, 2, boxCb, ldx, ldy);

                Barrier(cl, F0_[nextHist].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, F1_[nextHist].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, T_[0].Get(),       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, T_[1].Get(),       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, D_[0].Get(),       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, D_[1].Get(),       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                histCur_ = nextHist;
                histValid_ = true;
            }
            didPreThisFrame_ = false;
            fedFastIn8To16_ = false;
            if (alsoMake8Bit) ConvertTo8(cl);
            return true;
        }

        // eff == kInterleaved (post mode 1: pure SRV ilInc_[ilCur_] -> pure UAV ilInc_[ilNext])
        const uint32_t N = ilSlotsBuilt_ ? ilSlotsBuilt_ : (uint32_t)std::clamp(s.ilPasses, 2, 8);
        const uint32_t ilNext = ilCur_ ^ 1u;
        Barrier(cl, ilBase_[ilBcur_].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, fastIn_.Get(),          fastInState_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, netOut,                 netOutState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, ilTrust_.Get(),         D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, ilInc_[ilCur_].Get(),   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, ilInc_[ilNext].Get(),   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cl, fastOut_.Get(),         D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        UINT cb[32]{};
        cb[0] = w_; cb[1] = h_; cb[2] = 0; cb[3] = 0;
        cb[4] = w_; cb[5] = h_; cb[6] = 1u; cb[7] = lastPreSlot_;
        cb[8] = N;  cb[9] = lastPreFlags_; cb[10] = lastPreStyles_; cb[11] = ilBcur_;
        cb[12] = ilNext; cb[13] = ilWarmMask_;
        float k0 = 0.02f, k1 = 0.06f, mvx = 1.0f, mvy = 1.0f;
        memcpy(&cb[16], &mvx, 4); memcpy(&cb[17], &mvy, 4);
        memcpy(&cb[18], &k0, 4);  memcpy(&cb[19], &k1, 4);
        memcpy(&cb[20], &lastPreInten_[0], 16);

        ID3D12Resource* srvs[12] = {
            ilBase_[ilBcur_].Get(), fastIn_.Get(), ilInc_[ilCur_].Get(), srcColor, netOut, ilTrust_.Get()
        };
        ID3D12Resource* uavs[10] = { fastOut_.Get(), ilInc_[ilNext].Get() };
        Dispatch(cl, psoIl_.Get(), srvs, 6, uavs, 2, cb, gx, gy);

        Barrier(cl, fastOut_.Get(),         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, ilInc_[ilNext].Get(),   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, ilInc_[ilCur_].Get(),   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, ilTrust_.Get(),         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, netOut,                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, netOutState);
        Barrier(cl, fastIn_.Get(),          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        fastInState_ = D3D12_RESOURCE_STATE_COMMON;
        Barrier(cl, ilBase_[ilBcur_].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

        ilCur_ = ilNext;
        ilWarmMask_ |= (1u << lastPreSlot_);
        ++ilFrame_;
        if (ilWarmMask_ == ((1u << N) - 1u)) ilValid_ = true;
        didPreThisFrame_ = false;
        fedFastIn8To16_ = false;

        if (alsoMake8Bit) ConvertTo8(cl);
        return true;
    }

private:
    static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        if (!cl || !r || from == to) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.pResource = r;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cl->ResourceBarrier(1, &b);
    }

    static bool CreateTex2D(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT16 arraySize, bool shared,
                            bool allowUav, ComPtr<ID3D12Resource>& out, const char* name, std::string& err) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = arraySize; rd.MipLevels = 1;
        rd.Format = fmt; rd.SampleDesc.Count = 1;
        rd.Flags = allowUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = dev->CreateCommittedResource(&hp, shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE,
                                                  &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(out.GetAddressOf()));
        if (FAILED(hr) || !out) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "CreateTex2D(%s %ux%ux%u fmt=%d) hr=0x%08X", name, w, h, (unsigned)arraySize, (int)fmt, (unsigned)hr);
            err = buf;
            return false;
        }
        return true;
    }

    bool CompileCS(const std::string& src, const char* tag, ComPtr<ID3D12PipelineState>& outPso, std::string& err) {
        if (outPso) return true;
        ComPtr<ID3DBlob> cs, e;
        HRESULT hr = D3DCompile(src.data(), src.size(), tag, nullptr, nullptr, "main", "cs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e);
        if (FAILED(hr) || !cs) {
            err = std::string(tag) + ": " + (e ? std::string((const char*)e->GetBufferPointer(), e->GetBufferSize()) : "D3DCompile failed");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs_.Get();
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = dev_->CreateComputePipelineState(&pd, IID_PPV_ARGS(outPso.GetAddressOf()));
        if (FAILED(hr) || !outPso) {
            char b[128]; std::snprintf(b, sizeof(b), "%s CreateComputePipelineState hr=0x%08X", tag, (unsigned)hr);
            err = b;
            return false;
        }
        return true;
    }

    bool BuildRootSigAndHeap(std::string& err) {
        D3D12_DESCRIPTOR_RANGE rgS{};
        rgS.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rgS.NumDescriptors = kSrvCount;
        rgS.BaseShaderRegister = 0;
        D3D12_DESCRIPTOR_RANGE rgU{};
        rgU.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        rgU.NumDescriptors = kUavCount;
        rgU.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER rp[3]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1;
        rp[0].DescriptorTable.pDescriptorRanges = &rgS;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &rgU;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[2].Constants.ShaderRegister = 0;
        rp[2].Constants.Num32BitValues = 32;

        D3D12_STATIC_SAMPLER_DESC smp{};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.ShaderRegister = 0;
        smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3;
        rsd.pParameters = rp;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &smp;

        ComPtr<ID3DBlob> sig, e;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &e);
        if (FAILED(hr) || !sig) {
            err = std::string("RootSig serialize: ") + (e ? (const char*)e->GetBufferPointer() : "?");
            return false;
        }
        hr = dev_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(rs_.GetAddressOf()));
        if (FAILED(hr)) { err = "CreateRootSignature failed"; return false; }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kTotalPages * kPageStrides;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heap_.GetAddressOf()));
        if (FAILED(hr)) { err = "CreateDescriptorHeap failed"; return false; }
        inc_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    bool CompileCorePipelines(std::string& err) {
        const std::string mathStr(kFastMathHlsl);
        if (!CompileCS(mathStr + kFastFbInHlsl, "fast_fbin.hlsl", psoFbIn_, err)) return false;
        if (!CompileCS(mathStr + kFastPrepHlsl, "fast_prep.hlsl", psoPrep_, err)) return false;
        if (!CompileCS(kFastBoxHlsl,            "fast_box.hlsl",  psoBox_,  err)) return false;
        if (!CompileCS(mathStr + kFastApplyHlsl,"fast_apply.hlsl",psoApply_,err)) return false;
        if (!CompileCS(kFastTo8Hlsl,            "fast_to8.hlsl",  psoTo8_,  err)) return false;
        return true;
    }

    bool EnsureBaseResources(std::string& err) {
        if (!fastIn_   && !CreateTex2D(dev_.Get(), w_, h_, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, true,  true, fastIn_,   "fastIn",   err)) return false;
        if (!fastOut_  && !CreateTex2D(dev_.Get(), w_, h_, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, false, true, fastOut_,  "fastOut",  err)) return false;
        if (!fastOut8_ && !CreateTex2D(dev_.Get(), w_, h_, DXGI_FORMAT_R8G8B8A8_UNORM,     1, false, true, fastOut8_, "fastOut8", err)) return false;
        if (!fbAdd_    && !CreateTex2D(dev_.Get(), w_, h_, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, false, true, fbAdd_,    "fbAdd",    err)) return false;
        if (!fineH_    && !CreateTex2D(dev_.Get(), w_, h_, DXGI_FORMAT_R16G16_FLOAT,       1, false, true, fineH_,    "fineH",    err)) return false;
        for (int i = 0; i < 2; ++i) {
            if (!D_[i]  && !CreateTex2D(dev_.Get(), lowW_, lowH_, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, false, true, D_[i],  "gfD",  err)) return false;
            if (!T_[i]  && !CreateTex2D(dev_.Get(), lowW_, lowH_, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, false, true, T_[i],  "gfT",  err)) return false;
            if (!G_[i]  && !CreateTex2D(dev_.Get(), lowW_, lowH_, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, false, true, G_[i],  "gfG",  err)) return false;
            if (!F0_[i] && !CreateTex2D(dev_.Get(), lowW_, lowH_, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, false, true, F0_[i], "gfF0", err)) return false;
            if (!F1_[i] && !CreateTex2D(dev_.Get(), lowW_, lowH_, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, false, true, F1_[i], "gfF1", err)) return false;
        }
        return true;
    }

    bool EnsureInterleavedPipelinesAndResources(std::string& err) {
        const std::string ilSrc = std::string(kFastIlGradeHlsl) + kFastIlHlsl;
        if (!CompileCS(ilSrc,         "fast_il.hlsl",   psoIl_,   err)) return false;
        if (!CompileCS(kFastIlPpHlsl, "fast_ilpp.hlsl", psoIlPp_, err)) return false;

        for (int i = 0; i < 2; ++i) {
            if (!ilBase_[i] && !CreateTex2D(dev_.Get(), w_, h_, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, false, true, ilBase_[i], "ilBase", err)) return false;
            if (!ilInc_[i]  && !CreateTex2D(dev_.Get(), w_, h_, DXGI_FORMAT_R16G16B16A16_FLOAT, (UINT16)kMaxIlSlots, false, true, ilInc_[i], "ilInc", err)) return false;
            if (!ilDepth_[i] && !CreateTex2D(dev_.Get(), w_, h_, DXGI_FORMAT_R32_FLOAT, 1, false, true, ilDepth_[i], "ilDepth", err)) return false;
        }
        if (!ilTrust_ && !CreateTex2D(dev_.Get(), w_, h_, DXGI_FORMAT_R16_FLOAT, 1, false, true, ilTrust_, "ilTrust", err)) return false;
        UINT sw = w_, sh = h_;
        for (int l = 0; l < 6; ++l) {
            sw = (sw + 1u) / 2u; sh = (sh + 1u) / 2u;
            if (!ilPyr_[l]  && !CreateTex2D(dev_.Get(), sw * kMaxIlSlots, sh, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, false, true, ilPyr_[l],  "ilPyr",  err)) return false;
            if (!ilDPyr_[l] && !CreateTex2D(dev_.Get(), sw, sh, DXGI_FORMAT_R32G32_FLOAT, 1, false, true, ilDPyr_[l], "ilDPyr", err)) return false;
        }
        return true;
    }

    void ConvertTo8(ID3D12GraphicsCommandList* cl) {
        Barrier(cl, fastOut_.Get(),  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, fastOut8_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        UINT cb[32]{};
        cb[0] = w_; cb[1] = h_;
        ID3D12Resource* srvs[12] = { fastOut_.Get() };
        ID3D12Resource* uavs[10] = { fastOut8_.Get() };
        Dispatch(cl, psoTo8_.Get(), srvs, 1, uavs, 1, cb, (w_ + 15u) / 16u, (h_ + 15u) / 16u);
        Barrier(cl, fastOut8_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, fastOut_.Get(),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    static DXGI_FORMAT UnwrapTypeless(DXGI_FORMAT fmt) {
        switch (fmt) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:             return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:  return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:     return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:             return DXGI_FORMAT_R16_UNORM;
        default: return fmt;
        }
    }

    void Dispatch(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso,
                  ID3D12Resource* const* srvs, UINT srvN,
                  ID3D12Resource* const* uavs, UINT uavN,
                  const UINT (&cb)[32], UINT gx, UINT gy) {
        const UINT basePage = (frameParity_ * kPagesPerFrm) + (pageCursor_ % kPagesPerFrm);
        ++pageCursor_;
        const SIZE_T baseIdx = (SIZE_T)basePage * kPageStrides;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = heap_->GetGPUDescriptorHandleForHeapStart();

        for (UINT i = 0; i < kSrvCount; ++i) {
            ID3D12Resource* r = (i < srvN) ? srvs[i] : nullptr;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (r) {
                D3D12_RESOURCE_DESC rd = r->GetDesc();
                sd.Format = UnwrapTypeless(rd.Format);
                if (rd.DepthOrArraySize > 1) {
                    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    sd.Texture2DArray.MipLevels = 1;
                    sd.Texture2DArray.ArraySize = rd.DepthOrArraySize;
                } else {
                    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    sd.Texture2D.MipLevels = 1;
                }
            } else {
                sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE ch = cpuStart;
            ch.ptr += (baseIdx + i) * inc_;
            dev_->CreateShaderResourceView(r, &sd, ch);
        }

        for (UINT i = 0; i < kUavCount; ++i) {
            ID3D12Resource* r = (i < uavN) ? uavs[i] : nullptr;
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            if (r) {
                D3D12_RESOURCE_DESC rd = r->GetDesc();
                ud.Format = UnwrapTypeless(rd.Format);
                if (rd.DepthOrArraySize > 1) {
                    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    ud.Texture2DArray.ArraySize = rd.DepthOrArraySize;
                } else {
                    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                }
            } else {
                ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE ch = cpuStart;
            ch.ptr += (baseIdx + kSrvCount + i) * inc_;
            dev_->CreateUnorderedAccessView(r, nullptr, &ud, ch);
        }

        ID3D12DescriptorHeap* hs[] = { heap_.Get() };
        cl->SetDescriptorHeaps(1, hs);
        cl->SetComputeRootSignature(rs_.Get());
        cl->SetPipelineState(pso);

        D3D12_GPU_DESCRIPTOR_HANDLE gs = gpuStart;
        gs.ptr += baseIdx * inc_;
        D3D12_GPU_DESCRIPTOR_HANDLE gu = gpuStart;
        gu.ptr += (baseIdx + kSrvCount) * inc_;

        cl->SetComputeRootDescriptorTable(0, gs);
        cl->SetComputeRootDescriptorTable(1, gu);
        cl->SetComputeRoot32BitConstants(2, 32, cb, 0);
        cl->Dispatch(gx, gy, 1);
    }

    bool ready_  = false;
    bool failed_ = false;
    UINT w_ = 0, h_ = 0, lowW_ = 0, lowH_ = 0;
    UINT inc_ = 0;
    UINT frameParity_ = 0;
    UINT pageCursor_  = 0;

    bool     histValid_ = false;
    uint32_t histCur_   = 0;
    int      lastPreMethod_ = -1;
    bool     lastHasMv_     = false;
    bool     didPreThisFrame_ = false;
    bool     fedFastIn8To16_  = false;
    D3D12_RESOURCE_STATES fastInState_ = D3D12_RESOURCE_STATE_COMMON;

    int   lastSettingsMethod_ = -1;
    float lastPasses_         = 3.0f;
    bool  lastHalo_           = false;
    float lastFine_           = 1.0f;

    bool     ilValid_      = false;
    bool     ilDepthValid_ = false;
    bool     ilGradedLast_ = false;
    uint32_t ilCur_        = 0;
    uint32_t ilBcur_       = 0;
    uint32_t ilDcur_       = 0;
    uint32_t ilFrame_      = 0;
    uint32_t ilWarmMask_   = 0;
    uint32_t ilSlotsBuilt_ = 0;
    uint32_t lastPreSlot_  = 0;
    uint32_t lastPreFlags_ = 0;
    uint32_t lastPreStyles_= 0;
    float    lastPreInten_[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    ComPtr<ID3D12Device>         dev_;
    ComPtr<ID3D12RootSignature>  rs_;
    ComPtr<ID3D12DescriptorHeap> heap_;

    ComPtr<ID3D12PipelineState>  psoFbIn_, psoPrep_, psoBox_, psoApply_, psoIl_, psoIlPp_, psoTo8_;

    ComPtr<ID3D12Resource> fastIn_, fastOut_, fastOut8_, fbAdd_, fineH_;
    HANDLE fastInShared_ = nullptr;
    ComPtr<ID3D12Resource> D_[2], T_[2], G_[2], F0_[2], F1_[2];
    ComPtr<ID3D12Resource> ilBase_[2], ilInc_[2], ilDepth_[2], ilTrust_, ilPyr_[6], ilDPyr_[6];
};

} // namespace FastPasses
