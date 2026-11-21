// ngx_f18_backend.cpp -- "NGX feature 18" backend (drives dlssnr_core.dll as the NR runtime)
//
// Scope: seam layer only.
//   1) load dlssnr_core.dll and run the standard NGX flow (Init -> GetCapabilityParameters -> CreateFeature(18))
//   2) hand colour / motion / output to EvaluateFeature every frame
//   3) expose the answer texture for the caller to copy out
// The core (slot ring / cross-card / compose / present / diagnostics) is untouched.
//
// Everything here comes from measurement (2026-10-30, see the handoff doc section 9):
//   * the parameter block MUST come from GetCapabilityParameters() (not AllocateParameters)
//   * CreationNodeMask / VisibilityNodeMask = 1 at create time
//   * every subrect is written each frame (including OutputSubrect width/height)
//   * the OUTPUT texture MUST carry D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, otherwise the
//     core's UAV barrier is rejected, the list goes into an error state, Close() fails and the
//     device is removed (0x887A0001) -- measured, and proved by A/B
//   * the answer must NOT be read right after Evaluate in the same list (garbage); the caller
//     copies it in a later submission -- measured
//   * depth is not fed: the network takes no depth; dlssnr_core reads it only for the motion-vector
//     lookup when Motion Without Depth=0 (default 1)
//
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>

// ------------------------------------------------------------------ logging (next to our module)
static FILE* g_log = nullptr;
static void NgxLog(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}
static std::string ExeDir() {
    char p[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    char* s = strrchr(p, '\\'); if (s) s[1] = 0;
    return std::string(p);
}
static std::string NgxDir() {
#ifdef COGPU_ASI_BUILD
    return ExeDir();
#else
    char p[MAX_PATH] = { 0 };
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&NgxDir, &self);
    GetModuleFileNameA(self, p, MAX_PATH);
    char* s = strrchr(p, '\\'); if (s) s[1] = 0;
    return std::string(p);
#endif
}
static void NgxLogOpen() {
    if (g_log) return;
    std::string p = NgxDir() + "ngx_backend.log";
    fopen_s(&g_log, p.c_str(), "w");
}

// ------------------------------------------------------------------ NGX parameter block (17 slots)
struct ID3D11Resource;
struct NgxParam {
    virtual void Set(const char* n, unsigned long long v) = 0;
    virtual void Set(const char* n, float v) = 0;
    virtual void Set(const char* n, double v) = 0;
    virtual void Set(const char* n, unsigned int v) = 0;
    virtual void Set(const char* n, int v) = 0;
    virtual void Set(const char* n, ID3D11Resource* v) = 0;
    virtual void Set(const char* n, ID3D12Resource* v) = 0;
    virtual void Set(const char* n, void* v) = 0;
    virtual int  Get(const char* n, unsigned long long* o) const = 0;
    virtual int  Get(const char* n, float* o) const = 0;
    virtual int  Get(const char* n, double* o) const = 0;
    virtual int  Get(const char* n, unsigned int* o) const = 0;
    virtual int  Get(const char* n, int* o) const = 0;
    virtual int  Get(const char* n, ID3D11Resource** o) const = 0;
    virtual int  Get(const char* n, ID3D12Resource** o) const = 0;
    virtual int  Get(const char* n, void** o) const = 0;
    virtual void Reset() = 0;
};

typedef int (*PFN_Init)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
typedef int (*PFN_Alloc)(void**);
typedef int (*PFN_Destroy)(void*);
typedef int (*PFN_Create)(ID3D12GraphicsCommandList*, int, void*, void**);
typedef int (*PFN_Eval)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
typedef int (*PFN_Release)(void*);
typedef int (*PFN_Shutdown1)(ID3D12Device*);

static const int NGX_OK = 0x1;

static struct {
    HMODULE mod = nullptr;
    PFN_Init Init = nullptr;
    PFN_Alloc GetCap = nullptr;
    PFN_Destroy DestroyParams = nullptr;
    PFN_Create Create = nullptr;
    PFN_Eval Eval = nullptr;
    PFN_Release Release = nullptr;
    PFN_Shutdown1 Shutdown1 = nullptr;

    ID3D12Device* dev = nullptr;
    NgxParam* params = nullptr;
    void* handle = nullptr;
    ID3D12Resource* out = nullptr;     // answer texture (UAV)
    unsigned w = 0, h = 0;
    bool tried = false, ok = false;
    float intensity = 1.0f;
    unsigned style = 0u;
    float localTone = 1.0f;
    float localStruct = 1.0f;
    float skinStruct = -1.0f;
    unsigned useAutoMask = 0u;
} g_nb;

// ------------------------------------------------------------------ helpers
static ID3D12Resource* NgxMakeAnswerTex(ID3D12Device* dev, unsigned w, unsigned h) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // must allow UAV: the core writes the answer with a UAV (measured; without it the device is removed)
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // shared: the core imports it into its own Vulkan device ("sampled in place")
    ID3D12Resource* r = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td,
                                              D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) {
        NgxLog("!! answer texture failed 0x%08X (shared+UAV)", (unsigned)hr);
        return nullptr;
    }
    return r;
}

static void NbSetU(NgxParam* p, const char* n, unsigned v) { p->Set(n, v); }
static void NbSetF(NgxParam* p, const char* n, float v) { p->Set(n, v); }
static void NbSetR(NgxParam* p, const char* n, ID3D12Resource* v) { p->Set(n, v); }

static float NbClampControl(float v, float lo, float hi, float def) {
    if (v >= lo && v <= hi) return v;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return def;
}

static void NbApplyStyleControls(NgxParam* p) {
    NbSetF(p, "DLSSNR.Intensity", g_nb.intensity);
    NbSetU(p, "DLSSNR.Style", g_nb.style);
    NbSetF(p, "DLSSNR.LocalToneStrength", g_nb.localTone);
    NbSetF(p, "DLSSNR.LocalStructureStrength", g_nb.localStruct);
    NbSetF(p, "DLSSNR.SkinStructureStrength", g_nb.skinStruct);
    NbSetU(p, "DLSSNR.UseAutoMask", g_nb.useAutoMask);
}

// ------------------------------------------------------------------ public
extern "C" bool NgxBackendLoad(ID3D12Device* dev) {
    NgxLogOpen();
    if (g_nb.tried) return g_nb.ok;
    g_nb.tried = true;
    g_nb.dev = dev;

    std::string baseDir = NgxDir();
    std::string path = baseDir + "dlssnr_core.dll";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::string alt = ExeDir();
        if (GetFileAttributesA((alt + "dlssnr_core.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
            baseDir = alt;
            path = baseDir + "dlssnr_core.dll";
        }
    }
    // Optional: let the core score its own engine inputs (it prints a motion verdict
    // OK / SUSPICIOUS / BAD plus "how many px, what % agree within 1 px"). Opt-in only:
    // enabled when xess_proxy.nrcheck.txt exists next to this module. Must be set BEFORE
    // the core loads, because it reads its switches at init.
    {
        std::string mk = baseDir + "xess_proxy.nrcheck.txt";
        if (GetFileAttributesA(mk.c_str()) != INVALID_FILE_ATTRIBUTES) {
            SetEnvironmentVariableW(L"NR_INPUT_CHECK", L"1");
            NgxLog("input check: NR_INPUT_CHECK=1 (xess_proxy.nrcheck.txt present)");
        }
    }
    g_nb.mod = LoadLibraryA(path.c_str());
    if (!g_nb.mod) {
        NgxLog("!! load failed \"%s\" err=%lu", path.c_str(), GetLastError());
        return false;
    }
    g_nb.Init = (PFN_Init)GetProcAddress(g_nb.mod, "NVSDK_NGX_D3D12_Init");
    g_nb.GetCap = (PFN_Alloc)GetProcAddress(g_nb.mod, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    g_nb.DestroyParams = (PFN_Destroy)GetProcAddress(g_nb.mod, "NVSDK_NGX_D3D12_DestroyParameters");
    g_nb.Create = (PFN_Create)GetProcAddress(g_nb.mod, "NVSDK_NGX_D3D12_CreateFeature");
    g_nb.Eval = (PFN_Eval)GetProcAddress(g_nb.mod, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_nb.Release = (PFN_Release)GetProcAddress(g_nb.mod, "NVSDK_NGX_D3D12_ReleaseFeature");
    g_nb.Shutdown1 = (PFN_Shutdown1)GetProcAddress(g_nb.mod, "NVSDK_NGX_D3D12_Shutdown1");
    if (!g_nb.Init || !g_nb.GetCap || !g_nb.Create || !g_nb.Eval) {
        NgxLog("!! missing exports Init=%p GetCap=%p Create=%p Eval=%p",
               (void*)g_nb.Init, (void*)g_nb.GetCap, (void*)g_nb.Create, (void*)g_nb.Eval);
        return false;
    }

    std::wstring wdir; { std::string d = NgxDir(); wdir.assign(d.begin(), d.end()); }
    const int rc = g_nb.Init(0x4631385F53454C46ull /* "F18_SELF" */, wdir.c_str(), dev, nullptr, 0x0000015);
    NgxLog("Init -> 0x%X (sdkPath=%s)", (unsigned)rc, NgxDir().c_str());

    void* p = nullptr;
    const int rcParams = g_nb.GetCap(&p);
    g_nb.params = (NgxParam*)p;
    NgxLog("GetCapabilityParameters -> 0x%X params=%p", (unsigned)rcParams, p);
    if (!g_nb.params) return false;
    g_nb.ok = true;
    return true;
}

// cl must be an OPEN command list (CreateFeature takes the device from it)
extern "C" bool NgxBackendEnsure(ID3D12GraphicsCommandList* cl, unsigned w, unsigned h) {
    if (!g_nb.ok) return false;
    if (g_nb.handle && g_nb.w == w && g_nb.h == h) return true;
    if (g_nb.handle && (g_nb.w != w || g_nb.h != h)) {
        if (g_nb.Release) g_nb.Release(g_nb.handle);
        g_nb.handle = nullptr;
        NgxLog("size changed -> rebuild feature (%ux%u)", w, h);
    }
    if (!g_nb.out) {
        g_nb.out = NgxMakeAnswerTex(g_nb.dev, w, h);
        if (!g_nb.out) return false;
        NgxLog("answer texture ready %ux%u RGBA8 shared+UAV", w, h);
    }
    NgxParam* P = g_nb.params;
    NbSetU(P, "DLSSNR.Enabled", 1u);
    NbSetU(P, "DLSSNR.Width", w);
    NbSetU(P, "DLSSNR.Height", h);
    NbSetU(P, "CreationNodeMask", 1u);
    NbSetU(P, "VisibilityNodeMask", 1u);
    NbSetU(P, "DLSSNR.Hint.Render.Preset", 0u);
    NbSetU(P, "DLSSNR.UICorrection", 1u);
    NbApplyStyleControls(P);

    void* hnd = nullptr;
    const int rc = g_nb.Create(cl, 18, g_nb.params, &hnd);
    NgxLog("CreateFeature(18) -> 0x%X handle=%p (%ux%u)", (unsigned)rc, hnd, w, h);
    if (rc != NGX_OK || !hnd) { g_nb.ok = false; return false; }
    g_nb.handle = hnd;
    g_nb.w = w; g_nb.h = h;
    return true;
}

// Called every frame. Records into cl only; submission stays with the caller.
// mv may be null (= no motion vectors). Scales are UV ratios, same unit our residual
// reprojection uses, so the core's own verdict (motion: OK / SUSPICIOUS / BAD) can tune them.
extern "C" bool NgxBackendEval(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* mv,
                               unsigned w, unsigned h, bool reset, float mvScaleX, float mvScaleY) {
    if (!g_nb.ok || !g_nb.handle || !cl || !color) return false;
    NgxParam* P = g_nb.params;
    NbSetR(P, "DLSSNR.Color", color);
    NbSetR(P, "DLSSNR.Output", g_nb.out);
    NbSetU(P, "DLSSNR.Enabled", 1u);
    NbSetU(P, "DLSSNR.Width", w);
    NbSetU(P, "DLSSNR.Height", h);
    NbSetU(P, "DLSSNR.DepthInverted", 0u);
    NbSetU(P, "DLSSNR.Reset", reset ? 1u : 0u);
    // colour / output: whole allocation (origin must be zero)
    NbSetU(P, "DLSSNR.ColorSubrectBaseX", 0u);
    NbSetU(P, "DLSSNR.ColorSubrectBaseY", 0u);
    NbSetU(P, "DLSSNR.ColorSubrectWidth", w);
    NbSetU(P, "DLSSNR.ColorSubrectHeight", h);
    NbSetU(P, "DLSSNR.OutputSubrectBaseX", 0u);
    NbSetU(P, "DLSSNR.OutputSubrectBaseY", 0u);
    NbSetU(P, "DLSSNR.OutputSubrectWidth", w);
    NbSetU(P, "DLSSNR.OutputSubrectHeight", h);
    // depth: network takes no depth; dlssnr_core reads it only for the motion-vector lookup when Motion Without Depth=0 (default 1) -> not fed
    NbSetU(P, "DLSSNR.DepthSubrectBaseX", 0u);
    NbSetU(P, "DLSSNR.DepthSubrectBaseY", 0u);
    NbSetU(P, "DLSSNR.DepthSubrectWidth", 0u);
    NbSetU(P, "DLSSNR.DepthSubrectHeight", 0u);
    // motion vectors: whole allocation when present
    NbSetU(P, "DLSSNR.MVecSubrectBaseX", 0u);
    NbSetU(P, "DLSSNR.MVecSubrectBaseY", 0u);
    if (mv) {
        NbSetR(P, "DLSSNR.MVec", mv);
        NbSetU(P, "DLSSNR.MVecSubrectWidth", w);
        NbSetU(P, "DLSSNR.MVecSubrectHeight", h);
        NbSetF(P, "DLSSNR.MVecScaleX", mvScaleX);
        NbSetF(P, "DLSSNR.MVecScaleY", mvScaleY);
    } else {
        NbSetU(P, "DLSSNR.MVecSubrectWidth", 0u);
        NbSetU(P, "DLSSNR.MVecSubrectHeight", 0u);
        NbSetF(P, "DLSSNR.MVecScaleX", 1.0f);
        NbSetF(P, "DLSSNR.MVecScaleY", 1.0f);
    }
    {
        static int saidMv = -1;
        const int now = mv ? 1 : 0;
        if (saidMv != now) { saidMv = now; NgxLog("MV: %s (scale=%.6f)", mv ? "fed" : "not fed", mv ? mvScaleX : 0.0f); }
    }
    NbApplyStyleControls(P);

    const int rc = g_nb.Eval(cl, g_nb.handle, g_nb.params, nullptr);
    static int bad = 0;
    if (rc != NGX_OK) {
        if ((bad++ % 60) == 0) NgxLog("EvaluateFeature -> 0x%X (failure #%d)", (unsigned)rc, bad);
        return false;
    }
    return true;
}

extern "C" void NgxBackendSetControls(float intensity, unsigned style, float localTone,
                                      float localStruct, float skinStruct, unsigned useAutoMask) {
    const float ni = NbClampControl(intensity, 0.0f, 2.0f, 1.0f);
    const unsigned ns = (style <= 2u) ? style : 0u;
    const float nt = NbClampControl(localTone, 0.0f, 2.0f, 1.0f);
    const float nst = NbClampControl(localStruct, 0.0f, 2.0f, 1.0f);
    const float nsk = NbClampControl(skinStruct, -1.0f, 1.0f, -1.0f);
    const unsigned nm = useAutoMask ? 1u : 0u;
    const bool changed = (ni != g_nb.intensity || ns != g_nb.style || nt != g_nb.localTone ||
                          nst != g_nb.localStruct || nsk != g_nb.skinStruct || nm != g_nb.useAutoMask);
    g_nb.intensity = ni;
    g_nb.style = ns;
    g_nb.localTone = nt;
    g_nb.localStruct = nst;
    g_nb.skinStruct = nsk;
    g_nb.useAutoMask = nm;
    if (changed) {
        NgxLog("controls: intensity=%.2f style=%u tone=%.2f struct=%.2f skin=%.2f automask=%u",
               ni, ns, nt, nst, nsk, nm);
    }
}

extern "C" void* NgxBackendOutput() { return (void*)g_nb.out; }
extern "C" bool  NgxBackendActive() { return g_nb.ok && g_nb.handle != nullptr; }

extern "C" void NgxBackendShutdown() {
    if (g_nb.handle && g_nb.Release) g_nb.Release(g_nb.handle);
    g_nb.handle = nullptr;
    if (g_nb.Shutdown1 && g_nb.dev) g_nb.Shutdown1(g_nb.dev);
    g_nb.out = nullptr;
    g_nb.ok = false;
    NgxLog("shutdown");
}
