// ffx_adapter_on_core.cpp —— FFX(FSR) 适配层：只做"取数 + 参数搬运"，其余全部复用 RE2 核心
//
// 照搬原则：
//   ffx_core_reuse.cpp（= libxess_proxy_v9.cpp，仅改 1 处 seam）负责：
//     工作线程 / K 槽 / 跨卡共享缓冲 / NR 运行时生命周期 / 答案交付 / 合成 / 菜单热键
//   本文件只负责三件事：
//     ① 导出 FFX 的 5 个函数，转调真 FSR（amd_fidelityfx_upscaler_orig.dll）
//     ② ffxDispatch 时把 FFX 的 desc 打包成核心认识的 params（q[0]=color q[1]=mv q[2]=depth q[5]=output）
//     ③ 让核心的"真派发"回到真 FSR（trampoline：核心只给 (ctx,list,params) 三个参数）
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstddef>
#include <cmath>
#include <limits>
#include <d3dcompiler.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#include "ffx_api_min.h"
#include "adapter_frame_desc.h"
static_assert(sizeof(ffxDispatchDescUpscale) <= sizeof(AdapterFrameDesc::nativeStorage),
              "ffxDispatchDescUpscale exceeds AdapterFrameDesc::nativeStorage");

// adapterMvScale 单独传入（无效时为 NaN），不改变 CoreParams 或真派发的布局/签名。
extern "C" int   ProxyExecute(void* ctx, void* cmdList, const void* params, float adapterMvScale,
                              const AdapterFrameDesc* adapterDesc);
extern "C" void* g_realDispatchFn;

typedef struct ffxAllocationCallbacks ffxAllocationCallbacks;

typedef ffxReturnCode_t (*PFN_Create)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
typedef void            (*PFN_Destroy)(ffxContext*, const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*PFN_Dispatch)(ffxContext*, const ffxDispatchDescHeader*);
typedef ffxReturnCode_t (*PFN_Query)(ffxContext*, ffxQueryDescHeader*);
typedef ffxReturnCode_t (*PFN_Configure)(ffxContext*, const ffxConfigureDescHeader*);

static char g_dir[MAX_PATH] = { 0 };

static void EnsureDir() {
    if (g_dir[0]) return;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&EnsureDir, &self);
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    char* s = strrchr(g_dir, '\\'); if (s) s[1] = 0;
}

static void Logf(const char* fmt, ...) {
    static FILE* f = nullptr;
    static bool init = false;
    if (!init) { init = true; EnsureDir(); char p[MAX_PATH]; lstrcpyA(p, g_dir); lstrcatA(p, "ffx_on_core.log"); fopen_s(&f, p, "w"); }
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fflush(f);
}

struct RealFfx {
    HMODULE mod = nullptr;
    PFN_Create Create = nullptr;
    PFN_Destroy Destroy = nullptr;
    PFN_Dispatch Dispatch = nullptr;
    PFN_Query Query = nullptr;
    PFN_Configure Configure = nullptr;
    bool ok = false;
};
static RealFfx g_real;

static bool LoadReal() {
    if (g_real.ok) return true;
    EnsureDir();
    char p[MAX_PATH]; lstrcpyA(p, g_dir); lstrcatA(p, "amd_fidelityfx_upscaler_orig.dll");
    g_real.mod = LoadLibraryA(p);
    if (!g_real.mod) { Logf("!! 加载真 FSR 失败 err=%lu path=%s", GetLastError(), p); return false; }
    g_real.Create    = (PFN_Create)GetProcAddress(g_real.mod, "ffxCreateContext");
    g_real.Destroy   = (PFN_Destroy)GetProcAddress(g_real.mod, "ffxDestroyContext");
    g_real.Dispatch  = (PFN_Dispatch)GetProcAddress(g_real.mod, "ffxDispatch");
    g_real.Query     = (PFN_Query)GetProcAddress(g_real.mod, "ffxQuery");
    g_real.Configure = (PFN_Configure)GetProcAddress(g_real.mod, "ffxConfigure");
    g_real.ok = (g_real.Create && g_real.Destroy && g_real.Dispatch);
    Logf("适配层: 真 FSR 已加载 %s (Create=%p Dispatch=%p Query=%p Configure=%p)",
         p, (void*)g_real.Create, (void*)g_real.Dispatch, (void*)g_real.Query, (void*)g_real.Configure);
    return g_real.ok;
}

// =====================================================================================
// 跑马灯帧标记（诊断用，默认关闭；ffx_on_core.ini 里写 marquee=1 才生效）
//   在游戏 color 上按帧相位涂一条 1/4 屏宽的黑竖条：位置 = 帧号 % 4
//   用途：① 标记画在数据里（不是屏幕上）② 逐帧变相位 → 能判"这是哪一帧的内容"
//     · NR 关（F10）：输出上黑条应在当期位置
//     · NR 开：若残差与底图/喂入错帧，黑条边缘会出现"另一相位的淡影"
// =====================================================================================
static const char* kMarqueeCs = R"(
RWTexture2D<float4> gDst : register(u0);
cbuffer P : register(b0) { uint gX0; uint gW; uint gH; uint gPad; };
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gW || id.y >= gH) return;
    gDst[uint2(gX0 + id.x, id.y)] = float4(0.0, 0.0, 0.0, 1.0);
}
)";

struct MarqueeCtx {
    ComPtr<ID3D12Device>       dev;
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT inc = 0;
    UINT64 frame = 0;
    bool ready = false, failed = false;
};
static MarqueeCtx g_mq;
static bool g_marqueeOn = false;
static bool g_marqueeIniRead = false;
static UINT g_marqueeFrom = 600;      // 前 600 次派发不动（2077 进场景后才真正开始超分）

static void MarqueeReadIni() {
    if (g_marqueeIniRead) return;
    g_marqueeIniRead = true;
    EnsureDir();
    char ini[MAX_PATH]; lstrcpyA(ini, g_dir); lstrcatA(ini, "ffx_on_core.ini");
    FILE* f = nullptr; char line[64];
    if (fopen_s(&f, ini, "r") == 0 && f) {
        while (fgets(line, sizeof(line), f)) {
            int v = 0;
            if (sscanf_s(line, "marquee=%d", &v) == 1) g_marqueeOn = (v != 0);
            if (sscanf_s(line, "marquee_from=%d", &v) == 1 && v >= 0) g_marqueeFrom = (UINT)v;
        }
        fclose(f);
    }
    Logf("跑马灯: %s（从第 %u 次派发起启用；2077 菜单阶段不碰）",
         g_marqueeOn ? "开启" : "关闭", g_marqueeFrom);
}

static bool MarqueeEnsure(ID3D12Device* dev) {
    if (g_mq.ready) return true;
    if (g_mq.failed) return false;
    g_mq.dev = dev;
    D3D12_DESCRIPTOR_RANGE r{};
    r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r.NumDescriptors = 1; r.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 1; p[0].DescriptorTable.pDescriptorRanges = &r;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.ShaderRegister = 0; p[1].Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2; rd.pParameters = p;
    ID3DBlob* sig = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        Logf("跑马灯: 根签名失败 %s", err ? (const char*)err->GetBufferPointer() : "?"); g_mq.failed = true; return false;
    }
    dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_mq.rs));
    sig->Release();
    ID3DBlob* cs = nullptr; ID3DBlob* e2 = nullptr;
    if (FAILED(D3DCompile(kMarqueeCs, strlen(kMarqueeCs), "mq", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2)) || !cs) {
        Logf("跑马灯: 编译失败 %s", e2 ? (const char*)e2->GetBufferPointer() : "?"); g_mq.failed = true; return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_mq.rs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer(); pd.CS.BytecodeLength = cs->GetBufferSize();
    HRESULT hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_mq.pso));
    cs->Release(); if (e2) e2->Release();
    if (FAILED(hr)) { Logf("跑马灯: PSO 0x%08X", (unsigned)hr); g_mq.failed = true; return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_mq.heap)))) { g_mq.failed = true; return false; }
    g_mq.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_mq.ready = true;
    Logf("跑马灯: 就绪");
    return true;
}

// 在 color 上画本期黑条（记进游戏的命令 list，状态转回原位）
static void MarqueeDraw(ID3D12Resource* color, ID3D12GraphicsCommandList* cl, UINT stateDeclared) {
    if (!g_marqueeOn || !color || !cl) return;
    if (g_mq.frame < g_marqueeFrom) { ++g_mq.frame; return; }   // 菜单/加载阶段不画
    ID3D12Device* dev = nullptr;
    if (FAILED(color->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) return;
    if (!MarqueeEnsure(dev)) { dev->Release(); return; }
    D3D12_RESOURCE_DESC cd = color->GetDesc();
    const UINT W = (UINT)cd.Width, H = cd.Height;
    const UINT barW = (W + 3) / 4;
    const UINT phase = (UINT)(g_mq.frame % 4);
    const UINT x0 = phase * barW;

    // ★ 必须传 pDesc=nullptr（用资源自身格式）。之前传了非空描述且 Format=UNKNOWN →
    //   非法视图 → 写入即设备移除（2077 第一帧就崩，已踩）
    if (!(cd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        static int skip = 0;
        if (skip++ < 3) Logf("跑马灯: 该帧 color 不支持 UAV（flags=0x%X），跳过", (unsigned)cd.Flags);
        dev->Release();
        return;
    }
    dev->CreateUnorderedAccessView(color, nullptr, nullptr, g_mq.heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = color;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = (D3D12_RESOURCE_STATES)stateDeclared;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &b);

    ID3D12DescriptorHeap* hs[] = { g_mq.heap.Get() };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(g_mq.rs.Get());
    cl->SetPipelineState(g_mq.pso.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_mq.heap->GetGPUDescriptorHandleForHeapStart();
    cl->SetComputeRootDescriptorTable(0, g);
    UINT k[4] = { x0, barW, H, 0 };
    cl->SetComputeRoot32BitConstants(1, 4, k, 0);
    cl->Dispatch((barW + 7) / 8, (H + 7) / 8, 1);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = (D3D12_RESOURCE_STATES)stateDeclared;
    cl->ResourceBarrier(1, &b);
    ++g_mq.frame;
    if ((g_mq.frame % 120) == 1)
        Logf("跑马灯: 第 %llu 帧 黑条 x=[%u,%u) / 宽 %u", (unsigned long long)g_mq.frame, x0, x0 + barW, W);
    dev->Release();
}
// 本帧 desc（给 trampoline 用；核心在同一调用栈里同步回调）
static thread_local const ffxDispatchDescUpscale* g_pending = nullptr;
static thread_local const ffxDispatchDescUpscale* g_a3Override = nullptr;

// Legacy callers still pass a 72-byte CoreParams packet. Only A3's typed bridge
// opts in to a complete replacement; it never reinterprets those legacy bytes.
static int RealDispatchTrampoline(void* ctx, void* cmdList, const void* params) {
    if (!g_real.Dispatch || !g_pending) return FFX_API_RETURN_ERROR;
    const auto* d = params && params == g_a3Override ? g_a3Override : g_pending;
    if (d == g_a3Override && (d->commandList != cmdList || cmdList != g_pending->commandList ||
        d->output.resource != g_pending->output.resource || d->header.pNext != g_pending->header.pNext))
        return FFX_API_RETURN_ERROR_PARAMETER;
    return (int)g_real.Dispatch((ffxContext*)ctx, &d->header);
}
static bool ToD3D12State(uint32_t ffx, D3D12_RESOURCE_STATES& out) {
    switch (ffx) {
    case FFX_API_RESOURCE_STATE_COMMON: out = D3D12_RESOURCE_STATE_COMMON; return true;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: out = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; return true;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ: out = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; return true;
    case FFX_API_RESOURCE_STATE_PIXEL_READ: out = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; return true;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        out = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; return true;
    case FFX_API_RESOURCE_STATE_COPY_SRC: out = D3D12_RESOURCE_STATE_COPY_SOURCE; return true;
    case FFX_API_RESOURCE_STATE_COPY_DEST: out = D3D12_RESOURCE_STATE_COPY_DEST; return true;
    case FFX_API_RESOURCE_STATE_GENERIC_READ: out = D3D12_RESOURCE_STATE_GENERIC_READ; return true;
    case FFX_API_RESOURCE_STATE_PRESENT: out = D3D12_RESOURCE_STATE_PRESENT; return true;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET: out = D3D12_RESOURCE_STATE_RENDER_TARGET; return true;
    case FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT: out = D3D12_RESOURCE_STATE_DEPTH_WRITE; return true;
    case FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT: out = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT; return true;
    default: out = D3D12_RESOURCE_STATE_COMMON; return false;
    }
}
static AdapterResource ToAdapterRes(const FfxApiResource& src) {
    AdapterResource r{};
    r.resource = src.resource;
    if (src.resource) r.valid = ToD3D12State(src.state, r.state);
    return r;
}

extern "C" int AdapterDispatchA3(void* ctx, const AdapterFrameDesc* d) {
    if (!g_real.Dispatch || !g_pending || !d || d->nativeSize != sizeof(ffxDispatchDescUpscale))
        return FFX_API_RETURN_ERROR_PARAMETER;
    ffxDispatchDescUpscale rebuilt{};
    memcpy(&rebuilt, d->nativeStorage, sizeof(rebuilt));
    rebuilt.header = g_pending->header;
    rebuilt.commandList = d->commandList;
    rebuilt.output = g_pending->output;
    auto patchRes = [](FfxApiResource& dst, const AdapterResource& src, bool isComposed) {
        if (src.resource) {
            dst.resource = src.resource;
            dst.state = FFX_API_RESOURCE_STATE_COMMON;
            if (isComposed) dst.description.usage |= FFX_API_RESOURCE_USAGE_UAV;
        } else {
            dst.resource = nullptr;
        }
    };
    patchRes(rebuilt.color, d->color, d->colorComposed);
    patchRes(rebuilt.motionVectors, d->motionVectors, false);
    patchRes(rebuilt.depth, d->depth, false);
    patchRes(rebuilt.exposure, d->exposure, false);
    patchRes(rebuilt.reactive, d->reactive, false);
    patchRes(rebuilt.transparencyAndComposition, d->transparencyAndComposition, false);
    rebuilt.reset = d->reset;
    const auto* saved = g_a3Override;
    g_a3Override = &rebuilt;
    const int rc = RealDispatchTrampoline(ctx, rebuilt.commandList, &rebuilt);
    g_a3Override = saved;
    return rc;
}

// 核心 layout（RE2 实测）：q[0]=color q[1]=mv q[2]=depth q[5]=output
//                          +48 jitterX +52 jitterY +56 exposureScale +60 resetHistory +64/68 inputW/H
// ★ 必须与核心读取的偏移严格一致（RE2 实测）：
//   +0 pColor  +8 pVelocity  +16 pDepth  +24 pExposure  +32 pResponsiveMask  +40 pOutput
//   +48 jitterX +52 jitterY +56 exposureScale +60 resetHistory +64 inputW +68 inputH
struct CoreParams {
    void*    q[6];            // 0..47
    float    jitterX;         // 48
    float    jitterY;         // 52
    float    exposureScale;   // 56
    uint32_t resetHistory;    // 60
    uint32_t inputW;          // 64
    uint32_t inputH;          // 68
};
static_assert(sizeof(CoreParams) == 72 && sizeof(void*) == 8 && offsetof(CoreParams, q) == 0 &&
              offsetof(CoreParams, jitterX) == 48 && offsetof(CoreParams, jitterY) == 52 &&
              offsetof(CoreParams, exposureScale) == 56 && offsetof(CoreParams, resetHistory) == 60 &&
              offsetof(CoreParams, inputW) == 64 && offsetof(CoreParams, inputH) == 68,
              "CoreParams must match ProxyExecute's 72-byte XeSS-compatible layout");

extern "C" __declspec(dllexport) ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* memCb) {
    if (!LoadReal()) return FFX_API_RETURN_ERROR;
    g_realDispatchFn = (void*)&RealDispatchTrampoline;   // ★ 唯一的接缝注入
    Logf("适配层: ffxCreateContext 转发（核心已接管真派发）");
    return g_real.Create(context, desc, memCb);
}

extern "C" __declspec(dllexport) void ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb) {
    if (g_real.Destroy) g_real.Destroy(context, memCb);
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc) {
    if (!desc || !context) return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        return g_real.Dispatch ? g_real.Dispatch(context, desc) : FFX_API_RETURN_ERROR;
    }
    const ffxDispatchDescUpscale* d = (const ffxDispatchDescUpscale*)desc;
    if (!d->color.resource || !d->output.resource || !d->commandList) {
        return g_real.Dispatch ? g_real.Dispatch(context, desc) : FFX_API_RETURN_ERROR;
    }

    // FFXMASK diagnostic (read-only): log reactive / transparencyAndComposition on the first 3 dispatches,
    // every 300th dispatch, and immediately when null-ness or the resource pointer changes vs. the previous dispatch.
    // Static counters, no lock: only called from the game render thread.
    {
        static unsigned long long s_maskN = 0;
        static bool s_maskHasLast = false;
        static const void* s_lastReactive = nullptr;
        static const void* s_lastTc = nullptr;
        ++s_maskN;
        const void* curR = d->reactive.resource;
        const void* curT = d->transparencyAndComposition.resource;
        const bool changed = s_maskHasLast && (curR != s_lastReactive || curT != s_lastTc);
        const bool periodic = (s_maskN <= 3) || (s_maskN % 300ull == 0);
        if (changed || periodic) {
            const char* why = changed ? "change" : (s_maskN <= 3 ? "first3" : "every300");
            const struct { const char* name; const FfxApiResource* r; } items[2] = {
                { "reactive", &d->reactive },
                { "transparencyAndComposition", &d->transparencyAndComposition },
            };
            for (int i = 0; i < 2; ++i) {
                const FfxApiResource& r = *items[i].r;
                if (!r.resource) {
                    Logf("FFXMASK: #%llu (%s) %s resource=NULL(empty) type=%u format=%u width=%u height=%u depth=%u mipCount=%u flags=0x%X usage=0x%X state=0x%X",
                         s_maskN, why, items[i].name, r.description.type, r.description.format,
                         r.description.width, r.description.height, r.description.depth,
                         r.description.mipCount, r.description.flags, r.description.usage, r.state);
                } else {
                    Logf("FFXMASK: #%llu (%s) %s resource=%p type=%u format=%u width=%u height=%u depth=%u mipCount=%u flags=0x%X usage=0x%X state=0x%X",
                         s_maskN, why, items[i].name, r.resource, r.description.type, r.description.format,
                         r.description.width, r.description.height, r.description.depth,
                         r.description.mipCount, r.description.flags, r.description.usage, r.state);
                }
            }
        }
        s_maskHasLast = true;
        s_lastReactive = curR;
        s_lastTc = curT;
    }

    CoreParams cp{};
    cp.q[0] = d->color.resource;
    cp.q[1] = d->motionVectors.resource;
    cp.q[2] = d->depth.resource;
    cp.q[5] = d->output.resource;
    cp.jitterX = d->jitterOffset.x;
    cp.jitterY = d->jitterOffset.y;
    cp.exposureScale = 1.0f;
    cp.resetHistory = d->reset ? 1u : 0u;
    cp.inputW = d->renderSize.width;
    cp.inputH = d->renderSize.height;

    // ★ 对齐（适配层的本职）：把 FFX 的 MV 契约换算成核心要的 mv_scale
    //   FFX：像素位移 = mv × motionVectorScale  ⇒ UV 位移 = mv × (motionVectorScale / renderSize)
    //   核心：UV 位移 = mv × gMvScale（单标量），每次派发按当前参数换算。
    //   符号约定由 ffx_on_core.ini 的 mv_sign 控制（默认 -1，与核心"prevUV = uv - mv"一致）
    static const float sign = [] {
        float sign = -1.0f;
        char ini[MAX_PATH]; char line[64] = { 0 };
        EnsureDir(); lstrcpyA(ini, g_dir); lstrcatA(ini, "ffx_on_core.ini");
        FILE* f = nullptr;
        if (fopen_s(&f, ini, "r") == 0 && f) {
            while (fgets(line, sizeof(line), f)) {
                float v = 0.0f;
                if (sscanf_s(line, "mv_sign=%f", &v) == 1 && (v == 1.0f || v == -1.0f)) sign = v;
            }
            fclose(f);
        }
        return sign;
    }();
    float adapterMvScale = std::numeric_limits<float>::quiet_NaN();
    const UINT rw = d->renderSize.width;
    const float mvScaleX = d->motionVectorScale.x;
    if (rw > 0 && std::fabs(mvScaleX) > 1e-6f) {
        const float uvScale = (mvScaleX / static_cast<float>(rw)) * sign;
        if (std::isfinite(uvScale)) {
            adapterMvScale = uvScale;
            static float lastUvScale = std::numeric_limits<float>::quiet_NaN();
            if (uvScale != lastUvScale) {
                lastUvScale = uvScale;
                Logf("FFX-MV: uvScale=%.9g (sign=%+.0f, mvScaleX=%.9g, renderW=%u)",
                     uvScale, sign, mvScaleX, rw);
            }
        }
    }

    MarqueeReadIni();
    MarqueeDraw((ID3D12Resource*)d->color.resource, (ID3D12GraphicsCommandList*)d->commandList, d->color.state);

    AdapterFrameDesc ad{};
    ad.contextToken = context ? *context : nullptr;
    ad.commandList = d->commandList;
    ad.hasUnsupportedExt = (d->header.pNext != nullptr);
    ad.color = ToAdapterRes(d->color);
    ad.motionVectors = ToAdapterRes(d->motionVectors);
    ad.depth = ToAdapterRes(d->depth);
    ad.exposure = ToAdapterRes(d->exposure);
    ad.reactive = ToAdapterRes(d->reactive);
    ad.transparencyAndComposition = ToAdapterRes(d->transparencyAndComposition);
    ad.output = ToAdapterRes(d->output);
    ad.renderSize = { d->renderSize.width, d->renderSize.height };
    ad.upscaleSize = { d->upscaleSize.width, d->upscaleSize.height };
    ad.jitterOffset = { d->jitterOffset.x, d->jitterOffset.y };
    ad.reset = d->reset;
    ad.okCode = FFX_API_RETURN_OK;
    ad.errCode = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    ad.nativeSize = sizeof(*d);
    memcpy(ad.nativeStorage, d, sizeof(*d));

    const auto* saved = g_pending;
    g_pending = d;
    const int rc = ProxyExecute((void*)context, d->commandList, &cp, adapterMvScale, &ad);
    g_pending = saved;

    return (ffxReturnCode_t)rc;
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc) {
    if (!LoadReal()) return FFX_API_RETURN_ERROR;
    return g_real.Query(context, desc);
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    if (!LoadReal()) return FFX_API_RETURN_ERROR;
    return g_real.Configure(context, desc);
}
