
// Add4K —— 残差抬到 4K 后叠加（本文件可热重编译：保存后约 1 秒生效）
// 绑定固定，不能改：
//   t0 = 4K 底图(outp)   t1 = 合成后的 pColor(= 槽位原图 + e)   t2 = 槽位原图
//   u0 = 输出(4K)        s0 = 采样器
// 注意：t1 / t2 是 pColor 几何（可能 2560x1440），尺寸必须用 GetDimensions 问，不要硬编码。

Texture2D<float4>   gSrc : register(t0);
Texture2D<float4>   gRes : register(t1);
Texture2D<float4>   gMV  : register(t2);
SamplerState        gSmp : register(s0);
RWTexture2D<float4> gDst : register(u0);

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
    gDst[id.xy] = float4(saturate(o.rgb + (pre - col)), o.a);
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
    float3 esum = float3(0,0,0);
    float  wsum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j) {
        [unroll] for (int i = 0; i < 2; ++i) {
            int2 q = clamp(i0 + int2(i, j), int2(0,0), imax);
            float4 pr = gRes.Load(int3(q, 0));
            float4 co = gMV.Load(int3(q, 0));
            float3 e  = pr.rgb - co.rgb;
            float  ws = ((i == 0) ? (1.0 - wt.x) : wt.x) * ((j == 0) ? (1.0 - wt.y) : wt.y);
            float  cd = dot(abs(o.rgb - co.rgb), float3(0.3333, 0.3333, 0.3333));
            float  wc = saturate(1.0 - cd * 8.0);
            wc = wc * wc;
            esum += e * (ws * wc);
            wsum += ws * wc;
        }
    }
    float3 e4 = (wsum > 1e-4) ? (esum / wsum) : float3(0,0,0);
    gDst[id.xy] = float4(saturate(o.rgb + e4), o.a);
}
