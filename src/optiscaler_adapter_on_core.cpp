// optiscaler_adapter_on_core.cpp -- OptiScaler NGX ASI adapter on top of ffx_core_reuse.cpp
// Exports InitializeASI() (loaded from OptiScaler/plugins/cogpu_nr.asi) plus AmdNr*
// (exported from ffx_core_reuse.cpp for finalimg.addon64 B-form compatibility).
// Captures OptiScaler's D3D12 NGX Create/Evaluate/Release calls via MinHook and forwards
// SuperSampling frames into ProxyExecute + A3 / B-form core cleanly.

#include <windows.h>
#include <intrin.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstddef>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "MinHook.h"
#include "adapter_frame_desc.h"

extern "C" int  ProxyExecute(void* ctx, void* cmdList, const void* params, float adapterMvScale,
                             const AdapterFrameDesc* adapterDesc);
extern "C" void CoreSetRuntimeBaseModule(HMODULE mod);
extern "C" int  AmdNrNeedFinal();
extern "C" int  AmdNrBbHandoff(void* cl, void* tex, unsigned frameTag);
extern "C" void AmdNrSetPresentQueue(void* queue);
extern "C" void* g_realDispatchFn;

// ---- Minimal MSVC-ABI NGX declarations (matches NVSDK_NGX_Parameter vtable layout) ----
enum NVSDK_NGX_Result : int {
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_Fail = (int)0xBAD00000,
    NVSDK_NGX_Result_FAIL_InvalidParameter = (int)(0xBAD00000 | 5),
};

enum NVSDK_NGX_Feature : int {
    NVSDK_NGX_Feature_SuperSampling = 1,
    NVSDK_NGX_Feature_FrameGeneration = 11,
    NVSDK_NGX_Feature_RayReconstruction = 13,
};

struct NVSDK_NGX_Handle {
    unsigned int Id;
};

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_ParameterVtbl {
    void ( __cdecl* SetVoidPtr)(NVSDK_NGX_Parameter*, const char*, void*);
    void ( __cdecl* SetD3D12Resource)(NVSDK_NGX_Parameter*, const char*, ID3D12Resource*);
    void ( __cdecl* SetD3D11Resource)(NVSDK_NGX_Parameter*, const char*, void*);
    void ( __cdecl* SetInt)(NVSDK_NGX_Parameter*, const char*, int);
    void ( __cdecl* SetUInt)(NVSDK_NGX_Parameter*, const char*, unsigned int);
    void ( __cdecl* SetDouble)(NVSDK_NGX_Parameter*, const char*, double);
    void ( __cdecl* SetFloat)(NVSDK_NGX_Parameter*, const char*, float);
    void ( __cdecl* SetULL)(NVSDK_NGX_Parameter*, const char*, unsigned long long);

    NVSDK_NGX_Result ( __cdecl* GetVoidPtr)(const NVSDK_NGX_Parameter*, const char*, void**);
    NVSDK_NGX_Result ( __cdecl* GetD3D12Resource)(const NVSDK_NGX_Parameter*, const char*, ID3D12Resource**);
    NVSDK_NGX_Result ( __cdecl* GetD3D11Resource)(const NVSDK_NGX_Parameter*, const char*, void**);
    NVSDK_NGX_Result ( __cdecl* GetInt)(const NVSDK_NGX_Parameter*, const char*, int*);
    NVSDK_NGX_Result ( __cdecl* GetUInt)(const NVSDK_NGX_Parameter*, const char*, unsigned int*);
    NVSDK_NGX_Result ( __cdecl* GetDouble)(const NVSDK_NGX_Parameter*, const char*, double*);
    NVSDK_NGX_Result ( __cdecl* GetFloat)(const NVSDK_NGX_Parameter*, const char*, float*);
    NVSDK_NGX_Result ( __cdecl* GetULL)(const NVSDK_NGX_Parameter*, const char*, unsigned long long*);

    void ( __cdecl* Reset)(NVSDK_NGX_Parameter*);
};

struct NVSDK_NGX_Parameter {
    const NVSDK_NGX_ParameterVtbl* vtbl;

    void Set(const char* n, void* v)              { vtbl->SetVoidPtr(this, n, v); }
    void Set(const char* n, ID3D12Resource* v)    { vtbl->SetD3D12Resource(this, n, v); }
    void Set(const char* n, int v)                { vtbl->SetInt(this, n, v); }
    void Set(const char* n, unsigned int v)       { vtbl->SetUInt(this, n, v); }
    void Set(const char* n, float v)              { vtbl->SetFloat(this, n, v); }

    NVSDK_NGX_Result Get(const char* n, void** v) const            { return vtbl->GetVoidPtr(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** v) const  { return vtbl->GetD3D12Resource(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, int* v) const              { return vtbl->GetInt(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* v) const     { return vtbl->GetUInt(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, float* v) const            { return vtbl->GetFloat(this, n, v); }
};
static_assert(sizeof(NVSDK_NGX_Parameter) == sizeof(void*), "NVSDK_NGX_Parameter layout");
static_assert(sizeof(NVSDK_NGX_ParameterVtbl) == 17 * sizeof(void*), "NVSDK_NGX_ParameterVtbl 17 slots");

// ---- Shared CoreParams 72-byte layout ----
struct CoreParams {
    void*    q[6];            // +0 color, +8 mv, +16 depth, +24 exposure, +32 reactive, +40 output
    float    jitterX;         // +48
    float    jitterY;         // +52
    float    exposureScale;   // +56
    uint32_t resetHistory;    // +60
    uint32_t inputW;          // +64
    uint32_t inputH;          // +68
};
static_assert(sizeof(CoreParams) == 72 && offsetof(CoreParams, jitterX) == 48 &&
              offsetof(CoreParams, resetHistory) == 60 && offsetof(CoreParams, inputW) == 64,
              "CoreParams must match ProxyExecute layout");

using PFN_NgxCreate = NVSDK_NGX_Result (__cdecl*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                  NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_NgxEval   = NVSDK_NGX_Result (__cdecl*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                  NVSDK_NGX_Parameter*, void*);
using PFN_NgxRel    = NVSDK_NGX_Result (__cdecl*)(NVSDK_NGX_Handle*);

static HMODULE       g_opti = nullptr;
static FILE*         g_log = nullptr;
static SRWLOCK       g_logLock = SRWLOCK_INIT;
static char          g_baseDir[MAX_PATH] = {};
static PFN_NgxCreate g_realCreate = nullptr;
static PFN_NgxEval   g_realEval = nullptr;
static PFN_NgxRel    g_realRelease = nullptr;

static void EnsureBaseDir() {
    if (g_baseDir[0]) return;
    HMODULE m = g_opti ? g_opti : GetModuleHandleA(nullptr);
    char p[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(m, p, MAX_PATH);
    if (!n) return;
    char* s = strrchr(p, '\\');
    if (s) s[1] = 0;
    lstrcpynA(g_baseDir, p, MAX_PATH);
}

static void Logf(const char* fmt, ...) {
    AcquireSRWLockExclusive(&g_logLock);
    if (!g_log) {
        EnsureBaseDir();
        if (g_baseDir[0]) {
            char lp[MAX_PATH];
            lstrcpynA(lp, g_baseDir, MAX_PATH);
            lstrcatA(lp, "cogpu_nr_adapter.log");
            g_log = _fsopen(lp, "w", _SH_DENYNO);
        }
    }
    if (g_log) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

// ---- NGX parameter helpers ----
static ID3D12Resource* GetRes(const NVSDK_NGX_Parameter* p, const char* name, bool* asRes = nullptr) {
    if (!p || !p->vtbl) return nullptr;
    ID3D12Resource* r = nullptr;
    if (p->Get(name, &r) == NVSDK_NGX_Result_Success && r) {
        if (asRes) *asRes = true;
        return r;
    }
    void* v = nullptr;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success && v) {
        if (asRes) *asRes = false;
        return static_cast<ID3D12Resource*>(v);
    }
    return nullptr;
}

static void SetRes(NVSDK_NGX_Parameter* p, const char* name, bool asRes, void* res) {
    if (!p || !name) return;
    if (asRes) p->Set(name, static_cast<ID3D12Resource*>(res));
    else       p->Set(name, res);
}

static unsigned GetUInt(const NVSDK_NGX_Parameter* p, const char* name, unsigned fallback = 0) {
    if (!p || !p->vtbl) return fallback;
    unsigned u = 0;
    if (p->Get(name, &u) == NVSDK_NGX_Result_Success) return u;
    int i = 0;
    if (p->Get(name, &i) == NVSDK_NGX_Result_Success && i >= 0) return static_cast<unsigned>(i);
    return fallback;
}

static bool GetFloat(const NVSDK_NGX_Parameter* p, const char* name, float* out) {
    if (!p || !p->vtbl) return false;
    float f = 0.0f;
    if (p->Get(name, &f) != NVSDK_NGX_Result_Success) return false;
    *out = f;
    return true;
}

// ---- Feature tracking ----
struct TrackedFeature {
    int feature = -1;
    uint32_t createW = 0, createH = 0;
    uint32_t outW = 0, outH = 0;
    uint32_t evals = 0;
    bool logged = false;
};

static std::mutex g_featMtx;
static std::unordered_map<unsigned int, TrackedFeature> g_features;

static int InferUnseenFeature(const NVSDK_NGX_Parameter* p) {
    if (GetRes(p, "DLSSG.Backbuffer")) return NVSDK_NGX_Feature_FrameGeneration;
    for (const char* k : {"DLSSD.Color", "DLSSD.DiffuseAlbedo", "DLSSD.SpecularAlbedo",
                          "GBuffer.DiffuseAlbedo", "GBuffer.SpecularAlbedo"}) {
        if (GetRes(p, k)) return NVSDK_NGX_Feature_RayReconstruction;
    }
    return NVSDK_NGX_Feature_SuperSampling;
}

// ---- Pending frame state for trampolines ----
struct PendingNgxCall {
    ID3D12GraphicsCommandList* cmd = nullptr;
    const NVSDK_NGX_Handle* h = nullptr;
    NVSDK_NGX_Parameter* p = nullptr;
    void* callback = nullptr;

    CoreParams cp{};
    ID3D12Resource* color = nullptr;
    ID3D12Resource* mv = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* exposure = nullptr;
    ID3D12Resource* reactive = nullptr;
    ID3D12Resource* output = nullptr;

    bool colorAsRes = true;
    bool mvAsRes = true;
    bool depthAsRes = true;
    bool expoAsRes = true;
    bool reactAsRes = true;
    const char* reactiveKey = nullptr;

    float jx = 0.0f;
    float jy = 0.0f;
    bool hasReset = false;
    unsigned origReset = 0;
};

static thread_local PendingNgxCall* g_pending = nullptr;
static thread_local int g_inside = 0;

static void TransitionRes(ID3D12GraphicsCommandList* cl, void* res,
                          D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!cl || !res || before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = static_cast<ID3D12Resource*>(res);
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
}

static int RealNgxDispatchTrampoline(void* /*ctx*/, void* cmdList, const void* params) {
    if (!g_realEval || !g_pending) return NVSDK_NGX_Result_Fail;
    auto* cur = g_pending;
    auto* p = cur->p;

    bool patchedJitter = false;
    bool patchedGuides = false;
    if (params && params != &cur->cp && cmdList == cur->cmd) {
        const auto* cp = static_cast<const CoreParams*>(params);
        if (cp->jitterX != cur->jx || cp->jitterY != cur->jy) {
            p->Set("Jitter.Offset.X", cp->jitterX);
            p->Set("Jitter.Offset.Y", cp->jitterY);
            patchedJitter = true;
        }
        if ((cp->q[1] && cp->q[1] != cur->mv) || (cp->q[2] && cp->q[2] != cur->depth)) {
            if (cp->q[1] && cp->q[1] != cur->mv) {
                TransitionRes(cur->cmd, cp->q[1], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                SetRes(p, "MotionVectors", cur->mvAsRes, cp->q[1]);
            }
            if (cp->q[2] && cp->q[2] != cur->depth) {
                TransitionRes(cur->cmd, cp->q[2], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                SetRes(p, "Depth", cur->depthAsRes, cp->q[2]);
            }
            patchedGuides = true;
        }
    }

    const NVSDK_NGX_Result rc = g_realEval(cur->cmd, cur->h, p, cur->callback);

    if (patchedGuides) {
        const auto* cp = static_cast<const CoreParams*>(params);
        if (cp->q[1] && cp->q[1] != cur->mv) {
            SetRes(p, "MotionVectors", cur->mvAsRes, cur->mv);
            TransitionRes(cur->cmd, cp->q[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        if (cp->q[2] && cp->q[2] != cur->depth) {
            SetRes(p, "Depth", cur->depthAsRes, cur->depth);
            TransitionRes(cur->cmd, cp->q[2], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
    }
    if (patchedJitter) {
        p->Set("Jitter.Offset.X", cur->jx);
        p->Set("Jitter.Offset.Y", cur->jy);
    }
    return static_cast<int>(rc);
}

extern "C" int AdapterDispatchA3(void* /*ctx*/, const AdapterFrameDesc* d) {
    if (!g_realEval || !g_pending || !d) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    auto* cur = g_pending;
    if (d->commandList != cur->cmd || d->output.resource != cur->output)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    ID3D12GraphicsCommandList* cl = cur->cmd;
    NVSDK_NGX_Parameter* p = cur->p;

    const struct {
        const AdapterResource* ar;
        const char* key;
        bool asRes;
        void* orig;
    } patches[] = {
        { &d->color,         "Color",           cur->colorAsRes, cur->color },
        { &d->motionVectors, "MotionVectors",   cur->mvAsRes,    cur->mv },
        { &d->depth,         "Depth",           cur->depthAsRes, cur->depth },
        { &d->exposure,      "ExposureTexture", cur->expoAsRes,  cur->exposure },
        { &d->reactive,      cur->reactiveKey,  cur->reactAsRes, cur->reactive },
    };

    for (const auto& it : patches) {
        if (it.ar->resource && it.key) {
            TransitionRes(cl, it.ar->resource, it.ar->state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            SetRes(p, it.key, it.asRes, it.ar->resource);
        }
    }
    p->Set("Jitter.Offset.X", d->jitterOffset.x);
    p->Set("Jitter.Offset.Y", d->jitterOffset.y);
    p->Set("Reset", d->reset ? 1u : 0u);

    const NVSDK_NGX_Result rc = g_realEval(cl, cur->h, p, cur->callback);

    if (cur->hasReset) p->Set("Reset", cur->origReset);
    else               p->Set("Reset", 0u);
    p->Set("Jitter.Offset.X", cur->jx);
    p->Set("Jitter.Offset.Y", cur->jy);
    for (const auto& it : patches) {
        if (it.ar->resource && it.key) {
            SetRes(p, it.key, it.asRes, it.orig);
            TransitionRes(cl, it.ar->resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, it.ar->state);
        }
    }
    return static_cast<int>(rc);
}

namespace Step2bDxgi { static void Install(); }

// ---- Detours ----
static NVSDK_NGX_Result __cdecl hk_create(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Feature feature,
                                          NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out) {
    Step2bDxgi::Install();
    g_realDispatchFn = reinterpret_cast<void*>(&RealNgxDispatchTrampoline);
    const NVSDK_NGX_Result r = g_realCreate ? g_realCreate(cmd, feature, p, out) : NVSDK_NGX_Result_Fail;
    if (r == NVSDK_NGX_Result_Success && out && *out) {
        std::lock_guard<std::mutex> lk(g_featMtx);
        TrackedFeature& t = g_features[(*out)->Id];
        t = TrackedFeature{};
        t.feature = static_cast<int>(feature);
        if (p) {
            t.createW = GetUInt(p, "Width");
            t.createH = GetUInt(p, "Height");
            t.outW = GetUInt(p, "OutWidth");
            t.outH = GetUInt(p, "OutHeight");
        }
        Logf("OptiScaler CreateFeature id=%u feat=%d in=%ux%u out=%ux%u",
             (*out)->Id, t.feature, t.createW, t.createH, t.outW, t.outH);
    }
    return r;
}

static NVSDK_NGX_Result __cdecl hk_release(NVSDK_NGX_Handle* h) {
    if (h) {
        std::lock_guard<std::mutex> lk(g_featMtx);
        g_features.erase(h->Id);
    }
    return g_realRelease ? g_realRelease(h) : NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result __cdecl hk_evaluate(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Handle* h,
                                            NVSDK_NGX_Parameter* p, void* callback) {
    if (g_inside || !cmd || !h || !p || !g_realEval)
        return g_realEval ? g_realEval(cmd, h, p, callback) : NVSDK_NGX_Result_Fail;

    struct InsideGuard { InsideGuard() { ++g_inside; } ~InsideGuard() { --g_inside; } } inside;
    g_realDispatchFn = reinterpret_cast<void*>(&RealNgxDispatchTrampoline);

    TrackedFeature tCopy;
    {
        std::lock_guard<std::mutex> lk(g_featMtx);
        TrackedFeature& t = g_features[h->Id];
        ++t.evals;
        if (t.feature < 0) {
            HMODULE caller = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(_ReturnAddress()), &caller);
            t.feature = (caller == g_opti) ? NVSDK_NGX_Feature_SuperSampling : InferUnseenFeature(p);
            t.createW = GetUInt(p, "Width");
            t.createH = GetUInt(p, "Height");
            t.outW = GetUInt(p, "OutWidth");
            t.outH = GetUInt(p, "OutHeight");
            Logf("OptiScaler first-seen Evaluate id=%u callerOpti=%d inferredFeat=%d",
                 h->Id, (caller == g_opti) ? 1 : 0, t.feature);
        }
        tCopy = t;
    }

    if (tCopy.feature != NVSDK_NGX_Feature_SuperSampling) {
        return g_realEval(cmd, h, p, callback);
    }

    PendingNgxCall cur{};
    cur.cmd = cmd;
    cur.h = h;
    cur.p = p;
    cur.callback = callback;
    cur.color = GetRes(p, "Color", &cur.colorAsRes);
    cur.output = GetRes(p, "Output");
    cur.mv = GetRes(p, "MotionVectors", &cur.mvAsRes);
    cur.depth = GetRes(p, "Depth", &cur.depthAsRes);
    cur.exposure = GetRes(p, "ExposureTexture", &cur.expoAsRes);
    if ((cur.reactive = GetRes(p, "DLSS.Input.Bias.Current.Color.Mask", &cur.reactAsRes)) != nullptr) {
        cur.reactiveKey = "DLSS.Input.Bias.Current.Color.Mask";
    } else if ((cur.reactive = GetRes(p, "ReactiveMask", &cur.reactAsRes)) != nullptr) {
        cur.reactiveKey = "ReactiveMask";
    }

    if (!cur.color || !cur.output) {
        return g_realEval(cmd, h, p, callback);
    }

    const D3D12_RESOURCE_DESC cd = cur.color->GetDesc();
    const D3D12_RESOURCE_DESC od = cur.output->GetDesc();

    uint32_t rw = GetUInt(p, "DLSS.Render.Subrect.Dimensions.Width");
    uint32_t rh = GetUInt(p, "DLSS.Render.Subrect.Dimensions.Height");
    if (!rw || !rh) {
        rw = GetUInt(p, "Width", tCopy.createW ? tCopy.createW : static_cast<uint32_t>(cd.Width));
        rh = GetUInt(p, "Height", tCopy.createH ? tCopy.createH : cd.Height);
    }
    uint32_t ow = GetUInt(p, "DLSS.Output.Subrect.Dimensions.Width");
    uint32_t oh = GetUInt(p, "DLSS.Output.Subrect.Dimensions.Height");
    if (!ow || !oh) {
        ow = tCopy.outW ? tCopy.outW : static_cast<uint32_t>(od.Width);
        oh = tCopy.outH ? tCopy.outH : od.Height;
    }

    GetFloat(p, "Jitter.Offset.X", &cur.jx);
    GetFloat(p, "Jitter.Offset.Y", &cur.jy);
    float expoScale = 1.0f;
    if (!GetFloat(p, "DLSS.Exposure.Scale", &expoScale) || !std::isfinite(expoScale) || expoScale <= 0.0f)
        expoScale = 1.0f;

    unsigned rstU = 0;
    cur.hasReset = (p->Get("Reset", &rstU) == NVSDK_NGX_Result_Success);
    if (!cur.hasReset) {
        int rstI = 0;
        if (p->Get("Reset", &rstI) == NVSDK_NGX_Result_Success) {
            cur.hasReset = true;
            rstU = static_cast<unsigned>(rstI);
        }
    }
    cur.origReset = rstU;

    cur.cp.q[0] = cur.color;
    cur.cp.q[1] = cur.mv;
    cur.cp.q[2] = cur.depth;
    cur.cp.q[3] = cur.exposure;
    cur.cp.q[4] = cur.reactive;
    cur.cp.q[5] = cur.output;
    cur.cp.jitterX = cur.jx;
    cur.cp.jitterY = cur.jy;
    cur.cp.exposureScale = expoScale;
    cur.cp.resetHistory = rstU ? 1u : 0u;
    cur.cp.inputW = rw;
    cur.cp.inputH = rh;

    static const float sign = [] {
        float s = -1.0f;
        EnsureBaseDir();
        if (g_baseDir[0]) {
            char ini[MAX_PATH]; char line[64] = {};
            lstrcpynA(ini, g_baseDir, MAX_PATH);
            lstrcatA(ini, "ffx_on_core.ini");
            FILE* f = nullptr;
            if (fopen_s(&f, ini, "r") == 0 && f) {
                while (fgets(line, sizeof(line), f)) {
                    float v = 0.0f;
                    if (sscanf_s(line, "mv_sign=%f", &v) == 1 && (v == 1.0f || v == -1.0f)) s = v;
                }
                fclose(f);
            }
        }
        return s;
    }();

    float mvScaleX = 0.0f;
    float adapterMvScale = std::numeric_limits<float>::quiet_NaN();
    if (GetFloat(p, "MV.Scale.X", &mvScaleX) && rw > 0 && std::isfinite(mvScaleX) && std::fabs(mvScaleX) > 1e-6f) {
        const float uvScale = (mvScaleX / static_cast<float>(rw)) * sign;
        if (std::isfinite(uvScale)) {
            adapterMvScale = uvScale;
            static float lastUvScale = std::numeric_limits<float>::quiet_NaN();
            if (uvScale != lastUvScale) {
                lastUvScale = uvScale;
                Logf("NGX-MV: uvScale=%.9g (sign=%+.0f mvScaleX=%.9g renderW=%u)", uvScale, sign, mvScaleX, rw);
            }
        }
    }

    if (tCopy.evals <= 3) {
        Logf("Eval#%u id=%u render=%ux%u out=%ux%u color=%p mv=%p depth=%p j=(%.4f,%.4f) sub=(%u,%u)/(%u,%u)",
             tCopy.evals, h->Id, rw, rh, ow, oh,
             (void*)cur.color, (void*)cur.mv, (void*)cur.depth, cur.jx, cur.jy,
             GetUInt(p, "DLSS.Input.Color.Subrect.Base.X"),
             GetUInt(p, "DLSS.Input.Color.Subrect.Base.Y"),
             GetUInt(p, "DLSS.Output.Subrect.Base.X"),
             GetUInt(p, "DLSS.Output.Subrect.Base.Y"));
    }

    AdapterFrameDesc ad{};
    ad.contextToken = reinterpret_cast<void*>(static_cast<uintptr_t>(h->Id));
    ad.commandList = cmd;
    ad.hasUnsupportedExt = false;
    ad.color = { cur.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true };
    ad.motionVectors = { cur.mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true };
    ad.depth = { cur.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true };
    ad.exposure = { cur.exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true };
    ad.reactive = { cur.reactive, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true };
    ad.transparencyAndComposition = {};
    ad.output = { cur.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true };
    ad.renderSize = { rw, rh };
    ad.upscaleSize = { ow, oh };
    ad.jitterOffset = { cur.jx, cur.jy };
    ad.reset = (rstU != 0);
    ad.okCode = NVSDK_NGX_Result_Success;
    ad.errCode = NVSDK_NGX_Result_Fail;

    PendingNgxCall* saved = g_pending;
    g_pending = &cur;
    const int rc = ProxyExecute(ad.contextToken, cmd, &cur.cp, adapterMvScale, &ad);
    g_pending = saved;
    return static_cast<NVSDK_NGX_Result>(rc);
}

// ---- Step 2b: direct DXGI SwapChain Present capture (replaces ReShade + finalimg.addon64) ----
using Microsoft::WRL::ComPtr;

namespace Step2bDxgi {
    enum Kind { kPresent = 0, kPresent1, kCreateSc, kCreateHwnd, kCreateCore, kCreateFac, kCreateFac1, kCreateFac2, kKindCount };
    static constexpr int kSlots = 8;

    static std::mutex g_mtx;
    static std::unordered_map<void*, int> g_hookedFn;
    static void* g_orig[kKindCount][kSlots]{};
    static int   g_used[kKindCount]{};
    static std::unordered_map<IDXGISwapChain*, ComPtr<ID3D12CommandQueue>> g_scQueues;

    static thread_local int t_creating = 0;
    static thread_local int t_presentDepth = 0;

    static struct BbCapState {
        std::mutex mtx;
        ComPtr<ID3D12Device> dev;
        ComPtr<ID3D12Fence> fence;
        HANDLE fenceEv = nullptr;
        UINT64 fvNext = 0;
        UINT64 fv[2]{};
        ComPtr<ID3D12CommandAllocator> al[2];
        ComPtr<ID3D12GraphicsCommandList> cl[2];
        ComPtr<ID3D12Resource> staging[2];
        uint32_t w = 0, h = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        uint32_t frameTag = 0;
        uint64_t okCount = 0;
    } g_bb;

    static void ResetBbCapResources() {
        g_bb.staging[0].Reset(); g_bb.staging[1].Reset();
        g_bb.cl[0].Reset();      g_bb.cl[1].Reset();
        g_bb.al[0].Reset();      g_bb.al[1].Reset();
        g_bb.fence.Reset();
        if (g_bb.fenceEv) { CloseHandle(g_bb.fenceEv); g_bb.fenceEv = nullptr; }
        g_bb.dev.Reset();
        g_bb.fvNext = 0; g_bb.fv[0] = 0; g_bb.fv[1] = 0;
        g_bb.w = 0; g_bb.h = 0; g_bb.fmt = DXGI_FORMAT_UNKNOWN;
    }

    static void CaptureBackbufferBeforePresent(IDXGISwapChain* sc) {
        if (!sc) return;
        ComPtr<ID3D12CommandQueue> queue;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            auto it = g_scQueues.find(sc);
            if (it == g_scQueues.end() || !it->second) return;
            queue = it->second;
        }
        AmdNrSetPresentQueue(queue.Get());
        if (!AmdNrNeedFinal()) return;

        ComPtr<IDXGISwapChain3> sc3;
        if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
        const UINT bbIdx = sc3->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> bb;
        if (FAILED(sc3->GetBuffer(bbIdx, IID_PPV_ARGS(&bb))) || !bb) return;

        const D3D12_RESOURCE_DESC bd = bb->GetDesc();
        if (bd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || bd.SampleDesc.Count != 1 ||
            bd.Width < 1920 || bd.Height < 1000) {
            return;
        }

        ComPtr<ID3D12Device> qDev;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&qDev))) || !qDev) return;

        ID3D12CommandList* execList = nullptr;
        UINT64 signalVal = 0;
        int pi = 0;
        {
            std::lock_guard<std::mutex> lk(g_bb.mtx);
            if (g_bb.dev.Get() != qDev.Get() || g_bb.w != static_cast<uint32_t>(bd.Width) ||
                g_bb.h != bd.Height || g_bb.fmt != bd.Format) {
                ResetBbCapResources();
                g_bb.dev = qDev;
                g_bb.w = static_cast<uint32_t>(bd.Width);
                g_bb.h = bd.Height;
                g_bb.fmt = bd.Format;
                if (FAILED(qDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_bb.fence))) || !g_bb.fence) {
                    ResetBbCapResources();
                    return;
                }
                g_bb.fenceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                for (int i = 0; i < 2; ++i) {
                    if (FAILED(qDev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_bb.al[i]))) ||
                        FAILED(qDev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bb.al[i].Get(), nullptr, IID_PPV_ARGS(&g_bb.cl[i]))) ||
                        FAILED(g_bb.cl[i]->Close())) {
                        ResetBbCapResources();
                        return;
                    }
                    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                    D3D12_RESOURCE_DESC td{};
                    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    td.Width = g_bb.w; td.Height = g_bb.h; td.DepthOrArraySize = 1; td.MipLevels = 1;
                    td.Format = g_bb.fmt; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                    if (FAILED(qDev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                             IID_PPV_ARGS(&g_bb.staging[i])))) {
                        ResetBbCapResources();
                        return;
                    }
                }
                Logf("Step2b: staging ready %ux%u fmt=%d on queue=%p", g_bb.w, g_bb.h, (int)g_bb.fmt, (void*)queue.Get());
            }

            ++g_bb.frameTag;
            pi = static_cast<int>(g_bb.frameTag & 1u);
            const UINT64 done = g_bb.fence->GetCompletedValue();
            if (done == UINT64_MAX) { ResetBbCapResources(); return; }
            if (done < g_bb.fv[pi] && g_bb.fenceEv) {
                g_bb.fence->SetEventOnCompletion(g_bb.fv[pi], g_bb.fenceEv);
                WaitForSingleObject(g_bb.fenceEv, 100);
            }
            if (FAILED(g_bb.al[pi]->Reset()) || FAILED(g_bb.cl[pi]->Reset(g_bb.al[pi].Get(), nullptr))) return;

            auto* cl = g_bb.cl[pi].Get();
            TransitionRes(cl, bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            TransitionRes(cl, g_bb.staging[pi].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(g_bb.staging[pi].Get(), bb.Get());
            TransitionRes(cl, g_bb.staging[pi].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            TransitionRes(cl, bb.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            if (FAILED(cl->Close())) return;

            AmdNrBbHandoff(cl, g_bb.staging[pi].Get(), g_bb.frameTag);
            execList = cl;
            signalVal = ++g_bb.fvNext;
            g_bb.fv[pi] = signalVal;
            if ((++g_bb.okCount == 1) || (g_bb.okCount % 300) == 0) {
                Logf("Step2b: Present capture #%llu frameTag=%u sc=%p bb=%p (%ux%u fmt=%d)",
                     (unsigned long long)g_bb.okCount, g_bb.frameTag, (void*)sc, (void*)bb.Get(),
                     g_bb.w, g_bb.h, (int)g_bb.fmt);
            }
        }
        if (execList) {
            queue->ExecuteCommandLists(1, &execList);
            queue->Signal(g_bb.fence.Get(), signalVal);
        }
    }

    static void AdoptSwapChain(IUnknown* device, IDXGISwapChain* sc);
    static void AdoptFactory(void* fac);

    template <int S>
    struct Detours {
        static HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* sc, UINT sync, UINT flags) {
            const bool outer = (t_presentDepth++ == 0);
            if (outer && !(flags & DXGI_PRESENT_TEST) && !t_creating && !g_inside) {
                CaptureBackbufferBeforePresent(sc);
            }
            using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
            const HRESULT hr = reinterpret_cast<Fn>(g_orig[kPresent][S])(sc, sync, flags);
            --t_presentDepth;
            return hr;
        }
        static HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
            const bool outer = (t_presentDepth++ == 0);
            if (outer && !(flags & DXGI_PRESENT_TEST) && !t_creating && !g_inside) {
                CaptureBackbufferBeforePresent(sc);
            }
            using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
            const HRESULT hr = reinterpret_cast<Fn>(g_orig[kPresent1][S])(sc, sync, flags, params);
            --t_presentDepth;
            return hr;
        }
        static HRESULT STDMETHODCALLTYPE create_sc(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
            ++t_creating;
            using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
            const HRESULT hr = reinterpret_cast<Fn>(g_orig[kCreateSc][S])(f, dev, desc, out);
            --t_creating;
            if (SUCCEEDED(hr) && out && *out && !t_creating) AdoptSwapChain(dev, *out);
            return hr;
        }
        static HRESULT STDMETHODCALLTYPE create_hwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                     IDXGIOutput* output, IDXGISwapChain1** out) {
            ++t_creating;
            using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
            const HRESULT hr = reinterpret_cast<Fn>(g_orig[kCreateHwnd][S])(f, dev, hwnd, desc, fs, output, out);
            --t_creating;
            if (SUCCEEDED(hr) && out && *out && !t_creating) AdoptSwapChain(dev, *out);
            return hr;
        }
        static HRESULT STDMETHODCALLTYPE create_core(IDXGIFactory2* f, IUnknown* dev, IUnknown* win,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out) {
            ++t_creating;
            using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                   IDXGIOutput*, IDXGISwapChain1**);
            const HRESULT hr = reinterpret_cast<Fn>(g_orig[kCreateCore][S])(f, dev, win, desc, output, out);
            --t_creating;
            if (SUCCEEDED(hr) && out && *out && !t_creating) AdoptSwapChain(dev, *out);
            return hr;
        }
        static HRESULT WINAPI create_fac(REFIID riid, void** out) {
            using Fn = HRESULT(WINAPI*)(REFIID, void**);
            const HRESULT hr = reinterpret_cast<Fn>(g_orig[kCreateFac][S])(riid, out);
            if (SUCCEEDED(hr) && out && !t_creating) AdoptFactory(*out);
            return hr;
        }
        static HRESULT WINAPI create_fac1(REFIID riid, void** out) {
            using Fn = HRESULT(WINAPI*)(REFIID, void**);
            const HRESULT hr = reinterpret_cast<Fn>(g_orig[kCreateFac1][S])(riid, out);
            if (SUCCEEDED(hr) && out && !t_creating) AdoptFactory(*out);
            return hr;
        }
        static HRESULT WINAPI create_fac2(UINT flags, REFIID riid, void** out) {
            using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
            const HRESULT hr = reinterpret_cast<Fn>(g_orig[kCreateFac2][S])(flags, riid, out);
            if (SUCCEEDED(hr) && out && !t_creating) AdoptFactory(*out);
            return hr;
        }
        static void* of(Kind k) {
            switch (k) {
            case kPresent:    return reinterpret_cast<void*>(&present);
            case kPresent1:   return reinterpret_cast<void*>(&present1);
            case kCreateSc:   return reinterpret_cast<void*>(&create_sc);
            case kCreateHwnd: return reinterpret_cast<void*>(&create_hwnd);
            case kCreateCore: return reinterpret_cast<void*>(&create_core);
            case kCreateFac:  return reinterpret_cast<void*>(&create_fac);
            case kCreateFac1: return reinterpret_cast<void*>(&create_fac1);
            case kCreateFac2: return reinterpret_cast<void*>(&create_fac2);
            default: return nullptr;
            }
        }
    };

    static void* DetourOf(Kind k, int s) {
        switch (s) {
        case 0: return Detours<0>::of(k); case 1: return Detours<1>::of(k);
        case 2: return Detours<2>::of(k); case 3: return Detours<3>::of(k);
        case 4: return Detours<4>::of(k); case 5: return Detours<5>::of(k);
        case 6: return Detours<6>::of(k); case 7: return Detours<7>::of(k);
        default: return nullptr;
        }
    }

    static bool HookFnPtr(void* fn, Kind kind) {
        if (!fn) return false;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (auto it = g_hookedFn.find(fn); it != g_hookedFn.end()) return it->second >= 0;
        if (g_used[kind] >= kSlots) { g_hookedFn[fn] = -1; return false; }
        const int s = g_used[kind];
        if (MH_CreateHook(fn, DetourOf(kind, s), &g_orig[kind][s]) != MH_OK) {
            g_hookedFn[fn] = -1;
            return false;
        }
        if (MH_EnableHook(fn) != MH_OK) {
            MH_RemoveHook(fn);
            g_hookedFn[fn] = -1;
            return false;
        }
        g_hookedFn[fn] = s;
        ++g_used[kind];
        return true;
    }

    static bool HookVtableSlot(void* obj, int slot, Kind kind) {
        if (!obj) return false;
        void** vt = *reinterpret_cast<void***>(obj);
        return vt ? HookFnPtr(vt[slot], kind) : false;
    }

    static void AdoptSwapChain(IUnknown* device, IDXGISwapChain* sc) {
        if (!device || !sc) return;
        ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) return;

        // Require a real window-backed swapchain; B-form's DComp CreateSwapChainForComposition has no HWND.
        HWND hwnd = nullptr;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) || !sc1 || FAILED(sc1->GetHwnd(&hwnd)) || !hwnd) {
            DXGI_SWAP_CHAIN_DESC sd{};
            if (FAILED(sc->GetDesc(&sd)) || !sd.OutputWindow) return;
            hwnd = sd.OutputWindow;
        }

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_scQueues[sc] = queue;
        }
        AmdNrSetPresentQueue(queue.Get());
        const bool okP = HookVtableSlot(sc, 8, kPresent);
        const bool okP1 = sc1 ? HookVtableSlot(sc1.Get(), 22, kPresent1) : false;
        Logf("Step2b: adopted swapchain=%p hwnd=%p queue=%p (Present=%d Present1=%d)",
             (void*)sc, (void*)hwnd, (void*)queue.Get(), (int)okP, (int)okP1);
    }

    static void AdoptFactory(void* fac) {
        if (!fac) return;
        ComPtr<IDXGIFactory> f;
        if (SUCCEEDED(static_cast<IUnknown*>(fac)->QueryInterface(IID_PPV_ARGS(&f))) && f)
            HookVtableSlot(f.Get(), 10, kCreateSc);
        ComPtr<IDXGIFactory2> f2;
        if (SUCCEEDED(static_cast<IUnknown*>(fac)->QueryInterface(IID_PPV_ARGS(&f2))) && f2) {
            HookVtableSlot(f2.Get(), 15, kCreateHwnd);
            HookVtableSlot(f2.Get(), 16, kCreateCore);
        }
    }

    static void Install() {
        HMODULE dxgiMain = GetModuleHandleW(L"dxgi.dll");
        wchar_t sysPath[MAX_PATH] = {};
        HMODULE dxgiSys = nullptr;
        if (GetSystemDirectoryW(sysPath, MAX_PATH)) {
            wcscat_s(sysPath, L"\\dxgi.dll");
            dxgiSys = GetModuleHandleW(sysPath);
        }
        HMODULE mods[2] = { dxgiMain, (dxgiSys && dxgiSys != dxgiMain) ? dxgiSys : nullptr };
        const struct { const char* name; Kind kind; } entries[] = {
            { "CreateDXGIFactory",  kCreateFac  },
            { "CreateDXGIFactory1", kCreateFac1 },
            { "CreateDXGIFactory2", kCreateFac2 },
        };
        static HMODULE s_hookedMods[4]{};
        for (HMODULE m : mods) {
            if (!m) continue;
            bool seen = false;
            for (HMODULE hm : s_hookedMods) if (hm == m) seen = true;
            if (seen) continue;
            for (HMODULE& hm : s_hookedMods) if (!hm) { hm = m; break; }
            int cnt = 0;
            for (const auto& e : entries) {
                if (HookFnPtr(reinterpret_cast<void*>(GetProcAddress(m, e.name)), e.kind)) ++cnt;
            }
            char mpath[MAX_PATH] = {};
            GetModuleFileNameA(m, mpath, MAX_PATH);
            Logf("Step2b: hooked %d/3 DXGI factory entry points on %p (%s)", cnt, (void*)m, mpath);
        }
    }
}

static bool HookExport(const char* name, void* detour, void** orig) {
    if (!g_opti) return false;
    FARPROC proc = GetProcAddress(g_opti, name);
    if (!proc) {
        Logf("HookExport: %s not found on OptiScaler module %p", name, (void*)g_opti);
        return false;
    }
    const MH_STATUS c = MH_CreateHook(reinterpret_cast<LPVOID>(proc), detour, orig);
    if (c != MH_OK && c != MH_ERROR_ALREADY_CREATED) {
        Logf("HookExport: MH_CreateHook(%s) failed: %s", name, MH_StatusToString(c));
        return false;
    }
    const MH_STATUS e = MH_EnableHook(reinterpret_cast<LPVOID>(proc));
    if (e != MH_OK && e != MH_ERROR_ENABLED) {
        Logf("HookExport: MH_EnableHook(%s) failed: %s", name, MH_StatusToString(e));
        return false;
    }
    Logf("HookExport: %s hooked at %p", name, (void*)proc);
    return true;
}

static bool IsNonGameHelperExe() {
    char exe[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return false;
    const char* slash = strrchr(exe, '\\');
    const char* base = slash ? (slash + 1) : exe;
    return (_stricmp(base, "REDEngineErrorReporter.exe") == 0) ||
           (_stricmp(base, "CrashReporter.exe") == 0);
}

extern "C" void InitializeASI() {
    static bool done = false;
    if (done) return;
    done = true;
    if (IsNonGameHelperExe()) return;

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(_ReturnAddress()), &g_opti);
    if (!g_opti) g_opti = GetModuleHandleW(nullptr);
    CoreSetRuntimeBaseModule(g_opti);
    g_realDispatchFn = reinterpret_cast<void*>(&RealNgxDispatchTrampoline);

    EnsureBaseDir();
    char modPath[MAX_PATH] = {};
    GetModuleFileNameA(g_opti, modPath, MAX_PATH);
    Logf("InitializeASI: g_opti=%p (%s) baseDir=%s", (void*)g_opti, modPath, g_baseDir);

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        Logf("InitializeASI: MH_Initialize failed: %s", MH_StatusToString(init));
        return;
    }

    const bool ok = HookExport("NVSDK_NGX_D3D12_CreateFeature",   reinterpret_cast<void*>(&hk_create),  reinterpret_cast<void**>(&g_realCreate)) &&
                    HookExport("NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void*>(&hk_evaluate), reinterpret_cast<void**>(&g_realEval)) &&
                    HookExport("NVSDK_NGX_D3D12_ReleaseFeature",  reinterpret_cast<void*>(&hk_release), reinterpret_cast<void**>(&g_realRelease));
    Logf("InitializeASI: D3D12 NGX hooks %s", ok ? "READY" : "FAILED");
    Step2bDxgi::Install();
}
