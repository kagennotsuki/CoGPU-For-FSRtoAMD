// Add4K —— 残差抬到 4K 后叠加（本文件可热重编译：保存后约 1 秒生效）
// 绑定固定，不能改：
//   t0 = 4K 底图(outp)   t1 = 合成后的 pColor(= 槽位原图 + e)   t2 = 槽位原图
//   u0 = 输出(4K)        s0 = 采样器
// 注意：t1 / t2 是 pColor 几何（可能 2560x1440），尺寸必须用 GetDimensions 问，不要硬编码。

cbuffer P : register(b0) { float4 gC[5]; };

Texture2D<float4>   gSrc : register(t0);
Texture2D<float4>   gRes : register(t1);
Texture2D<float4>   gMV  : register(t2);
SamplerState        gSmp : register(s0);
RWTexture2D<float4> gDst : register(u0);

static const float kAddPqM1 = 2610.0 / 16384.0;
static const float kAddPqM2 = 2523.0 / 32.0;
static const float kAddPqC1 = 3424.0 / 4096.0;
static const float kAddPqC2 = 2413.0 / 128.0;
static const float kAddPqC3 = 2392.0 / 128.0;

float3 AddCleanRgb(float3 c) {
    if ((asuint(c.r) & 0x7F800000u) == 0x7F800000u) c.r = 0.0;
    if ((asuint(c.g) & 0x7F800000u) == 0x7F800000u) c.g = 0.0;
    if ((asuint(c.b) & 0x7F800000u) == 0x7F800000u) c.b = 0.0;
    return c;
}
float3 AddPQToLin(float3 pq) {
    float3 s = saturate(AddCleanRgb(pq));
    float3 p = pow(max(s, 1e-7), 1.0 / kAddPqM2);
    float3 num = max(p - kAddPqC1, 0.0);
    float3 den = max(kAddPqC2 - kAddPqC3 * p, 1e-6);
    float3 res = pow(max(num / den, 1e-7), 1.0 / kAddPqM1);
    return lerp(float3(0.0, 0.0, 0.0), res, step(1e-6, num) * step(1e-7, s));
}
float3 AddLinToPQ(float3 linNorm) {
    float3 x = max(AddCleanRgb(linNorm), 0.0);
    float3 lm = pow(max(x, 1e-7), kAddPqM1);
    float3 pq = pow(max((kAddPqC1 + kAddPqC2 * lm) / max(1.0 + kAddPqC3 * lm, 1e-6), 1e-7), kAddPqM2);
    return lerp(float3(0.0, 0.0, 0.0), saturate(pq), step(1e-7, x));
}
float3 AddBt2020To709(float3 c) {
    return float3(
        dot(c, float3( 1.6604910, -0.5876411, -0.0728499)),
        dot(c, float3(-0.1245505,  1.1328999, -0.0083494)),
        dot(c, float3(-0.0181508, -0.1005789,  1.1187297))
    );
}
float3 AddBt709To2020(float3 c) {
    return float3(
        dot(c, float3(0.6274040, 0.3292830, 0.0433130)),
        dot(c, float3(0.0690970, 0.9195400, 0.0113620)),
        dot(c, float3(0.0163914, 0.0880133, 0.8955950))
    );
}
float3 AddApplyDelta(float3 o, float3 pre, float3 col) {
    o = AddCleanRgb(o);
    pre = AddCleanRgb(pre);
    col = AddCleanRgb(col);
    int hdrMode = (gC[4].w > 0.5) ? (int)(gC[0].w + 0.5) : 0;
    if (hdrMode == 1) {
        o = clamp(o, -65504.0, 65504.0);
        float3 oPos = max(o, 0.0);
        float3 prePos = max(pre, 0.0);
        float3 colPos = max(col, 0.0);
        float3 dAdd = prePos - colPos;
        float3 dRat = oPos * clamp(prePos / max(colPos, 1e-4) - 1.0, -0.5, 1.0);
        float  trust = smoothstep(0.05, 0.25, min(dot(oPos, float3(0.2126, 0.7152, 0.0722)),
                                                  dot(colPos, float3(0.2126, 0.7152, 0.0722))));
        float3 e = lerp(dAdd, dRat, trust);
        return clamp(o + e, min(o, 0.0), 65504.0);
    }
    if (hdrMode == 2) {
        float3 oLin = max(AddBt2020To709(AddPQToLin(o)), 0.0);
        float3 pLin = max(AddBt2020To709(AddPQToLin(pre)), 0.0);
        float3 cLin = max(AddBt2020To709(AddPQToLin(col)), 0.0);
        float3 dAdd = pLin - cLin;
        float3 dRat = oLin * clamp(pLin / max(cLin, 1e-5) - 1.0, -0.5, 1.0);
        float  trust = smoothstep(0.002, 0.02, min(dot(oLin, float3(0.2126, 0.7152, 0.0722)),
                                                   dot(cLin, float3(0.2126, 0.7152, 0.0722))));
        float3 res = max(oLin + lerp(dAdd, dRat, trust), 0.0);
        return saturate(AddLinToPQ(max(AddBt709To2020(res), 0.0)));
    }
    return saturate(o + (pre - col));
}

// 当前使用：双线性抬升（最便宜，实测 +0.45ms）
[numthreads(8,8,1)]
void Add4K(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv  = (float2(id.xy) + 0.5) / float2(w, h);
    float4 o   = gSrc.Load(int3((int)id.x, (int)id.y, 0));
    float3 pre = gRes.SampleLevel(gSmp, uv, 0).rgb;
    float3 col = gMV.SampleLevel(gSmp, uv, 0).rgb;
    gDst[id.xy] = float4(AddApplyDelta(o.rgb, pre, col), o.a);
}

// 备选：引导式 JBU（边缘更稳，但 8 次散读，实测 +4ms / -12fps）
// 想用就把下面的函数改名成 Add4K，并删掉上面的 Add4K。
[numthreads(8,8,1)]
void Add4K_JBU(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint lw, lh; gRes.GetDimensions(lw, lh);
    float4 o  = gSrc.Load(int3((int)id.x, (int)id.y, 0));
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float2 lc = uv * float2(lw, lh) - 0.5;
    float2 fl = floor(lc);
    float2 wt = lc - fl;
    int2   i0 = int2(fl);
    int2   imax = int2((int)lw - 1, (int)lh - 1);
    float3 preSum = float3(0,0,0), colSum = float3(0,0,0);
    float  wsum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j) {
        [unroll] for (int i = 0; i < 2; ++i) {
            int2 q = clamp(i0 + int2(i, j), int2(0,0), imax);
            float4 pr = gRes.Load(int3(q, 0));
            float4 co = gMV.Load(int3(q, 0));
            float  ws = ((i == 0) ? (1.0 - wt.x) : wt.x) * ((j == 0) ? (1.0 - wt.y) : wt.y);
            float  cd = dot(abs(o.rgb - co.rgb), float3(0.3333, 0.3333, 0.3333));
            float  wc = saturate(1.0 - cd * 8.0);
            wc = wc * wc;
            preSum += pr.rgb * (ws * wc);
            colSum += co.rgb * (ws * wc);
            wsum += ws * wc;
        }
    }
    if (wsum > 1e-4) {
        gDst[id.xy] = float4(AddApplyDelta(o.rgb, preSum / wsum, colSum / wsum), o.a);
    } else {
        gDst[id.xy] = o;
    }
}
