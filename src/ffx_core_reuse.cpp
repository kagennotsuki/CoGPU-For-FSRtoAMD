// libxess.dll 代理 v9 —— 方案 B：槽位队列 + 延迟呈现
//
// 目标：让整条链（NR + 上采样器）按 NR 的节奏跑，宁可变慢，也不丢帧、不错位。
//
// 每帧流程（K=4 槽位，延迟 3 帧）：
//   ① [CPU] 阻塞等待 slot[N-3] 的答案（无条件等待）
//   ② [CPU] Map 上一帧记录的降采样回读 → 喂 worker（对应 slot[N-1]）
//   ③ [GPU] 把本帧原始 pColor / MV / depth 拷进 slot[N]
//   ④ [GPU] 把 slot[N-1] 的 color 降采样到 1080p → 回读到缓冲
//   ⑤ [GPU] 把 slot[N-3] 的残差叠回它的原图 → g_pre → 写进 pColor
//   ⑥ 调真 XeSS，传入 slot[N-3] 的 MV/depth/jitter（关键：让上采样器看到自洽序列）
//
// 残差处理参考 AMDNR：限幅 0.2 / 边缘保护 0.6 / 输出平滑 0.8

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <tlhelp32.h>
#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include <wrl/client.h>
#include <dcomp.h>              // S1 镜像叠层：合成交换链 + DComp 视觉（不建独立窗口，不抢焦点）
#include <d3d11.h>              // 仅用于「D3D12 设备不能当 IDXGIDevice」时的降级路径
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3d11.lib")
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <cmath>
#include <share.h>
#include <sys/stat.h>
#include "b_ui_trace.h"
#include "wm_probe_common.h"

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::high_resolution_clock;

extern "C" void* g_xessTab[72] = {};
static const char* kNames[72] = {
#include "xess_fwd_names.inc"
};

static HMODULE g_self = nullptr, g_real = nullptr, g_runtimeBaseMod = nullptr;
extern "C" void CoreSetRuntimeBaseModule(HMODULE mod) { g_runtimeBaseMod = mod; }
static FILE* g_log = nullptr;
static volatile bool g_nrOn = true;
static bool g_disabled = false;
static ComPtr<ID3D12Resource> g_fedTex[8];   // ≥ K（K 上限 8）
// （SRV 槽位映射见 K 声明之后的 kSrv* 常量）
static ComPtr<ID3D12Resource> g_ansLocal;      // S5-4：本地中转 buffer（跨卡 buffer 不能建 SRV）
static inline UINT64 AnsBytes();             // 随 kNetW × kNetH 动态计算
static volatile bool g_useGpuResid = true;   // S5-5：残差改用 GPU 计算（读本地中转 buffer）
static ComPtr<ID3D12Heap> g_xHeapIn, g_xHeapOut, g_xHeapInW, g_xHeapOutW;
static ComPtr<ID3D12Resource> g_xBufIn, g_xBufOut, g_xBufInW, g_xBufOutW;
// NGX backend only: motion-vector cross-card channel (1080p RG16F = 8.3MB). Built only when backend=ngx.
static ComPtr<ID3D12Heap> g_xHeapVel, g_xHeapVelW;
static ComPtr<ID3D12Resource> g_xBufVel, g_xBufVelW;
static ComPtr<ID3D12Fence> g_xFence;
static ComPtr<ID3D12Fence> g_xFenceW;
static bool g_xOk = false;
static volatile bool g_xWrite = true;
static volatile bool g_xRead  = true;
static bool g_cpuIn = false;   // xess_proxy.cpuin.txt=1 → 强制 CPU 回读路径    // R4：worker 从共享 buffer 读输入    // R3：游戏侧是否写共享 buffer
static ComPtr<ID3D12Resource> g_xHdrUp;  // 头部上传缓冲
static const UINT64 XHDR = 512;

static volatile bool g_menuOpen = false;
static long g_cntCompose = 0, g_cntSkip = 0, g_cntMiss = 0;   // 速率统计用
static int  g_feedLagEff = 1;   // 有效喂帧滞后：GPU 快路径=1（内容就是 N-1）；CPU 回读路径=2
static bool g_ringOn = true;   // xess_proxy.ring.txt=0 → 用 RE2 的单段寻址（不做 K 段环）
static volatile int g_triOn = -1;
static volatile int g_baseFromPresent = 0;   // 1 = 用 Present 最终画面作底图
static volatile int g_frameNow = 0;           // 当前帧号（FSR 节点写，Present 钩子读）
static void EnsurePresentHook();
static ID3D12Resource* PresentGrabQuery(int* frameOut, int* w, int* h, int* fmt);
static void InstallVtablePresentHook();
static void DoPendingGrab();
static HRESULT STDMETHODCALLTYPE Detour_Present(IDXGISwapChain* self, UINT sync, UINT flags);
static HRESULT STDMETHODCALLTYPE Detour_Present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp);
static std::vector<ComPtr<ID3D12Resource>> g_imgSlots;   // 每槽一份"最终画面"副本（共享 vtable 钩子写入）
static volatile int g_imgFrame[64] = { 0 };              // 每槽写入的帧号（0 = 还没写过）   // 向同进程 dxgi.dll 索取最终画面   // 三横带视图（F8 切换）
static volatile int g_marqueeOn = -1;   // 跑马灯总开关（-1=未读，1=开，0=关）   // （保留：旧的按帧计数，已不用）
static inline long long QpcNow() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
// ★ 底图行探针：把"合成用的底图"的中间那一行拷到回读缓冲，下一帧在 CPU 上判黑条位置
static ComPtr<ID3D12Resource> g_rowRb;      // 1920x1 FP16 回读
static UINT g_rowW = 0;   // 底图行探针本次实际读取的像素宽度
static int  g_rowPendingSlot = -1;          // 有待读的槽号（-1 = 无）
static int  g_rowPendingFrame = -1;
static long g_cntPaint = 0, g_cntPaintSkip = 0, g_cntPaintNoUav = 0, g_cntPaintNoPso = 0;   // 跑马灯涂标计数   // 菜单是否打开（提前声明，供 IAT hook 使用）
static volatile float g_uiScale = 1.0f;          // 菜单界面缩放（0.6~2.5）
static ImGuiStyle g_baseStyle;                    // 未缩放的基准样式
static float g_uiApplied = -1.0f;                 // 已应用的缩放值
static volatile float g_transfer = 1.0f;          // 官方细节强度 0..1（+64）
static volatile float g_colorStr = 1.0f;          // 官方色彩强度 0..1（+68）
static volatile bool  g_dejitter = false;      // NR 输入去抖动（用该帧 jitter 反向偏移降采样）
static volatile float g_dejitterSign = 1.0f;   // 符号：+1 或 -1，试错用
static volatile float g_curJitX = 0.0f, g_curJitY = 0.0f;   // 本帧 jitter（像素）
static volatile float g_jitterScale = 0.5f;   // 实测 0.5 为闪/锐度折中点（1.0 抖、0 太软）   // XeSS jitter 缩放（实验：0=不抖，稳定但无时域增益）
static volatile int   g_debugView = 0;            // 0=正常 1=代理 2=纯网络 3=差异x20 4=着色
static volatile bool  g_bypass = false;           // 直出运行时输出（配合 debug_view 使用）

static std::mutex g_logMtx;   // ★ Log 可能被多个线程（渲染/Present/worker）调用
static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// BTRACE1 的发射者只有启动时的 SESSION 和之后的 HotkeyThread。
// 每批最多 BATCH 条轨迹加 STATS / LOSS；先无锁攒批，和普通日志只在一次 fwrite/fflush 时串行。
static char g_btLogBatch[(BTRACE1_FLUSH_BATCH + 2) * BTRACE1_REC_MAX];
static size_t g_btLogUsed = 0;
static void FlushBtLogBatch() {
    if (!g_btLogUsed) return;
    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_log) {
        fwrite(g_btLogBatch, 1, g_btLogUsed, g_log);
        fflush(g_log);
    }
    g_btLogUsed = 0;
}
static void StageBtLogLine(const char* line) {
    const size_t bytes = strlen(line);
    if (bytes >= sizeof(g_btLogBatch)) { FlushBtLogBatch(); Log("%s", line); return; }
    if (bytes + 1 > sizeof(g_btLogBatch) - g_btLogUsed) FlushBtLogBatch();
    memcpy(g_btLogBatch + g_btLogUsed, line, bytes);
    g_btLogUsed += bytes;
    g_btLogBatch[g_btLogUsed++] = '\n';
}
static std::string ModuleDir() {
    char p[MAX_PATH] = { 0 };
#ifdef COGPU_ASI_BUILD
    HMODULE dirMod = g_runtimeBaseMod;
#else
    HMODULE dirMod = g_self;
#endif
    DWORD n = GetModuleFileNameA(dirMod, p, MAX_PATH);
    if (!n) return std::string();
    std::string s(p); size_t q = s.find_last_of("\\/");
    return (q == std::string::npos) ? std::string() : s.substr(0, q + 1);
}
static void OpenLog() {
    if (g_log) return;
    std::string s = ModuleDir() + "xess_proxy.log";
    g_log = _fsopen(s.c_str(), "w", _SH_DENYNO);
}

// ============ dlssnr-amd.ini 的 [Preprocess] 读写 ============
//   ★ 实测 2026-10-31：核心会监视 dlssnr-amd.ini，外部改写后约 1 秒内热生效，日志打
//     [nr] preprocess on: exposure auto, ExposureBias +0.00 EV, curve aces, contrast 1.00, saturation 1.20
//     ⇒ 菜单只需要"把键写进文件"，不用通知核心、不用重建网络。
//   ★ 这条是 NGX 后端下【唯一】能动到画面的参数面（风格组在 OptiScaler 路由上不可控，已实测）。
static bool IniReadAll(const std::string& path, std::string& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    out.assign((size_t)n, '\0');
    size_t got = n ? fread(&out[0], 1, (size_t)n, f) : 0;
    fclose(f);
    out.resize(got);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}
static std::string IniTrim(std::string s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
static bool IniGetValue(const std::string& text, const char* section, const char* key, std::string& val) {
    std::string want = std::string("[") + section + "]";
    bool inSec = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = IniTrim(text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos));
        if (!line.empty() && line[0] == '[') {
            inSec = (_stricmp(line.c_str(), want.c_str()) == 0);
        } else if (inSec && !line.empty() && line[0] != ';') {
            size_t eq = line.find('=');
            if (eq != std::string::npos && _stricmp(IniTrim(line.substr(0, eq)).c_str(), key) == 0) {
                val = IniTrim(line.substr(eq + 1));
                return true;
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return false;
}
// 改写 [section] 里的 key；返回是否写成功（写盘用临时文件 + 原子替换，避免核心读到半个文件）
static bool IniSetValue(const char* section, const char* key, const std::string& val) {
    const std::string path = ModuleDir() + "dlssnr-amd.ini";
    std::string text;
    if (!IniReadAll(path, text)) { Log("菜单(PP): 读不到 dlssnr-amd.ini"); return false; }
    std::string want = std::string("[") + section + "]";
    // 拆行（保留原行内容，不改动别的键与注释）
    std::vector<std::string> lines; std::string eol = "\r\n";
    {
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            std::string line;
            if (nl == std::string::npos) { line = text.substr(pos); pos = text.size() + 1; }
            else { line = text.substr(pos, nl - pos); pos = nl + 1; }
            if (!line.empty() && line.back() == '\r') { line.pop_back(); }
            lines.push_back(line);
            if (nl == std::string::npos) break;
        }
        if (lines.size() >= 2 && lines.back().empty()) lines.pop_back();
    }
    int secIdx = -1, secEnd = (int)lines.size();
    for (int i = 0; i < (int)lines.size(); ++i) {
        std::string t = IniTrim(lines[i]);
        if (!t.empty() && t[0] == '[') {
            if (secIdx >= 0) { secEnd = i; break; }
            if (_stricmp(t.c_str(), want.c_str()) == 0) secIdx = i;
        }
    }
    if (secIdx < 0) { Log("菜单(PP): ini 里没有 [%s] 段，跳过", section); return false; }
    int hit = -1;
    for (int i = secIdx + 1; i < secEnd; ++i) {
        std::string t = IniTrim(lines[i]);
        if (t.empty() || t[0] == ';') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (_stricmp(IniTrim(t.substr(0, eq)).c_str(), key) == 0) { hit = i; break; }
    }
    std::string newLine = std::string(key) + " = " + val;
    if (hit >= 0) lines[hit] = newLine;
    else lines.insert(lines.begin() + secEnd, newLine);
    std::string out;
    out.reserve(text.size() + 64);
    for (size_t i = 0; i < lines.size(); ++i) { out += lines[i]; out += eol; }
    std::string tmp = path + ".tmp-menu";
    FILE* f = nullptr;
    if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) { Log("菜单(PP): 写临时文件失败"); return false; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp.c_str());
        Log("菜单(PP): 替换 dlssnr-amd.ini 失败 err=%lu", GetLastError());
        return false;
    }
    return true;
}

static bool  g_ppOn = true;
static int   g_ppExpMode = 1;       // 0 off / 1 auto / 2 fixed
static float g_ppBias = 0.0f;
static int   g_ppCurve = 3;         // 0 none /1 neutral /2 reinhard /3 filmic /4 gt /5 aces /6 agx
static float g_ppContrast = 1.0f;
static float g_ppSat = 1.0f;
static bool  g_ppLoaded = false;
static const char* kPpExpName[3] = { "off", "auto", "fixed" };
static const char* kPpCurveName[7] = { "none", "neutral", "reinhard", "filmic", "gt", "aces", "agx" };

static void LoadPreprocessFromIni() {
    g_ppLoaded = true;
    std::string t, v;
    if (!IniReadAll(ModuleDir() + "dlssnr-amd.ini", t)) return;
    if (IniGetValue(t, "Preprocess", "Enabled", v)) g_ppOn = (v == "1");
    if (IniGetValue(t, "Preprocess", "Exposure", v)) {
        g_ppExpMode = 1;
        for (int i = 0; i < 3; ++i) if (_stricmp(v.c_str(), kPpExpName[i]) == 0) g_ppExpMode = i;
    }
    if (IniGetValue(t, "Preprocess", "ExposureBias", v)) g_ppBias = (float)atof(v.c_str());
    if (IniGetValue(t, "Preprocess", "Curve", v)) {
        g_ppCurve = 3;
        for (int i = 0; i < 7; ++i) if (_stricmp(v.c_str(), kPpCurveName[i]) == 0) g_ppCurve = i;
    }
    if (IniGetValue(t, "Preprocess", "Contrast", v)) g_ppContrast = (float)atof(v.c_str());
    if (IniGetValue(t, "Preprocess", "Saturation", v)) g_ppSat = (float)atof(v.c_str());
}
static void EnsurePreprocessLoadedFromIni() {
    if (!g_ppLoaded) LoadPreprocessFromIni();
}
static void SavePreprocessToIni() {
    char buf[32];
    const int em = (g_ppExpMode >= 0 && g_ppExpMode < 3) ? g_ppExpMode : 1;
    const int cv = (g_ppCurve >= 0 && g_ppCurve < 7) ? g_ppCurve : 3;
    IniSetValue("Preprocess", "Enabled", g_ppOn ? "1" : "0");
    IniSetValue("Preprocess", "Exposure", kPpExpName[em]);
    snprintf(buf, sizeof(buf), "%.2f", g_ppBias);
    IniSetValue("Preprocess", "ExposureBias", buf);
    IniSetValue("Preprocess", "Curve", kPpCurveName[cv]);
    snprintf(buf, sizeof(buf), "%.2f", g_ppContrast);
    IniSetValue("Preprocess", "Contrast", buf);
    snprintf(buf, sizeof(buf), "%.2f", g_ppSat);
    IniSetValue("Preprocess", "Saturation", buf);
}

static HMODULE RealXess() {
    if (g_real) return g_real;
    std::string s = ModuleDir() + "libxess_orig.dll";
    g_real = LoadLibraryA(s.c_str());
    if (!g_real) Log("!! 加载失败 err=%lu", GetLastError());
    return g_real;
}
static void InitForwarders() {
    HMODULE m = RealXess();
    for (int i = 0; i < 72; ++i) {
        g_xessTab[i] = m ? (void*)GetProcAddress(m, kNames[i]) : nullptr;
        if (!g_xessTab[i]) Log("!! 缺少导出: %s", kNames[i]);
    }
}

// ---------------- lmxxf ABI ----------------
struct ApiTable {
    uint32_t struct_size; uint32_t abi_version;
    int32_t (*QueryCapabilities)(void*);
    int32_t (*Create)(const void*, void**);
    int32_t (*Destroy)(void*);
    int32_t (*PrepareSession)(void*);
    int32_t (*PrepareFrame)(void*, const void*, void*);
    int32_t (*RecordInputs)(void*, void*, void*);
    int32_t (*EnqueueHip)(void*, void*, void*);
    int32_t (*RecordOutputs)(void*, void*, void*);
    int32_t (*ExecuteAfterProducer)(void*, void*, void*);
    int32_t (*CancelUnsubmitted)(void*, void*);
    int32_t (*Poll)(void*, void*, uint32_t*);
    int32_t (*Retire)(void*, void*);
    int32_t (*ResetHistory)(void*);
    int32_t (*Drain)(void*);
    int32_t (*GetStatus)(void*, char*, uint32_t);
    int32_t (*GetLastError)(char*, uint32_t);
    int32_t (*GetTimings)(void*, void*);
    void* s18; void* s19;
};
struct FrameInfo160 {
    uint32_t struct_size; uint32_t pad0;
    uint64_t session_id, frame_id, list_generation;
    void* command_list;
    uint32_t color_width, color_height;
    void* color;
    uint32_t color_state, flags;
    float transfer_strength, color_strength;
    uint32_t debug_view; float model_scale;
    void* exposure; uint32_t exposure_state;
    float pre_exposure, exposure_scale;
    void* depth; void* velocity;
    float ctlTone, ctlStructure, ctlSkin, ctlOther, ctlStyle;
    uint32_t ctlPad;
    void* historyMask; void* reserved152;
};
static_assert(sizeof(FrameInfo160) == 160, "");
struct Job { uint32_t struct_size; void* handle; void* private_output; };
struct CreateInfo { uint32_t struct_size; uint32_t pad0; void* device; void* queue; const wchar_t* assets_directory; uint32_t flags; uint32_t pad1; };
static_assert(sizeof(CreateInfo) == 40, "CreateInfo 必须 40 字节（对齐官方 LmxxfNrCreateInfo）");

// ---- 官方 include/LmxxfNrApi.h 的 ABI 常量（2026-10-26 核对，见知识库 §11.2）----
// 表：v1 = 136（2×u32 + 16 个函数指针）；当前 = 144（尾部追加 GetTimings）
// FrameInfo：v1 = 80（止于 model_scale）；当前 = 104（追加 exposure/exposure_state/pre_exposure/exposure_scale）
static const uint32_t kApiSizeCurrent   = 144u;
static const uint32_t kApiSizeV1        = 136u;
static const uint32_t kFrameSizeCurrent = 104u;
static const uint32_t kFrameSizeV1      = 80u;

struct Capabilities {   // LmxxfNrCapabilities
    uint32_t struct_size; uint32_t abi_version;
    uint32_t max_input_width; uint32_t max_input_height;
    uint32_t history_supported, overlap_supported, graph_supported, hip_ready, gfx1201_target;
};

static UINT kNetW = 1920, kNetH = 1080;
static inline UINT64 AnsBytes() { return (UINT64)kNetW * (UINT64)kNetH * 4ull; }
static volatile int  g_nrScaleMode = 0;      // 0=1080p, 1=50%, 2=67%, 3=75%, 4=100%, 5=720p, 6=900p, 7=1440p
static std::atomic<bool> g_netRebuilding{false};
static void ComputeNetDims(int mode, UINT refW, UINT refH, UINT& outW, UINT& outH) {
    switch (mode) {
    case 5: outW = 1280; outH = 720; return;
    case 6: outW = 1600; outH = 900; return;
    case 7: outW = 2560; outH = 1440; return;
    case 1: case 2: case 3: case 4: {
        if (refW < 320 || refH < 180) { refW = 1920; refH = 1080; }
        static const float kPct[4] = { 0.50f, 0.6667f, 0.75f, 1.00f };
        float sc = kPct[mode - 1];
        UINT rw = (UINT)lroundf((float)refW * sc);
        UINT rh = (UINT)lroundf((float)refH * sc);
        rw = ((rw + 32u) / 64u) * 64u;
        rh = ((rh + 4u) / 8u) * 8u;
        if (rw < 640u)  rw = 640u;  if (rw > 2560u) rw = 2560u;
        if (rh < 360u)  rh = 360u;  if (rh > 1440u) rh = 1440u;
        outW = rw; outH = rh; return;
    }
    default: outW = 1920; outH = 1080; return;
    }
}
// FP16 → float（底图行探针用）
static inline float HalfToFloat(uint16_t h) {
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    if (e == 0) return (m == 0) ? 0.0f : ((s ? -1.0f : 1.0f) * ((float)m / 1024.0f) * powf(2.0f, -14.0f));
    if (e == 31) return m ? 0.0f : (s ? -1e30f : 1e30f);
    float v = (1.0f + (float)m / 1024.0f) * powf(2.0f, (float)e - 15.0f);
    return s ? -v : v;
}

#include "f9_frame_identity.h"

static const int  K = 6;                       // 槽位数（与 RE2 完全一致）
// 呈现延迟固定 3、喂帧滞后固定 2 —— 这两个不是"可调参数"，是结构事实：
//   · 降采样在帧 N 录制、结果要帧 N+1 才能从回读缓冲读出 ⇒ 能拿到的最新小图永远是第 N-2 帧的
//     （实测把滞后写成 1 只是标签改错：内容仍是 N-2、答案被写进 N-1 ⇒ 错位，而等待均值照样 0.8ms）
//   · 再由 kDelay ≥ 滞后+1 且 kDelay ≤ K-1（K=4）⇒ kDelay 只能是 3
// 要余量（kDelay − 滞后）> 1 必须先加槽位（主文档 §56）
// ---- SRV/UAV 槽位映射：全部按 K 算，禁止写死（K>4 时旧版会撞车）----
// 颜色 [0,K) / 残差 K / netS K+1 / pre K+2 / smooth K+3 / MV [K+4,2K+3) / fed [2K+4,3K+3) / 答案 3K+4
static const UINT kSrvRes     = (UINT)K;
static const UINT kSrvNetS    = (UINT)K + 1;
static const UINT kSrvPre     = (UINT)K + 2;
static const UINT kSrvSmooth  = (UINT)K + 3;
static const UINT kSrvMvBase  = (UINT)K + 4;
static const UINT kSrvFed     = 2u * (UINT)K + 4u;
static const UINT kSrvAns     = 3u * (UINT)K + 4u;
static const UINT kSrvCount   = 3u * (UINT)K + 5u;
// ---- 路线 B / B1：残差改在 4K 上叠加 ----
//   kSrvOutp    : outp 的 4K SRV（Add4K 的底图）
//   kSrvPreSrv  : g_pre 的 SRV（合成后的 pColor = 槽位原图 + e）
//   kSrvPre4K   : g_pre4k 的 4K UAV（叠加结果，之后拷回 outp）
static const UINT kSrvOutp    = kSrvCount;
static const UINT kSrvPreSrv  = kSrvCount + 1u;
static const UINT kSrvPre4K   = kSrvCount + 2u;
static const UINT kSrvMvCur   = kSrvCount + 3u;    // 当前帧原始 MV 的 SRV（打包用）
static const UINT kSrvMvPacked= kSrvCount + 4u;    // 打包后的 MV（1080p RG16F）UAV
static const UINT kSrvCount2  = kSrvCount + 5u;
static const UINT kSrvOutp8   = kSrvCount2;        // 4K 后置：8bit 转换目标（UAV）
static const UINT kSrvCount3  = kSrvCount2 + 1u;
static const UINT kSrvOutp8b  = kSrvCount3;        // 第二缓冲的 UAV（双缓冲必须各一个描述符）
static const UINT kSrvCount4  = kSrvCount3 + 1u;
// 跑马灯：涂标记用的临时 UAV 槽（每帧把当前槽位底图挂上去，用完即弃）
static const UINT kSrvMark    = kSrvCount4;
static const UINT kSrvBbSrc0  = kSrvCount4 + 1u;   // C4-S3: T_src[0] SRV（28/10 转换源）
static const UINT kSrvBbSrc1  = kSrvCount4 + 2u;   // C4-S3: T_src[1] SRV（双缓冲）
static const UINT kSrvBbSrc2  = kSrvCount4 + 3u;   // 2026-11-13 c4srv：第 3 个 T_src SRV（与 C4 三套 allocator/list 一一对应，按套号 ai 索引）
static const UINT kSrvOutp8c  = kSrvCount4 + 4u;   // 2026-11-14 c4outp3：第 3 个 outp8 UAV（三套 C4 allocator 对齐）
static const UINT kSrvA2Mv = kSrvCount4 + 5u;       // dedicated A2 MV scratch, never B/mvpair
static volatile int  g_kDelay = 3;                // 呈现延迟（帧），固定 3
static const int     kFeedLag = 2;                // 喂帧滞后（结构固定）
#include <memory>
#include "adapter_frame_desc.h"
#include "a2_input.h"
static volatile bool g_kDelayAuto = true;         // 保留字段：现在不再改 kDelay

// ---------------- backend select: lmxxf (default) / NGX feature 18 ----------------
// Only when xess_proxy.backend.txt says "ngx". Missing file or anything else = today's behaviour.
// The backend itself lives in ngx_f18_backend.cpp (load + hand over textures + Evaluate).
extern "C" bool NgxBackendLoad(ID3D12Device* dev);
extern "C" bool NgxBackendEnsure(ID3D12GraphicsCommandList* cl, unsigned w, unsigned h, DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM);
extern "C" bool NgxBackendEval(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* mv,
                               unsigned w, unsigned h, bool reset, float mvScaleX, float mvScaleY);
extern "C" void NgxBackendSetControls(float intensity, unsigned style, float localTone,
                                      float localStruct, float skinStruct, unsigned useAutoMask);
extern "C" void* NgxBackendOutput();
extern "C" DXGI_FORMAT NgxBackendFormat();
extern "C" bool  NgxBackendActive();
static bool g_backendNgx = false;
static volatile LONG  g_c4On = 0;                   // xess_proxy.c4.txt = 1 才跑（ProxyExecute init 读一次）
static bool g_backendRead = false;
static void BackendPollOnce() {
    if (g_backendRead) return;
    g_backendRead = true;
    std::string p = ModuleDir() + "xess_proxy.backend.txt";
    FILE* f = nullptr; char b[16] = { 0 };
    if (fopen_s(&f, p.c_str(), "r") == 0 && f) {
        if (fgets(b, sizeof(b), f)) {
            if (_strnicmp(b, "ngx", 3) == 0) g_backendNgx = true;
        }
        fclose(f);
    }
    Log("后端: %s", g_backendNgx ? "NGX feature 18（dlssnr_core.dll）" : "lmxxf/mochizuki（默认）");
}

// ---------------- worker ----------------
enum NgxBatch { NgxEval, NgxReadback, NgxCompose, NgxBatchCount };
static constexpr DWORD kWorkerWaitMs = 30;
enum A1ResetReason : unsigned {
    A1GameReset = 1u, A1UiResume = 2u, A1Rebuild = 4u, A1SwitchA = 8u,
};
// All accesses under Worker::m. The epoch survives acknowledgement; every new event
// invalidates older inputs even if it only repeats a reason already pending.
struct A1HistoryReset {
    uint64_t epoch = 0;
    unsigned reasons = 0;
    int lastEventFrame = -1;
    int chainGen = 0;
    bool inA = true;

    void Raise(A1ResetReason reason, int frame) {
        ++epoch;
        reasons |= reason;
        lastEventFrame = frame;
    }
    bool Current(int gen, uint64_t feedEpoch) const { return gen == chainGen && feedEpoch == epoch; }
    unsigned Pending(int gen, uint64_t feedEpoch) const { return inA && Current(gen, feedEpoch) ? reasons : 0; }
    bool Ack(int gen, uint64_t feedEpoch) {
        if (!Pending(gen, feedEpoch)) return false;
        reasons = 0;
        return true;
    }
};
struct Worker {
    std::thread th; std::mutex m; std::condition_variable cv; std::condition_variable cvOut;
    std::condition_variable pausedCv;
    bool pauseReq = false, pausedAck = false;
    std::vector<uint8_t> inBuf, outBuf;
    bool hasIn = false, hasOut = false, quit = false;
    int inIndex = -1, outIndex = -1;
    int inFrame = -1;                 // 这一份输入对应哪一帧（身份校验用）
    int f9Dispatch = -1;              // 诊断：CPU 喂入请求所在的 A dispatch
    bool inHasAPixels = false;        // B 唤醒可只携带帧身份，不携带 A 像素
    int inAGen = -1;                  // A 喂帧代际，与 hasIn 一起由 m 保护
    uint64_t inAResetEpoch = 0;       // 捕获链的复位代际，不是在 worker 解包时重新取值
    A2In::Token inA2;                  // with hasIn under m
    bool inA3 = false;                 // notification; no fixed A input frame
    A1HistoryReset aReset;
    std::vector<std::vector<uint8_t>> ansBuf;   // [K] 每槽一份答案（身份化交付）
    std::vector<int> ansFrame;                  // [K] 该槽答案对应的帧号，-1 = 还没算过
    std::vector<int> ansAGen;                   // [K] 仅由使用 A 输入的 NGX 答案写入
    int processed = 0; double lastMs = 0;
    ComPtr<ID3D12Device> dev; ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al; ComPtr<ID3D12CommandAllocator> al2;   // ★ 双分配器轮转（见 ai/aiFv）
    ComPtr<ID3D12GraphicsCommandList> cl;
    struct NgxSubmission {
        ComPtr<ID3D12CommandAllocator> al;
        ComPtr<ID3D12GraphicsCommandList> cl;
        UINT64 lastFv = 0;
    } ngx[NgxBatchCount];
    HANDLE waitEvent = nullptr;
    UINT64 waitEventFv = 0;  // A timeout does not cancel SetEventOnCompletion's registration.
    HANDLE a3WakeEvent = nullptr;
    HANDLE a3ProdEvents[A2In::kSlots]{};
    ID3D12Fence* a3ProdFences[A2In::kSlots]{};
    UINT64 a3ProdEventSeq[A2In::kSlots]{};
    ~Worker() {
        A2In::a3WakeEvent.store(nullptr, std::memory_order_release);
        if (waitEvent) CloseHandle(waitEvent);
        if (a3WakeEvent) CloseHandle(a3WakeEvent);
        for (HANDLE h : a3ProdEvents) if (h) CloseHandle(h);
    }
    int      ai = 0;                     // 本轮用哪一个（0/1 轮转）
    UINT64   aiFv[2] = { 0, 0 };         // 该分配器上次提交后的围栏值（复用前要确认已完成）
    ID3D12CommandAllocator* curAl = nullptr;
    ComPtr<ID3D12Fence> fe; UINT64 fv = 0;
    ComPtr<ID3D12Fence> xferFe;   // 跨卡共享围栏（A 的 4K 拷贝完成 → B 解码前等它）
    ComPtr<ID3D12Resource> color, colorFp16, up, rb, rbPriv;
    ComPtr<ID3D12Resource> vel;   // NGX only: 1080p RG16F motion vectors (shared, the core imports it)
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}, fpPriv{};
    UINT64 bytes = 0, bytesPriv = 0;
    void* upPtr = nullptr;
    HMODULE mod = nullptr; ApiTable api{}; void* ctx = nullptr; bool ok = false;
    bool extAbi = true;   // true = 144 字节表（当前 lmxxf）；false = 官方 v1 / 136（mochizuki 等）
    uint32_t frameSize = 160;   // FrameInfo 声明尺寸，跟随被接受的 ABI（160 / 104 / 80）
    std::wstring assetsDir;     // 建会话用的资源目录（重建会话时复用）
    bool isMochi = false;       // 后端身份：mochizuki / lmxxf 系
    int  modelApplied = -1;     // 当前会话对应哪个模型请求（1=Original, 0=Small）
    void* setControls = nullptr;   // MochizukiNrSetControls（不在 ApiTable 里，单独 GetProcAddress）
    D3D12_RESOURCE_STATES ngxColState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES ngxColFp16State = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES ngxMvState  = D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12Resource> bMvZeroUp;
    UINT ngxMvDimW = 0, ngxMvDimH = 0;
    float ngxMvX = 1920.0f, ngxMvY = 1080.0f;
};
static Worker g_worker;
static char g_statusLine[256] = { 0 };   // 运行时自报状态（GetStatus），菜单里显示
static volatile int g_modelSel = 1;      // 模型请求：1 = Original 71-block，0 = 31-block Small
// ---- mochizuki 风格化控制块（S4）：布局来自交接包 §九.3（反汇编 parse 得到）----
// 0x00 size(0x20C) / 0x08 intensity / 0x0C style / 0x10 tone / 0x14 structure /
// 0x18 skin / 0x1C automask / 0x20 grade / 0x2C 总闸（必须非 0，否则整块无效）
static volatile float g_nrIntensity = 1.0f, g_nrTone = 1.0f, g_nrStruct = 1.0f;
static volatile float g_nrSkin = -1.0f, g_nrGrade = 2.0f;
static volatile int   g_nrStyle = 0;                 // 0 Default / 1 Natural / 2 Cinematic
static volatile bool  g_nrAutoMask = false, g_nrCtrlOn = true;
static volatile bool  g_ctrlDirty = true;            // 让 worker 下一帧重新下发

// ============ BTRACE1 观测接缝（纯观测，不改 GPU 调用次数/顺序/参数） ============

static volatile long g_workerIter = 0;   // 单调 worker 迭代号（跨恢复仍递增，避免同 frame 误配）
static unsigned int  g_workerDevId = 0;  // ★ D1：worker 适配器 DeviceId（WorkerInit 里既有 GetDesc 顺手记，只读）

static thread_local long        g_tlIter = 0;
static thread_local int         g_tlFrame = -1;
static thread_local const char* g_tlPhase = "?";
static thread_local HRESULT     g_tlResetHr = S_OK;      // list Reset HRESULT
static thread_local HRESULT     g_tlAllocResetHr = S_OK; // allocator Reset HRESULT
static thread_local HRESULT     g_tlCloseHr = S_OK;      // Close HRESULT
static thread_local int         g_tlDecodeWaitEnqueued = -1;  // -1=unknown / 0=NGX 无等待 / 1=lmxxf 有等待

// ============ G0 探针：UIGate 观察全局状态（只读，无日志输出于此） ============
static volatile LONGLONG  g_g0LastTick  = 0;      // 上次 ProxyExecute 调用 tick（只走 InterlockedExchange64）
static volatile LONGLONG  g_g0Seq       = 0;      // 调用序列号（InterlockedIncrement64；读侧不读清）
static volatile LONGLONG  g_g0MaxIv     = 0;      // 本秒最大间隔 ms（入口 CAS 原子 max；热键线程读清）
static volatile LONGLONG  g_g0Count     = 0;      // 本秒调用计数（热键线程读清）
static volatile LONGLONG  g_g0EarlyNull = 0;      // 本秒 early-null 次数（热键线程读清）
static volatile LONGLONG  g_g0EarlyOff  = 0;      // 本秒 early-off 次数（热键线程读清）
static volatile LONGLONG  g_g0Pass      = 0;      // 本秒正常往下走次数（热键线程读清）
static volatile LONG      g_g0Mode      = 0;      // 0=关闭 1=观察（热键线程重读；非 1 一律当 0）
static volatile LONGLONG  g_g0ThrMs     = 150;    // 间隔阈值 ms（热键线程重读）
// ★ G0 观察版第二轮：配置只在热键线程启动时读一次，跨线程共享只走 Interlocked。
static volatile LONG      g_g0ModeShared = 0;     // 启动时读到的 mode（InterlockedExchange 写入）
static volatile LONGLONG  g_g0ThrMsShared = 0;    // 启动时读到的阈值 ms；0=未初始化（mode≠1 保持 0）
static volatile LONGLONG  g_g0GapLastMs  = 0;     // 生产侧算出的最近一次大空档时长 ms（InterlockedExchange64）
static volatile LONGLONG  g_g0GapEndTick = 0;     // 该空档结束 tick（InterlockedExchange64）
static volatile LONGLONG  g_g0GapCount   = 0;     // 大空档累计次数（InterlockedIncrement64；热键线程只读）

static void BtWorkerExec(const char* phase) {
    Worker& W = g_worker;
    g_tlPhase = phase;
    BtTrace("WORKER_EXECUTE", g_tlFrame, phase,
            "iter=%ld list=0x%llx queue=0x%llx fence=0x%llx side=w fv_pre=%llu reset_hr=0x%08lX allocator_reset_hr=0x%08lX close_hr=0x%08lX",
            g_tlIter, BtPtr(W.cl.Get()), BtPtr(W.q.Get()), BtPtr(W.fe.Get()),
            (unsigned long long)W.fv, (unsigned long)g_tlResetHr, (unsigned long)g_tlAllocResetHr, (unsigned long)g_tlCloseHr);
}

static void BtWaitEnd(Worker& W, UINT64 required, bool result) {
    UINT64 comp = W.fe->GetCompletedValue();
    int removed = (comp == UINT64_MAX) ? 1 : 0;
    BtTrace("WAITW_END", g_tlFrame, g_tlPhase,
            "iter=%ld required=%llu completed=%llu result=%d valid=%d device_removed=%d fence=0x%llx side=w",
            g_tlIter, (unsigned long long)required, (unsigned long long)comp,
            result ? 1 : 0, removed ? 0 : 1, removed, BtPtr(W.fe.Get()));
}

static HRESULT SignalW() {
    Worker& W = g_worker;
    HRESULT hr = W.q->Signal(W.fe.Get(), ++W.fv);
    BtTrace("WORKER_SIGNAL", g_tlFrame, g_tlPhase, "iter=%ld fence=0x%llx side=w value=%llu hr=0x%08lX",
            g_tlIter, BtPtr(W.fe.Get()), (unsigned long long)W.fv, (unsigned long)hr);
    return hr;
}

static bool WaitWFence(UINT64 required, DWORD ms) {
    Worker& W = g_worker;
    if (!W.fe) return false;
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        const UINT64 completed = W.fe->GetCompletedValue();
        if (completed == UINT64_MAX || completed >= required || Clock::now() >= deadline) break;
        if (!W.waitEvent) W.waitEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!W.waitEvent || W.waitEventFv > required) { SwitchToThread(); continue; }
        // Never register a second target on an event that can still receive an old completion.
        if (!W.waitEventFv) {
            if (FAILED(W.fe->SetEventOnCompletion(required, W.waitEvent))) { SwitchToThread(); continue; }
            W.waitEventFv = required;
        }
        const auto now = Clock::now();
        if (now >= deadline) break;
        const DWORD left = (DWORD)std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        const DWORD wr = WaitForSingleObjectEx(W.waitEvent, left, FALSE);
        if (wr == WAIT_OBJECT_0) W.waitEventFv = 0;
        else if (wr != WAIT_TIMEOUT) break;
    }
    const UINT64 completed = W.fe->GetCompletedValue();
    const bool ok = completed != UINT64_MAX && completed >= required;
    if (!ok) {
        static unsigned missed = 0;
        if ((missed++ % 20) == 0)
            Log("!! worker fence wait failed (%u, required=%llu completed=%llu timeout_ms=%lu removed=%d)",
                missed, (unsigned long long)required, (unsigned long long)completed,
                (unsigned long)ms, completed == UINT64_MAX ? 1 : 0);
    }
    BtWaitEnd(W, required, ok);
    return ok;
}

static bool WaitW() {
    Worker& W = g_worker;
    if (SUCCEEDED(SignalW())) return WaitWFence(W.fv, kWorkerWaitMs);
    BtWaitEnd(W, W.fv, false);
    return false;
}

static bool ResetNgxBatch(Worker& W, NgxBatch batch, const char* phase) {
    auto& sub = W.ngx[batch];
    g_tlPhase = phase;
    if (sub.lastFv && !WaitWFence(sub.lastFv, kWorkerWaitMs)) return false;
    W.cl = sub.cl;
    g_tlAllocResetHr = sub.al->Reset();
    g_tlResetHr = SUCCEEDED(g_tlAllocResetHr) ? W.cl->Reset(sub.al.Get(), nullptr) : E_PENDING;
    if (SUCCEEDED(g_tlResetHr)) return true;
    BtTrace("WORKER_RESET_SKIP", g_tlFrame, phase,
            "iter=%ld allocator_reset_hr=0x%08lX reset_hr=0x%08lX",
            g_tlIter, (unsigned long)g_tlAllocResetHr, (unsigned long)g_tlResetHr);
    return false;
}

static bool SubmitNgxBatch(Worker& W, NgxBatch batch, const char* phase) {
    BtWorkerExec(phase);
    ID3D12CommandList* lists[] = { W.cl.Get() };
    W.q->ExecuteCommandLists(1, lists);
    if (FAILED(SignalW())) {
        BtWaitEnd(W, W.fv, false);
        return false;
    }
    W.ngx[batch].lastFv = W.fv;
    return true;
}
static ComPtr<ID3D12Resource> MakeTex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt,
                                      D3D12_RESOURCE_STATES st, bool uav, bool shared = false) {
    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    dev->CreateCommittedResource(&dh, shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE,
                                &td, st, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
    return r;
}

static bool WorkerCreateSession();   // 前向声明（WorkerInit 里调用；定义在 WorkerInit 之后）

static unsigned int g_wantDevId = 0;   // 安装器指定的神经卡 DeviceId（0=未指定）
static bool WorkerAllocNetResources() {
    Worker& W = g_worker;
    if (!W.dev) return false;
    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;

    // mochizuki 是 Vulkan 后端：它要把我们给的 D3D12 纹理导入 Vulkan（CreateSharedHandle），
    // 所以这张输入纹理必须带 D3D12_HEAP_FLAG_SHARED；否则导入失败、首帧就卡住（本机实测现象）。
    W.color = MakeTex(W.dev.Get(), kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, false, true);
    if (g_backendNgx) {
        W.colorFp16 = MakeTex(W.dev.Get(), kNetW, kNetH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON, false, true);
        W.vel = MakeTex(W.dev.Get(), kNetW, kNetH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COMMON, false, true);
        Log("worker: FP16 shared color (%s) | MV texture (%ux%u RG16F shared) %s",
            W.colorFp16 ? "ready" : "FAILED", kNetW, kNetH, W.vel ? "ready" : "FAILED -> MV not fed");
    }
    if (!W.color || (g_backendNgx && !W.colorFp16)) { Log("worker: 纹理失败"); return false; }
    D3D12_RESOURCE_DESC cd = W.color->GetDesc();
    D3D12_RESOURCE_DESC plain = cd; plain.Flags = D3D12_RESOURCE_FLAG_NONE;
    UINT rows = 0; UINT64 rs = 0, total = 0;
    W.dev->GetCopyableFootprints(&plain, 0, 1, 0, &W.fp, &rows, &rs, &total);
    W.bytes = total;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = W.bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    W.up.Reset(); W.upPtr = nullptr;
    W.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(W.up.GetAddressOf()));
    {
        D3D12_RANGE zr{ 0, 0 };
        if (W.up && SUCCEEDED(W.up->Map(0, &zr, &W.upPtr)) && W.upPtr) Log("worker: 上传缓冲持久映射 ok (%ux%u, %llu B)", kNetW, kNetH, (unsigned long long)W.bytes);
    }
    W.inBuf.assign((size_t)W.bytes, 0);
    W.outBuf.assign((size_t)W.bytes, 0);
    W.ansBuf.assign(K, std::vector<uint8_t>((size_t)kNetW * kNetH * 4, 0));
    W.ansFrame.assign(K, -1);
    W.ansAGen.assign(K, -1);
    W.ngxColState = D3D12_RESOURCE_STATE_COMMON;
    W.ngxColFp16State = D3D12_RESOURCE_STATE_COMMON;
    W.ngxMvState  = D3D12_RESOURCE_STATE_COMMON;
    W.bMvZeroUp.Reset();
    W.ngxMvDimW = W.ngxMvDimH = 0;
    return true;
}

static bool WorkerInit() {
    Worker& W = g_worker;
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) return false;
    ComPtr<IDXGIAdapter1> ad;
    {   // 运行时要求 D3D12 设备与 HIP 设备同卡
        // ★ 可选：安装器写入的神经卡选择。支持两种写法：
        //     nr=dev:7550                 按 PCI DeviceId（安装器用这种，简单可靠）
        //     nr=luid:00000000:0001A2B3   按 LUID（同名双卡时更精确）
        //   文件不存在 / 解析失败 / 不匹配 → 完全回落到原有自动选择（0x7550 → 序数 1）
        LUID wantLuid{}; bool haveWant = false; bool haveWantDev = false;
        {
            std::string gp = ModuleDir() + "xess_proxy.gpu.txt";
            FILE* gf = nullptr;
            if (fopen_s(&gf, gp.c_str(), "r") == 0 && gf) {
                unsigned long long hi = 0, lo = 0;
                unsigned int wantDev = 0;
                int gotDev = fscanf_s(gf, "nr=dev:%x", &wantDev);
                if (gotDev == 1 && wantDev) { g_wantDevId = wantDev; haveWantDev = true; }
                else if (fscanf_s(gf, "nr=luid:%llx:%llx", &hi, &lo) == 2) {
                    wantLuid.HighPart = (LONG)hi; wantLuid.LowPart = (DWORD)lo; haveWant = true;
                }
                fclose(gf);
                if (haveWantDev) Log("worker: 读到安装器的神经卡选择 DeviceId=0x%04X", g_wantDevId);
                else if (haveWant) Log("worker: 读到安装器的神经卡选择 LUID=%08X:%08X", (unsigned)wantLuid.HighPart, (unsigned)wantLuid.LowPart);
            }
        }
        ComPtr<IDXGIAdapter1> best, fallback;
        for (UINT i = 0; i < 8; ++i) {
            ComPtr<IDXGIAdapter1> a1;
            if (fac->EnumAdapters1(i, a1.GetAddressOf()) != S_OK) break;
            DXGI_ADAPTER_DESC dd{};
            a1->GetDesc(&dd);
            Log("  worker 候选[%u] = %ls  DeviceId=0x%04X", i, dd.Description, dd.DeviceId);
            if (haveWantDev) { if (dd.DeviceId == g_wantDevId && !best) best = a1; }
            else if (haveWant && (dd.AdapterLuid.HighPart == wantLuid.HighPart) && (dd.AdapterLuid.LowPart == wantLuid.LowPart)) best = a1;
            else if (!haveWant && dd.DeviceId == 0x7550 && !best) best = a1;   // 未指定时按 DeviceId（0x7550 = RX 9070 XT）
        // 不再按名字兜底：显卡可被改名，只有 DeviceId 可靠（0x7550 = Navi48 / RX 9070 XT）
            if (i == 1 && !fallback) fallback = a1;
        }
        ad = best ? best : fallback;
    }
    if (!ad) { Log("worker: 找不到适配器"); return false; }
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(W.dev.GetAddressOf())))) {
        Log("worker: 设备创建失败"); return false;
    }
    {
        DXGI_ADAPTER_DESC dc{};
        ad->GetDesc(&dc);
        g_workerDevId = (unsigned int)dc.DeviceId;   // ★ D1：只记账，不参与任何选择逻辑
        Log("worker: 使用 %ls", dc.Description);
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    W.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(W.q.GetAddressOf()));
    // backend select: only does anything when backend.txt says "ngx"
    BackendPollOnce();
    if (g_backendNgx && !NgxBackendLoad(W.dev.Get())) {
        Log("!! NGX backend failed to load -> falling back to lmxxf/mochizuki");
        g_backendNgx = false;
    }
    if (!W.q) return false;
    if (g_backendNgx) {
        for (auto& sub : W.ngx) {
            if (FAILED(W.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&sub.al))) ||
                FAILED(W.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, sub.al.Get(), nullptr, IID_PPV_ARGS(&sub.cl))) ||
                FAILED(sub.cl->Close())) {
                Log("!! worker: NGX command allocator/list setup failed");
                return false;
            }
        }
    } else {
        W.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(W.al.GetAddressOf()));
        W.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(W.al2.GetAddressOf()));  // ★ 第二组（轮转用）
        if (!W.al || !W.al2 || FAILED(W.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, W.al.Get(), nullptr, IID_PPV_ARGS(W.cl.GetAddressOf()))) ||
            FAILED(W.cl->Close())) return false;
    }
    // mochizuki 的 Windows 路线要把 D3D12 fence 导入 Vulkan timeline semaphore ⇒ fence 必须 SHARED。
    // lmxxf(HIP) 不关心这个标志，加了对它无影响。
    if (FAILED(W.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(W.fe.GetAddressOf())))) return false;
    if (!W.a3WakeEvent) W.a3WakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    A2In::a3WakeEvent.store(W.a3WakeEvent, std::memory_order_release);

    // mochizuki 是 Vulkan 后端：输入纹理与上传缓冲统一由 WorkerAllocNetResources() 按 kNetW×kNetH 创建
    if (!WorkerAllocNetResources()) return false;
    if (g_backendNgx) {
        W.modelApplied = g_modelSel;
        W.ok = true;
        Log("worker: NGX feature 18 后端初始化完成（不加载 LmxxfNrRuntime.dll）");
        return true;
    }

    std::string dir = ModuleDir();
    std::wstring pak(dir.begin(), dir.end()); pak += L"LmxxfNrRuntime.pak";
    std::wstring dllp = pak;
    size_t sl = dllp.find_last_of(L'\\');
    if (sl != std::wstring::npos) dllp = dllp.substr(0, sl + 1);
    dllp += L"LmxxfNrRuntime.dll";
    // 模型请求：先读 xess_proxy.model.txt（"1"/"2"/"3" = Original 单/2/3 遍，"0.5" = Small），
    // 缺省 Original。环境变量在 WorkerCreateSession() 里、Create 之前才写，
    // 因为真正的开关是"用哪个模型建会话"（= AMDNR 的做法：换模型就重建会话）。
    g_modelSel = 1;
    {
        std::string mp = ModuleDir() + "xess_proxy.model.txt";
        FILE* mf = nullptr;
        if (fopen_s(&mf, mp.c_str(), "r") == 0 && mf) {
            char ln[32] = { 0 };
            if (fgets(ln, sizeof(ln), mf)) {
                std::string v(ln);
                while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
                size_t s = v.find_first_not_of(" \t");
                if (s != std::string::npos) v = v.substr(s);
                if (v == "0.5") g_modelSel = 0;
                else if (v == "1" || v == "2" || v == "3") g_modelSel = 1;
            }
            fclose(mf);
            Log("worker: xess_proxy.model.txt = %s → 请求 %s", ln, g_modelSel ? "Original 71-block" : "Small 31-block");
        }
    }
    W.mod = LoadLibraryW(dllp.c_str());
    if (!W.mod) { Log("worker: lmxxf 加载失败（%ls, err=%lu）", dllp.c_str(), GetLastError()); return false; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(W.mod, "LmxxfNrGetApi");
    W.setControls = (void*)GetProcAddress(W.mod, "MochizukiNrSetControls");   // 不在 ApiTable 里
    Log("worker: MochizukiNrSetControls = %p（风格控制块用）", W.setControls);
    if (!getApi) { Log("worker: 导出 LmxxfNrGetApi 缺失"); return false; }

    // ABI 协商：三个已知尺寸，按"最兼容 → 最保守"依次试
    //   160 = 本机 AMDNR 版运行时的扩展表（原基线一直在用，实测被接受）
    //   144 = 上游 lmxxf 当前头文件 sizeof(LmxxfNrApi)（含 GetTimings）
    //   136 = 官方 ABI v1（LMXXF_NR_API_V1_SIZE；mochizuki 等只认这个）
    static const uint32_t kApiSizes[] = { 160u, 144u, 136u };
    int32_t abiRc = -1;
    uint32_t abiTried = 0;
    for (uint32_t sz : kApiSizes) {
        memset(&W.api, 0, sizeof(W.api));
        W.api.struct_size = sz;
        abiRc = getApi(1, &W.api);
        abiTried = sz;
        if (abiRc == 0) break;
    }
    if (abiRc != 0) { Log("worker: GetApi 失败（160/144/136 均被拒 rc=%d）", (int)abiRc); return false; }
    W.extAbi = (abiTried != kApiSizeV1);
    // FrameInfo 尺寸与表尺寸配对：160(AAMDNR 扩展，含 ctl*/historyMask) / 104(上游当前) / 80(v1)
    W.frameSize = (abiTried == 160u) ? 160u : ((abiTried == kApiSizeCurrent) ? kFrameSizeCurrent : kFrameSizeV1);
    Log("worker: ABI 就绪 abi_version=%u 请求=%u 声明 struct_size=%u FrameInfo=%u 扩展=%d",
        W.api.abi_version, abiTried, W.api.struct_size, W.frameSize, (int)W.extAbi);

    // 运行时身份：mochizuki 才走 dlssnr-amd 资源目录（不能只看 ABI 尺寸——136 也可能被 lmxxf 系接受）
    bool isMochi = (GetProcAddress(W.mod, "MochizukiNrGetInfo") != nullptr) ||
                   (GetProcAddress(W.mod, "MochizukiNrSetControls") != nullptr);
    Log("worker: 运行时 = %s", isMochi ? "mochizuki" : "lmxxf/其它");

    // 能力查询：只在扩展 ABI 上调用，v1 路径与稳定基线保持一致
    if (W.extAbi && W.api.QueryCapabilities) {
        Capabilities cap{}; cap.struct_size = (uint32_t)sizeof(Capabilities);
        if (W.api.QueryCapabilities(&cap) == 0)
            Log("worker: caps 输入<=%ux%u history=%u overlap=%u graph=%u hip=%u gfx1201=%u",
                cap.max_input_width, cap.max_input_height, cap.history_supported,
                cap.overlap_supported, cap.graph_supported, cap.hip_ready, cap.gfx1201_target);
        else Log("worker: QueryCapabilities 失败");
    }

    // 资源目录：
    //   · lmxxf 系运行时要求 assets_directory = 【modules 目录】（内含 SHA256SUMS + *.hsaco）
    //     —— 实测：传 pak 文件路径 → "assets_directory is not a directory"；
    //              传游戏目录   → "modules directory needs SHA256SUMS + hsaco"；
    //              传 lmxxf-modules → Create=0 / PrepareSession=0 / weights=1
    //   · mochizuki → dlssnr-amd
    std::wstring assets(dir.begin(), dir.end());
    if (isMochi) {
        std::wstring alt = assets; alt += L"dlssnr-amd";
        if (GetFileAttributesW(alt.c_str()) != INVALID_FILE_ATTRIBUTES) assets = alt;
        else Log("worker: 未找到 dlssnr-amd，mochizuki 将自行解析");
    } else {
        std::wstring mod = assets; mod += L"lmxxf-modules";
        if (GetFileAttributesW(mod.c_str()) != INVALID_FILE_ATTRIBUTES) assets = mod;
        else Log("worker: 未找到 lmxxf-modules，退回模块目录");
    }
    Log("worker: assets 目录 = %ls", assets.c_str());
    W.assetsDir = assets;
    W.isMochi = isMochi;
    return WorkerCreateSession();
}

// 下发 NR 网络控制参数（NGX feature 18 与 mochizuki 共用这一组菜单状态）。
// mochizuki 实测必须在 PrepareSession 之后再调一次，且 0x2C（总闸）非 0 才有效。
static void ApplyMochiControls(bool force = false) {
    Worker& W = g_worker;
    if (!force && !g_ctrlDirty) return;
    if (g_backendNgx) {
        const bool on = g_nrCtrlOn;
        const float fi = on ? g_nrIntensity : 1.0f;
        const unsigned st = (on && g_nrStyle >= 0 && g_nrStyle <= 2) ? (unsigned)g_nrStyle : 0u;
        const float ft = on ? g_nrTone : 1.0f;
        const float fs = on ? g_nrStruct : 1.0f;
        const float fsk = on ? g_nrSkin : -1.0f;
        const unsigned mk = (on && g_nrAutoMask) ? 1u : 0u;
        NgxBackendSetControls(fi, st, ft, fs, fsk, mk);
        g_ctrlDirty = false;
        Log("worker: NGX SetControls | style=%u intensity=%.2f tone=%.2f struct=%.2f skin=%.2f mask=%u enable=%d",
            st, fi, ft, fs, fsk, mk, (int)on);
        return;
    }
    if (!W.isMochi || !W.setControls || !W.ctx) return;
    uint8_t blk[256] = { 0 };
    auto putF = [&](int off, float v) { memcpy(blk + off, &v, 4); };
    auto putU = [&](int off, uint32_t v) { memcpy(blk + off, &v, 4); };
    putU(0x00, 0x20Cu);                              // struct_size（运行时 min(size,0x20C)，且要求 >=4）
    putF(0x08, g_nrIntensity);
    putU(0x0C, (uint32_t)g_nrStyle);
    putF(0x10, g_nrTone);
    putF(0x14, g_nrStruct);
    putF(0x18, g_nrSkin);
    putU(0x1C, g_nrAutoMask ? 1u : 0u);
    putF(0x20, g_nrGrade);
    putU(0x2C, g_nrCtrlOn ? 1u : 0u);                // ★ 总闸
    using Fn = int32_t(*)(void*, void*);
    int32_t r = ((Fn)W.setControls)(W.ctx, blk);
    g_ctrlDirty = false;
    Log("worker: SetControls r=%d | style=%d intensity=%.2f tone=%.2f struct=%.2f skin=%.2f mask=%d grade=%.2f enable=%d",
        (int)r, (int)g_nrStyle, g_nrIntensity, g_nrTone, g_nrStruct, g_nrSkin,
        (int)g_nrAutoMask, g_nrGrade, (int)g_nrCtrlOn);
}

// 建会话（建哪张网络由 g_modelSel 决定）。换模型 = 重建会话，这是 AMDNR 的做法：
// 请求值在 Create 之前写环境变量，销毁旧会话后重新 Create/PrepareSession。
// mochizuki 的模型选择器 = 哪个权重文件在位（实测唯一有效的通道）：
//   在 = CC_SMALL_FP8（新），不在 = 回退 CC_SILVER_AARDWOLD（旧）
// 切换 = 改文件名 + 重建会话（照 AMDNR 的编排：旧网络继续出帧 → 新网络建好 → 换过去）
static bool MochiSetWeights(int sel) {   // sel: 0=Small, 1=Original
    std::string base = ModuleDir() + "dlssnr-amd\\dlssnr-small.bin";
    std::string off1 = base + ".off";
    std::string off2 = base + ".off-1026";
    bool on = (GetFileAttributesA(base.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (sel == 1 && on) {
        if (MoveFileA(base.c_str(), off1.c_str())) { Log("worker: 模型选择 → Original（已隐藏 small 权重）"); }
        else { Log("worker: 隐藏 small 权重失败 err=%lu", GetLastError()); return false; }
    } else if (sel == 0 && !on) {
        const char* src = (GetFileAttributesA(off1.c_str()) != INVALID_FILE_ATTRIBUTES) ? off1.c_str() : off2.c_str();
        if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) {
            Log("worker: 找不到 small 权重备份，无法切到 Small"); return false;
        }
        if (MoveFileA(src, base.c_str())) { Log("worker: 模型选择 → Small（已恢复 small 权重）"); }
        else { Log("worker: 恢复 small 权重失败 err=%lu", GetLastError()); return false; }
    }
    Log("worker: 当前 small 权重在位=%d（1=新模型 0=旧模型）", (int)(GetFileAttributesA(base.c_str()) != INVALID_FILE_ATTRIBUTES));
    return true;
}

static bool WorkerCreateSession() {
    Worker& W = g_worker;
    if (W.isMochi) MochiSetWeights(g_modelSel);      // mochizuki：靠权重在位
    const wchar_t* mp = g_modelSel ? L"1" : L"0.5";
    SetEnvironmentVariableW(L"DLSS5_MULTI_PASS", mp);   // lmxxf 系：靠环境变量（mochizuki 忽略）
    {
        wchar_t rb[16] = { 0 };
        GetEnvironmentVariableW(L"DLSS5_MULTI_PASS", rb, 16);
        Log("worker: 建会话 → 请求 DLSS5_MULTI_PASS=%ls（回读 %ls，%s）",
            mp, rb, g_modelSel ? "Original 71-block" : "Small 31-block");
    }
    CreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = W.dev.Get(); ci.queue = W.q.Get(); ci.assets_directory = W.assetsDir.c_str();
    if (W.api.Create(&ci, &W.ctx) != 0) {
        char eb[256] = { 0 }; if (W.api.GetLastError) W.api.GetLastError(eb, 256);
        Log("worker: Create 失败 err=%s assets=%ls", eb, W.assetsDir.c_str()); W.ok = false; return false;
    }
    if (W.api.PrepareSession(W.ctx) != 0) {
        char eb[256] = { 0 }; if (W.api.GetLastError) W.api.GetLastError(eb, 256);
        Log("worker: PrepareSession 失败 err=%s", eb); W.ok = false; return false;
    }
    W.modelApplied = g_modelSel;
    W.ok = true;
    Log("worker: 会话就绪 (%ux%u, %llu 字节) 模型=%s", kNetW, kNetH,
        (unsigned long long)W.bytes, g_modelSel ? "Original" : "Small");
    ApplyMochiControls(true);   // 会话建好后下发风格控制块（实测必须在这一步之后再调一次）
    return true;
}

// 换模型：销毁旧会话 → 用新请求重建。调用方是 worker 线程，游戏线程不受阻塞。
static bool WorkerRebuildForModel(int sel) {
    Worker& W = g_worker;
    if (!W.ctx || !W.api.Destroy) { Log("worker: 没有会话，无法重建"); return false; }
    auto t0 = Clock::now();
    W.ok = false;
    W.api.Destroy(W.ctx); W.ctx = nullptr;
    g_modelSel = sel;
    bool ok = WorkerCreateSession();
    // 实测结论（2026-10-26）：kDelay=2 视觉上不可用（帧配对错位），3 是本链路的硬下限 ——
    // 因为喂帧滞后固定 2 帧（inIndex=(N-2)%K），present 必须再退 1 帧才能配到"同一帧的内容与残差"。
    // 所以换模型不改 kDelay，保持 3。
    (void)g_kDelayAuto;
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    Log("worker: 模型切换 → %s（重建会话 %s，%.0f ms；时域状态按新会话重来）",
        sel ? "Original 71-block" : "Small 31-block", ok ? "成功" : "失败", ms);
    return ok;
}


// ---- B 形态：这几个定义在文件前半部分（WorkerMain 要用），其余定义在后面 ----
static bool g_bformOn = false;
static bool g_bformReady = false;
static int  g_upSlot = 0;
static ComPtr<ID3D12Resource> g_upBufW;
static bool BformBDecode(ID3D12GraphicsCommandList* cl, int slot);   // ★ v4fix2：返回是否"真的解出了有效新帧"
static bool BformBEnsureDS(ID3D12Device* dev);
static bool BformBDownsample(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst1080);
// ★ 2026-10-31 N2：把槽位解出的 MV（g_bTexM，普通纹理）拷进共享纹理 W.vel（核心要 import 到 Vulkan）
//   curDst = velDst 当前所处的状态（由调用方跟踪，核心 eval 之后它会停在 SRV 态）
static bool BformSlotMvToShared(ID3D12GraphicsCommandList* cl, ID3D12Resource* velDst,
                                D3D12_RESOURCE_STATES curDst);
// ★ 2026-10-31 F7：B 侧跑马灯（画在解出的 4K 底图上；条外像素不动，保留原画面）
static bool BformBMarkSlot(ID3D12GraphicsCommandList* cl, int frameIdx);
// ★ 2026-10-31 F7 叠加版：在合成结果 g_bComp 上叠一条会动的参考条（条外像素不动）
static bool BformBOverlay(ID3D12GraphicsCommandList* cl, int frameIdx, int slot);
static volatile int g_bMarkFrame = 0;      // 最近一次画条用的帧号（叠加版沿用同一个相位）
static bool g_bCompReady = false;
static bool g_bUpReady = false;
static int  g_bformStage = 0;   // 1..5：只开到第 N 步
static bool BformBEnsureComp(ID3D12Device* dev);
static bool BformBCompose(ID3D12GraphicsCommandList* cl, ID3D12Resource* ansTex, int slot = 0);
// ★ 2026-10-31 F8：B 形态三横带诊断（写进 g_bComp；出画链路不变）
static bool BformBTri(ID3D12GraphicsCommandList* cl, ID3D12Resource* ansTex, int slot);
static bool BformBEnsureUp(ID3D12Device* dev);
static bool BformPPEnsure(ID3D12Device* dev);
static void BformPPStage1(ID3D12GraphicsCommandList* cl, int i, ID3D12Resource* nrColorDst, ID3D12Resource* a, ID3D12Resource* b);
static bool BformPPStage2(int j);
static bool g_ppReady = false;
static int  g_ppCur = 0;
static bool g_ppHavePrev = false;
static volatile ULONGLONG g_workerBeat = 0;
static UINT g_diagCnt = 1;
static ComPtr<ID3D12Resource> g_bUp4k, g_bTexC;   // 诊断要用，提前定义
static void BformDiagEnqueue(Worker& W, ID3D12Resource* ans, int slot = 0, bool upWritten = true);   // 五源同块对照（挂 worker 自己的 list）
static void BformDiagReadback(Worker& W);   // 回读 + 打一行数字
static ComPtr<ID3D12Resource> g_ppAns[2];   // WorkerMain 里要用，提前声明
static bool BformBUpscale(ID3D12GraphicsCommandList* cl, int slot = 0);
static HWND g_gameHwnd = nullptr;
// ★ 取句柄不能依赖菜单：B 形态出画（DComp 贴到游戏窗口）必须要 HWND，
//   而以前唯一的取句柄入口在 Del 菜单里 → 不开一次菜单就永远 g_gameHwnd == nullptr
//   → B 形态永远不初始化出画（现象：进游戏后 NR 画面不出现，开一次菜单才出现）
static HWND ResolveGameHwnd();
static volatile int g_upLastSlot = 0;   // A 最近写完的槽号（B 直接读它，省掉 kFeedLag 的 2 帧延迟）
// ★ 2026-11-08 HOTDIAG：热诊断总开关（默认 0=关；游戏内 F6 边沿切换）。只门控低频日志/计数，不改任何执行路径。
static volatile LONG g_hotDiagOn = 0;
static volatile LONG g_c4LastOkFrame = -1;   // 最近一次成功 C4Kick 的帧号（仅 HOTDIAG 读取）
static volatile LONG g_c4LastOkSlot = -1;    // 同上：槽号
static volatile bool g_bStarved = false; // 防饿死门：本轮 B 落后太多 → 跳过合成（保持上一帧，不要撕裂帧）
// 槽位"面单"：记录每个槽位里装的是【第几帧】+ 该帧的 jitter。
//   A 写、B 读，都在同一个进程里 → 不需要过显卡、也不需要回读，直接 CPU 侧共享。
//   用途：① 让 B 知道自己解得的是第几帧（以前完全不知道）② 给 TAA 做抖动补偿
// ★ v4 P4：槽位令牌 = 身份三字段（epoch / attempt / frame）+ 资源字段 + 发布门 valid。
//   原来只有 frame/jx/jy（"面单"）。新增字段全部由 UiPublishSlot / UiTokenValid 读写：
//   发布端先清 valid → 写全部字段 → MemoryBarrier → 置 valid=1；消费端先查 valid 再校验三字段。
//   注意：valid 是【发布代际门】，不是单次消费锁 —— 消费端不清 0（本链路没有"任务队列"，
//   同一槽在一次发布内可能被 B 连续读两轮，清 0 会让第二轮误判为"未发布"）。
struct BformSlotTag {
    volatile int   frame;        // 游戏帧号（身份字段 3）
    volatile float jx, jy;       // 该帧 jitter（面单原有用途）
    volatile UINT  epoch;        // 换代计数（身份字段 1）
    volatile UINT  attempt;      // 批次序号（身份字段 2）
    volatile UINT64 readyFence;  // GPU 完成围栏值（只记录，等待仍走 g_slotReadyFv）
    ID3D12Resource* outputTex;   // 输出纹理（原生指针，不持有引用）
    volatile LONG  valid;        // 0=未发布 / 1=已发布
};
static BformSlotTag g_slotTag[K];
// ===== r28 块1（2026-11-16）：票据环数据结构（r28-实施拆分 §3.1）。块1 只建结构，不参与运行；块2 起使用 =====
enum : uint8_t { R_FREE = 0, R_RESV, R_COLW, R_READY, R_LEASED, R_RETIRING, R_SEALED };
// gate = epoch:16 | gen:8 | state:8，一次 32 位 CAS 完成状态迁移
struct R28Slot {
    volatile LONG gate;      // InterlockedCompareExchange
    std::atomic<int>    T;       // R_RESV 起有效；READY 扫描可能与 A 回收并发，认领仍须校验 gate
    std::atomic<UINT64> fvColor; // g_xqFe2 的 Signal 值（Signal hr 成功才非 0）
    UINT64  bvProducer;          // g_bbFe 的值：A 侧这次写槽的 list 完成以后才允许回收（SEALED 退休用）
    std::atomic<UINT64> leaseRv; // B worker 本轮 W.fv+1；B 的拷贝完成 = W.fe >= leaseRv
    UINT    mvOk;            // 块4：MV 段与 T 同帧 = 1；块2/3 恒为 0
};
static R28Slot g_r28[K];
static volatile LONG g_r28Epoch = 1;          // 只在 CPU 侧使用，不进像素（D1=A）
static volatile LONG g_r28EpochFirstT = 0;    // epoch 起点的 g_frame；T < 它的捕获一律丢弃（块2）
static volatile LONG g_r28LastPubT = -1;      // A：已 READY 的最大 T（判 DUP，块2）
static std::atomic<UINT64> g_r28LastConsT{0}; // 高 32 位 epoch，低 32 位该代已 LEASE 的最大 T；0 表示尚无消费
static volatile LONG g_r28Mode = 0;           // xess_proxy.r28.txt：0 旧 / 1 影子诊断 / 2 票据环选槽（块1 中等同 1）
// r28 块2 统计计数（原子；R28TK: 日志打印）
static volatile LONG g_r28CntDup      = 0;   // A：DUP（T <= LastPubT）
static volatile LONG g_r28CntDropFull = 0;   // A：环满无可回收槽
static volatile LONG g_r28CntOldEpoch = 0;   // A：T < EpochFirstT
static volatile LONG g_r28CntSealSig  = 0;   // A：Signal(g_xqFe2) 失败 → SEALED
static volatile LONG g_r28CntWaitFail = 0;   // B：Wait(xferFe,fvColor) 失败
static volatile LONG g_r28CntNoNew    = 0;   // B：BClaimSnapshot 无可认领的新票
static volatile LONG g_r28CntNoComp   = 0;   // B：有同代新 READY，但生产 GPU 尚未写完
static volatile LONG g_r28CntReclaimReady = 0; // A：成功回收已完成且可替代的 READY
static volatile LONG g_r28CntAckWater = 0;   // B：RETIRING→FREE 累计水位
static volatile LONG g_r28CntDevRem   = 0;   // B：fence UINT64_MAX（设备移除）
// r28 块4 计数（提前到此块，供 R28ReadCfg/:1299 和 R28PublishStats/:1436 使用；HIT-1 修复前向引用）
static volatile LONG g_r28MvEn        = 1;   // r28.txt 第二个数：1=拷 MV（默认）；0=mvOk 恒 0
static volatile LONG g_r28CntMvOkZero = 0;   // B 端 mvOk=0 次数计数
// 2026-11-19 mvpair：MV 暂存入槽的"提交身份配对"计数（R28TK 行尾 mvPair 段打印；定义/用法检索 MvPair）
static volatile LONG g_mvPairCntReg     = 0;   // ProxyExecute 登记待提交记录次数
static volatile LONG g_mvPairCntRaw     = 0;   // XferMaybeKick 命中：lists[i] == 登记 cl 原指针
static volatile LONG g_mvPairCntCanon   = 0;   // 命中：lists[i] == 登记 cl 的 ReShade 解包指针（lists 已是裸对象）
static volatile LONG g_mvPairCntDeep    = 0;   // 命中：解包(lists[i]) == 登记 cl 的解包指针（中间还有别的包装层）
static volatile LONG g_mvPairCntSigFail = 0;   // 命中但该次 Signal 失败 → 该帧围栏记 0
static volatile LONG g_mvPairCntEvict   = 0;   // 待提交记录未被任何提交命中就被淘汰（环位复用 / 同 cl 重录）
static volatile LONG g_mvPairCntC4Ok    = 0;   // C4 命中本帧且等待已满足或成功排队
static volatile LONG g_mvPairCntC4Rej   = 0;   // C4 开启 MV 却无有效源 / 等待失败 → mvOk=0
static volatile LONG g_mvPairCntWaitFail = 0;  // C4 有围栏但设备移除或 Wait 失败
static volatile LONG g_mvPairDeepMode   = 0;   // 0=未定（深比对开）/ 1=浅比对已证实（深比对关）/ 2=已确认深命中 / 3=淘汰后恢复深比对（2、3 都保持开启）
static volatile LONG g_r28LastUiEp    = -1;  // 被动 epoch bump（D6=B）：上次检测到的 g_uiState.epoch
// 块1 影子记账（A 侧，present 线程单写；B worker 只用 Interlocked 读来打汇总行）。与上面块2 的真实字段分开，互不影响。
struct R28ShadowA {
    volatile LONG   kick;      // 计入的 KICK 次数（发布 + 空洞）
    volatile LONG   hole;      // 解码失败（wmtOnly，不落槽）= 空洞
    volatile LONG   dup;       // frame <= 影子 LastPubT 的发布
    volatile LONG   pairKick;  // 两次 KICK 之间 g_xqFv 未变化（同一波 present 两次 KICK）
    volatile LONG   lastPubT;  // 影子 LastPubT：已发布的最大 T
    volatile LONG64 lastV2;    // 最近一次发布的 g_xqFe2 值
};
static R28ShadowA g_r28Sh = { 0, 0, 0, 0, -1, 0 };
static bool g_bpReady = false;
static bool g_bpFailed = false;
// 答案交付（按形态分路）：A 形态在 worker 内等落地并当场交付；B 形态整段跳过
static int    g_pendIdx = -1;      // 保留字段（历史），未使用
static int    g_pendFrame = -1;
static UINT64 g_pendFv = 0;
static bool   g_pendValid = false;
static bool BformPresentInit();
static bool BformPresentFrame(int slot, UINT64 composeFv, bool keepalive = false);
// 覆盖层可见性：切回 A（stage<6）时把 DComp 视觉卸掉，否则会留一张冻结的 B 画面盖住 A
static void BformPresentShow(bool show);

// =====================================================================================
// 【UI 自动开关 v4 · B0 基础设施】声明与全局状态
//   设计：E:\AI\多角色编排\04_证据\runs\run-20261105-1822\ui-auto-switch-design-v4-lead-20261114.md
//   本节标号 P1..P23 对应设计 §9.1 的修改点；实现顺序 B0→B1/B3/B4→B2→B5→B6。
//   三条硬约束（设计 §1.2）：
//     ① 原子发布：整个 UiGateStatePacket 由 SRWLOCK 保护（独占写 / 共享读），不分步 CAS；
//     ② 租约配对：游戏线程每帧 +1，UiGateAutoLeave 析构时 -1（RAII 覆盖所有 return 路径）；
//     ③ 令牌完整：BformSlotTag 的 epoch + attempt + frame 三字段都校验。
//   默认【完全直通】：xess_proxy.uigate.txt 不存在或 mode=0 时 g_uiFeatureOn=0，
//   UiGateEnter 只做一对 Interlocked ±1，其余行为与旧版一模一样（不影响现役配置）。
// =====================================================================================
enum UiGateState {
    UIG_OFF = 0,    // 功能关闭（NR 不走我们的链，直通 real()）
    UIG_OBSERVE,    // 观察模式（只记日志/统计，不改行为）
    UIG_RUNNING,    // 正常运行（B-NR on）
    UIG_STOPPING,   // 正在停止（等租约排空）
    UIG_STOPPED,    // 已停止（A-NR on / 画面走 game 自己的 present）
    UIG_STARTING    // 正在启动（预热 primeLeft 帧）
};

// 状态包（唯一原子发布单元）。字段与设计 §2.3 一一对应，
// 另加 minFrame：设计 §4.3 消费端用 snap.minFrame 做 frame 下限校验，但 §2.3 漏列了该字段
// （两处不自洽）；这里补上，STARTING 时写入当前帧号，让上一代的旧令牌整体过期。
struct UiGateStatePacket {
    LONG   state;       // UiGateState
    UINT   epoch;       // 换代计数（STOPPING / STARTING / F10 / 热更新时 ++）
    LONG   inFlight;    // 租约计数（==0 即"已排空"，双重职责）
    UINT   attempt;     // 当前批次序号
    LONG   primeLeft;   // 预热剩余帧数（仅 STARTING 有效）
    UINT64 timestamp;   // 状态切换 tick（cooldown / 排空超时用）
    UINT   minFrame;    // 本代允许的最小帧号（令牌 frame 下限）
};
static __declspec(align(64)) UiGateStatePacket g_uiState = { UIG_OFF, 0, 0, 0, 0, 0, 0 };
static SRWLOCK g_uiStateLock = SRWLOCK_INIT;   // 保护 g_uiState（独占写 / 共享读）
static SRWLOCK g_c4Lock      = SRWLOCK_INIT;   // 保护 C4 pending（BbHandoff ⇄ C4Kick ⇄ ClearC4State）

// v4 §6.1 / §7.1 / §8.1 的全局量
static volatile LONG g_c4Enabled      = 1;   // 1=启用 0=禁用（停止期间置 0）
static volatile LONG g_hardFail       = 0;   // 0=正常 1=硬错（只记录第一次原因）
static volatile LONG g_overlayDesired = 0;   // 覆盖层期望：0=隐藏 1=显示
static volatile LONG g_overlayApplied = 0;   // 覆盖层已生效状态（失败不更新 → 下次重试）
// 模式矩阵（P21/P22）：feature=0 完全直通；mode=1 观察；mode=2 自动
static volatile LONG g_uiFeatureOn  = 0;     // 0=未启用（旧行为） / 1=启用状态机
static volatile LONG g_uiAutoOn     = 0;     // 1=真的改行为（mode 2）
static volatile LONG g_uiModeCfg    = 0;     // 热更新读到的 mode（0/1/2）
static volatile LONG g_uiOpenThrMs    = 150;  // UI 打开判定阈值（ProxyExecute 调用间隔）
static volatile LONG g_uiResumeCoolMs = 2000; // STOPPED → STARTING 冷却（设计 §5.3：2 秒）
static volatile LONG g_uiDrainWarnMs  = 3000; // 排空告警/边界等待上限（设计 §5.2：3 秒）
static volatile LONG g_uiPrimeFrames  = 4;    // 预热帧数（设计 §5.3：4 帧）

// ===== v4fix2：B 出画「身份」传播 + 单一显示闸门（2077 B 形态退出 UI 闪旧画面）=====
// 现场证据（xess_proxy.log）：「UI closed, STOPPED -> STARTING」紧跟「Overlay shown(desired=1)」
//   再紧跟「Priming 1/4」——说明覆盖层是在 STARTING（还没有任何新帧）时就被挂上的，
//   贴上去的是进 UI 前最后一帧（旧交换链内容），于是 UI 一关先闪一下冻结旧画面。
// 根因两层：
//   ① ProxyExecute 热路径每帧 BformPresentShow(stage>=6 && bpReady && UiGateAllowsNr()) 抢先显示，
//      而 UiGateAllowsNr 有意放行 STARTING（不放行会饿死预热）；
//   ② OnPresentSuccess 对【每一次】Present 成功都递减 primeLeft，不校验该帧是不是"有效新帧"，
//      所以 4 帧预热没有任何内容语义。
// 修法（加强版 A）：
//   ① 热路径只许 Hide；Show 只有一条出路 → OnPresentSuccess → OverlayGateTick；
//   ② BformBDecode 成功时把槽位身份（epoch/attempt/frame）发布出来并推进解码序号；
//   ③ 只有"有效新 epoch 新帧"的 B present 才推进 primeLeft；
//   ④ 预热完成后武装闸门，再等【下一次】同 epoch 有效 B present 且其出画 fence 已落地
//      （非阻塞轮询，天然多留一帧 compositor 裕量），然后重读 state/epoch/NR/stage/窗口再 ApplyOverlay；
//   ⑤ 边界：UI 打开/关闭/排空/硬错/F10 一律取消 pending；全程有兜底期限，绝不无界等待。
static volatile LONG     g_bDecSeq = 0;            // 有效解码序号（令牌通过且是新帧才 ++）
static volatile LONG     g_bDecFrame = -1;         // 最近一次有效解码的槽位帧号
static volatile LONG     g_bDecEpoch = -1;         // 最近一次有效解码的槽位 epoch
static volatile LONG     g_bDecAttempt = -1;       // 最近一次有效解码的槽位 attempt
static volatile LONGLONG g_bDecTick = 0;           // 最近一次有效解码时刻（GetTickCount64）
static volatile LONGLONG g_bPresFv = 0;            // 最近一次有效 B 出画的出画围栏值（g_bpFv）
static volatile LONGLONG g_bPresTick = 0;          // 最近一次有效 B 出画时刻
static volatile LONG     g_uiPrimeLastSeq = -1;    // 已计入预热的解码序号（同一帧不重复计数）
static volatile LONG     g_ovGateArm = 0;          // 1=显示闸门已武装（等下一次有效出画 + fence）
static volatile LONG     g_ovGateEpoch = 0;        // 武装时所属 epoch（旧回调晚到的判据）
static volatile LONGLONG g_ovGateFence = 0;        // 需要等到的出画围栏值（0=还没看到武装后的有效出画）
static volatile LONGLONG g_ovGateArmTick = 0;      // 武装时刻（兜底时限）
static volatile LONG     g_ovGateShownEpoch = -1;  // 本代已显示过的 epoch（同一代不反复武装）
static SRWLOCK           g_ovLock = SRWLOCK_INIT;  // 串行化 显示/卸载 与 DComp Commit（防旧回调晚到）
static volatile LONG     g_uiPrimeDeadlineMs = 3000;  // STARTING 长时间无有效新帧的兜底（ms）
static volatile LONG     g_uiGateWaitMs = 500;        // 等出画 fence 落地的兜底（ms）

// ---- 前置声明（定义集中在 C4 全局量之后的那一段，见"B1~B6 实现"）----
static void LogInfo(const char* fmt, ...);
static void LogWarn(const char* fmt, ...);
static void LogError(const char* fmt, ...);
static bool UiGateEnter(UiGateStatePacket* snap);
static void UiGateLeave();
static void UiGateTick();
static void UiGateInit(LONG mode, LONG thrMs);
static bool UiGateAllowsNr();
static void NrHardFail(const char* reason);
static void ShowOverlay();
static void HideOverlay();
static void ApplyOverlay();
static void UiOpenDetected();
static void UiCloseDetected();
static void CheckDrained();
static bool CaptureStopBoundary();
static void ClearC4State();
static void UiGateHotkeyToggle();
static volatile LONG g_wmtOff = 0;   // 水印已整体关闭（保留，供 R28 路径检查）
static void UiPublishSlot(int slot, int frame, ID3D12Resource* outputTex, UINT64 readyFence, float jx, float jy);
static bool UiTokenValid(int slot);
static void OnPresentSuccess();
static void OnBPresentSuccess();
static void UiPrimeStep(bool fromB);
static void OverlayGateArm(UINT epoch);
static void OverlayGateCancel(const char* why);
static void OverlayGateTick();
static bool C4GateAllows();
static bool C4TakePending();
static void C4RestorePending();
static void C4DropPending();
static bool HrIsDeviceLost(HRESULT hr);

// 租约 RAII：构造点紧跟在 UiGateEnter 之后（Enter 内已 +1），析构统一 -1；
// 这样 ProxyExecute 后面几十个 return 分支都不用各自配对（设计 §3.1 配对铁律）。
struct UiGateAutoLeave {
    ~UiGateAutoLeave() { UiGateLeave(); }
};

// ---- B 形态 TAA-lite（时域上采样：治噪点/锯齿）----
//   bform=8 = 阶段 6 + 一个 TAA pass（jitter 由共享 buffer 头部传；本版先不做 jitter 补偿）
//   出画源从 g_bUp4k 换成 TAA 输出；bform=6 保持原样，方便 A/B 对照
static bool g_taaOn = false;
// ---- 4K 后置：10bit→8bit 转换（pOutput 是 R10G10B10A2，我们整条链是 8bit）----
static const char* kBformFmt8 = R"(
Texture2D<float4>   gS : register(t0);
RWTexture2D<float4> gD : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h; gD.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    gD[id.xy] = float4(gS.Load(int3(id.xy, 0)).rgb, 1.0);
}
)";
static ComPtr<ID3D12Resource> g_outp8[3];   // 4K / B8G8R8A8 转换结果（三缓冲：c4outp3 与 C4 三套 allocator 对齐）
static ComPtr<ID3D12PipelineState> g_psoFmt8;
static bool g_outp8Ready[3] = { false, false, false };
static ComPtr<ID3D12CommandQueue>        g_xqQ;
static ComPtr<ID3D12CommandAllocator>    g_xqAl[3];   // 3 组轮转（单组会在上一份拷贝没跑完时 Reset 失败 → 半帧不更新 → 画面交替）
static ComPtr<ID3D12GraphicsCommandList> g_xqCl[3];
static int      g_xqAi = 0;                        // 3 组轮转指针
static UINT64   g_xqFvUsed[3] = { 0, 0, 0 };        // 每组上次的完成围栏值
static ComPtr<ID3D12Fence>               g_xqFe;    // 游戏提交后的记号（A 内部排序）
static ComPtr<ID3D12Fence>               g_xqFe2;   // 拷贝完成（共享给 B）
static UINT64        g_xqFv = 0, g_xqFv2 = 0;
static HANDLE        g_xqShare = nullptr;
static volatile UINT64 g_slotReadyFv[K] = { 0,0,0,0,0,0 };   // 每槽：B 解码前要等的围栏值
static ComPtr<ID3D12Fence> g_bpFe;   // B 出画围栏（提前声明：COMPOSE_BEGIN_SAMPLE 在 BformBCompose 里采样）
static UINT64 g_bpFv = 0;
static volatile bool g_xferOwn = false;                       // true = 颜色由我们的队列拷
static volatile LONG g_xferKick = 0, g_xferSkip = 0;
static volatile UINT64 g_lastSigV = 0;   // 最近一次"提交记号"的值（拷贝等它）
// ---- 抓帧导出（全局声明 + 前向声明；实现在 BformCopy 附近）----
static std::string g_dumpDir;
static int   g_dumpMask, g_dumpLeft;
static UINT  g_dumpW, g_dumpH, g_dumpPitch;
static ComPtr<ID3D12Resource> g_dumpRb[3];   // 0=A 1=N 2=C
static bool g_dumpPend[3] = {false,false,false};
static int  g_dumpPendFrame[3] = {0,0,0};
static void DumpPollFile();
static void DumpRecord(ID3D12GraphicsCommandList* cl, ID3D12Device* dev, int st,
                       ID3D12Resource* src, D3D12_RESOURCE_STATES cur);
static void DumpFlush(int st);
static volatile UINT64 g_xferPendSig = 0;   // 登记 pending 时的记号值
static volatile int g_xferPendFrameTag = 0;   // 待拷那一帧的帧号（拷贝成功后用它发布面单）
// ★ 待拷队列（4 坑 FIFO）：以前只有 1 个坑，某帧没及时触发就被下一帧覆盖 → 那个槽位永远没颜色
struct XferPend { volatile bool valid; int slot; int parity; unsigned long long sig; int frameTag; };
static XferPend g_pend[4];
static volatile LONG g_pendW = 0;
static volatile bool g_xferPendValid = false;
static volatile int  g_xferPendParity = -1;                   // 钩子里要拷的缓冲奇偶
static volatile UINT g_xferPendSlot = 0;
static ID3D12GraphicsCommandList* g_ourList = nullptr;        // 本帧我们录过转换的那条 list
static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,  // 前沿声明（worker 里的 4K 直通要用）
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
static volatile bool g_bform4k = false;   // ★ 必须 volatile：worker 线程要读它              // bform=9：取 pOutput（XeSS 之后, 4K）作底图，B 端直通出画
static ComPtr<ID3D12Resource> g_outpRes;    // 本帧的 pOutput（每帧在 hook 里赋值，供 4K 模式取帧）
static bool g_taaReady = false;
static bool g_taaValid = false;          // 历史是否有效（切换/首帧时无效）
static ComPtr<ID3D12Resource> g_taaOut;  // TAA 输出（诊断要提前引用，定义在此，后面不再重复定义）
// TAA 两个参数（可由 xess_proxy.taa.txt 热调，格式："alpha clampK"）
static volatile float g_taaAlpha  = 0.15f;
static volatile float g_taaClampK = 3.0f;
static bool BformTaaEnsure(ID3D12Device* dev);
static bool BformTaaRun(ID3D12GraphicsCommandList* cl);
static ID3D12Resource* BformPresentSrc(int slot, bool direct4k);

// ===== r28 块1：影子选槽 + 屏上帧号诊断（只记账/打日志；不改 slot、decSlot、发布、等待） =====
#include "r28_shadow.h"   // R28Pick / R28EpochAccept：纯函数，主机单测 _r28_test.cpp 共用
#include "a3_ring.h"
// 只在 B 4K + C4 + NGX 且 r28>=1 时执行；A 形态、r28=0 零执行、零日志。
static inline bool R28ShOn() {
    return g_r28Mode >= 1 && g_bformOn && g_bform4k && g_c4On && g_backendNgx;
}
// 开机读一次 xess_proxy.r28.txt 首行整数（读法同 wmcamo.txt）。缺文件 / 0 = 旧路径且不打日志。
static void R28ReadCfg() {
    static bool done = false;
    if (done) return;
    done = true;
    std::string p = ModuleDir() + "xess_proxy.r28.txt";
    FILE* f = nullptr; char b[16] = { 0 };
    if (fopen_s(&f, p.c_str(), "r") != 0 || !f) return;
    bool ok = false; long v = 0;
    if (fgets(b, sizeof(b), f)) {
        for (char* q = b; *q; ++q) if (*q == '\r' || *q == '\n') { *q = 0; break; }
        char* nb = b;
        if ((unsigned char)nb[0] == 0xEF && (unsigned char)nb[1] == 0xBB && (unsigned char)nb[2] == 0xBF) nb += 3;
        char* end = nullptr;
        v = strtol(nb, &end, 10);
        while (end && (*end == ' ' || *end == '\t')) ++end;
        // 原 while 已把 end 推过第一个数后的空白，end 现在指向第一个非空白字符
        // "2\0"→end='\0'; "2 1\0"（行已裁剪）→after while→end='1'
        ok = (end && end != nb && v >= 0 && v <= 2);
        // r28 块4：解析可选第二个数（MV 入槽开关）：格式 "2 1" 或 "2 0"，单 "2" 等同 "2 1"
        if (ok && v == 2 && end && *end != 0 && *end != '\r' && *end != '\n') {
            char* end2 = nullptr;
            long mv2 = strtol(end, &end2, 10);
            while (end2 && (*end2 == ' ' || *end2 == '\t')) ++end2;
            if (end2 && end2 != end && (!*end2 || *end2 == '\r' || *end2 == '\n') && (mv2 == 0 || mv2 == 1)) {
                InterlockedExchange(&g_r28MvEn, mv2);
                end = end2;   // 推进 end 以便下面的 ok 判断
            } else {
                Log("R28CFG: 第二个数 \"%s\" 不是 0/1，按 1（拷 MV）", end);
                ok = false;
            }
        }
        ok = ok && (!end || *end == 0 || *end == '\r' || *end == '\n');
    }
    fclose(f);
    if (!ok) { Log("R28CFG: xess_proxy.r28.txt 首行 \"%s\" 不是 0~2 的整数，按 0（旧路径）", b); return; }
    if (v == 0) return;
    InterlockedExchange(&g_r28Mode, v);
    Log("R28CFG: r28=%ld mvEn=%ld", v, (long)InterlockedCompareExchange(&g_r28MvEn, 0, 0));
}
// A 侧（present 线程，C4KickLocked 内，C4 有重入守卫 ⇒ 单写者）。published=false 表示解码失败（空洞）。
static void R28ShKick(int frame, UINT64 v2, bool published) {
    if (!R28ShOn()) return;
    static LONG64 lastXv = 0; static bool haveXv = false;
    const LONG64 xv = InterlockedCompareExchange64((volatile LONG64*)&g_xqFv, 0, 0);
    if (haveXv && xv == lastXv) InterlockedIncrement(&g_r28Sh.pairKick);
    lastXv = xv; haveXv = true;
    InterlockedIncrement(&g_r28Sh.kick);
    if (!published) { InterlockedIncrement(&g_r28Sh.hole); return; }
    InterlockedExchange64(&g_r28Sh.lastV2, (LONG64)v2);
    if (frame <= g_r28Sh.lastPubT) InterlockedIncrement(&g_r28Sh.dup);
    else InterlockedExchange(&g_r28Sh.lastPubT, frame);
}
// B 侧（B worker 线程单写，只在本线程打日志）。
struct R28ShadowB {
    LONG rounds = 0, same = 0, oldBack = 0, shBack = 0, noNew = 0, shown = 0;
    int  lastCons = -1, maxShown = -1;            // 影子 LastCons（影子选中即推进）、已解出的最大帧号
    int  oldSlot = -1, oldT = -1, pickSlot = -1, pickT = -1;
    bool active = false;                          // 本轮做过影子扫描 ⇒ 本轮解码成功时才记 shown
};
static R28ShadowB g_r28ShB;
static void R28ShPick(int decSlot) {
    R28ShadowB& s = g_r28ShB;
    s.active = false;
    if (!R28ShOn() || decSlot < 0 || decSlot >= K) return;
    int tags[6]; bool valid[6];
    for (int i = 0; i < 6; ++i) {
        valid[i] = (InterlockedCompareExchange(&g_slotTag[i].valid, 1, 1) == 1);
        tags[i] = (int)g_slotTag[i].frame;
    }
    const int ps = R28Pick(tags, valid, s.lastCons);
    ++s.rounds;
    s.oldSlot = decSlot; s.oldT = tags[decSlot];
    s.pickSlot = ps; s.pickT = (ps >= 0) ? tags[ps] : -1;
    if (ps == decSlot) ++s.same;
    if (ps < 0) ++s.noNew;
    else {
        if (s.pickT <= s.lastCons) ++s.shBack;   // 自检：影子选择应严格递增，恒为 0
        s.lastCons = s.pickT;
    }
    s.active = true;
    if ((s.rounds % 300) == 0)
        Log("R28SH: 轮=%ld old==shadow=%ld old倒退=%ld shadow倒退=%ld shadowNO_NEW=%ld DUP=%ld 成对KICK=%ld 空洞=%ld "
            "| KICK=%ld 解出=%ld maxShown=%d 影子LastCons=%d 影子LastPubT=%ld v2=%lld",
            s.rounds, s.same, s.oldBack, s.shBack, s.noNew,
            InterlockedCompareExchange(&g_r28Sh.dup, 0, 0), InterlockedCompareExchange(&g_r28Sh.pairKick, 0, 0),
            InterlockedCompareExchange(&g_r28Sh.hole, 0, 0), InterlockedCompareExchange(&g_r28Sh.kick, 0, 0),
            s.shown, s.maxShown, s.lastCons, InterlockedCompareExchange(&g_r28Sh.lastPubT, 0, 0),
            (long long)InterlockedCompareExchange64(&g_r28Sh.lastV2, 0, 0));
}
// BformBDecode 成功后（B worker）：记屏上帧号 shownT / maxShown / backward。只在倒退时单独打一行。
static void R28ShShown(int slot) {
    R28ShadowB& s = g_r28ShB;
    if (!s.active) return;
    s.active = false;
    if (!R28ShOn() || slot < 0 || slot >= K) return;
    const int shownT = (int)g_slotTag[slot].frame;
    ++s.shown;
    if (shownT < s.maxShown) {
        ++s.oldBack;
        Log("R28SH 倒退#%ld: old slot=%d frame=%d（已解出最大 %d）| shadow slot=%d frame=%d | 轮=%ld",
            s.oldBack, slot, shownT, s.maxShown, s.pickSlot, s.pickT, s.rounds);
    } else s.maxShown = shownT;
}

// ===== r28 块2（2026-11-16）：票据环选槽（颜色单面）+ LEASE/ack + 块C =====
// 只在 r28=2 && B-4K+C4+NGX 时有效；r28<2 时调用点全部走旧路径，不执行任何函数体。
static inline bool g_r28Mode_live() {
    return InterlockedCompareExchange(&g_r28Mode, 0, 0) >= 2
        && g_bformOn && g_bform4k && g_c4On && g_backendNgx;
}
// gate = epoch:16 | gen:8 | state:8
static inline uint8_t  R28GateState(LONG g) { return (uint8_t)(g & 0xFF); }
static inline uint8_t  R28GateGen(LONG g)   { return (uint8_t)((g >> 8) & 0xFF); }
static inline uint16_t R28GateEpoch(LONG g) { return (uint16_t)((g >> 16) & 0xFFFF); }
static inline LONG     R28MakeGate(uint16_t ep, uint8_t gen, uint8_t st) {
    return (LONG)((ULONG)ep << 16 | (ULONG)gen << 8 | (ULONG)st);
}
// r28 块2 辅助：通过指针延迟访问尚未定义的全局量（避免向前引用）
// g_frame 在约 :2330 定义；g_bbFe 在约 :5711 定义
// 在 R28BumpEpoch / R28Claim 内通过这两个函数读取，而不是直接引用变量名
static int     R28GetFrame();
static UINT64  R28GetBbDone();
// 实现放在 g_frame / g_bbFe 定义之后（文件尾附近，见 "R28BumpEpoch 延迟实现" 注释）

static LONG R28LastConsForEpoch(LONG epoch) {
    const UINT64 stamp = g_r28LastConsT.load();
    return (uint32_t)(stamp >> 32) == (uint32_t)epoch ? (LONG)(uint32_t)stamp : -1;
}

// 切代仅封存旧 READY；正由 A 写入或 B 租用/退休的槽保留所有权，由各自线程收尾。
static void R28BumpEpoch(const char* reason) {
    if (InterlockedCompareExchange(&g_r28Mode, 0, 0) == 0) return;
    const LONG ep = InterlockedIncrement(&g_r28Epoch);
    const uint16_t ep16 = (uint16_t)(ep & 0xFFFF);
    InterlockedExchange(&g_r28EpochFirstT, (LONG)R28GetFrame());
    InterlockedExchange(&g_r28LastPubT, -1);
    for (int i = 0; i < K; ++i) {
        LONG old = InterlockedCompareExchange(&g_r28[i].gate, 0, 0);
        if (R28GateState(old) == R_READY && R28GateEpoch(old) != ep16) {
            LONG want = R28MakeGate(ep16, R28GateGen(old), R_SEALED);
            InterlockedCompareExchange(&g_r28[i].gate, want, old);
        }
    }
    static LONG bumpN = 0;
    Log("R28TK: BUMP#%ld reason=%s epoch=%ld EpochFirstT=%ld", InterlockedIncrement(&bumpN),
        reason ? reason : "-", (long)ep, (long)R28GetFrame());
}

// R28PublishStats：每 300 轮打一行 R28TK: 计数（块3：补 xqFv2/bbFe/W.fe 完成值）
static void R28PublishStats() {
    static volatile LONG sn = 0;
    LONG n = InterlockedIncrement(&sn);
    if (n == 1 || (n % 300) == 0) {
        const UINT64 xqFv2snap = (UINT64)InterlockedCompareExchange64((volatile LONG64*)&g_xqFv2, 0, 0);
        const UINT64 bbDone    = R28GetBbDone();
        const UINT64 wfDone    = g_worker.fe ? g_worker.fe->GetCompletedValue() : 0;
        Log("R28TK: n=%ld DUP=%ld DROP_FULL=%ld OLD_EPOCH=%ld SEAL_SIG=%ld WAIT_FAIL=%ld NO_NEW=%ld noComp=%ld reclaimReady=%ld ACK_WATER=%ld DEV_REMOVED=%ld mvOkZero=%ld | xqFv2=%llu bbFe=%llu W.fe=%llu",
            n,
            InterlockedCompareExchange(&g_r28CntDup,      0,0),
            InterlockedCompareExchange(&g_r28CntDropFull, 0,0),
            InterlockedCompareExchange(&g_r28CntOldEpoch, 0,0),
            InterlockedCompareExchange(&g_r28CntSealSig,  0,0),
            InterlockedCompareExchange(&g_r28CntWaitFail, 0,0),
            InterlockedCompareExchange(&g_r28CntNoNew,    0,0),
            InterlockedCompareExchange(&g_r28CntNoComp,   0,0),
            InterlockedCompareExchange(&g_r28CntReclaimReady, 0,0),
            InterlockedCompareExchange(&g_r28CntAckWater, 0,0),
            InterlockedCompareExchange(&g_r28CntDevRem,   0,0),
            InterlockedCompareExchange(&g_r28CntMvOkZero, 0,0),
            (unsigned long long)xqFv2snap,
            (unsigned long long)bbDone,
            (unsigned long long)wfDone);
        // 2026-11-19 mvpair：同频（首条 + 每 300 轮）补一行配对统计，不进热路径
        Log("R28TK: mvPair reg=%ld hit(raw/canon/deep)=%ld/%ld/%ld sigFail=%ld evict=%ld c4(ok/rej)=%ld/%ld waitFail=%ld deepMode=%ld",
            InterlockedCompareExchange(&g_mvPairCntReg,     0,0),
            InterlockedCompareExchange(&g_mvPairCntRaw,     0,0),
            InterlockedCompareExchange(&g_mvPairCntCanon,   0,0),
            InterlockedCompareExchange(&g_mvPairCntDeep,    0,0),
            InterlockedCompareExchange(&g_mvPairCntSigFail, 0,0),
            InterlockedCompareExchange(&g_mvPairCntEvict,   0,0),
            InterlockedCompareExchange(&g_mvPairCntC4Ok,    0,0),
            InterlockedCompareExchange(&g_mvPairCntC4Rej,   0,0),
            InterlockedCompareExchange(&g_mvPairCntWaitFail,0,0),
            InterlockedCompareExchange(&g_mvPairDeepMode,   0,0));
    }
}

// R28Seal：Signal 失败时把槽置 SEALED（A 端 present 线程）
static void R28Seal(int slot, UINT64 bv) {
    if (slot < 0 || slot >= K) return;
    LONG old = InterlockedCompareExchange(&g_r28[slot].gate, 0, 0);
    uint8_t st = R28GateState(old);
    if (st != R_COLW && st != R_RESV) return;
    g_r28[slot].bvProducer = bv;
    InterlockedExchange(&g_r28[slot].gate,
        R28MakeGate(R28GateEpoch(old), R28GateGen(old), R_SEALED));
    InterlockedIncrement(&g_r28CntSealSig);
}

// R28PublishReady：Signal 成功后发布（A 端 present 线程）
// 只在持有 COLW 且 CAS 成功时才置 READY；CAS 失败不改状态（B 可能已抢走，视为 SEALED）
static void R28PublishReady(int slot, UINT64 fvColor) {
    if (slot < 0 || slot >= K) return;
    LONG old = InterlockedCompareExchange(&g_r28[slot].gate, 0, 0);
    if (R28GateState(old) != R_COLW) return;   // 不在 COLW → 不改
    g_r28[slot].fvColor = fvColor;
    MemoryBarrier();
    LONG want = R28MakeGate(R28GateEpoch(old), R28GateGen(old), R_READY);
    InterlockedCompareExchange(&g_r28[slot].gate, want, old);   // CAS 失败则放弃（B 已介入）
}

// R28Claim：A 端 present 线程。返回槽号 (0..K-1) 或 -1（DUP/DROP_FULL/旧epoch）。
static int R28Claim(int frame, LONG* colwGate) {
    // 被动 epoch bump（D6=B）：g_uiState.epoch 变化时 bump
    {
        LONG curUiEp = (LONG)g_uiState.epoch;
        LONG prev = InterlockedCompareExchange(&g_r28LastUiEp, curUiEp, curUiEp);
        if (prev != curUiEp) {
            InterlockedExchange(&g_r28LastUiEp, curUiEp);
            R28BumpEpoch("uiEpoch");
        }
    }
    const LONG curEpochFirstT = InterlockedCompareExchange(&g_r28EpochFirstT, 0, 0);
    if (!R28EpochAccept(frame, (int)curEpochFirstT)) {
        InterlockedIncrement(&g_r28CntOldEpoch); return -1;
    }
    const LONG lastPub = InterlockedCompareExchange(&g_r28LastPubT, 0, 0);
    if (frame <= (int)lastPub) {
        InterlockedIncrement(&g_r28CntDup); return -1;
    }
    const LONG  curEp  = InterlockedCompareExchange(&g_r28Epoch, 0, 0);
    const uint16_t ep16 = (uint16_t)(curEp & 0xFFFF);
    const UINT64 bbDone = R28GetBbDone();
    const UINT64 xqDone = g_xqFe2 ? g_xqFe2->GetCompletedValue() : 0;
    const bool fencesValid = bbDone != UINT64_MAX && xqDone != UINT64_MAX;
    const auto tryReserve = [&](int i, LONG gate) {
        const uint8_t newGen = (uint8_t)(R28GateGen(gate) + 1);
        const LONG want = R28MakeGate(ep16, newGen, R_RESV);
        if (InterlockedCompareExchange(&g_r28[i].gate, want, gate) != gate) return false;
        g_r28[i].T = frame; g_r28[i].fvColor = 0; g_r28[i].leaseRv = 0; g_r28[i].mvOk = 0;
        InterlockedExchange(&g_r28LastPubT, (LONG)frame);
        *colwGate = R28MakeGate(ep16, newGen, R_COLW);
        InterlockedExchange(&g_r28[i].gate, *colwGate);
        return true;
    };

    // 先用 FREE/已退休 SEALED。B 的 RETIRING 只能由 B 在自己的围栏完成后释放。
    for (int i = 0; i < K; ++i) {
        const LONG gate = InterlockedCompareExchange(&g_r28[i].gate, 0, 0);
        const uint8_t st = R28GateState(gate);
        if ((st == R_FREE || (st == R_SEALED && fencesValid &&
             bbDone >= g_r28[i].bvProducer && g_r28[i].leaseRv == 0)) && tryReserve(i, gate))
            return i;
    }

    if (fencesValid) {
        const LONG lastCons = R28LastConsForEpoch(curEp);
        int protectSlot = -1, protectT = (int)lastCons;
        // 同一轮完成快照；先找本代最新且未消费的已完成票。
        for (int i = 0; i < K; ++i) {
            const LONG gate = InterlockedCompareExchange(&g_r28[i].gate, 0, 0);
            if (R28GateState(gate) != R_READY || R28GateEpoch(gate) != ep16) continue;
            const UINT64 fv = g_r28[i].fvColor;
            const int t = g_r28[i].T;
            if (fv && fv <= xqDone && t > protectT) { protectSlot = i; protectT = t; }
        }
        int oldestSlot = -1, oldestT = 0;
        LONG oldestGate = 0;
        for (int i = 0; i < K; ++i) {
            if (i == protectSlot) continue;
            const LONG gate = InterlockedCompareExchange(&g_r28[i].gate, 0, 0);
            if (R28GateState(gate) != R_READY) continue;
            const UINT64 fv = g_r28[i].fvColor;
            if (!fv || fv > xqDone) continue;
            const int t = g_r28[i].T;
            if (R28GateEpoch(gate) == ep16 && t > lastCons && (protectSlot < 0 || t >= protectT)) continue;
            if (oldestSlot < 0 || t < oldestT) { oldestSlot = i; oldestT = t; oldestGate = gate; }
        }
        if (oldestSlot >= 0 && tryReserve(oldestSlot, oldestGate)) {
            InterlockedIncrement(&g_r28CntReclaimReady);
            return oldestSlot;
        }
    }
    InterlockedIncrement(&g_r28CntDropFull); return -1;
}

// C4 建套/录制失败时，只有本次尚未提交的 COLW 可以直接释放。
struct R28UnsubmittedClaim {
    int slot = -1;
    LONG gate = 0;
    ~R28UnsubmittedClaim() {
        if (slot < 0) return;
        const LONG want = R28MakeGate(R28GateEpoch(gate), R28GateGen(gate), R_FREE);
        InterlockedCompareExchange(&g_r28[slot].gate, want, gate);
    }
};

// B worker 线程用的 lease 快照（栈上分配，不跨线程）
struct R28BSnapshot { int slot; int T; UINT64 fvColor; LONG leaseGate; };

// 尚未录制/提交任何读取该槽的 GPU 命令，按本次租约精确退租。
static void R28BReleaseUnused(const R28BSnapshot& s) {
    if (s.slot < 0) return;
    const LONG want = R28MakeGate(R28GateEpoch(s.leaseGate), R28GateGen(s.leaseGate), R_FREE);
    InterlockedCompareExchange(&g_r28[s.slot].gate, want, s.leaseGate);
}

// R28BClaimSnapshot：B worker 线程。只选生产 GPU 已完成的本代新票。
static int R28BClaimSnapshot(Worker& W, R28BSnapshot* s) {
    *s = { -1, -1, 0, 0 };
    const UINT64 prodDone = W.xferFe ? W.xferFe->GetCompletedValue() : 0;
    if (prodDone == UINT64_MAX) {
        InterlockedIncrement(&g_r28CntDevRem);
        NrHardFail("Fence device lost (r28 producer)");
        return -1;
    }
    const LONG curEp = InterlockedCompareExchange(&g_r28Epoch, 0, 0);
    const uint16_t ep16 = (uint16_t)(curEp & 0xFFFF);
    const LONG lastCons = R28LastConsForEpoch(curEp);
    int bestSlot = -1, bestT = (int)lastCons;
    LONG bestGate = 0;
    UINT64 bestFv = 0;
    bool hasIncomplete = false;
    for (int i = 0; i < K; ++i) {
        const LONG gate = InterlockedCompareExchange(&g_r28[i].gate, 0, 0);
        if (R28GateState(gate) != R_READY || R28GateEpoch(gate) != ep16) continue;
        const int t = g_r28[i].T;
        if (t <= bestT) continue;
        const UINT64 fv = g_r28[i].fvColor;
        if (!fv || fv > prodDone) { hasIncomplete = true; continue; }
        bestT = t; bestSlot = i; bestGate = gate; bestFv = fv;
    }
    if (bestSlot < 0) {
        if (hasIncomplete) InterlockedIncrement(&g_r28CntNoComp);
        InterlockedIncrement(&g_r28CntNoNew);
        return -1;
    }
    // 用扫描时的 gate 做 CAS 期望值；A 同时回收时不可误租新一代内容。
    const LONG want = R28MakeGate(ep16, R28GateGen(bestGate), R_LEASED);
    if (InterlockedCompareExchange(&g_r28[bestSlot].gate, want, bestGate) != bestGate) {
        InterlockedIncrement(&g_r28CntNoNew); return -1;
    }
    const R28BSnapshot claimed = { bestSlot, bestT, bestFv, want };
    if (InterlockedCompareExchange(&g_r28Epoch, 0, 0) != curEp) {
        R28BReleaseUnused(claimed);
        InterlockedIncrement(&g_r28CntNoNew);
        return -1;
    }
    g_r28LastConsT.store(((UINT64)(uint32_t)curEp << 32) | (uint32_t)bestT);
    *s = claimed;
    return bestSlot;
}

// R28BPollAck：B worker 每轮开头调用。处理 RETIRING→FREE 和 UINT64_MAX→NrHardFail
static void R28BPollAck() {
    if (InterlockedCompareExchange(&g_r28Mode, 0, 0) < 2) return;
    for (int i = 0; i < K; ++i) {
        LONG gate = InterlockedCompareExchange(&g_r28[i].gate, 0, 0);
        uint8_t st = R28GateState(gate);
        // 处理 RETIRING → FREE
        if (st == R_RETIRING) {
            UINT64 lr = g_r28[i].leaseRv;
            if (lr == 0) continue;
            Worker& W = g_worker;
            UINT64 done = W.fe ? W.fe->GetCompletedValue() : 0;
            if (done == UINT64_MAX) {
                InterlockedIncrement(&g_r28CntDevRem);
                NrHardFail("Fence device lost (r28 B)");
                return;
            }
            if (done >= lr) {
                LONG want = R28MakeGate(R28GateEpoch(gate), R28GateGen(gate), R_FREE);
                if (InterlockedCompareExchange(&g_r28[i].gate, want, gate) == gate) {
                    g_r28[i].leaseRv = 0;
                    InterlockedIncrement(&g_r28CntAckWater);
                }
            }
            continue;
        }
        // 防御性处理带 B 租约的 SEALED：B 确认自己的围栏后清 leaseRv，A 再判生产退休。
        if (st == R_SEALED) {
            UINT64 lr = g_r28[i].leaseRv;
            if (lr == 0) continue;
            Worker& W = g_worker;
            UINT64 done = W.fe ? W.fe->GetCompletedValue() : 0;
            if (done == UINT64_MAX) {
                InterlockedIncrement(&g_r28CntDevRem);
                NrHardFail("Fence device lost (r28 B sealed)");
                return;
            }
            if (done >= lr) {
                // gen+1 防止 ABA；保留 SEALED 状态（A 侧会按 bvProducer 决定是否回收）
                g_r28[i].leaseRv = 0;   // 清零后 A 才能用 leaseRv==0 条件回收
            }
        }
    }
    R28PublishStats();
}

// Worker-owned metadata. A mode switch also bumps r28's epoch; B1 only reads the generations.
static void UiGateFillSnap(UiGateStatePacket* snap);
struct BCompStamp {
    UINT64 composeFv = 0;
    UINT64 lastPresentFv = 0;    // any submitted copy, even if DXGI Present later fails
    UINT uiEpoch = 0;
    LONG modeEpoch = 0;
    bool direct4k = false;
    bool hadValidPresent = false;
};
static BCompStamp g_bCompStamp[2];
static UINT64 g_bUpLastPresentFv = 0, g_bUpComposeFv = 0;
static bool g_bpSyncBroken = false;

static UINT BformUiEpoch() {
    UiGateStatePacket snap{};
    UiGateFillSnap(&snap);
    return snap.epoch;
}

static bool BformWaitPresentReads(Worker& W, int slot, bool useUpscaled) {
    if (g_bpSyncBroken) return false;
    const UINT64 need = (std::max)(g_bCompStamp[slot].lastPresentFv,
                                   useUpscaled ? g_bUpLastPresentFv : UINT64(0));
    if (!need) return true;
    if (g_bpFe) {
        const UINT64 completed = g_bpFe->GetCompletedValue();
        if (completed != UINT64_MAX &&
            (completed >= need || SUCCEEDED(W.q->Wait(g_bpFe.Get(), need)))) return true;
    }
    g_bpSyncBroken = true;
    NrHardFail("B1: worker wait for present copy failed");
    return false;
}

static int BformCompletedSlot(Worker& W, UINT uiEpoch, LONG modeEpoch) {
    const UINT64 completed = W.fe->GetCompletedValue();
    if (completed == UINT64_MAX) return -1;
    int best = -1;
    for (int i = 0; i < 2; ++i) {
        const auto& s = g_bCompStamp[i];
        if (!s.hadValidPresent || !s.composeFv || s.composeFv > completed ||
            s.uiEpoch != uiEpoch || s.modeEpoch != modeEpoch || s.direct4k != g_bform4k ||
            (!s.direct4k && s.composeFv != g_bUpComposeFv)) continue;
        if (best < 0 || s.composeFv > g_bCompStamp[best].composeFv) best = i;
    }
    return best;
}

static bool A3WaitForInput(Worker& W, A2In::Key key) {
    if (W.quit || g_bformOn) return false;
    const auto info = A3::InspectPending(key);
    if (!info.activeMatches) return false;
    if (info.hasReadyNow) return true;
    if (!info.hasPending) return false;
    if (info.prodAOnlyLag && !info.fenceCount) {
        if (W.a3WakeEvent) WaitForSingleObject(W.a3WakeEvent, 1);
        else Sleep(1);
        return true;
    }
    HANDLE handles[A2In::kSlots + 1]{};
    DWORD count = 0;
    if (W.a3WakeEvent) handles[count++] = W.a3WakeEvent;
    for (int i = 0; i < info.fenceCount; ++i) {
        const auto& f = info.fences[i];
        if (f.slot < 0 || f.slot >= A2In::kSlots || !f.prodW) continue;
        if (!W.a3ProdEvents[f.slot]) W.a3ProdEvents[f.slot] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE ev = W.a3ProdEvents[f.slot];
        if (!ev) continue;
        if (W.a3ProdFences[f.slot] != f.prodW.Get() || W.a3ProdEventSeq[f.slot] != f.seq) {
            ResetEvent(ev);
            if (SUCCEEDED(f.prodW->SetEventOnCompletion(f.seq, ev))) {
                W.a3ProdFences[f.slot] = f.prodW.Get();
                W.a3ProdEventSeq[f.slot] = f.seq;
            } else continue;
        }
        handles[count++] = ev;
    }
    if (!count) { Sleep(1); return true; }
    WaitForMultipleObjects(count, handles, FALSE, info.prodAOnlyLag ? 1 : (info.fenceCount > 0 ? 50 : 25));
    return true;
}

static void WorkerMain() {
    Worker& W = g_worker;
    {
        std::lock_guard<std::mutex> lk(W.m);
        W.ansBuf.assign(K, std::vector<uint8_t>((size_t)kNetW * kNetH * 4, 0));
        W.ansFrame.assign(K, -1);
        W.ansAGen.assign(K, -1);
    }
    int lastEvaluatedAGen = -1;
    int lastA3Gen = -1, lastA3Dispatch = -1;
    uint64_t lastA3Epoch = 0;
    bool a3PendingCheck = false;
    while (true) {
        std::vector<uint8_t> local;
        int idx = -1, frameId = -1, feedAGen = -1, f9Dispatch = -1;
        uint64_t feedAResetEpoch = 0;
        bool feedHasAPixels = false, a3Wake = false;
        A2In::Token a2Token;
        A3::Work a3;
        for (;;) {
            bool needPause = false, haveJob = false, checkA3 = false;
            {
                std::unique_lock<std::mutex> lk(W.m);
                if (!W.pauseReq && !a3PendingCheck) W.cv.wait(lk, [&] { return W.quit || W.pauseReq || W.hasIn; });
                if (W.quit) break;
                if (W.pauseReq) {
                    needPause = true;
                } else if (W.hasIn) {
                    feedHasAPixels = W.inHasAPixels;
                    feedAGen = W.inAGen;
                    f9Dispatch = W.f9Dispatch;
                    feedAResetEpoch = W.inAResetEpoch;
                    a2Token = W.inA2; W.inA2 = {};
                    a3Wake = W.inA3; W.inA3 = false;
                    if (feedHasAPixels && !a2Token) local = W.inBuf;
                    idx = W.inIndex; frameId = W.inFrame; W.hasIn = false;
                    if (a3Wake) {
                        lastA3Gen = feedAGen;
                        lastA3Epoch = feedAResetEpoch;
                        lastA3Dispatch = f9Dispatch;
                    }
                    haveJob = !a3Wake;
                    checkA3 = a3Wake;
                } else if (a3PendingCheck) {
                    checkA3 = !g_bformOn && lastA3Gen >= 0;
                    a3Wake = checkA3;
                    if (checkA3) {
                        feedAGen = lastA3Gen;
                        feedAResetEpoch = lastA3Epoch;
                        f9Dispatch = lastA3Dispatch;
                    }
                }
                a3PendingCheck = false;
            }
            if (needPause) {
                if (W.fv) WaitWFence(W.fv, 2000);
                std::unique_lock<std::mutex> lk(W.m);
                W.pausedAck = true;
                W.hasIn = false; W.inA2 = {}; W.inA3 = false;
                a3PendingCheck = false; lastEvaluatedAGen = -1; lastA3Gen = -1;
                W.pausedCv.notify_all();
                W.cv.wait(lk, [&] { return W.quit || !W.pauseReq; });
                W.pausedAck = false;
                if (W.quit) break;
                continue;
            }
            if (haveJob) break;
            if (!checkA3) continue;
            if (g_bformOn) { a3Wake = false; continue; }
            a2Token = a3.Claim({-1, feedAGen, feedAResetEpoch}, f9Dispatch);
            if (a2Token) {
                feedHasAPixels = true;
                frameId = a2Token.key.frame;
                idx = int(a2Token.slot - A2In::slots);
                a3PendingCheck = true;
                break;
            }
            if (A3WaitForInput(W, {-1, feedAGen, feedAResetEpoch})) a3PendingCheck = true;
        }
        if (W.quit) break;
        A2In::Lease a2Lease(a2Token, &W.cvOut);
        g_tlIter = InterlockedIncrement(&g_workerIter);
        g_tlFrame = frameId;
        g_tlPhase = "begin";
        BtTrace("WORKER_BEGIN", frameId, "begin",
                "iter=%ld input_slot=%d b_on=%d stage=%d run_state=-1 starved=%d",
                g_tlIter, idx, (int)g_bformOn, g_bformStage, (int)g_bStarved);
        auto t0 = Clock::now();
        bool delivered = false;   // 本帧是否把答案交付给了 A（B 形态下恒为 false）

        // 模型切换：菜单/文件请求了不同模型 → 在本帧之前重建会话（旧会话出的帧已经交付，不打断 present）
        if (!g_backendNgx && W.modelApplied != g_modelSel) {
            int want = g_modelSel;
            Log("worker: 收到换模型请求（%s），开始重建会话", want ? "Original 71-block" : "Small 31-block");
            WorkerRebuildForModel(want);
        }
        // S4：菜单改了风格参数 → 重新下发控制块（很便宜，不需要重建会话）
        if (g_ctrlDirty) ApplyMochiControls();

        const bool uploadA = feedHasAPixels && !A2In::requested && !(g_xRead && g_xOk && g_xBufInW);
        if (uploadA && !W.upPtr) continue;
        if (g_backendNgx && g_bpSyncBroken) break;
        if (!g_backendNgx && uploadA) memcpy(W.upPtr, local.data(), local.size());

        // ==================== NGX feature 18 backend (default OFF) ====================
        // Only when xess_proxy.backend.txt says "ngx"; otherwise the lmxxf block below runs untouched.
        // Three measured requirements are visible here:
        //   1) the answer texture carries ALLOW_UNORDERED_ACCESS (created in ngx_f18_backend.cpp)
        //   2) the answer is read ONE submission LATER (reading right after Evaluate gives garbage)
        //   3) the core leaves the answer in UNORDERED_ACCESS, our barriers account for that
        if (g_backendNgx) {
            ID3D12Resource* po = nullptr;
            const UINT uiEpoch = g_bformOn ? BformUiEpoch() : 0;
            const LONG modeEpoch = InterlockedCompareExchange(&g_r28Epoch, 0, 0);
            const bool r28Live = !a3Wake && g_r28Mode_live();
            // ================= ★ 2026-10-31 N1/N2：B 形态在 NGX 后端下的"准备阶段" =================
            //   背景（这是本改动唯一的原因）：B 形态的"解码槽位 + 降采样 + 装盘"原本只写在
            //   lmxxf 分支里（:1043~1106），而走 backend=ngx 时那个 else-if 整块被跳过
            //   ⇒ B 端从不解码/降采样，NGX 吃的是 A 形态通道的 1080p（错图）；
            //   ⇒ 槽位解出的 MV 也从不更新（W.vel 一直停在旧内容）。
            //   这里按【解码 → 降采样进 W.color → 槽位MV拷进 W.vel】的顺序补齐，顺序是命门。
            bool bPrep = false, bMvOk = false, bAttempted = false;
            // ★ step1 返工：只表示"本帧 BformBDecode 的返回值"。初值 true = 没走到解码就不拦截出画。
            bool decOk = true;
            // r28 块2：decSlot/r28LeaseOk/r28WaitFv 提升到外层（BtWorkerExec("eval") 前要用）
            int decSlot = -1; UINT64 r28WaitFv = 0; bool r28LeaseOk = false;
            R28BSnapshot r28s{ -1, -1, 0, 0 };
            g_tlDecodeWaitEnqueued = 0;
            if (!a3Wake && g_bformOn && g_bformReady && g_upBufW && W.color && g_bformStage >= 3 && UiGateAllowsNr() &&
                BformBEnsureDS(W.dev.Get())) {
                bAttempted = true;
                R28BPollAck();   // r28 块2：每轮开头处理 RETIRING→FREE 和 MAX→NrHardFail
                // 先打开生产围栏，首轮选票即可读完成值；旧路径仍可按滞后回退。
                static bool xfTriedNgx = false;
                if (!W.xferFe && g_xqShare && !xfTriedNgx) {
                    xfTriedNgx = true;
                    if (FAILED(W.dev->OpenSharedHandle(g_xqShare, IID_PPV_ARGS(W.xferFe.GetAddressOf()))))
                        Log("XFER: B 端打开共享围栏失败（%s）", r28Live ? "r28 不认领" : "退回靠滞后");
                    else Log("XFER: B 端已接上跨卡就绪围栏");
                }
                if (r28Live) {
                    decSlot = R28BClaimSnapshot(W, &r28s);
                    if (decSlot >= 0) { r28WaitFv = r28s.fvColor; r28LeaseOk = true; }
                } else {
                    decSlot = (g_upLastSlot + K - 1) % K;
                    r28LeaseOk = true;  // 旧路径：直接允许解码
                }
                R28ShPick(decSlot < 0 ? 0 : decSlot);   // r28 块1：影子扫描，只记账/打日志
                if (g_hotDiagOn) {   // ★ HOTDIAG D1：NGX B 解码侧低频快照（默认关，F6 开；不改解码/等待/槽位行为）
                    static volatile LONG d1n = 0;
                    LONG n = InterlockedIncrement(&d1n);
                    if (n == 1 || (n % 60) == 0)
                        Log("HOTDIAG D1 n=%ld iter=%ld frame=%d upLastSlot=%d decSlot=%d c4LastOkFrame=%d c4LastOkSlot=%d "
                            "tag=%d,%d,%d,%d,%d,%d rd=%llu,%llu,%llu,%llu,%llu,%llu",
                            n, g_tlIter, (int)g_tlFrame, (int)g_upLastSlot, decSlot,
                            (int)g_c4LastOkFrame, (int)g_c4LastOkSlot,
                            (int)g_slotTag[0].frame, (int)g_slotTag[1].frame, (int)g_slotTag[2].frame,
                            (int)g_slotTag[3].frame, (int)g_slotTag[4].frame, (int)g_slotTag[5].frame,
                            (unsigned long long)g_slotReadyFv[0], (unsigned long long)g_slotReadyFv[1],
                            (unsigned long long)g_slotReadyFv[2], (unsigned long long)g_slotReadyFv[3],
                            (unsigned long long)g_slotReadyFv[4], (unsigned long long)g_slotReadyFv[5]);
                }
                if (r28Live && r28LeaseOk) {
                    // 已知 r28WaitFv <= prodDone；保留跨适配器队列顺序，但不会等未完成的生产工作。
                    const HRESULT hrWait = W.q->Wait(W.xferFe.Get(), r28WaitFv);
                    if (FAILED(hrWait)) {
                        InterlockedIncrement(&g_r28CntWaitFail);
                        R28BReleaseUnused(r28s);
                        r28s.slot = -1;
                        r28LeaseOk = false; decSlot = -1;
                    } else {
                        g_tlDecodeWaitEnqueued = 1;
                    }
                }
            }
            const bool holdNoNew = r28Live && !r28LeaseOk;
            if (r28Live && InterlockedCompareExchange(&g_hardFail, 0, 0)) {
                R28BReleaseUnused(r28s);
                break;
            }
            // 无票也无 A 端输入：没有任何 W.cl 录制需求，不 Reset/Close，更不提交。
            if (holdNoNew && !feedHasAPixels) continue;
            if (!ResetNgxBatch(W, NgxEval, "eval")) {
                R28BReleaseUnused(r28s);
                continue;
            }
            if (uploadA) memcpy(W.upPtr, local.data(), local.size());
            if (bAttempted && (!r28Live || r28LeaseOk)) {
                if (!r28Live) {
                    const UINT64 need = g_slotReadyFv[decSlot];
                    g_tlDecodeWaitEnqueued = (need && W.xferFe) ? 1 : 0;
                    if (need && W.xferFe) W.q->Wait(W.xferFe.Get(), need);
                }
                // 解码失败（令牌不匹配 / 未就绪）本帧不出新画。
                decOk = BformBDecode(W.cl.Get(), decSlot);
                // ★ F7：B 侧跑马灯（画在解出的 4K 底图上，必须在降采样之前 ⇒ 喂入图也带上同一条）
                //   ★ 相位/数字必须用【该槽真正属于的帧号】g_slotTag[..].frame，不能用槽号：
                //     槽号只有 0..5 循环（且 g_upLastSlot 在 xferOwn 路径下由 XferKickColor 发布），
                //     用它当相位会出现"条一直落在第一格不动"（2026-10-31 实测踩到）。
                // B 形态已关闭 F7 跑马灯标记，保持与 A 形态一致（仅保留纯净真实画面）
                if (false && decOk && decSlot >= 0 && *(int*)&g_marqueeOn == 1) {
                    // 面单由 XferKickColor 在【颜色拷贝成功后】发布（:4768），所以这个帧号是可信的；
                    // 还没发布过（=0）就本轮不画，等它发布。
                    int mf = (int)g_slotTag[decSlot].frame;
                    if (mf > 0 && decOk) BformBMarkSlot(W.cl.Get(), mf);
                }
                if (decOk && g_bTexC) {
                    // W.color 上一帧可能被核心留在 SRV 态（sampled in place），先摆回 COMMON；
                    // 降采样内部对它不做状态转换（沿用 B 形态原约定），所以必须由我们摆好。
                    Barrier(W.cl.Get(), W.color.Get(), W.ngxColState, D3D12_RESOURCE_STATE_COMMON);
                    if (BformBDownsample(W.cl.Get(), W.color.Get())) {
                        W.ngxColState = D3D12_RESOURCE_STATE_COMMON;
                        bPrep = true;
                    }
                }
                // MV：g_bTexM（槽位解出的 RG16F，普通纹理）→ W.vel（shared：核心要 import 到 Vulkan）
                // r28 块4：若 s.mvOk==0，跳过 BformSlotMvToShared；HIT-3：改为把 W.vel 清零（upload 零拷贝），让 NGX 走"无运动"假设
                bMvOk = false;
                {
                    bool mvOkSlot = !r28Live || (decSlot >= 0 && decSlot < K && g_r28[decSlot].mvOk);
                    if (!mvOkSlot) InterlockedIncrement(&g_r28CntMvOkZero);
                    if (bPrep && W.vel) {
                        if (mvOkSlot) {
                            if (BformSlotMvToShared(W.cl.Get(), W.vel.Get(), W.ngxMvState)) {
                                W.ngxMvState = D3D12_RESOURCE_STATE_COMMON;
                                bMvOk = true;
                            }
                        } else {
                            // HIT-3：mvOk=0 → 喂零 MV（upload heap 零拷贝，不需要 UAV）
                            const UINT mvZeroPitch = ((UINT)kNetW * 4u + 255u) & ~255u;
                            if (!W.bMvZeroUp && W.dev) {
                                D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
                                D3D12_RESOURCE_DESC bd{};
                                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                                bd.Width = (UINT64)mvZeroPitch * kNetH;
                                bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
                                bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
                                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                                ComPtr<ID3D12Resource> up;
                                if (SUCCEEDED(W.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)))) {
                                    void* p = nullptr;
                                    if (SUCCEEDED(up->Map(0, nullptr, &p))) {
                                        memset(p, 0, (size_t)bd.Width);
                                        up->Unmap(0, nullptr);
                                    }
                                    W.bMvZeroUp = std::move(up);
                                    Log("R28B4-HIT3: 零MV upload buffer 就绪（%dx%d pitch=%u）", kNetW, kNetH, mvZeroPitch);
                                }
                            }
                            if (W.bMvZeroUp) {
                                D3D12_TEXTURE_COPY_LOCATION vd{};
                                vd.pResource = W.vel.Get();
                                vd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                vd.SubresourceIndex = 0;
                                D3D12_TEXTURE_COPY_LOCATION vs{};
                                vs.pResource = W.bMvZeroUp.Get();
                                vs.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                vs.PlacedFootprint.Offset = 0;
                                vs.PlacedFootprint.Footprint.Format    = DXGI_FORMAT_R16G16_FLOAT;
                                vs.PlacedFootprint.Footprint.Width     = (UINT)kNetW;
                                vs.PlacedFootprint.Footprint.Height    = (UINT)kNetH;
                                vs.PlacedFootprint.Footprint.Depth     = 1;
                                vs.PlacedFootprint.Footprint.RowPitch  = mvZeroPitch;
                                Barrier(W.cl.Get(), W.vel.Get(), W.ngxMvState, D3D12_RESOURCE_STATE_COPY_DEST);
                                W.cl->CopyTextureRegion(&vd, 0, 0, 0, &vs, nullptr);
                                Barrier(W.cl.Get(), W.vel.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                                W.ngxMvState = D3D12_RESOURCE_STATE_COMMON;
                            }
                        }
                    }
                }
                static int nprep = 0;
                if ((nprep++ % 300) == 0)
                    Log("BFORM-B(NGX): 准备阶段已接入第 %d 次（槽 %d，色 %s / MV %s）",
                        nprep, decSlot, bPrep ? "OK" : "FAIL", bMvOk ? "装盘" : "缺");
            }
            // A fallback is valid only for a wake carrying A pixels. A B-only wake must not read the old shared input.
            const auto currentAInput = [&] {
                std::lock_guard<std::mutex> lk(W.m);
                return W.aReset.Current(feedAGen, feedAResetEpoch);
            };
            const bool wantsA = !bPrep && feedHasAPixels && currentAInput();
            auto* a2Source = wantsA && !holdNoNew && A2In::requested ?
                a2Lease.Acquire({frameId, feedAGen, feedAResetEpoch}, W.q.Get(), W.color.Get(), W.vel.Get()) : nullptr;
            const bool useAInput = wantsA && (!A2In::requested || a2Source != nullptr);
            F9Id::Path f9ColorPath = F9Id::NoInputCopy, f9MvPath = F9Id::NoInputCopy;
            const bool useFp16Ngx = (a2Source != nullptr && a2Token.slot && a2Token.slot->a3);
            if (a2Source) {
                A2In::RecordWorkerCopies(W.cl.Get(), a2Source,
                                         useFp16Ngx ? W.colorFp16.Get() : W.color.Get(), W.vel.Get(),
                                         useFp16Ngx ? W.ngxColFp16State : W.ngxColState, W.ngxMvState);
                if (useFp16Ngx) W.ngxColFp16State = D3D12_RESOURCE_STATE_COMMON;
                else            W.ngxColState     = D3D12_RESOURCE_STATE_COMMON;
                W.ngxMvState = D3D12_RESOURCE_STATE_COMMON;
                f9ColorPath = f9MvPath = F9Id::A2Pair;
            } else if (useAInput) {
                D3D12_TEXTURE_COPY_LOCATION cd{};
                cd.pResource = W.color.Get();
                cd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                cd.SubresourceIndex = 0;
                if (g_xRead && g_xOk && g_xBufInW) {
                    D3D12_TEXTURE_COPY_LOCATION cs{};
                    f9ColorPath = F9Id::Shared;
                    cs.pResource = g_xBufInW.Get();
                    cs.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    cs.PlacedFootprint.Offset = XHDR;
                    cs.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    cs.PlacedFootprint.Footprint.Width = kNetW;
                    cs.PlacedFootprint.Footprint.Height = kNetH;
                    cs.PlacedFootprint.Footprint.Depth = 1;
                    cs.PlacedFootprint.Footprint.RowPitch = kNetW * 4;
                    Barrier(W.cl.Get(), W.color.Get(), W.ngxColState, D3D12_RESOURCE_STATE_COPY_DEST);
                    W.cl->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
                    Barrier(W.cl.Get(), W.color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                    W.ngxColState = D3D12_RESOURCE_STATE_COMMON;
                } else if (W.up) {
                    f9ColorPath = F9Id::CpuUpload;
                    D3D12_TEXTURE_COPY_LOCATION cs{};
                    cs.pResource = W.up.Get();
                    cs.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    cs.PlacedFootprint = W.fp;
                    Barrier(W.cl.Get(), W.color.Get(), W.ngxColState, D3D12_RESOURCE_STATE_COPY_DEST);
                    W.cl->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
                    Barrier(W.cl.Get(), W.color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                    W.ngxColState = D3D12_RESOURCE_STATE_COMMON;
                }
            }
            // MV: cross-card buffer -> this card's net RG16F texture (shared, the core samples it in place)
            if (useAInput && !a2Source && W.vel && g_xBufVelW) {
                f9MvPath = F9Id::Shared;
                D3D12_TEXTURE_COPY_LOCATION vd{};
                vd.pResource = W.vel.Get();
                vd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                vd.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION vs{};
                vs.pResource = g_xBufVelW.Get();
                vs.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                vs.PlacedFootprint.Offset = 0;
                vs.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
                vs.PlacedFootprint.Footprint.Width = kNetW;
                vs.PlacedFootprint.Footprint.Height = kNetH;
                vs.PlacedFootprint.Footprint.Depth = 1;
                vs.PlacedFootprint.Footprint.RowPitch = kNetW * 4;
                Barrier(W.cl.Get(), W.vel.Get(), W.ngxMvState, D3D12_RESOURCE_STATE_COPY_DEST);
                W.cl->CopyTextureRegion(&vd, 0, 0, 0, &vs, nullptr);
                Barrier(W.cl.Get(), W.vel.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                W.ngxMvState = D3D12_RESOURCE_STATE_COMMON;
            }
            const DXGI_FORMAT wantNgxFmt = useFp16Ngx ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            if (!holdNoNew && NgxBackendActive() && NgxBackendFormat() != wantNgxFmt) {
                if (W.ngx[NgxReadback].lastFv) WaitWFence(W.ngx[NgxReadback].lastFv, kWorkerWaitMs);
                if (W.ngx[NgxCompose].lastFv)  WaitWFence(W.ngx[NgxCompose].lastFv, kWorkerWaitMs);
            }
            const bool ensureOk = !holdNoNew && NgxBackendEnsure(W.cl.Get(), kNetW, kNetH, wantNgxFmt);
            if (W.ngxMvDimW != kNetW || W.ngxMvDimH != kNetH) {
                W.ngxMvDimW = kNetW; W.ngxMvDimH = kNetH;
                W.ngxMvX = (float)kNetW;
                W.ngxMvY = (float)kNetH;
                std::string fp = ModuleDir() + "xess_proxy.mvscale.txt";
                FILE* mf = nullptr; float ax = 0.0f, ay = 0.0f;
                if (fopen_s(&mf, fp.c_str(), "r") == 0 && mf) {
                    const int n = fscanf_s(mf, "%f %f", &ax, &ay);
                    if (n >= 1) { W.ngxMvX = ax; W.ngxMvY = (n >= 2) ? ay : ax; }
                    fclose(mf);
                }
                Log("NGX: MV scale = %.4f, %.4f px/MV-unit（xess_proxy.mvscale.txt 可写两个数覆盖）", W.ngxMvX, W.ngxMvY);
            }
            // The NGX pixel scales above are independent of residual reprojection's UV scale.
            // r28 NO_NEW 不执行 Eval；没有 B 准备结果、也没有 A 输入的唤醒同样不执行。
            bool freshA = false;
            unsigned resetReasons = 0;
            int resetEventFrame = -1;
            if (useAInput && !holdNoNew && ensureOk) {
                std::lock_guard<std::mutex> lk(W.m);
                freshA = W.aReset.Current(feedAGen, feedAResetEpoch) &&
                         (!a2Source || !A2In::broken.load());
                resetReasons = W.aReset.Pending(feedAGen, feedAResetEpoch);
                resetEventFrame = W.aReset.lastEventFrame;
            }
            const bool skipEval = holdNoNew || (!bPrep && !freshA);
            const bool resetA = freshA && feedAGen != lastEvaluatedAGen;
            F9Id::WRecord f9Eval(W.cl.Get(), W.dev.Get(), f9Dispatch, feedAGen, feedAResetEpoch);
            ID3D12Resource* ngxColorIn = useFp16Ngx ? W.colorFp16.Get() : W.color.Get();
            if (f9Eval.part && freshA && !skipEval && ensureOk) {
                const auto audit = a3.a ? A3::Audit(*a3.a) : F9Id::A3Meta{};
                f9Eval.Tap(F9Id::WColor, ngxColorIn, useFp16Ngx ? W.ngxColFp16State : W.ngxColState, frameId, frameId, f9ColorPath, audit);
                if (W.vel) f9Eval.Tap(F9Id::WMv, W.vel.Get(), W.ngxMvState, frameId,
                                      a2Source ? frameId : frameId + 1, f9MvPath, audit);
                else F9Id::Count(F9Id::WMv, F9Id::NotProduced);
            } else if (f9Eval.part && feedHasAPixels && !bPrep) {
                F9Id::Count(F9Id::WColor, F9Id::NotProduced);
                F9Id::Count(F9Id::WMv, F9Id::NotProduced);
            }
            const bool evalOk = !skipEval && ensureOk && NgxBackendEval(W.cl.Get(), ngxColorIn, W.vel.Get(),
                                                           kNetW, kNetH, W.processed < 3 || resetA || resetReasons != 0,
                                                           W.ngxMvX, W.ngxMvY);
            if (evalOk) {   // the core samples colour/MV in place and leaves them shader-readable
                if (useFp16Ngx) W.ngxColFp16State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                else            W.ngxColState     = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                W.ngxMvState  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
            BtTrace("BPREP_END", frameId, "prep",
                    "iter=%ld b_on=%d attempted=%d color_recorded=%d mv_recorded=%d ensure=%d eval=%d",
                    g_tlIter, (int)g_bformOn, bAttempted ? 1 : 0, bPrep ? 1 : 0, bMvOk ? 1 : 0,
                    ensureOk ? 1 : 0, evalOk ? 1 : 0);
            g_tlCloseHr = W.cl->Close();
            if (FAILED(g_tlCloseHr)) { NrHardFail("B1: NGX eval command list close failed"); break; }
            // r28 块2：记 leaseRv = W.fv+1（SignalW 内部 ++W.fv，完成 = W.fe >= leaseRv）
            if (r28Live && r28LeaseOk && decSlot >= 0) {
                UINT64 lr = W.fv + 1;
                g_r28[decSlot].leaseRv = lr;
                LONG gate = InterlockedCompareExchange(&g_r28[decSlot].gate, 0, 0);
                InterlockedExchange(&g_r28[decSlot].gate,
                    R28MakeGate(R28GateEpoch(gate), R28GateGen(gate), R_RETIRING));
            }
            const bool submittedEval = SubmitNgxBatch(W, NgxEval, "eval");
            a2Lease.Finish(submittedEval, W.fe.Get(), W.fv, evalOk); // both input copies are on this exact W batch
            f9Eval.Finish(submittedEval, evalOk, W.q.Get(), W.fe.Get(), W.fv);
            if (!submittedEval) { NrHardFail("B1: NGX eval signal failed"); break; }
            if (evalOk) {
                lastEvaluatedAGen = useAInput ? feedAGen : -1;
                bool keepOutput = true, acked = false;
                if (useAInput) {
                    std::lock_guard<std::mutex> lk(W.m);
                    keepOutput = W.aReset.Current(feedAGen, feedAResetEpoch) &&
                                 (!a2Source || !A2In::broken.load());
                    // Compare the exact epoch after Eval, Close, Execute and Signal. Never clear
                    // an event raised during an in-flight Eval, even for the same reason.
                    acked = resetReasons && W.aReset.Ack(feedAGen, feedAResetEpoch);
                }
                if (acked) {
                    Log("[A1-NGX-RESET] reset=1 event_frame=%d feed_frame=%d gen=%d epoch=%llu reasons=%s%s%s%s",
                        resetEventFrame, frameId, feedAGen, (unsigned long long)feedAResetEpoch,
                        (resetReasons & A1GameReset) ? "gameReset " : "", (resetReasons & A1UiResume) ? "uiResume " : "",
                        (resetReasons & A1Rebuild) ? "rebuild " : "", (resetReasons & A1SwitchA) ? "switchA" : "");
                }
                if (keepOutput) po = (ID3D12Resource*)NgxBackendOutput();
            }
            if (!po) {
                if (f9Eval.part && useAInput) F9Id::Count(F9Id::WAnswer, F9Id::NotProduced);
                static int nb = 0;
                if ((nb++ % 120) == 0) Log("NGX: no answer this frame (ensure=%d eval=%d)", (int)ensureOk, (int)evalOk);
            } else if (a3Wake) {
                if (!a3.a || !ResetNgxBatch(W, NgxReadback, "a3-writeback")) continue;
                F9Id::WRecord f9Answer(W.cl.Get(), W.dev.Get(), f9Dispatch, feedAGen, feedAResetEpoch);
                F9Id::Transition(W.cl.Get(), po, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                F9Id::Transition(W.cl.Get(), a3.a->bufW.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                f9Answer.Tap(F9Id::WAnswer, po, D3D12_RESOURCE_STATE_COPY_SOURCE, frameId, frameId,
                             F9Id::Shared, A3::Audit(*a3.a));
                const auto dst = A2In::Foot(a3.a->bufW.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
                const auto src = A2In::Tex(po);
                W.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                F9Id::Transition(W.cl.Get(), a3.a->bufW.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                F9Id::Transition(W.cl.Get(), po, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                const bool submitted = SUCCEEDED(W.cl->Close()) && SubmitNgxBatch(W, NgxReadback, "a3-writeback");
                f9Answer.Finish(submitted, true, W.q.Get(), W.fe.Get(), W.fv);
                a3.Submitted(submitted, W.fe.Get(), W.fv, f9Dispatch);
                if (!submitted) { NrHardFail("A3: NGX writeback close/signal failed"); break; }
                if (WaitWFence(W.ngx[NgxReadback].lastFv, kWorkerWaitMs)) {
                    std::lock_guard lk(A3::m);
                    A3::SettleWriter(f9Dispatch);
                }
            } else {
                D3D12_RESOURCE_DESC pd = po->GetDesc();
                D3D12_RESOURCE_DESC pl = pd; pl.Flags = D3D12_RESOURCE_FLAG_NONE;
                UINT rows2 = 0; UINT64 rs2 = 0, tot2 = 0;
                W.dev->GetCopyableFootprints(&pl, 0, 1, 0, &W.fpPriv, &rows2, &rs2, &tot2);
                if (!W.rbPriv || W.bytesPriv != tot2) {
                    g_tlPhase = "readback";
                    if (W.ngx[NgxReadback].lastFv && !WaitWFence(W.ngx[NgxReadback].lastFv, kWorkerWaitMs)) continue;
                    D3D12_RESOURCE_DESC bd2{};
                    bd2.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    bd2.Width = tot2; bd2.Height = 1; bd2.DepthOrArraySize = 1; bd2.MipLevels = 1;
                    bd2.Format = DXGI_FORMAT_UNKNOWN; bd2.SampleDesc.Count = 1;
                    bd2.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    D3D12_HEAP_PROPERTIES rhb{}; rhb.Type = D3D12_HEAP_TYPE_READBACK;
                    rhb.CreationNodeMask = 1; rhb.VisibleNodeMask = 1;
                    W.rbPriv.Reset();
                    W.dev->CreateCommittedResource(&rhb, D3D12_HEAP_FLAG_NONE, &bd2,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(W.rbPriv.GetAddressOf()));
                    W.bytesPriv = tot2;
                }
                // second submission: this is where the answer is copied out (one frame late)
                if (!W.rbPriv || !ResetNgxBatch(W, NgxReadback, "readback")) continue;
                D3D12_RESOURCE_BARRIER nb2{};
                nb2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                nb2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                nb2.Transition.pResource = po;
                nb2.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                nb2.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                W.cl->ResourceBarrier(1, &nb2);
                F9Id::WRecord f9Answer(W.cl.Get(), W.dev.Get(), f9Dispatch, feedAGen, feedAResetEpoch);
                if (useAInput && g_xOk && g_xBufOutW) {   // 只有 A 输入产生的答案才写 A 跨卡通道
                    f9Answer.Tap(F9Id::WAnswer, po, D3D12_RESOURCE_STATE_COPY_SOURCE, frameId, frameId, F9Id::Shared);
                    D3D12_TEXTURE_COPY_LOCATION xd{};
                    xd.pResource = g_xBufOutW.Get();
                    xd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    xd.PlacedFootprint.Offset = XHDR + (UINT64)idx * (UINT64)(kNetW * kNetH * 4);
                    xd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    xd.PlacedFootprint.Footprint.Width = kNetW;
                    xd.PlacedFootprint.Footprint.Height = kNetH;
                    xd.PlacedFootprint.Footprint.Depth = 1;
                    xd.PlacedFootprint.Footprint.RowPitch = kNetW * 4;
                    D3D12_TEXTURE_COPY_LOCATION xs{};
                    xs.pResource = po;
                    xs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    xs.SubresourceIndex = 0;
                    W.cl->CopyTextureRegion(&xd, 0, 0, 0, &xs, nullptr);
                }
                D3D12_TEXTURE_COPY_LOCATION dd{};
                dd.pResource = W.rbPriv.Get();
                dd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dd.PlacedFootprint = W.fpPriv;
                D3D12_TEXTURE_COPY_LOCATION ss{};
                ss.pResource = po;
                ss.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                ss.SubresourceIndex = 0;
                W.cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);
                nb2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                nb2.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;   // restore: the core writes it again
                W.cl->ResourceBarrier(1, &nb2);
                g_tlCloseHr = W.cl->Close();
                const bool submittedReadback = SUCCEEDED(g_tlCloseHr) && SubmitNgxBatch(W, NgxReadback, "readback");
                f9Answer.Finish(submittedReadback, true, W.q.Get(), W.fe.Get(), W.fv);
                if (!submittedReadback) {
                    NrHardFail("B1: NGX readback close/signal failed"); break;
                }
                void* rm = nullptr;
                D3D12_RANGE rr{ 0, (SIZE_T)W.bytesPriv };
                if (useAInput && WaitWFence(W.ngx[NgxReadback].lastFv, kWorkerWaitMs) &&
                    SUCCEEDED(W.rbPriv->Map(0, &rr, &rm)) && rm) {
                    UINT pitch = W.fpPriv.Footprint.RowPitch;
                    for (UINT y = 0; y < kNetH; ++y)
                        memcpy(&W.outBuf[(size_t)y * kNetW * 4],
                               (unsigned char*)rm + (size_t)y * pitch, (size_t)kNetW * 4);
                    D3D12_RANGE nw{ 0, 0 };
                    W.rbPriv->Unmap(0, &nw);
                    {
                        std::lock_guard<std::mutex> lk(W.m);
                        if (W.aReset.Current(feedAGen, feedAResetEpoch) &&
                            idx >= 0 && idx < K && (int)W.ansBuf.size() == K) {
                            memcpy(W.ansBuf[idx].data(), W.outBuf.data(),
                                   (std::min)(W.ansBuf[idx].size(), W.outBuf.size()));
                            W.ansFrame[idx] = frameId;
                            W.ansAGen[idx] = feedAGen;
                            delivered = true;
                            W.hasOut = true;
                        }
                    }
                    if (delivered) W.cvOut.notify_all();
                }
            }
            // ============ ★ 2026-10-31 N1-收尾：B 形态在 NGX 后端下的"合成 + 出画" ============
            //   同一个原因：合成/出画原本也只写在 lmxxf 分支里（:1214 / :1300），走 ngx 时一并被跳过
            //   ⇒ B 卡永远只"解码 + 喂网"，从不出画。这里补齐：合成 →（4K 时直通）→ 覆盖层出画。
            //   答案状态：核心把 po 留在 UNORDERED_ACCESS（下次评估还要写），而 BformBCompose 从
            //   COMMON 起跳 ⇒ 进之前摆回 COMMON，出之后恢复 UAV。
            static bool bCompFailNgx = false;
            static int lastCompSlot = 1;
            const bool composeAllowed = po && decOk && g_bformOn && g_bformReady && g_bformStage >= 4 &&
                !bCompFailNgx && UiGateAllowsNr() && modeEpoch == InterlockedCompareExchange(&g_r28Epoch, 0, 0) &&
                uiEpoch == BformUiEpoch();
            if (composeAllowed) {
                if (!BformBEnsureComp(W.dev.Get()) ||
                    (g_bformStage >= 5 && !BformBEnsureUp(W.dev.Get()))) {
                    bCompFailNgx = true;
                    Log("BFORM-B(NGX): 合成/上采样资源就绪失败 → 本会话不再尝试 B 出画");
                } else {
                    if (g_bformStage >= 6 && !g_bpReady && !g_bpFailed) BformPresentInit();
                    const int slot = lastCompSlot ^ 1;
                    const bool direct4k = g_bform4k;
                    const bool upscale = g_bformStage >= 5 && !direct4k && g_bUpReady;
                    if (!ResetNgxBatch(W, NgxCompose, "compose")) continue;
                    // The compose allocator fence also guards the previous DIAG4 copy.
                    BformDiagReadback(W);
                    if (!BformWaitPresentReads(W, slot, upscale)) {
                        W.cl->Close();   // discard the empty recording
                        break;
                    }
                    Barrier(W.cl.Get(), po, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                    // B 形态已关闭 F8 三横带诊断，直接走正常 BformBCompose 合成
                    const bool recorded = BformBCompose(W.cl.Get(), po, slot);
                    const bool upRecorded = recorded && upscale && BformBUpscale(W.cl.Get(), slot);
                    if (recorded) BformDiagEnqueue(W, po, slot, upRecorded);
                    Barrier(W.cl.Get(), po, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    g_tlCloseHr = W.cl->Close();
                    if (FAILED(g_tlCloseHr)) { NrHardFail("B1: NGX compose command list close failed"); break; }
                    if (recorded) {
                        if (!SubmitNgxBatch(W, NgxCompose, "compose")) { NrHardFail("B1: NGX compose signal failed"); break; }
                        const UINT64 composeFv = W.ngx[NgxCompose].lastFv;
                        auto& stamp = g_bCompStamp[slot];
                        stamp.composeFv = composeFv;
                        stamp.uiEpoch = uiEpoch;
                        stamp.modeEpoch = modeEpoch;
                        stamp.direct4k = direct4k;
                        stamp.hadValidPresent = false;
                        lastCompSlot = slot;
                        if (upRecorded) g_bUpComposeFv = composeFv;
                        if ((direct4k || upRecorded) && g_bformOn && g_bformStage >= 6 && g_bpReady &&
                            UiGateAllowsNr() && uiEpoch == BformUiEpoch() &&
                            modeEpoch == InterlockedCompareExchange(&g_r28Epoch, 0, 0))
                            stamp.hadValidPresent = BformPresentFrame(slot, composeFv);
                    }
                }
            }
            // NO_NEW 由 DComp 保持当前画面，不补 CopyResource/Present；保留原解码失败回退。
            const bool holdDecode = po != nullptr && !decOk;
            if (holdDecode && !holdNoNew && g_bformOn && g_bformStage >= 6 && g_bpReady && UiGateAllowsNr()) {
                const UINT nowUiEpoch = BformUiEpoch();
                const LONG nowModeEpoch = InterlockedCompareExchange(&g_r28Epoch, 0, 0);
                const int slot = BformCompletedSlot(W, nowUiEpoch, nowModeEpoch);
                if (slot >= 0) {
                    static unsigned holds = 0;
                    if ((holds++ % 300) == 0)
                        Log("BFORM-B: 解码失败，保持本代已完成上一帧（slot=%d，累计 %u）", slot, holds);
                    BformPresentFrame(slot, g_bCompStamp[slot].composeFv, true);
                }
            }
        } else if (W.api.PrepareFrame) {
            // ★ 待清理：lmxxf 后端分支。2077 线默认后端为 NGX（xess_proxy.backend.txt=ngx），本段默认不会执行，属无用代码，后续统一清理；在此之前不要再改这里的逻辑。
            // ★ 双分配器轮转：本轮换用另一个分配器，并确认它上次那批命令已经执行完（有界等待）。
            //   以前只有一个分配器、Reset 也不检查 → D3D12 非法用法，偶发丢帧/长帧的嫌疑就在这。
            W.ai ^= 1;
            W.curAl = W.ai ? W.al2.Get() : W.al.Get();
            if (W.aiFv[W.ai]) {
                ULONGLONG tw0 = GetTickCount64();
                while (W.fe->GetCompletedValue() < W.aiFv[W.ai] && (GetTickCount64() - tw0) < 25)
                    SwitchToThread();
            }
            W.curAl->Reset(); W.cl->Reset(W.curAl, nullptr);
            {
                D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = W.color.Get();
                d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
                static bool loggedR4 = false;
                if (g_xRead && g_xOk && g_xBufInW) {
                    D3D12_TEXTURE_COPY_LOCATION s{};
                    s.pResource = g_xBufInW.Get();
                    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    s.PlacedFootprint.Offset = XHDR;   // RE2 原样：单段
                    s.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    s.PlacedFootprint.Footprint.Width = kNetW;
                    s.PlacedFootprint.Footprint.Height = kNetH;
                    s.PlacedFootprint.Footprint.Depth = 1;
                    s.PlacedFootprint.Footprint.RowPitch = kNetW * 4;
                    W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                    if (!loggedR4) { loggedR4 = true; Log("R4: worker 已改为从跨卡共享 buffer 读输入"); }
                } else {
                    D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = W.up.Get();
                    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = W.fp;
                    W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                }
                // 【B 形态 S2/S3】把 A 送来的原始 pColor/MV/深度 解成本地纹理，
                // 然后**在 B 上自己降采样**喂给 NR —— 不再依赖 A 传过来的 1080p
                if (g_bformOn && g_bformReady && g_upBufW && g_bformStage >= 2 && UiGateAllowsNr()) {
                    static bool dsFail = false, compFail = false, upFail = false, ppFail = false;
                    if (g_bformStage >= 7) {
                        // 【流水线 Stage1】解码 + 降采样 → 本组缓冲；NR 输入也在这一步准备好
                        if (!ppFail) {
                            if (!BformPPEnsure(W.dev.Get()) || !BformBEnsureDS(W.dev.Get()) ||
                                !BformBEnsureComp(W.dev.Get()) || !BformBEnsureUp(W.dev.Get())) ppFail = true;
                            else BformPPStage1(W.cl.Get(), g_ppCur, W.color.Get(), nullptr, nullptr);
                        }
                    } else {
                        // 读【上一帧写完的槽】：比最新槽安全（A 的跨卡拷贝肯定已完成）
                        const int decSlot = (g_upLastSlot + K - 1) % K;
                        // ★ 防饿死门：如果 A 已经跑到我们前面 K-2 帧以外，这一格可能正被改写/已被绕圈覆盖
                        //   ⇒ 解码会读到"半新半旧"的底图（撕裂帧）。这时本轮跳过【合成】，画面保持上一帧，
                        //     宁可少更新一次，也不出一帧撕裂。（解码/喂网络照旧，不影响恢复）
                        {
                            static int lastDecF = 0;
                            int aNewF = g_slotTag[g_upLastSlot % K].frame;
                            g_bStarved = (lastDecF > 0) && ((aNewF - lastDecF) >= (K - 2));
                            if (g_bStarved) {
                                static int starve = 0;
                                if ((starve++ % 60) == 0)
                                    Log("!! 防饿死：B 落后 %d 帧（>=K-2）→ 本轮跳过合成，累计 %d 次",
                                        aNewF - lastDecF, starve);
                            }
                            lastDecF = g_slotTag[decSlot].frame;   // 记下本解码到的帧号
                        }
                        // ★ 等 A 那边"4K 颜色拷贝"完成才解码（跨卡共享围栏；GPU 侧等待，不占 CPU）
                        {
                            static bool xfTried = false;
                            if (!W.xferFe && g_xqShare && !xfTried) {
                                xfTried = true;
                                if (FAILED(W.dev->OpenSharedHandle(g_xqShare, IID_PPV_ARGS(W.xferFe.GetAddressOf()))))
                                    Log("XFER: B 端打开共享围栏失败（退回靠滞后）");
                                else Log("XFER: B 端已接上跨卡就绪围栏");
                            }
                            UINT64 need = g_slotReadyFv[decSlot];
                            g_tlDecodeWaitEnqueued = (need && W.xferFe) ? 1 : 0;
                            if (need && W.xferFe) W.q->Wait(W.xferFe.Get(), need);
                        }
                        DumpFlush(1);
                        BformBDecode(W.cl.Get(), decSlot);
                        DumpRecord(W.cl.Get(), W.dev.Get(), 1, g_bTexC.Get(), D3D12_RESOURCE_STATE_COMMON);
                        // 【面单校验】把"A 最新写的是第几帧 / B 这轮解码的是第几帧 / 落后几帧"打出来。
                        //   以前这一层完全不可见：配错了只会表现为"残差怪"，不会报错。
                        {
                            static int pairCnt = 0;
                            if ((pairCnt++ % 150) == 0) {
                                int aNew = g_slotTag[g_upLastSlot % K].frame;
                                int bDec = g_slotTag[decSlot].frame;
                                Log("BFORM-PAIR: A 最新写第 %d 帧（槽%d）| B 解码第 %d 帧（槽%d）| 落后 %d 帧 | jitter=%.2f,%.2f",
                                    aNew, (int)(g_upLastSlot % K), bDec, decSlot,
                                    aNew - bDec, (float)g_slotTag[decSlot].jx, (float)g_slotTag[decSlot].jy);
                            }
                        }
                        // ★ 每个 Ensure 只尝试一次：失败就永久跳过，否则会每帧重编译 shader（FPS 曾被打到 18）
                        if (g_bformStage >= 3 && !dsFail) {
                            if (!BformBEnsureDS(W.dev.Get())) dsFail = true;
                            else BformBDownsample(W.cl.Get(), W.color.Get());
                        }
                        if (g_bformStage >= 4) {
                            if (!compFail && !BformBEnsureComp(W.dev.Get())) compFail = true;
                            if (!upFail && !BformBEnsureUp(W.dev.Get())) upFail = true;
                        }
                    }
                }
            }
            Job job{};
            job.struct_size = (uint32_t)sizeof(Job);
            FrameInfo160 fi{};
            fi.struct_size = W.frameSize;   // 跟随 ABI：160 / 104 / 80
            // 注：下面的 exposure/dLSS 自造字段（depth/velocity/ctl*）写在 104 之后，声明尺寸更小时运行时不读，无副作用
            fi.session_id = 1; fi.frame_id = (uint64_t)W.processed + 1; fi.list_generation = 1;
            fi.command_list = W.cl.Get();
            fi.color_width = kNetW; fi.color_height = kNetH;
            fi.color = W.color.Get();
            fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
            // flags：bit0=强度生效 bit1=调试视图生效（否则这两个字段会被运行时忽略）
            {
                uint32_t fl = 0;
                if (g_transfer < 0.999f || g_colorStr < 0.999f) fl |= 1u;   // STRENGTH
                if (g_debugView != 0) fl |= 2u;                                // DEBUG_VIEW
                fi.flags = fl;
            }
            fi.transfer_strength = (float)g_transfer; fi.color_strength = (float)g_colorStr;
            fi.debug_view = (uint32_t)g_debugView;
            fi.model_scale = 1.0f; fi.pre_exposure = 1.0f; fi.exposure_scale = 1.0f;
            fi.depth = nullptr; fi.velocity = nullptr;
            fi.ctlTone = 1.0f; fi.ctlStructure = 1.0f; fi.ctlSkin = 3.0f; fi.ctlOther = 2.0f; fi.ctlStyle = 0.0f;

            // rc=4 = "building the network"（mochizuki 首次建网要几百毫秒~几十秒）。
            // 这时不能把它当成"这一帧失败"丢掉：丢掉会导致下游不再喂帧（第一帧永远没有答案）。
            // 正确做法是原地重试同一帧（官方 call-order 文档也是"PrepareFrame 很便宜"）。
            int32_t r;
            int waits = 0;
            for (;;) {
                r = W.api.PrepareFrame(W.ctx, &fi, &job);
                if (r != 4 || waits >= 1200) break;      // 最多等 120 秒
                if (waits == 0) Log("worker: 运行时还在建网络，原地重试同一帧（不丢帧）");
                Sleep(100);
                ++waits;
                job = Job{}; job.struct_size = (uint32_t)sizeof(Job);
            }
            if (waits > 0) Log("worker: 建网络等待结束（等了 %d×100ms，rc=%d）", waits, (int)r);
            if (W.processed < 3) Log("worker: [%d] PrepareFrame 返回 %d", W.processed, (int)r);
            if (r != 0) {
                // 关键：PrepareFrame 失败（例如 rc=4 "building the network"）也可能留下了一个 job 句柄，
                // mochizuki 在下一次 PrepareFrame 里会等上一个 job 被交还 —— 不归还就会永久阻塞。
                if (job.handle) {
                    int32_t cr = -1;
                    if (W.api.CancelUnsubmitted) cr = W.api.CancelUnsubmitted(W.ctx, job.handle);
                    else if (W.api.Retire)       cr = W.api.Retire(W.ctx, job.handle);
                    if (W.processed < 3) Log("worker: [%d] 释放失败 job handle（CancelUnsubmitted -> %d）",
                                             W.processed, (int)cr);
                }
                if (W.processed < 3 || (W.processed % 150) == 0) {
                    char eb[256] = { 0 }; W.api.GetLastError(eb, 256);
                    Log("worker: PrepareFrame -> %d err=%s（第 %d 帧）", (int)r, eb, W.processed);
                }
            } else {
                if (W.processed < 3) Log("worker: [%d] 进入 RecordInputs", W.processed);
                W.api.RecordInputs(W.ctx, job.handle, W.cl.Get());
                g_tlCloseHr = W.cl->Close();
                BtWorkerExec("eval");
                { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                // 【流水线 Stage2】趁着 NR 还在 GPU 上跑，把【上一帧】的 合成+上采样+出画 发到 g_bpQ
                //   读的是另一组缓冲，与 Stage1(N) 不冲突；这是本方案唯一能真正并行的地方
                if (g_bformOn && g_bformStage >= 7 && g_ppReady && g_bpReady && g_ppHavePrev)
                    BformPPStage2(1 - g_ppCur);
                if (!WaitW() && W.processed < 3) Log("worker: [%d] producer 提交后等待超时", W.processed);
                if (W.processed < 3) Log("worker: [%d] 进入 ExecuteAfterProducer", W.processed);
                if (W.api.ExecuteAfterProducer(W.ctx, job.handle, W.q.Get()) == 0) {
                    if (W.processed < 3) Log("worker: [%d] ExecuteAfterProducer ok，进入 RecordOutputs", W.processed);
                    g_tlAllocResetHr = W.curAl->Reset(); g_tlResetHr = W.cl->Reset(W.curAl, nullptr);
                    W.api.RecordOutputs(W.ctx, job.handle, W.cl.Get());
                    g_tlCloseHr = W.cl->Close();
                    BtWorkerExec("readback");
                    { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                    if (!WaitW() && W.processed < 3) Log("worker: [%d] consumer 提交后等待超时", W.processed);

                    ID3D12Resource* po = (ID3D12Resource*)job.private_output;
                    if (po) {
                        D3D12_RESOURCE_DESC pd = po->GetDesc();
                        D3D12_RESOURCE_DESC pl = pd; pl.Flags = D3D12_RESOURCE_FLAG_NONE;
                        UINT r2 = 0; UINT64 rs2 = 0, tot2 = 0;
                        W.dev->GetCopyableFootprints(&pl, 0, 1, 0, &W.fpPriv, &r2, &rs2, &tot2);
                        {   // 【诊断·一次性】把 NR 答案纹理的真实描述打出来：格式/尺寸/数组/行距
                            //   我们建 SRV 时写死 R8G8B8A8_UNORM（BformBCompose:1917），若真实格式不同，
                            //   shader 采到的就不是答案 —— 这是"B 屏无效果"的第一号嫌疑，先量掉它
                            static bool loggedPoDesc = false;
                            if (!loggedPoDesc) {
                                loggedPoDesc = true;
                                Log("DIAG-PO: 答案纹理 fmt=%d %ux%u array=%u mip=%u samples=%u 行距=%u",
                                    (int)pd.Format, (UINT)pd.Width, (UINT)pd.Height, pd.DepthOrArraySize,
                                    pd.MipLevels, pd.SampleDesc.Count, W.fpPriv.Footprint.RowPitch);
                            }
                        }
                        if (!W.rbPriv || W.bytesPriv != tot2) {
                            D3D12_RESOURCE_DESC bd2{};
                            bd2.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                            bd2.Width = tot2; bd2.Height = 1; bd2.DepthOrArraySize = 1; bd2.MipLevels = 1;
                            bd2.Format = DXGI_FORMAT_UNKNOWN; bd2.SampleDesc.Count = 1;
                            bd2.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                            D3D12_HEAP_PROPERTIES rhb{}; rhb.Type = D3D12_HEAP_TYPE_READBACK;
                            rhb.CreationNodeMask = 1; rhb.VisibleNodeMask = 1;
                            W.rbPriv.Reset();
                            W.dev->CreateCommittedResource(&rhb, D3D12_HEAP_FLAG_NONE, &bd2,
                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(W.rbPriv.GetAddressOf()));
                            W.bytesPriv = tot2;
                        }
                        g_tlAllocResetHr = W.curAl->Reset(); g_tlResetHr = W.cl->Reset(W.curAl, nullptr);
                        // 【B 形态 S4b】答案刚出来 → 立刻在 B 上做残差合成（用本帧收到的 pColor/MV + 本地降采样）
                        bool bCompRecorded = false;
                        if (g_bformOn && g_bformStage >= 4 && (g_bCompReady || g_bform4k) && !g_bStarved &&
                            BformWaitPresentReads(W, 0, false)) {
                            if (g_bform4k) {
                                // 【bform=9】4K 后置：残差直接合成到 4K 底图上（g_bTexC 就是 XeSS 的 4K 输出）
                                //   BformBCompose 的输出尺寸取自 g_upW/g_upH = 4K，所以这里天然是 4K 合成；
                                //   合成后不走放大（底图已经是 4K），present 直接取 g_bComp。
                                bCompRecorded = BformBCompose(W.cl.Get(), po);
                            } else {
                            bCompRecorded = BformBCompose(W.cl.Get(), po);
                            if (g_bformStage >= 5 && g_bUpReady) BformBUpscale(W.cl.Get());
                            // 【TAA-lite】bform=8 时，在上采样之后再多走一个时域累积 pass（治噪点/锯齿）
                            if (g_taaOn && g_bUpReady) {
                                static bool taaFail = false;
                                if (!taaFail) {
                                    if (!BformTaaEnsure(W.dev.Get())) taaFail = true;
                                    else if (!BformTaaRun(W.cl.Get())) taaFail = true;
                                }
                            }
                            }
                        }
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = po;
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        W.cl->ResourceBarrier(1, &b);
                        static bool loggedR5 = false;
                        // 【流水线】把 NR 的答案快照进本组缓冲（po 下一帧就被运行时复用，必须拷走）
                        if (g_bformOn && g_bformStage >= 7 && g_ppReady && g_ppAns[g_ppCur]) {
                            D3D12_RESOURCE_BARRIER pb{};
                            pb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            pb.Transition.pResource = g_ppAns[g_ppCur].Get();
                            pb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                            pb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                            pb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                            W.cl->ResourceBarrier(1, &pb);
                            W.cl->CopyResource(g_ppAns[g_ppCur].Get(), po);
                            pb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                            pb.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                            W.cl->ResourceBarrier(1, &pb);
                        }
                        // ★ mode 9（4K 后置）下 A 不合成，答案不需要回传 → 跳过这次 8.3MB 的跨卡直写，
                        //   省 B 的队列时间 + 省 PCIe 带宽（和 A→B 的底图传输抢带宽）。
                        //   代价：热切回 A 形态后头几帧没现成答案（A 会等一次 ~20ms，之后正常）。
                        if (g_xOk && g_xBufOutW && !g_bform4k) {
                            D3D12_TEXTURE_COPY_LOCATION xd{};
                            xd.pResource = g_xBufOutW.Get();
                            xd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                            xd.PlacedFootprint.Offset = XHDR + (UINT64)idx * (UINT64)(kNetW * kNetH * 4);
                            xd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                            xd.PlacedFootprint.Footprint.Width = kNetW;
                            xd.PlacedFootprint.Footprint.Height = kNetH;
                            xd.PlacedFootprint.Footprint.Depth = 1;
                            xd.PlacedFootprint.Footprint.RowPitch = kNetW * 4;
                            D3D12_TEXTURE_COPY_LOCATION xs5{};
                            xs5.pResource = po;
                            xs5.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            xs5.SubresourceIndex = 0;
                            W.cl->CopyTextureRegion(&xd, 0, 0, 0, &xs5, nullptr);
                            if (!loggedR5) { loggedR5 = true; Log("R5: 答案已 GPU 直写进跨卡共享 buffer"); }
                        if ((W.processed % 30) == 0) Log("TRACE-WRK: processed=%d idx=%d frameId=%d", W.processed, idx, frameId);
                        }
                        D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = W.rbPriv.Get();
                        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = W.fpPriv;
                        D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = po;
                        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
                        W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                        W.cl->ResourceBarrier(1, &b);
                        // 【诊断】五源同块对照的"拷贝"必须挂在这个 list 上（跨队列碰同一批资源 = S1 崩过）
                        if (bCompRecorded) BformDiagEnqueue(W, po);
                        g_tlCloseHr = W.cl->Close();
                        if (FAILED(g_tlCloseHr)) {
                            W.api.Retire(W.ctx, job.handle);
                            NrHardFail("B1: legacy compose command list close failed");
                            break;
                        }
                        BtWorkerExec("compose");
                        { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }

                        // 【2026-10-28 14:40 回退：不等围栏】
                        //   查明：A 的合成用的是 B 侧 GPU 直写进跨卡 buffer 的那份答案（use_gpu_resid=1），
                        //   本来就没有错帧；CPU 回读这份只喂诊断和 CPU 残差路径（未启用）。
                        //   为它牺牲帧率不划算 → 回到"提交完立刻 Map"。
                        //   已知代价：DIAGA 诊断数字偏高（它比的是错帧），不要用它下画质结论。
                        if (FAILED(SignalW())) {
                            BtWaitEnd(W, W.fv, false);
                            W.api.Retire(W.ctx, job.handle);
                            NrHardFail("B1: legacy compose signal failed");
                            break;
                        }
                        W.aiFv[W.ai] = W.fv;   // ★ 记下本分配器的围栏值：下次轮到它复用前要先确认已完成
                        WaitWFence(W.aiFv[W.ai], kWorkerWaitMs);

                        // Present waits on this submitted compose value even on a slow GPU.
                        if (bCompRecorded && g_bformOn && g_bformStage >= 6 && g_bUpReady && UiGateAllowsNr()) {
                            if (!g_bpReady && !g_bpFailed) BformPresentInit();
                            if (g_bpReady) BformPresentFrame(0, W.fv);
                        }

                        // 回读本轮答案并交付给 A（不等围栏）
                        {
                            D3D12_RANGE rr{ 0, (SIZE_T)tot2 };
                            void* rm = nullptr;
                            if (SUCCEEDED(W.rbPriv->Map(0, &rr, &rm)) && rm) {
                                // ===== 一次性诊断（只在游戏目录存在 xess_proxy.podiag.txt 时生效）=====
                                //   目的：把运行时答案（private_output）的真实字节打出来，
                                //   与我们在 FFX 代理里的同口径观测对照，判断"答案到底是不是浮点残差"。
                                {
                                    static bool diagDone = false;
                                    static int diagGate = -1;
                                    if (diagGate < 0) {
                                        std::string gate = ModuleDir() + "xess_proxy.podiag.txt";
                                        diagGate = (GetFileAttributesA(gate.c_str()) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
                                    }
                                    static int diagShot = 0;
                                    const bool shoot = (diagGate == 1) &&
                                                       ((diagShot == 0 && W.processed >= 60) ||
                                                        (diagShot == 1 && W.processed >= 300) ||
                                                        (diagShot == 2 && W.processed >= 600));
                                    if (shoot) {
                                        diagShot++;
                                        const unsigned char* b0 = (const unsigned char*)rm;
                                        D3D12_RESOURCE_DESC pd2 = ((ID3D12Resource*)job.private_output)->GetDesc();
                                        Log("DIAG-POB: po fmt=%d %ux%u RowPitch=%u | 前16字节= %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                                            (int)pd2.Format, (unsigned)pd2.Width, (unsigned)pd2.Height, (unsigned)W.fpPriv.Footprint.RowPitch,
                                            b0[0],b0[1],b0[2],b0[3],b0[4],b0[5],b0[6],b0[7],
                                            b0[8],b0[9],b0[10],b0[11],b0[12],b0[13],b0[14],b0[15]);
                                        float f0 = 0; memcpy(&f0, b0, 4);
                                        Log("DIAG-POB2: 帧%d 按 R32_FLOAT 首像素 = %.6g ；outBuf[0..3]=%u,%u,%u,%u",
                                            (int)W.processed, f0, W.outBuf[0], W.outBuf[1], W.outBuf[2], W.outBuf[3]);
                                        {   // 前 4096 字节里有多少非零；前 1024 个 float 的 min/max/mean
                                            int nz = 0; for (int k = 0; k < 4096; ++k) if (b0[k]) ++nz;
                                            float mn = 1e30f, mx = -1e30f; double sum = 0; int n = 0;
                                            for (int k = 0; k < 1024; ++k) { float v; memcpy(&v, b0 + k * 4, 4);
                                                if (v < mn) mn = v; if (v > mx) mx = v; sum += v; ++n; }
                                            Log("DIAG-POB3: 帧%d 前4096字节非零=%d/4096 | float min=%.6g max=%.6g mean=%.6g",
                                                (int)W.processed, nz, mn, mx, n ? sum / n : 0.0);
                                        }
                                    }
                                }
                                UINT pitch = W.fpPriv.Footprint.RowPitch;
                                for (UINT y = 0; y < kNetH; ++y)
                                    memcpy(&W.outBuf[(size_t)y * kNetW * 4],
                                           (unsigned char*)rm + (size_t)y * pitch, (size_t)kNetW * 4);
                                D3D12_RANGE nw{ 0, 0 };
                                W.rbPriv->Unmap(0, &nw);
                                {
                                    std::lock_guard<std::mutex> lk(W.m);
                                    if (idx >= 0 && idx < K && (int)W.ansBuf.size() == K) {
                                        memcpy(W.ansBuf[idx].data(), W.outBuf.data(),
                                               (std::min)(W.ansBuf[idx].size(), W.outBuf.size()));
                                        W.ansFrame[idx] = frameId;
                                    }
                                }
                                W.cvOut.notify_all();
                                delivered = true;
                            }
                        }
                        W.hasOut = delivered;

                        // 【诊断】回读 + 打一行五源对照（每 150 帧一次）
                        BformDiagReadback(W);
                    }
                    W.api.Retire(W.ctx, job.handle);
                }
                // 【流水线】本轮结束：切到另一组缓冲，下一轮 Stage2 就处理这一帧的结果
                if (g_bformOn && g_bformStage >= 7 && g_ppReady) {
                    g_ppCur ^= 1;
                    g_ppHavePrev = true;
                }
                g_workerBeat = GetTickCount64();   // 看门狗心跳：每轮结束更新一次
            }
            // ★ 待清理：lmxxf 后端分支到此结束。
        }
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(W.m);
            W.outIndex = idx;
            W.processed++;
            W.lastMs = ms;
            if (W.processed <= 3 || W.processed % 150 == 0) {
                char sb[256] = { 0 };
                if (W.api.GetStatus) W.api.GetStatus(W.ctx, sb, 256);
                Log("worker: #%d(槽%d) %.1f ms %s | 状态: %s",
                    W.processed, idx, ms, delivered ? "ok" : "无输出", sb[0] ? sb : "(空)");
                strncpy_s(g_statusLine, sizeof(g_statusLine), sb[0] ? sb : "(空)", _TRUNCATE);
            }
            W.cvOut.notify_all();
        }
    }
}

// ---------------- 游戏侧 ----------------
struct Slot {
    ComPtr<ID3D12Resource> color;    // 本帧原始 pColor
    ComPtr<ID3D12Resource> mv;       // 本帧 MV
    ComPtr<ID3D12Resource> depth;    // 本帧深度
    float jx = 0, jy = 0;
    int   frame = -1;
    bool  captured = false;
    bool  hasAnswer = false;
    int   answerFrame = -1;          // 这份答案对应哪一帧（身份校验）
    std::vector<uint8_t> answer;     // 1080p 答案
    std::vector<uint8_t> feedInput;  // 1080p 喂进去的输入（算残差基准）
    int feedFrame = -1;             // CPU 读回有效时才登记；不能配给未刷新的槽
    UINT64 a2OfferSeq = 0;           // paired mirror token, main thread only
};
static Slot g_slots[K];

static ComPtr<ID3D12Device> g_dev;
static ComPtr<ID3D12RootSignature> g_rs;
static ComPtr<ID3D12PipelineState> g_psoDown, g_psoResid, g_psoAdd4K, g_psoMvPack, g_psoMark, g_psoTri;
static ComPtr<ID3D12DescriptorHeap> g_heap;
static UINT g_inc = 0;
static D3D12_CPU_DESCRIPTOR_HANDLE g_cpuResSmooth{};
static ComPtr<ID3D12Resource> g_netS;      // 1080p 降采样目标
static ComPtr<ID3D12Resource> g_pre;       // pColor 几何的合成目标
static ComPtr<ID3D12Resource> g_pre4k;     // B1：4K 叠加结果（outp 几何）
static bool g_resid4k = false;             // B1 开关：残差改在 4K 上叠加（默认关）
static ComPtr<ID3D12Resource> g_resTex;    // 1080p 残差（上传）
static ComPtr<ID3D12Resource> g_resSmooth; // 残差时域状态
static ComPtr<ID3D12Resource> g_resUp, g_rb;   // 上传/回读缓冲
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fpRes{}, g_fpRb{};
static UINT64 g_bytesRes = 0, g_bytesRb = 0;
static UINT g_cw = 0, g_ch = 0, g_outW = 0, g_outH = 0;
static DXGI_FORMAT g_cfmt = DXGI_FORMAT_UNKNOWN;
static int g_frame = 0;
static int g_fedIdx = 0;         // 已喂出的作业序号
// r28 块2 延迟实现：g_frame 现在已定义；g_bbFe 在下方 ~5700 行定义，通过指针参数传入
static int    R28GetFrame()  { return g_frame; }
// R28GetBbDone 实现放在 g_bbFe 定义之后（搜索"R28GetBbDone 实现"）
static double g_sumWait = 0; static int g_nWait = 0;

static volatile float g_limit = 0.2f, g_edge = 0.6f, g_smooth = 0.0f, g_gain = 0.66f, g_sat = 0.9f;
static volatile float g_temporalStatic = 0.02f;  // 静止时的时域混合（1.0=关，0.20=最强；实测 0.20 完全消除闪烁）
static volatile float g_motionAlpha = 0.20f;  // 运动时的 alpha 上限（不再到 1.0；由 conf/gate 决定实际历史量）
static volatile float g_motionLo = 0.02f;   // 运动量下界（低于此=完全平滑）
static volatile float g_motionHi = 0.04f;   // 运动量上界（实测 0.04：静止不闪 + 运动无残影）
static volatile float g_alpha = 1.0f;             // 本帧实际用的 alpha（CPU 按运动量算）
static volatile float g_varGain = 0.0f;           // 方差门限强度（F6）
static int g_stateWarmup = 4;
// 仅 FFX 适配层写入：保留最近一次有效契约，首个有效值之前兜底 -1；B 侧 worker 也会读。
static std::atomic<float> g_mvScale{-1.0f};
static float MvScaleUv() { return g_mvScale.load(std::memory_order_relaxed); }  // 残差重投影的 MV -> UV 比例
static volatile float g_mvConf  = 1.0f;          // MV 置信度衰减（实测最佳）
static volatile int   g_mvConfIdx = 1;   // 当前档位（屏幕显示用）
static volatile float g_gateK = 8.0f;    // 实测 8 + motionHi 0.04：静止不闪且运动无拖影（40 会小幅闪、0 会拖影）
static volatile int   g_gateIdx = 4;
static ComPtr<ID3D12Resource> g_dumpRb1, g_dumpRb2;
static UINT64 g_dumpBytes = 0;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_dumpFp{};
static int g_dumpPhase = 0;                       // 0=未开始 1=已记录拷贝 2=已完成

// ---------------- S1 镜像叠层开关（ini 键 mirror / 菜单勾选 / F11） ----------------
// 纯观测功能：把游戏已出好的成品帧再叠一层到游戏窗口上，用来测「自己拥有 present」的代价。
// 默认关；关着的时候一行代码都不执行。
static bool g_mirOn = false;
static int  g_mirState = 0;     // 0=未建 3=已关(可再开) 1=就绪 2=失败(不再重试)

// ---------------- 参数快照 / 用户存档（菜单：保存 / 撤销 / 恢复 / 出厂） ----------------
// 三层语义，互不污染：
//   出厂默认 = 代码里的常量（FactoryParams）—— 任何按钮都不会改它
//   用户存档 = xess_proxy.saved.ini —— “保存设置”写入，独立文件，不覆盖出厂
//   启动状态 = 本次游戏启动、首次跑 NR 时的快照 —— “撤销改动”回到这里
//   FFX 的实时 MV 比例不属于参数快照或存档。
struct Params {
    float gain, limit, edge, sat, temporal, motionAlpha, motionLo, motionHi;
    float mvConf, gateK, transfer, colorStr, jitter, dejitterSign, uiScale, varGain;
    int   debugView;
    bool  dejitter, bypass, useGpuRes;
    // S4 风格化（发给 mochizuki 控制块）
    float nrIntensity, nrTone, nrStruct, nrSkin, nrGrade;
    int   nrStyle;
    bool  nrAutoMask, nrCtrlOn;
    bool  mirrorOn;          // S1 镜像叠层
    // [Preprocess]（跨卡喂图预处理 + dlssnr-amd.ini 同步）
    bool  ppOn;
    int   ppExpMode, ppCurve;
    float ppBias, ppContrast, ppSat;
    int   nrScaleMode;
};
static Params g_sessionStart{};
static bool   g_sessionStartValid = false;

static Params CaptureParams() {
    EnsurePreprocessLoadedFromIni();
    Params p{};
    p.gain = g_gain; p.limit = g_limit; p.edge = g_edge; p.sat = g_sat;
    p.temporal = g_temporalStatic; p.motionAlpha = g_motionAlpha;
    p.motionLo = g_motionLo; p.motionHi = g_motionHi;
    p.mvConf = g_mvConf; p.gateK = g_gateK;
    p.transfer = g_transfer; p.colorStr = g_colorStr; p.jitter = g_jitterScale;
    p.dejitterSign = g_dejitterSign; p.uiScale = g_uiScale; p.varGain = g_varGain;
    p.debugView = g_debugView; p.dejitter = g_dejitter; p.bypass = g_bypass; p.useGpuRes = g_useGpuResid;
    p.nrIntensity = g_nrIntensity; p.nrTone = g_nrTone; p.nrStruct = g_nrStruct; p.nrSkin = g_nrSkin;
    p.nrGrade = g_nrGrade; p.nrStyle = g_nrStyle; p.nrAutoMask = g_nrAutoMask; p.nrCtrlOn = g_nrCtrlOn;
    p.mirrorOn = g_mirOn;
    p.ppOn = g_ppOn; p.ppExpMode = g_ppExpMode; p.ppBias = g_ppBias;
    p.ppCurve = g_ppCurve; p.ppContrast = g_ppContrast; p.ppSat = g_ppSat;
    p.nrScaleMode = g_nrScaleMode;
    return p;
}
static void ApplyParams(const Params& p) {
    g_gain = p.gain; g_limit = p.limit; g_edge = p.edge; g_sat = p.sat;
    g_temporalStatic = p.temporal; g_stateWarmup = 4;
    g_motionAlpha = p.motionAlpha; g_motionLo = p.motionLo; g_motionHi = p.motionHi;
    g_mvConf = p.mvConf; g_gateK = p.gateK;
    g_transfer = p.transfer; g_colorStr = p.colorStr; g_jitterScale = p.jitter;
    g_dejitterSign = p.dejitterSign; g_uiScale = p.uiScale; g_varGain = p.varGain;
    g_debugView = p.debugView; g_dejitter = p.dejitter; g_bypass = p.bypass; g_useGpuResid = p.useGpuRes;
    g_nrIntensity = p.nrIntensity; g_nrTone = p.nrTone; g_nrStruct = p.nrStruct; g_nrSkin = p.nrSkin;
    g_nrGrade = p.nrGrade; g_nrStyle = p.nrStyle; g_nrAutoMask = p.nrAutoMask; g_nrCtrlOn = p.nrCtrlOn;
    if (g_mirOn != p.mirrorOn) Log("MIR: 镜像叠层 %s（来自参数应用）", p.mirrorOn ? "开" : "关");
    g_mirOn = p.mirrorOn;
    const bool ppChanged = !g_ppLoaded || (g_ppOn != p.ppOn) || (g_ppExpMode != p.ppExpMode) ||
                           (g_ppBias != p.ppBias) || (g_ppCurve != p.ppCurve) ||
                           (g_ppContrast != p.ppContrast) || (g_ppSat != p.ppSat);
    g_ppOn = p.ppOn; g_ppExpMode = p.ppExpMode; g_ppBias = p.ppBias;
    g_ppCurve = p.ppCurve; g_ppContrast = p.ppContrast; g_ppSat = p.ppSat;
    g_ppLoaded = true;
    if (ppChanged) SavePreprocessToIni();
    g_nrScaleMode = (p.nrScaleMode >= 0 && p.nrScaleMode <= 7) ? p.nrScaleMode : 0;
    g_ctrlDirty = true;
}
static Params FactoryParams() {
    Params p{};
    p.gain = 0.66f; p.limit = 0.20f; p.edge = 0.6f; p.sat = 0.9f;
    p.temporal = 0.02f; p.motionAlpha = 0.20f; p.motionLo = 0.02f; p.motionHi = 0.04f;
    p.mvConf = 1.0f; p.gateK = 8.0f;
    p.transfer = 1.0f; p.colorStr = 1.0f; p.jitter = 0.5f;
    p.dejitterSign = 1.0f; p.uiScale = 1.0f; p.varGain = 0.0f;
    p.debugView = 0; p.dejitter = false; p.bypass = false; p.useGpuRes = true;
    p.nrIntensity = 1.0f; p.nrTone = 1.0f; p.nrStruct = 1.0f; p.nrSkin = -1.0f; p.nrGrade = 2.0f;
    p.nrStyle = 0; p.nrAutoMask = false; p.nrCtrlOn = true;
    p.mirrorOn = false;
    p.ppOn = true; p.ppExpMode = 1; p.ppBias = 0.0f;
    p.ppCurve = 3; p.ppContrast = 1.0f; p.ppSat = 1.0f;
    p.nrScaleMode = 0;
    return p;
}
static bool SaveParamsToFile(const Params& p) {
    std::string path = ModuleDir() + "xess_proxy.saved.ini";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "w") != 0 || !f) { Log("菜单: 保存失败（打不开 %s）", path.c_str()); return false; }
    fprintf(f, "; 跨卡 NR 用户存档（菜单“保存设置”写入；出厂默认在代码里，不会被这里覆盖）\n");
    fprintf(f, "gain=%.6f\nlimit=%.6f\nedge=%.6f\nsat=%.6f\ntemporal=%.6f\n", p.gain, p.limit, p.edge, p.sat, p.temporal);
    fprintf(f, "motion_alpha=%.6f\nmotion_lo=%.6f\nmotion_hi=%.6f\n", p.motionAlpha, p.motionLo, p.motionHi);
    fprintf(f, "mv_conf=%.6f\ngate_k=%.6f\n", p.mvConf, p.gateK);
    fprintf(f, "transfer=%.6f\ncolour=%.6f\njitter=%.6f\nde_jitter_sign=%.1f\n", p.transfer, p.colorStr, p.jitter, p.dejitterSign);
    fprintf(f, "ui_scale=%.6f\nvar_gain=%.6f\n", p.uiScale, p.varGain);
    fprintf(f, "nr_intensity=%.6f\nnr_style=%d\nnr_tone=%.6f\nnr_struct=%.6f\nnr_skin=%.6f\nnr_grade=%.6f\nnr_automask=%d\nnr_ctrl_on=%d\n",
            p.nrIntensity, p.nrStyle, p.nrTone, p.nrStruct, p.nrSkin, p.nrGrade, (int)p.nrAutoMask, (int)p.nrCtrlOn);
    fprintf(f, "debug_view=%d\nde_jitter=%d\nbypass=%d\nuse_gpu_resid=%d\n",
            p.debugView, (int)p.dejitter, (int)p.bypass, (int)p.useGpuRes);
    fprintf(f, "mirror=%d\n", (int)p.mirrorOn);
    fprintf(f, "pp_enabled=%d\npp_exposure=%d\npp_bias=%.6f\npp_curve=%d\npp_contrast=%.6f\npp_saturation=%.6f\n",
            (int)p.ppOn, p.ppExpMode, p.ppBias, p.ppCurve, p.ppContrast, p.ppSat);
    fprintf(f, "nr_scale_mode=%d\n", p.nrScaleMode);
    fclose(f);
    Log("菜单: 已保存设置 → %s", path.c_str());
    return true;
}
static bool LoadParamsFromFile(Params& p) {   // 只覆盖文件里出现的键
    std::string path = ModuleDir() + "xess_proxy.saved.ini";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") != 0 || !f) { Log("菜单: 没有存档（%s）", path.c_str()); return false; }
    char line[256]; int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char* k = line; while (*k == ' ' || *k == '\t') ++k;
        if (!strcmp(k, "mv_scale")) continue;   // 兼容旧存档：比例只由实时 FFX 契约决定
        const char* v = eq + 1;
        float fv = (float)atof(v); int iv = atoi(v);
        if (!strcmp(k, "gain")) p.gain = fv;
        else if (!strcmp(k, "limit")) p.limit = fv;
        else if (!strcmp(k, "edge")) p.edge = fv;
        else if (!strcmp(k, "sat")) p.sat = fv;
        else if (!strcmp(k, "temporal")) p.temporal = fv;
        else if (!strcmp(k, "motion_alpha")) p.motionAlpha = fv;
        else if (!strcmp(k, "motion_lo")) p.motionLo = fv;
        else if (!strcmp(k, "motion_hi")) p.motionHi = fv;
        else if (!strcmp(k, "mv_conf")) p.mvConf = fv;
        else if (!strcmp(k, "gate_k")) p.gateK = fv;
        else if (!strcmp(k, "transfer")) p.transfer = fv;
        else if (!strcmp(k, "colour")) p.colorStr = fv;
        else if (!strcmp(k, "jitter")) p.jitter = fv;
        else if (!strcmp(k, "de_jitter_sign")) p.dejitterSign = fv;
        else if (!strcmp(k, "ui_scale")) p.uiScale = fv;
        else if (!strcmp(k, "var_gain")) p.varGain = fv;
        else if (!strcmp(k, "nr_intensity")) p.nrIntensity = fv;
        else if (!strcmp(k, "nr_style")) p.nrStyle = iv;
        else if (!strcmp(k, "nr_tone")) p.nrTone = fv;
        else if (!strcmp(k, "nr_struct")) p.nrStruct = fv;
        else if (!strcmp(k, "nr_skin")) p.nrSkin = fv;
        else if (!strcmp(k, "nr_grade")) p.nrGrade = fv;
        else if (!strcmp(k, "nr_automask")) p.nrAutoMask = (iv != 0);
        else if (!strcmp(k, "nr_ctrl_on")) p.nrCtrlOn = (iv != 0);
        else if (!strcmp(k, "debug_view")) p.debugView = iv;
        else if (!strcmp(k, "de_jitter")) p.dejitter = (iv != 0);
        else if (!strcmp(k, "bypass")) p.bypass = (iv != 0);
        else if (!strcmp(k, "use_gpu_resid")) p.useGpuRes = (iv != 0);
        else if (!strcmp(k, "mirror")) p.mirrorOn = (iv != 0);
        else if (!strcmp(k, "pp_enabled")) p.ppOn = (iv != 0);
        else if (!strcmp(k, "pp_exposure")) p.ppExpMode = (iv >= 0 && iv < 3) ? iv : 1;
        else if (!strcmp(k, "pp_bias")) p.ppBias = fv;
        else if (!strcmp(k, "pp_curve")) p.ppCurve = (iv >= 0 && iv < 7) ? iv : 3;
        else if (!strcmp(k, "pp_contrast")) p.ppContrast = fv;
        else if (!strcmp(k, "pp_saturation")) p.ppSat = fv;
        else if (!strcmp(k, "nr_scale_mode")) p.nrScaleMode = (iv >= 0 && iv <= 7) ? iv : 0;
        else continue;
        ++n;
    }
    fclose(f);
    Log("菜单: 已读取存档 %d 项（%s）", n, path.c_str());
    return n > 0;
}

static const char* kPreprocessHLSL = R"(
float3 PpNeutral(float3 c) {
    float peak = max(c.r, max(c.g, c.b));
    if (peak < 0.76) return c;
    float np = 1.0 - 0.0576 / (peak - 0.52);
    c *= np / max(peak, 1e-6);
    float g = 1.0 - 1.0 / (0.15 * (peak - np) + 1.0);
    return lerp(c, float3(np, np, np), g);
}
float PpHable1(float x) {
    return ((x * (0.15 * x + 0.05) + 0.004) / (x * (0.15 * x + 0.50) + 0.06)) - 0.02 / 0.30;
}
float PpGt1(float x) {
    const float m = 0.22, S0 = 0.374, S1 = 0.374, CP = -1.0 / (1.0 - S1);
    float w0 = 1.0 - smoothstep(0.0, m, x);
    float w2 = step(S0, x);
    float w1 = 1.0 - w0 - w2;
    float T = m * pow(max(x, 0.0) / m, 1.33);
    float S = 1.0 - (1.0 - S1) * exp(CP * (x - S0));
    return T * w0 + x * w1 + S * w2;
}
float PpAgxSig(float x) {
    float x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float3 PpAgx(float3 c) {
    float3 v = float3(
        dot(c, float3(0.84247906, 0.07843360, 0.07922375)),
        dot(c, float3(0.04232824, 0.87846864, 0.07916613)),
        dot(c, float3(0.04237565, 0.07843360, 0.87914297))
    );
    v = clamp(log2(max(v, 1e-10)), -12.47393, 4.026069);
    v = (v + 12.47393) / 16.5;
    v = float3(PpAgxSig(v.r), PpAgxSig(v.g), PpAgxSig(v.b));
    float3 o = float3(
        dot(v, float3( 1.19687901, -0.09802088, -0.09902974)),
        dot(v, float3(-0.05289685,  1.15190313, -0.09896118)),
        dot(v, float3(-0.05297164, -0.09804345,  1.15107367))
    );
    return pow(max(o, 0.0), 2.2);
}
float3 PpApply(float3 col, float en, float gain, float curveId, float contrast, float sat) {
    if (en <= 0.5) return col;
    float3 x = max(col, 0.0) * gain;
    if (abs(contrast - 1.0) > 1e-4) x = 0.18 * pow(max(x / 0.18, 0.0), contrast);
    if (abs(sat - 1.0) > 1e-4) {
        float l = dot(x, float3(0.2126, 0.7152, 0.0722));
        x = max(l + sat * (x - l), 0.0);
    }
    int cid = (int)(curveId + 0.5);
    if (cid == 1) return saturate(PpNeutral(x));
    if (cid == 2) { float3 k = x * 1.2195122; return saturate(k / (1.0 + k)); }
    if (cid == 3) { float3 k = x * 2.9275228; float w = PpHable1(11.2); return saturate(float3(PpHable1(k.r), PpHable1(k.g), PpHable1(k.b)) / w); }
    if (cid == 4) { float3 k = x * 1.0052344; return saturate(float3(PpGt1(k.r), PpGt1(k.g), PpGt1(k.b))); }
    if (cid == 5) { float3 k = x * 0.7231708; return saturate((k * (2.51 * k + 0.03)) / (k * (2.43 * k + 0.59) + 0.14)); }
    if (cid == 6) { return saturate(PpAgx(x * 0.8083602)); }
    return saturate(x);
}
)";

static const char* kHLSL = R"(
cbuffer P : register(b0) { float gLimit; float gEdge; float gSmooth; float gGain;
                            float gSat; float gAlpha; float gVarGain; float gTemporal;
                            float gMvScale; float gMvConf; float gMvIdx; float gConfIdx;
                            float gGateK; float gGateIdx; float gBypass; float gUseGpuRes;
    float gJitX; float gJitY; float gDejitter; float gPadJ; };

Texture2D<float4>   gSrc  : register(t0);   // 槽位里的原始小图
Texture2D<float4>   gRes  : register(t1);   // 1080p 残差（0.5 中性）
Texture2D<float4>   gMV   : register(t2);
Texture2D<float4>   gFed  : register(t3);   // 该槽位被喂进去的 1080p 输入
Buffer<float4>      gAns  : register(t4);   // 答案（本地中转 buffer, RGBA8 有类型视图）
SamplerState        gSmp  : register(s0);
RWTexture2D<float4> gDst  : register(u0);
RWTexture2D<float4> gSt   : register(u1);   // 残差时域状态

// 跑马灯标记：把相位黑条画进【我们自己的】喂入图（不动游戏资源）
//   x0 = gPadJ*W（相位 0/.25/.5/.75），宽 = gDejitter*W
[numthreads(8,8,1)]
void MarkCS(uint3 id : SV_DispatchThreadID)
{
    uint w,h; gDst.GetDimensions(w,h);
    if (id.x>=w||id.y>=h) return;
    float x0 = gPadJ * float(w);
    float bw = gDejitter * float(w);
    float x1 = x0 + bw;
    float px = float(id.x), py = float(id.y);
    if (px < x0 || px >= x1) { gDst[id.xy] = float4(1.0,1.0,1.0,1.0); return; }   // 白底（原图不留）

    // 黑条内默认黑；在【上/中/下三个三分之一】处各画同一个数字（1~8 循环，gLimit 传数字）
    float3 col = float3(0.0,0.0,0.0);
    int digit = (int)clamp(gLimit, 1.0, 8.0);
    int m = 127;                                     // 七段码：a=1 b=2 c=4 d=8 e=16 f=32 g=64
    if (digit == 1) m = 6;
    else if (digit == 2) m = 91;
    else if (digit == 3) m = 79;
    else if (digit == 4) m = 102;
    else if (digit == 5) m = 109;
    else if (digit == 6) m = 125;
    else if (digit == 7) m = 7;

    float thirdH = float(h) / 3.0;
    float gw = bw * 0.55;
    float gx0 = x0 + (bw - gw) * 0.5;
    for (int t = 0; t < 3; ++t) {
        float gy0 = float(t) * thirdH + thirdH * 0.22;
        float gh  = thirdH * 0.56;
        if (px >= gx0 && px < gx0 + gw && py >= gy0 && py < gy0 + gh) {
            float lx = (px - gx0) / gw, ly = (py - gy0) / gh;      // 0..1
            float T = 0.16;
            bool sA = (ly < T)        && (lx > T*0.8) && (lx < 1.0 - T*0.8);
            bool sD = (ly > 1.0 - T)  && (lx > T*0.8) && (lx < 1.0 - T*0.8);
            bool sG = (abs(ly-0.5) < T*0.6) && (lx > T*0.8) && (lx < 1.0 - T*0.8);
            bool sF = (lx < T)        && (ly > T*0.8) && (ly < 0.5 - T*0.3);
            bool sB = (lx > 1.0 - T)  && (ly > T*0.8) && (ly < 0.5 - T*0.3);
            bool sE = (lx < T)        && (ly > 0.5 + T*0.3) && (ly < 1.0 - T*0.8);
            bool sC = (lx > 1.0 - T)  && (ly > 0.5 + T*0.3) && (ly < 1.0 - T*0.8);
            bool on = (sA && (m & 1))  || (sB && (m & 2))  || (sC && (m & 4))  || (sD && (m & 8))
                   || (sE && (m & 16)) || (sF && (m & 32)) || (sG && (m & 64));
            if (on) col = float3(6.0,6.0,6.0);       // 亮白（HDR 下才够醒目）
        }
    }
    gDst[id.xy] = float4(col, 1.0);
}

// 三横带对照视图（诊断）：上=答案 / 中=残差 / 下=【我们涂的底图】+残差×强度
//   底图故意用"白底 + 相位黑条"，不保留游戏原图 —— 底图与残差同帧时黑条干净，
//   错帧时黑条位置会出现亮条（残差里带着另一个相位的条）。
//   x0 = gPadJ*W（相位 0/.25/.5/.75），条宽 = gDejitter*W，强度 = gGain
[numthreads(8,8,1)]
void TriCS(uint3 id : SV_DispatchThreadID)
{
    uint w,h; gDst.GetDimensions(w,h);
    if (id.x>=w||id.y>=h) return;
    float2 uv = (float2(id.xy)+0.5)/float2(w,h);
    float  fy = float(id.y)/float(h);
    uint nw, nh; gFed.GetDimensions(nw, nh);
    int2   n  = int2(clamp(uv * float2(nw, nh), float2(0.5, 0.5), float2(float(nw) - 0.5, float(nh) - 0.5)));
    float3 a  = gAns[n.y * nw + n.x].rgb;        // 答案（网络尺寸 buffer）
    float3 f  = gFed.Load(int3(n,0)).rgb;      // 该槽位的喂入图
    float3 e  = a - f;                         // 残差
    float el = dot(e, float3(0.2126, 0.7152, 0.0722));   // 残差亮度（纯灰阶，不加颜色）
    if (fy < 0.33333) {
        // 上：答案（本身是我们的图案 = 黑白；×2 让白更白）
        gDst[id.xy] = float4(a * 2.0, 1.0);
    } else if (fy < 0.66666) {
        // 中：残差灰阶 —— 中灰(0.18)=零，更黑=负，更白=正
        float g = saturate(0.18 + el * 8.0);
        gDst[id.xy] = float4(g, g, g, 1.0);
    } else {
        // 下：底图原样（我们涂的图案 = 纯白底 + 纯黑条 + 条内纯白数字），
        //     不叠残差、不留任何游戏原图 —— 直接显示槽位底图本身
        gDst[id.xy] = float4(gSrc.Load(int3(id.xy, 0)).rgb * 2.0, 1.0);
    }
    // 三条带之间的分界线（便于读数，2 像素黑线）
    if (id.y == uint(h / 3) || id.y == uint(2 * h / 3)) gDst[id.xy] = float4(0.0,0.0,0.0,1.0);
}

float3 SrgbEnc(float3 v) {
    v = saturate(v);
    float3 a = 12.92 * v;
    float3 b = 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055;
    return lerp(a, b, step(0.0031308, v));
}
float3 SrgbDec(float3 v) {
    v = saturate(v);
    float3 a = v / 12.92;
    float3 b = pow((v + 0.055) / 1.055, 2.4);
    return lerp(a, b, step(0.04045, v));
}
float3 HybridEnc(float3 v) {
    v = clamp(v, 0.0, 1000.0);
    float m = max(v.r, max(v.g, v.b));
    if (m <= 0.75) return v;
    float e = (m - 0.75) * 4.0;
    float h = 0.75 + 0.25 * (e * rsqrt(e * e + 1.0));
    return v * (h / max(m, 1e-6));
}

// 小图 → 1080p（喂给网络）
[numthreads(8,8,1)]
void DownCS(uint3 id : SV_DispatchThreadID)
{
    uint w,h; gDst.GetDimensions(w,h);
    if (id.x>=w||id.y>=h) return;
    float2 uv=(float2(id.xy)+0.5)/float2(w,h);
    // 去抖动：用该帧的 jitter 反向偏移采样，使 NR 看到"投影稳定"的图像
    if (abs(gDejitter) > 0.001) {
        uint sw, sh; gSrc.GetDimensions(sw, sh);
        uv += float2(gJitX, gJitY) / float2(max(sw,1), max(sh,1)) * gDejitter;
    }
    // ★对齐修正（2077 适配）：原来写死"源比目标大约 1.16 倍"的 2x2 平均。
    //   比例随游戏/分辨率变化（2077 首测是 1280x720 → 1920x1080 = 放大 1.5 倍），
    //   放大时做"2x2 平均"等于只采一个点还引入偏移 → 静态闪烁。改为按真实比例分流：
    //     源 ≥ 目标（缩小）→ 4 抽头盒式平均（偏移按真实比例）
    //     源 <  目标（放大）→ 单次双线性（唯一正确的做法）
    uint sw, sh; gSrc.GetDimensions(sw, sh);
    float2 srcTexel = 1.0 / float2(max(sw, 1), max(sh, 1));
    float ratio = float(sw) / float(max(w, 1));      // <1 放大；>1 缩小
    float4 c;
    if (ratio > 1.05) {
        float2 t = srcTexel * 0.5 * ratio;           // 半个目标纹素折算到源空间
        c  = gSrc.SampleLevel(gSmp, uv + float2(-t.x, -t.y), 0);
        c += gSrc.SampleLevel(gSmp, uv + float2( t.x, -t.y), 0);
        c += gSrc.SampleLevel(gSmp, uv + float2(-t.x,  t.y), 0);
        c += gSrc.SampleLevel(gSmp, uv + float2( t.x,  t.y), 0);
        c *= 0.25;
    } else {
        c = gSrc.SampleLevel(gSmp, uv, 0);           // 放大或 1:1：双线性一次到位
    }
    if (gPadJ > 0.5) {
        // A3 FP16 路径：可逆混合曲线（<=0.75 恒等，>0.75 无损压进 [0.75,1)）+ sRGB 编码
        // 严格保证送进 Swin 网络的数据落在 [0,1] 显示域内，消除窗口浮点溢出闪烁与色偏
        gDst[id.xy] = float4(SrgbEnc(HybridEnc(c.rgb)), 1.0);
    } else {
        gDst[id.xy] = float4(PpApply(c.rgb, gEdge, gVarGain, gMvConf, gMvIdx, gGateK), 1.0);
    }
}

// 槽位原图 + 残差 → 合成后的 pColor
[numthreads(8,8,1)]
void ResidCS(uint3 id : SV_DispatchThreadID)
{
    uint w,h; gDst.GetDimensions(w,h);
    if (id.x>=w||id.y>=h) return;

    float4 c = gSrc.Load(int3(id.xy,0));
    float2 uv=(float2(id.xy)+0.5)/float2(w,h);

    // 直出模式：把运行时给的原样当作画面（配合 debug_view 看残差热图）
    if (gBypass > 0.5) { gDst[id.xy] = float4(gRes.SampleLevel(gSmp, uv, 0).rgb, 1.0); return; }

    // 输出平滑：残差在网络输入尺寸上做 9 抽头
    uint nw, nh; gFed.GetDimensions(nw, nh);
    float2 texel = float2(1.0 / float(nw), 1.0 / float(nh)) * gSmooth;
    float3 e;
    if (gUseGpuRes > 0.5) {
        // S5-5：答案与输入都在 GPU 上，直接相减（本地 buffer 载荷从 0 开始）
        // 手工双线性：buffer 不能过滤采样，用 4 邻域插值，消除亚像素移动时的纹素跳变
        float2 fc = uv * float2(nw, nh) - 0.5;
        float2 fl = floor(fc);
        int2 i0 = int2(fl);
        float2 wt = fc - fl;
        int2 c0 = clamp(i0,          int2(0, 0),       int2((int)nw - 2, (int)nh - 2));
        int2 c11 = clamp(i0 + int2(1, 1), int2(1, 1),  int2((int)nw - 1, (int)nh - 1));
        int2 c10 = int2(c11.x, c0.y);
        int2 c01 = int2(c0.x, c11.y);
        float w00 = (1.0 - wt.x) * (1.0 - wt.y);
        float w10 = wt.x * (1.0 - wt.y);
        float w01 = (1.0 - wt.x) * wt.y;
        float w11 = wt.x * wt.y;
        float4 a = gAns[c0.y * nw + c0.x] * w00 + gAns[c0.y * nw + c11.x] * w10
                 + gAns[c11.y * nw + c0.x] * w01 + gAns[c11.y * nw + c11.x] * w11;
        float4 f = gFed.Load(int3(c0, 0))  * w00 + gFed.Load(int3(c10, 0)) * w10
                 + gFed.Load(int3(c01, 0)) * w01 + gFed.Load(int3(c11, 0)) * w11;
        if (gPadJ > 0.5) {
            // 不做渐近线 HybridDec 反解（避免 m>2 时导数爆炸把高亮天空与亮边微抖动放大成刺眼白噪点）：
            //   · <=0.75 主体区：HybridEnc 为 1:1 恒等，dHyb 即精确线性光残差
            //   · >0.75 高亮区：按压缩域相对比例 dProp = c * (dHyb / fHyb) 回填，并在过曝区平滑收敛回原图
            float3 aHyb = SrgbDec(a.rgb);
            float3 fHyb = SrgbDec(f.rgb);
            float3 dHyb = aHyb - fHyb;
            float3 cHyb = HybridEnc(c.rgb);
            float mF = max(fHyb.r, max(fHyb.g, fHyb.b));
            float mC = max(cHyb.r, max(cHyb.g, cHyb.b));
            float mRef = max(mF, mC);
            float3 dProp = max(c.rgb, 0.0) * clamp(dHyb / max(fHyb, 0.125), -0.35, 0.35);
            float wProp = smoothstep(0.65, 0.85, mRef);
            float fadeHi = 1.0 - smoothstep(0.88, 0.99, mRef);
            e = lerp(dHyb, dProp, wProp) * fadeHi;
        } else {
            e = (a.rgb - f.rgb);
        }
    } else if (gSmooth < 0.05) {
        e = gRes.SampleLevel(gSmp, uv, 0).rgb - 0.5;
    } else {
        e  = (gRes.SampleLevel(gSmp, uv + float2( 0.0, 1.0)*texel, 0).rgb - 0.5);
        e += (gRes.SampleLevel(gSmp, uv + float2( 0.0,-1.0)*texel, 0).rgb - 0.5);
        e += (gRes.SampleLevel(gSmp, uv + float2( 1.0, 0.0)*texel, 0).rgb - 0.5);
        e += (gRes.SampleLevel(gSmp, uv + float2(-1.0, 0.0)*texel, 0).rgb - 0.5);
        e += (gRes.SampleLevel(gSmp, uv, 0).rgb - 0.5);
        e /= 5.0;
    }
    // 代理后处理关闭：保留当前 NR 残差，不读取或混合历史。

    float3 outc = (gPadJ > 0.5) ? max(c.rgb + e, 0.0) : saturate(c.rgb + e);

gDst[id.xy] = float4(outc, c.a);
}

// ★ Add4K 已移出本文件 —— 现在从 xess_proxy.add4k.hlsl 读并在运行时编译（可热重编译）
)";

static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &br);
    // ★ UAV 屏障：UAV 写之后被 SRV/同一资源读取时必须有它，否则 GPU 内部会读到旧内容
}

static void UavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    br.UAV.pResource = r;
    cl->ResourceBarrier(1, &br);
}

static void SetPass(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso,
                    UINT srv0, UINT srv1, UINT srv2, UINT srv3, UINT srv4,
                    UINT uav, UINT uav1, UINT w, UINT h,
                    ID3D12DescriptorHeap* a3Heap = nullptr, ID3D12RootSignature* a3Rs = nullptr,
                    bool a3Bypass = false) {
    cl->SetPipelineState(pso);
    cl->SetComputeRootSignature(a3Rs ? a3Rs : g_rs.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE b = (a3Heap ? a3Heap : g_heap.Get())->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE a1 = b; a1.ptr += (SIZE_T)srv0 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE a2 = b; a2.ptr += (SIZE_T)srv1 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE a3 = b; a3.ptr += (SIZE_T)srv2 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE a4 = b; a4.ptr += (SIZE_T)srv3 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE a5 = b; a5.ptr += (SIZE_T)srv4 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE u  = b; u.ptr  += (SIZE_T)uav  * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE u2 = b; u2.ptr += (SIZE_T)uav1 * g_inc;
    cl->SetComputeRootDescriptorTable(0, a1);
    cl->SetComputeRootDescriptorTable(1, a2);
    cl->SetComputeRootDescriptorTable(2, a3);
    cl->SetComputeRootDescriptorTable(3, a4);
    cl->SetComputeRootDescriptorTable(4, a5);
    cl->SetComputeRootDescriptorTable(5, u);
    cl->SetComputeRootDescriptorTable(6, u2);
    EnsurePreprocessLoadedFromIni();
    float dj = g_dejitter ? (float)g_dejitterSign : 0.0f;
    float ppGain = (g_ppExpMode == 0) ? 1.0f : exp2f((float)g_ppBias);
    float c[20] = { 1.0f, (!a3Heap && g_ppOn) ? 1.0f : 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, ppGain, 1.0f,
                    MvScaleUv(), (float)g_ppCurve, (float)g_ppContrast, (float)g_mvConfIdx,
                    (float)g_ppSat, (float)g_gateIdx, (a3Heap ? a3Bypass : g_bypass) ? 1.0f : 0.0f,
                    (a3Heap || g_useGpuResid) ? 1.0f : 0.0f, g_curJitX, g_curJitY, dj, a3Heap ? 1.0f : 0.0f };
    cl->SetComputeRoot32BitConstants(7, 20, c, 0);
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

// 跑马灯：在【我们自己的】g_netS 上画相位黑条（w/h 为网络尺寸）
static void MarkPass(ID3D12GraphicsCommandList* cl, int frameIdx) {
    if (!g_psoMark || !g_netS) return;
    cl->SetPipelineState(g_psoMark.Get());
    cl->SetComputeRootSignature(g_rs.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE b = g_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE u = b; u.ptr += (SIZE_T)kSrvNetS * g_inc;
    cl->SetComputeRootDescriptorTable(5, u);
    const float phase = (float)((frameIdx & 3)) * 0.25f;   // 0 .25 .5 .75
    float c[20] = { 0 };
    c[0]  = (float)((frameIdx & 7) + 1);   // gLimit → 数字 1~8（与 MarkSlot 一致）
    c[18] = 0.25f;    // gDejitter → 黑条宽度比例
    c[19] = phase;    // gPadJ     → 黑条起始相位
    cl->SetComputeRoot32BitConstants(7, 20, c, 0);
    cl->Dispatch((kNetW + 7) / 8, (kNetH + 7) / 8, 1);
}

// 跑马灯：把相位黑条涂进【槽位底图】（在 DownCS 喂入之前调用 ⇒ 喂入的图必然是跑马灯）
//   st0 = 进入前的真实状态；内部临时切 UAV，画完切到 afterState
static void MarkSlot(ID3D12GraphicsCommandList* cl, ID3D12Resource* tex, int frameIdx,
                     UINT w, UINT h, D3D12_RESOURCE_STATES st0, D3D12_RESOURCE_STATES afterState) {
    if (!g_psoMark || !tex || !g_dev) { ++g_cntPaintNoPso; return; }
    if (w == 0 || h == 0) { D3D12_RESOURCE_DESC d = tex->GetDesc(); w = (UINT)d.Width; h = d.Height; }
    {   // 没有 ALLOW_UNORDERED_ACCESS 的资源绝不能建 UAV（否则非法描述符 → 设备移除）
        D3D12_RESOURCE_DESC d2 = tex->GetDesc();
        if (!(d2.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
            static int sk = 0;
            if (sk++ < 3) Log("跑马灯(底图): 该纹理不支持 UAV（flags=0x%X），跳过", (unsigned)d2.Flags);
            return;
        }
    }
    g_dev->CreateUnorderedAccessView(tex, nullptr, nullptr,
        [&]{ D3D12_CPU_DESCRIPTOR_HANDLE hh = g_heap->GetCPUDescriptorHandleForHeapStart(); hh.ptr += (SIZE_T)kSrvMark * g_inc; return hh; }());

    Barrier(cl, tex, st0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetPipelineState(g_psoMark.Get());
    cl->SetComputeRootSignature(g_rs.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE u = g_heap->GetGPUDescriptorHandleForHeapStart();
    u.ptr += (SIZE_T)kSrvMark * g_inc;
    cl->SetComputeRootDescriptorTable(5, u);
    const float phase = (float)(frameIdx & 3) * 0.25f;
    float c[20] = { 0 };
    c[0]  = (float)((frameIdx & 7) + 1);   // gLimit → 数字 1~8 循环（三个三分之一处各画一次）
    c[18] = 0.25f;    // gDejitter → 条宽
    c[19] = phase;    // gPadJ     → 相位
    cl->SetComputeRoot32BitConstants(7, 20, c, 0);
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ++g_cntPaint;          // 成功涂标计数
    UavBarrier(cl, tex);   // ★ UAV 写 → 马上被 SRV 读，必须插 UAV 屏障（否则偶发读到旧内容）
    Barrier(cl, tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, afterState);
}

// 三横带对照视图：覆写 g_pre（UAV）为"答案 / 残差 / 我们的底图+残差"三条带
static void TriPass(ID3D12GraphicsCommandList* cl, int frameIdx, UINT w, UINT h, UINT srvFed, UINT srvAns, UINT srvBase) {
    if (!g_psoTri || !g_pre) return;
    cl->SetPipelineState(g_psoTri.Get());
    cl->SetComputeRootSignature(g_rs.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE b = g_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE a0 = b; a0.ptr += (SIZE_T)srvBase * g_inc;   // t0 = 槽位底图（我们涂的图案）
    D3D12_GPU_DESCRIPTOR_HANDLE a3 = b; a3.ptr += (SIZE_T)srvFed * g_inc;   // t3 = 喂入图
    D3D12_GPU_DESCRIPTOR_HANDLE a4 = b; a4.ptr += (SIZE_T)srvAns * g_inc;   // t4 = 答案
    D3D12_GPU_DESCRIPTOR_HANDLE u  = b; u.ptr  += (SIZE_T)kSrvPre * g_inc;  // u0 = g_pre
    cl->SetComputeRootDescriptorTable(0, a0);
    cl->SetComputeRootDescriptorTable(3, a3);
    cl->SetComputeRootDescriptorTable(4, a4);
    cl->SetComputeRootDescriptorTable(5, u);
    const float phase = (float)(frameIdx & 3) * 0.25f;
    float c[20] = { 0 };
    c[3]  = 1.0f;      // 三横带显示未加工残差
    c[18] = 0.25f;     // gDejitter → 条宽
    c[19] = phase;     // gPadJ     → 相位
    cl->SetComputeRoot32BitConstants(7, 20, c, 0);
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

// 描述符槽位：0..3 = 各槽位 color SRV，4 = 残差 SRV，5 = g_netS UAV，6 = g_pre UAV
// =====================================================================================
// 带宽/拷贝引擎测试：每帧在【copy 队列】上搬 33MB（= 新形态要传的 4K RGBA8 原生色）
//   · 目的：验证"拷贝类工作能否被计算完全掩盖"（探针里 ALU ∥ 拷贝 = 0.88，这里用真负载）
//   · 开关：xess_proxy.bwtest.txt 写 1/0（每 300 帧读一次）
//   · 上一帧的拷贝还没完成就跳过本帧 —— 绝不阻塞
// =====================================================================================
static bool g_bwOn = false;
static ComPtr<ID3D12CommandQueue> g_bwQ;
static ComPtr<ID3D12CommandAllocator> g_bwAlloc;
static ComPtr<ID3D12GraphicsCommandList> g_bwList;
static ComPtr<ID3D12Fence> g_bwFence;
static UINT64 g_bwFv = 0;
static ComPtr<ID3D12Resource> g_bwA, g_bwB;
static std::string g_bwPath;

static void BwPollFile() {
    if (g_bwPath.empty()) g_bwPath = ModuleDir() + "xess_proxy.bwtest.txt";
    static int s_last = -1;
    FILE* f = nullptr;
    if (fopen_s(&f, g_bwPath.c_str(), "r") != 0 || !f) { return; }
    char b[8] = { 0 };
    size_t n = fread(b, 1, 7, f);
    fclose(f);
    if (!n) return;
    int v = (b[0] == '1') ? 1 : 0;
    if (v == s_last) return;
    s_last = v;
    g_bwOn = (v != 0);
    Log("带宽测试: %s（帧 %d）", g_bwOn ? "开（每帧 copy 队列搬 33MB）" : "关", g_frame);
}

static bool BwInit(ID3D12Device* dev) {
    g_bwPath = ModuleDir() + "xess_proxy.bwtest.txt";
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_bwQ)))) { Log("带宽测试: 建 copy 队列失败"); return false; }
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&g_bwAlloc)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_bwAlloc.Get(), nullptr, IID_PPV_ARGS(&g_bwList)))) return false;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_bwFence)))) return false;
    g_bwList->Close();

    const UINT64 bytes = 3840ull * 2160ull * 4ull;   // 33.18 MB
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (int i = 0; i < 2; ++i) {
        ComPtr<ID3D12Resource>* t = i ? &g_bwB : &g_bwA;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                __uuidof(ID3D12Resource), (void**)t->GetAddressOf()))) {
            Log("带宽测试: 建 buffer 失败");
            return false;
        }
    }
    Log("带宽测试: 就绪（copy 队列 + 2×33.2MB）");
    return true;
}

static void BwTick() {
    if (!g_bwOn || !g_bwQ || !g_bwList || !g_bwFence) return;
    if (g_bwFence->GetCompletedValue() + 1 < g_bwFv) return;   // 上一帧还没搬完 → 本帧跳过
    if (FAILED(g_bwAlloc->Reset())) return;
    if (FAILED(g_bwList->Reset(g_bwAlloc.Get(), nullptr))) return;
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.pResource = g_bwA.Get();
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_bwList->ResourceBarrier(1, &br);
    br.Transition.pResource = g_bwB.Get();
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_bwList->ResourceBarrier(1, &br);
    g_bwList->CopyResource(g_bwB.Get(), g_bwA.Get());
    br.Transition.pResource = g_bwA.Get();
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_bwList->ResourceBarrier(1, &br);
    br.Transition.pResource = g_bwB.Get();
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_bwList->ResourceBarrier(1, &br);
    if (FAILED(g_bwList->Close())) return;
    ID3D12CommandList* ls[] = { g_bwList.Get() };
    g_bwQ->ExecuteCommandLists(1, ls);
    g_bwQ->Signal(g_bwFence.Get(), ++g_bwFv);
    static int cnt = 0;
    if ((cnt++ % 300) == 0) Log("带宽测试: 已提交 %d 次 33MB 拷贝（帧 %d）", cnt, g_frame);
}

// ---- MV 打包：1440p R16G16B16A16_SNORM(8B) → 1080p R16G16_FLOAT(4B) ----
// 传输量从 29.5MB 降到 8.3MB；只用 .rg，信息无损
static const char* kMvPackHLSL = R"(
Texture2D<float4>   gS : register(t0);
SamplerState        gM : register(s0);
RWTexture2D<float2> gD : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gD.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    // Motion vectors are per-pixel FACTS: point-sample them, never interpolate.
    // (Linear filtering averages vectors across motion edges -> smeared/under-estimated motion.)
    uint sw, sh; gS.GetDimensions(sw, sh);
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    int2 sp = int2(min(uv * float2(sw, sh), float2(sw - 1, sh - 1)));
    gD[id.xy] = gS.Load(int3(sp, 0)).rg;
}
)";
static ComPtr<ID3D12Resource> g_mvPacked;
// r28 块4：MV 本地暂存环（kNetW×kNetH RG16F DEFAULT，8 个环位，以 T%8 为下标）
static ComPtr<ID3D12Resource> g_mvHist[8];
static int    g_mvHistT[8] = {-1,-1,-1,-1,-1,-1,-1,-1};  // 各环位对应帧号，初始 -1
// g_r28MvEn / g_r28CntMvOkZero 已提前到 r28 计数块（:960 附近），HIT-1 修复前向引用
// -------------------------------------------------------------------------------------
// 2026-11-19 mvpair：MV 暂存入槽的"提交身份 + 帧号"配对（替换单格 g_mvPendingT 与只存围栏值的 g_fvMv[K]）
//   生产端 ProxyExecute：在游戏传入的 cl 上录完 MvPack + CopyResource(g_mvHist[T&7]) 后，
//     MvPairRegister(N, cl) 登记 {N, cl 原指针, cl 的 ReShade 解包指针}（每帧 1 次 QI，环 8 位按 N&7）。
//   HookECL 在原 ExecuteCommandLists 前按 lists[] 认领本次提交、之后才在命中队列打 MV 专用 Signal。
//   g_xqFe 是多队列共享的旧颜色记号，不能用作本帧 MV 的完成凭证。MV 每个历史环位一个 fence，
//   只有前一次 Signal 已完成才可复用；无新值能抢先越过未完成的旧值。
//   身份比对三层（前两层只是指针相等，零 QI）：
//     raw   ：lists[i] == cl 原指针（两边同层，常态：钩子收到的是游戏交给代理层的原数组）
//     canon ：lists[i] == cl 解包指针（lists 已是裸对象）
//     deep  ：Unwrap(lists[i]) == cl 解包指针（中间还有别的包装层），仅在有待配对记录、该登记没被前两层命中、
//             且 g_mvPairDeepMode != 1 时才做；浅命中后暂停（累计 8 次未命中淘汰即恢复），恢复后或观察到深命中则常开。
//   g_fvMv[K] = {frame, v}：C4 只认 frame 与本帧完全相等且 v != 0（MvFenceFor），frame-6 残留与跨帧错配都拒收。
//   登记和发布由 g_mvPairLock 保护；临界区内不调 COM、不打日志。
// -------------------------------------------------------------------------------------
struct MvFenceRec { LONG frame; UINT64 serial, v; ID3D12Fence* fence; };  // v == 0 ⇒ 无效
static MvFenceRec g_fvMv[K] = {};
struct MvPairPend { LONG frame; UINT64 serial; const void* raw; const void* canon; bool live; };
struct MvPairHit { LONG frame; UINT64 serial; int kind; };
static MvPairPend g_mvPairPend[8] = {};                   // 待提交记录（下标 = N&7，与 g_mvHist 同步）
static UINT64 g_mvPairSerial = 0;                        // 区分同帧重录时的迟到提交结果
static volatile LONG g_mvPairLive = 0;                    // live=0：消费端直接返回
static volatile LONG g_mvPairMissRun = 0;                 // 浅比对暂停期间累计淘汰数
static SRWLOCK g_mvPairLock = SRWLOCK_INIT;
// 每个环位独占其 fence（进程期持有；不持有游戏队列引用）。busy 只保护 CPU 发信号，不等待抢占。
struct MvPairSignalSlot { ComPtr<ID3D12Fence> fence; UINT64 issued = 0; volatile LONG busy = 0; };
static MvPairSignalSlot g_mvPairSignals[8];
// ReShade 6.7+ IID_UnwrappedObject（与 C4 heap 解包的 kC4IidReShadeUnwrappedObject 同值；OptiScaler Util.cpp 同源）
static constexpr GUID kIidReShadeUnwrappedMv =
    { 0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };
static inline int MvPairIdx8(LONG t) { return (int)(((t % 8) + 8) % 8); }
static inline int MvPairIdxK(LONG t) { return (int)(((t % K) + K) % K); }

// 解包身份：ReShade 代理 → _orig 指针值；E_NOINTERFACE → 对象本身；其它失败 → nullptr（未知）。
// 只取指针值做比较，不持有引用（QI 得到的引用立即 Release；被包装对象由包装层持有，存活期覆盖本帧提交）。
static const void* MvPairCanon(IUnknown* obj) {
    if (!obj) return nullptr;
    void* p = nullptr;
    const HRESULT hr = obj->QueryInterface(kIidReShadeUnwrappedMv, &p);
    if (SUCCEEDED(hr) && p) { static_cast<IUnknown*>(p)->Release(); return p; }
    if (p) static_cast<IUnknown*>(p)->Release();   // 防御：失败却给了指针
    return (hr == E_NOINTERFACE) ? (const void*)obj : nullptr;
}

static LONG MvPairRecountLocked() {
    LONG live = 0;
    for (int i = 0; i < 8; ++i) live += g_mvPairPend[i].live ? 1 : 0;
    InterlockedExchange(&g_mvPairLive, live);
    return live;
}

// 淘汰计数 + 浅比对模式下的失配回退（锁外调用）
static void MvPairNoteEvict(LONG evicted) {
    if (evicted <= 0) return;
    InterlockedExchangeAdd(&g_mvPairCntEvict, evicted);
    if (InterlockedCompareExchange(&g_mvPairDeepMode, 0, 0) == 1 &&
        InterlockedExchangeAdd(&g_mvPairMissRun, evicted) + evicted >= 8) {
        InterlockedExchange(&g_mvPairMissRun, 0);
        if (InterlockedCompareExchange(&g_mvPairDeepMode, 3, 1) != 1) return;
        static volatile LONG rn = 0;
        const LONG r = InterlockedIncrement(&rn);
        if (r <= 5 || (r % 100) == 0)
            Log("MVPAIR: 浅比对期间累计 8 次未命中淘汰 → 重新打开深比对（第 %ld 次）", r);
    }
}

// 生产端：ProxyExecute 在 cl 上录完 MV 暂存拷贝后调用（游戏渲染线程，每帧 1 次）
static void MvPairRegister(LONG frame, ID3D12GraphicsCommandList* cl) {
    if (!cl) return;
    const void* canon = MvPairCanon(cl);                 // 锁外 QI
    LONG evicted = 0;
    AcquireSRWLockExclusive(&g_mvPairLock);
    for (int i = 0; i < 8; ++i) {
        MvPairPend& e = g_mvPairPend[i];
        if (!e.live) continue;
        // 同环位（g_mvHist 已被本帧覆盖）/ 同一条 list 被重录（旧录制已提交却没被看见，或已丢弃）/
        // 帧号倒退或超出 K（其 g_fvMv 位已被新帧占用，即使再命中也交付不到 C4）⇒ 旧记录作废，
        // 绝不让它之后被别的提交"顺带"认领
        const bool sameSlot = MvPairIdx8(e.frame) == MvPairIdx8(frame);
        const bool sameList = (e.raw == (const void*)cl) || (canon && e.canon == canon);
        const bool stale    = (e.frame >= frame) || (frame - e.frame) >= K;
        if (sameSlot || sameList || stale) { e.live = false; ++evicted; }
    }
    const int idx = MvPairIdx8(frame);
    const UINT64 serial = ++g_mvPairSerial;
    g_mvPairPend[idx] = {frame, serial, cl, canon, true};
    g_mvHistT[idx] = frame;
    g_fvMv[MvPairIdxK(frame)] = {frame, serial, 0, nullptr};   // 只有本次登记的真实命中才能发布完成凭证
    MvPairRecountLocked();
    ReleaseSRWLockExclusive(&g_mvPairLock);
    MvPairNoteEvict(evicted);
    const LONG n = InterlockedIncrement(&g_mvPairCntReg);
    if (n == 1)
        Log("MVPAIR: 首次登记 frame=%ld cl=%p canon=%p 解包路径=%s",
            (long)frame, (void*)cl, canon,
            !canon ? "未知(QI失败，仅原指针比对)" : (canon == (const void*)cl ? "原生(E_NOINTERFACE)" : "ReShade代理→_orig"));
}

// 只消费匹配本次列表的登记，不碰其它列表的票；每条登记至多认领一次。
static void MvPairClaimLocked(UINT n, ID3D12CommandList* const* lists, const void* const* canonL,
                              UINT64 before, MvPairHit* hits, int& nhit) {
    const int prev = nhit;
    for (MvPairPend& e : g_mvPairPend) {
        if (!e.live || e.serial > before) continue;
        int how = 0;
        for (UINT i = 0; i < n && !how; ++i) {
            const void* li = lists[i];
            if (canonL) { if (e.canon && canonL[i] == e.canon) how = 3; }
            else if (li == e.raw)              how = 1;
            else if (e.canon && li == e.canon) how = 2;
        }
        if (!how) continue;
        e.live = false;
        hits[nhit++] = {e.frame, e.serial, how};
    }
    if (prev != nhit) MvPairRecountLocked();
}

// 提交之前固定本批登记代次；不在原 ECL 执行期间持锁，也不让之后才登记的记录搭这次 Signal。
static int MvPairConsume(UINT n, ID3D12CommandList* const* lists, MvPairHit (&hits)[8]) {
    if (!lists || n == 0 || !InterlockedCompareExchange(&g_mvPairLive, 0, 0)) return 0;
    int nhit = 0;
    UINT64 before = 0;
    AcquireSRWLockExclusive(&g_mvPairLock);
    before = g_mvPairSerial;
    MvPairClaimLocked(n, lists, nullptr, before, hits, nhit);
    ReleaseSRWLockExclusive(&g_mvPairLock);
    if (InterlockedCompareExchange(&g_mvPairDeepMode, 0, 0) == 1) return nhit;
    const void* canonL[32];
    for (UINT base = 0; base < n && nhit < 8 && InterlockedCompareExchange(&g_mvPairLive, 0, 0);) {
        const UINT m = (n - base < 32u) ? (n - base) : 32u;
        for (UINT i = 0; i < m; ++i) canonL[i] = MvPairCanon(lists[base + i]);  // 锁外 QI
        AcquireSRWLockExclusive(&g_mvPairLock);
        // 排除锁外解包期间新增的登记，只认领本批开始时已有的代次。
        MvPairClaimLocked(m, lists + base, canonL, before, hits, nhit);
        ReleaseSRWLockExclusive(&g_mvPairLock);
        base += m;
    }
    return nhit;
}

// 原 ECL 成功返回之后才在同一队列入 Signal。上一值已经到达才可复用（不等 GPU，忙则零 MV）。
static void MvPairSignal(ID3D12CommandQueue* q, const MvPairHit& hit) {
    MvPairSignalSlot& s = g_mvPairSignals[MvPairIdx8(hit.frame)];
    const char* stage = "signal_busy";
    HRESULT hr = E_PENDING;
    ID3D12Fence* f = nullptr;
    UINT64 v = 0;
    if (InterlockedCompareExchange(&s.busy, 1, 0) == 0) {
        if (!s.fence) {
            stage = "get_device";
            ComPtr<ID3D12Device> dev;
            hr = q->GetDevice(IID_PPV_ARGS(&dev));
            if (SUCCEEDED(hr)) {
                stage = "create_fence";
                hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence));
            }
        }
        if (s.fence) {
            const UINT64 done = s.fence->GetCompletedValue();
            if (done != UINT64_MAX && done >= s.issued) {
                stage = "queue_signal";
                hr = q->Signal(s.fence.Get(), s.issued + 1);
                if (SUCCEEDED(hr)) { v = ++s.issued; f = s.fence.Get(); }
            } else {
                stage = "signal_pending";
                hr = (done == UINT64_MAX) ? DXGI_ERROR_DEVICE_REMOVED : E_PENDING;
            }
        }
        InterlockedExchange(&s.busy, 0);
    }
    if (v) {
        AcquireSRWLockExclusive(&g_mvPairLock);
        MvFenceRec& r = g_fvMv[MvPairIdxK(hit.frame)];
        if (r.frame == hit.frame && r.serial == hit.serial && g_mvHistT[MvPairIdx8(hit.frame)] == hit.frame) {
            r.v = v; r.fence = f;
        }
        ReleaseSRWLockExclusive(&g_mvPairLock);
    } else {
        const LONG fails = InterlockedIncrement(&g_mvPairCntSigFail);
        if (fails <= 5 || fails % 300 == 0)
            Log("MVPAIR: 完成凭证失败 frame=%ld stage=%s hr=0x%08X queue=%p", hit.frame, stage, (unsigned)hr, (void*)q);
    }
    volatile LONG* cnt = (hit.kind == 1) ? &g_mvPairCntRaw : (hit.kind == 2) ? &g_mvPairCntCanon : &g_mvPairCntDeep;
    const LONG c = InterlockedIncrement(cnt);
    const LONG prev = (hit.kind == 3) ? InterlockedExchange(&g_mvPairDeepMode, 2)
                                    : InterlockedCompareExchange(&g_mvPairDeepMode, 1, 0);
    if (c == 1 || (hit.kind == 3 && prev != 2) || prev == 0)
        Log("MVPAIR: 命中路径=%s frame=%ld queue=%p fence=%p v=%llu sigOk=%d", hit.kind == 1 ? "raw" : hit.kind == 2 ? "canon" : "deep",
            hit.frame, (void*)q, (void*)f, (unsigned long long)v, v ? 1 : 0);
}

// 只认同一帧、同一历史环槽且 Signal 已成功排队的 {fence,v} 快照。
static MvFenceRec MvFenceFor(LONG frame) {
    MvFenceRec snap{};
    AcquireSRWLockShared(&g_mvPairLock);
    const MvFenceRec& r = g_fvMv[MvPairIdxK(frame)];
    if (r.frame == frame && r.v && g_mvHistT[MvPairIdx8(frame)] == frame) snap = r;
    ReleaseSRWLockShared(&g_mvPairLock);
    return snap;
}
static bool g_mvPackReady = false;

// =====================================================================================
// B 形态 步骤1：把「上采样所需的输入」跨卡传给 9070 XT
// -------------------------------------------------------------------------------------
// 传什么：合成后的 pColor（= 槽位原图 + 残差）+ 该槽位的 MV + 深度 + jitter
// 为什么传这些：B 要在自己的卡上跑上采样（真 XeSS / FSR4），它必须拿到这些输入。
// 放哪：一个跨适配器共享 buffer，2 槽轮转（每槽 512B 头 + 三张线性图）
// 开关：xess_proxy.bform.txt 写 1/0（每 300 帧读一次）；默认 0，不影响现有 A 形态
// 注意：RowPitch 必须 256 对齐（D3D12_TEXTURE_DATA_PITCH_ALIGNMENT）
// =====================================================================================
// 前置声明（MakeXBuf 定义在后面）
static bool MakeXBuf(UINT64 bytes, const char* tag,
                     ComPtr<ID3D12Heap>& hA, ComPtr<ID3D12Resource>& bA,
                     ComPtr<ID3D12Heap>& hB, ComPtr<ID3D12Resource>& bB);
static ComPtr<ID3D12Heap>     g_upHeap, g_upHeapW;
static ComPtr<ID3D12Resource> g_upBuf;
static UINT64 g_upSlotBytes = 0;      // 单槽字节数
static UINT64 g_upOffC = 0, g_upOffM = 0, g_upOffD = 0;   // 槽内三段偏移
static UINT   g_upPitchC = 0, g_upPitchM = 0, g_upPitchD = 0;
static UINT   g_upW = 0, g_upH = 0;
static UINT   g_upMw = 0, g_upMh = 0;
static UINT   g_upDw = 0, g_upDh = 0;
static DXGI_FORMAT g_upFmtC = DXGI_FORMAT_UNKNOWN;
static DXGI_FORMAT g_upFmtM = DXGI_FORMAT_UNKNOWN;
static DXGI_FORMAT g_upFmtD = DXGI_FORMAT_UNKNOWN;
static UINT   g_upFrameCnt = 0;
static std::string g_bformPath;

static UINT UpPitch(UINT w, UINT bpp) {
    UINT p = w * bpp;
    return (p + 255u) & ~255u;
}
// 返回每像素字节数；返回 0 表示【该格式不能用 PlacedFootprint 拷进 buffer】
//   典型不可拷：typeless（如 MV 的 R32G8X24_TYPELESS、深度的 typeless 变体）、
//               压缩格式、以及所有 depth/stencil。宁可跳过，也不要发非法调用（已因此崩过一次）。
static UINT FmtBytesCopyable(DXGI_FORMAT f) {
    switch (f) {
    // 4 字节
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:  case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
        return 4;
    // 8 字节
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
        return 8;
    // 2 字节
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
        return 2;
    // 1 字节
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT:
        return 1;
    // 其余（含所有 typeless / depth / stencil / BC 压缩）→ 不可拷
    default:
        return 0;
    }
}
static UINT FmtBytes(DXGI_FORMAT f) {
    UINT b = FmtBytesCopyable(f);
    return b ? b : 4u;    // 仅供不可拷格式算个"占位"尺寸，实际会被跳过
}

static void BformPollFile() {
    if (g_bformPath.empty()) g_bformPath = ModuleDir() + "xess_proxy.bform.txt";
    static int s_last = -1;
    FILE* f = nullptr;
    if (fopen_s(&f, g_bformPath.c_str(), "r") != 0 || !f) return;
    char b[8] = { 0 };
    size_t n = fread(b, 1, 7, f);
    fclose(f);
    if (!n) return;
    int v = (b[0] >= '0' && b[0] <= '9') ? (b[0] - '0') : 0;   // ★ 加档位必须同步改范围（曾写死 0~8）
    if (v == s_last) return;
    s_last = v;
    const bool taa = (v == 8);
    const bool b4k = (v == 9);
    g_bform4k = b4k;                     // 9 = 4K 后置：取 pOutput(XeSS 之后, 4K) 作底图，B 直通出画
    if (v == 6)
        Log("!! bform=6 已弃用：它是 Catmull-Rom 放大（画质明显差于 9）。保留仅作应急对照，推荐写 9 或 0");
    R28BumpEpoch("bformSwitch");         // r28 块2（NEW-HIT4）：先 bump，再置新档位，B 不能消费旧票
    g_taaOn = taa;
    {
        std::lock_guard<std::mutex> lk(g_worker.m);
        // A1: while B is selected, even its A fallback cannot acknowledge an A reset.
        g_worker.aReset.inA = (v == 0);
        if (g_bformOn && g_worker.aReset.inA) g_worker.aReset.Raise(A1SwitchA, g_frame);
        g_bformOn = (v != 0);
    }
    g_bformStage = (taa || b4k) ? 6 : v;  // 8/9 = 阶段 6 的变体（复用同一条链路）
    g_taaValid = false;                  // 任何切换都让时域历史作废，避免把上一形态的画面拖进来
    Log("BFORM: 开关 -> %s 阶段=%d%s（帧 %d）%s", g_bformOn ? "开" : "关", g_bformStage,
        taa ? " +TAA-lite" : "", g_frame,
        g_bformOn && !g_bformReady ? "（但资源未就绪，本帧跳过）" : "");
}

// 【探针 5】运行时对照实验：改 xess_proxy.diag5.txt 即刻生效（不用改代码、不用重启）
//   0=正常  1=成品整屏洋红（验证"我们看到的到底是不是成品"）
//   2=强制把残差抬到 +0.2（验证残差能不能上屏）  3=成品直接放 NR 答案（看网络输出本身）
static volatile int g_diag5Mode = 0;
static std::string g_diag5Path;
static void Diag5PollFile() {
    if (g_diag5Path.empty()) g_diag5Path = ModuleDir() + "xess_proxy.diag5.txt";
    static int s_last = -1;
    FILE* f = nullptr;
    if (fopen_s(&f, g_diag5Path.c_str(), "r") != 0 || !f) return;
    char b[8] = { 0 };
    size_t n = fread(b, 1, 7, f);
    fclose(f);
    if (!n) return;
    int v = (b[0] >= '0' && b[0] <= '4') ? (b[0] - '0') : 0;   // ★ 范围必须覆盖所有档位（曾写死 0~3，把 4 静默吃掉）
    if (v == s_last) return;
    s_last = v;
    g_diag5Mode = v;
    Log("DIAG5: 探针模式 = %d（%s）", v,
        v == 0 ? "正常" : v == 1 ? "整屏洋红 → 验证覆盖层可见性" :
        v == 2 ? "强制残差 +0.2 → 验证残差能上屏" :
        v == 3 ? "成品直接放 NR 答案 → 看网络输出本身" :
                 "残差低频扣除 → 只留细节修正，整帧不变亮变暗");
}

// 【TAA 参数热调】xess_proxy.taa.txt 写两个数："alpha clampK"（例如 0.25 3.0），每 60 帧读一次。
//   为什么要热调：TAA 的观感/拖尾全靠这两个数，改一次重启一次太浪费。
static void TaaPollFile() {
    static std::string path;
    if (path.empty()) path = ModuleDir() + "xess_proxy.taa.txt";
    static float sA = -1.0f, sC = -1.0f;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") != 0 || !f) return;
    float a = 0, c = 0;
    int n = fscanf_s(f, "%f %f", &a, &c);
    fclose(f);
    if (n < 2) return;
    if (a < 0.01f) a = 0.01f; if (a > 1.0f) a = 1.0f;
    if (c < 0.0f)  c = 0.0f;  if (c > 8.0f) c = 8.0f;
    if (a == sA && c == sC) return;
    sA = a; sC = c;
    g_taaAlpha = a; g_taaClampK = c;
    Log("TAA: 参数更新 alpha=%.2f clampK=%.2f", a, c);
}

static bool BformInit(ID3D12Device* dev, ID3D12Resource* color, ID3D12Resource* mv, ID3D12Resource* dep) {
    if (g_mvPackReady && g_mvPacked) mv = g_mvPacked.Get();   // 传的是打包后的 1080p MV
    if (!color || !mv || !dep) { Log("BFORM: 缺输入纹理，跳过初始化"); return false; }
    D3D12_RESOURCE_DESC cd = color->GetDesc();
    D3D12_RESOURCE_DESC md = mv->GetDesc();
    D3D12_RESOURCE_DESC dd = dep->GetDesc();

    g_upW = (UINT)cd.Width; g_upH = cd.Height; g_upFmtC = cd.Format;
    // ★ bform=9（4K 后置）：槽位与下游全部按【pOutput】的尺寸/格式 —— 底图是 XeSS 上采样之后的 4K
    if (g_bform4k && g_outpRes) {
        D3D12_RESOURCE_DESC od = g_outpRes->GetDesc();
        g_upW = (UINT)od.Width; g_upH = od.Height;
        g_upFmtC = DXGI_FORMAT_B8G8R8A8_UNORM;   // ★ 传 8bit 转换结果（与 A 侧底图同为 BGRA）；10bit 直传实验已回退
        Log("BFORM: 4K 后置模式 → 底图 %ux%u（8bit，由 pOutput 转换而来）", g_upW, g_upH);
    }
    g_upMw = (UINT)md.Width; g_upMh = md.Height; g_upFmtM = md.Format;
    g_upDw = (UINT)dd.Width; g_upDh = dd.Height; g_upFmtD = dd.Format;

    // 逐路检查"能不能用 PlacedFootprint 拷进 buffer"；不能就整路跳过（不发非法调用）
    UINT bppC = FmtBytesCopyable(g_upFmtC);
    UINT bppM = FmtBytesCopyable(g_upFmtM);
    UINT bppD = FmtBytesCopyable(g_upFmtD);
    if (!bppC) { Log("BFORM: 颜色格式 %d 不可拷，B 形态不可用", (int)g_upFmtC); return false; }
    if (!bppM) { Log("BFORM: MV 格式 %d 不可拷 → 跳过 MV 这一路", (int)g_upFmtM); g_upMw = g_upMh = 0; }
    if (!bppD) { Log("BFORM: 深度格式 %d 不可拷（typeless/深度）→ 跳过深度这一路", (int)g_upFmtD); g_upDw = g_upDh = 0; }

    g_upPitchC = UpPitch(g_upW, bppC);
    g_upPitchM = g_upMw ? UpPitch(g_upMw, bppM) : 0;
    g_upPitchD = g_upDw ? UpPitch(g_upDw, bppD) : 0;

    const UINT64 HDR = 512;
    g_upOffC = HDR;
    g_upOffM = g_upOffC + (UINT64)g_upPitchC * g_upH;
    g_upOffD = g_upOffM + (UINT64)g_upPitchM * g_upMh;
    g_upSlotBytes = g_upOffD + (UINT64)g_upPitchD * g_upDh;
    // 每槽起点按 64K 对齐（共享堆/放置要求）
    g_upSlotBytes = (g_upSlotBytes + 65535ull) & ~65535ull;

    UINT64 total = g_upSlotBytes * (UINT64)K;
    if (!MakeXBuf(total, "B形态上采样输入(K槽)", g_upHeap, g_upBuf, g_upHeapW, g_upBufW)) {
        Log("BFORM: 建跨卡 buffer 失败");
        return false;
    }
    g_upSlot = 0;
    g_upFrameCnt = 0;
    g_bformReady = true;
    Log("BFORM: 就绪 | 色 %ux%u fmt=%d pitch=%u | MV %ux%u fmt=%d pitch=%u | 深度 %ux%u fmt=%d pitch=%u | 每槽 %.1f MB × %d",
        g_upW, g_upH, (int)g_upFmtC, g_upPitchC,
        g_upMw, g_upMh, (int)g_upFmtM, g_upPitchM,
        g_upDw, g_upDh, (int)g_upFmtD, g_upPitchD,
        (double)g_upSlotBytes / (1024.0 * 1024.0), (int)K);
    return true;
}

// ===== 抓帧导出（2026-10-28）：把链路各环节的画面存成 .raw（BGRA8, 4K），供离线转 PNG =====
// 开关：xess_proxy.dumpshot.txt 内容 "<stage> <frames>"
//   stage = A（我方转换后的 4K，即准备传过去的） / N（B 解码到的底图） / C（B 合成结果）
// 文件落盘：<游戏目录>\dump\<stage>_f<帧号>.raw（4800x2700 之类请用同目录 .txt 里的宽高）
// (重复声明已移除) static std::string g_dumpDir;
// (重复声明已移除) static int   g_dumpStage = 0, g_dumpLeft = 0;
// (重复) static ComPtr<ID3D12Resource> g_dumpRbA, g_dumpRbB;
// (重复声明已移除) static bool  g_dumpPendA = false, g_dumpPendB = false;
// (重复) static ComPtr<ID3D12Resource> g_dumpRbA_, g_dumpRbB_;
// (重复) static int   g_dumpPendFrameA = 0, g_dumpPendFrameB = 0;


static const char* kDumpName[3] = { "A", "N", "C" };

static void DumpPollFile() {
    static int lastMask = -2, lastN = -1;
    if (g_dumpDir.empty()) { g_dumpDir = ModuleDir() + "dump"; CreateDirectoryA(g_dumpDir.c_str(), nullptr); }
    std::string p = ModuleDir() + "xess_proxy.dumpshot.txt";
    FILE* f = nullptr; if (fopen_s(&f, p.c_str(), "r") != 0 || !f) return;
    char st[16] = {0}; int n = 0;
    int got = fscanf_s(f, "%15s %d", st, (unsigned)sizeof(st), &n);
    fclose(f);
    if (got < 2) return;
    if (n <= 0) { g_dumpMask = 0; g_dumpLeft = 0; return; }   // ★ 写 0 就是关闭
    int mask = 0;
    for (int i = 0; st[i]; ++i) {
        char ch = (char)toupper((unsigned char)st[i]);
        if (ch == (char)65) mask |= 1;   // A
        else if (ch == (char)78) mask |= 2;  // N
        else if (ch == (char)67) mask |= 4;  // C
    }
    if (!mask) return;
    if (mask != lastMask || n != lastN) { lastMask = mask; lastN = n; g_dumpMask = mask; g_dumpLeft = n; Log("DUMP: 开始抓帧 mask=0x%X（A/N/C）共 %d 帧", mask, n); }
}

static void DumpRecord(ID3D12GraphicsCommandList* cl, ID3D12Device* dev, int st,
                       ID3D12Resource* src, D3D12_RESOURCE_STATES cur) {
    if (!cl || !dev || !src || g_dumpLeft <= 0) return;   // ★ <=0 必须停（之前写成 !g_dumpLeft 导致负数时继续抓）
    if (!(g_dumpMask & (1 << st))) return;
    D3D12_RESOURCE_DESC sd = src->GetDesc();
    UINT w = (UINT)sd.Width, h = sd.Height, pitch = (w * 4u + 255u) & ~255u;
    if (!g_dumpRb[st]) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = (UINT64)pitch * h; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&g_dumpRb[st])))) { Log("DUMP: 建回读缓冲失败(st=%d)", st); return; }
    }
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = src;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = g_dumpRb[st].Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = sd.Format;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter  = cur;
    cl->ResourceBarrier(1, &b);
    g_dumpW = w; g_dumpH = h; g_dumpPitch = pitch;
    g_dumpPend[st] = true; g_dumpPendFrame[st] = g_frame;
}

static void DumpFlush(int st) {
    if (!g_dumpPend[st] || !g_dumpRb[st] || g_dumpLeft <= 0) return;
    g_dumpPend[st] = false;
    void* m = nullptr; D3D12_RANGE rr{ 0, (SIZE_T)g_dumpPitch * g_dumpH };
    if (FAILED(g_dumpRb[st]->Map(0, &rr, &m)) || !m) return;
    char name[256];
    sprintf_s(name, "%s\\%s_f%d.raw", g_dumpDir.c_str(), kDumpName[st], g_dumpPendFrame[st]);
    FILE* f = nullptr;
    if (fopen_s(&f, name, "wb") == 0 && f) {
        fwrite(m, 1, (size_t)g_dumpPitch * g_dumpH, f); fclose(f);
        sprintf_s(name, "%s\\%s_f%d.txt", g_dumpDir.c_str(), kDumpName[st], g_dumpPendFrame[st]);
        if (fopen_s(&f, name, "w") == 0 && f) { fprintf(f, "w=%u h=%u pitch=%u fmt=BGRA8\n", g_dumpW, g_dumpH, g_dumpPitch); fclose(f); }
        --g_dumpLeft;
        Log("DUMP: 已存 %s_f%d（%ux%u）剩余 %d", kDumpName[st], g_dumpPendFrame[st], g_dumpW, g_dumpH, g_dumpLeft);
    }
    D3D12_RANGE nw{ 0, 0 }; g_dumpRb[st]->Unmap(0, &nw);
}
// 记在游戏的命令列表上：三张图 → 当前槽
static void BformCopy(ID3D12GraphicsCommandList* cl, ID3D12Resource* color,
                      ID3D12Resource* mv, ID3D12Resource* dep, float jx, float jy) {
    if (!g_bformOn || !g_bformReady || !cl || !color || !mv || !dep) return;
    // 槽位按【帧号】索引（K 槽）：B 处理第 N 帧时去读 N%K，不会被后来的帧覆盖
    const UINT64 base = (UINT64)(g_frame % K) * g_upSlotBytes;

    Barrier(cl, g_upBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);

    auto one = [&](ID3D12Resource* src, DXGI_FORMAT fmt, UINT w, UINT h, UINT pitch, UINT64 off,
                   D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES back) {
        Barrier(cl, src, from, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_upBuf.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = base + off;
        dst.PlacedFootprint.Footprint.Format = fmt;
        dst.PlacedFootprint.Footprint.Width = w;
        dst.PlacedFootprint.Footprint.Height = h;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = pitch;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = src;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
        Barrier(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, back);
    };
    // 状态与 ③ 步捕获槽位时完全一致（照抄，避免又多一种状态假设）
    if (!g_xferOwn)   // 颜色由我们自己的 COPY 队列拷（见 XferKickColor），这里只拷 MV/深度
        one(color, g_upFmtC, g_upW, g_upH, g_upPitchC, g_upOffC,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
    if (g_upMw && g_upMh)
        one(mv, g_upFmtM, g_upMw, g_upMh, g_upPitchM, g_upOffM,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (g_upDw && g_upDh)
        one(dep, g_upFmtD, g_upDw, g_upDh, g_upPitchD, g_upOffD,
            D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_READ);

    Barrier(cl, g_upBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);

    if (!g_xferOwn) {   // ★ 4K 模式下 tag/lastSlot 由 XferKickColor 在【颜色拷贝成功后】发布
        const int sl = g_frame % K;
        // v4 P6：改走令牌发布协议；jitter 作为面单字段随包发布（原来只写 frame/jx/jy 三个字段）
        UiPublishSlot(sl, g_frame, nullptr, 0, jx, jy);
        g_upLastSlot = sl;
    }
    if ((g_upFrameCnt++ % 300) == 0)
        Log("BFORM: 已拷贝第 %u 次（帧 %d 槽 %d，每槽 %.1f MB）jitter=%.3f,%.3f",
            g_upFrameCnt, g_frame, g_frame % K, (double)g_upSlotBytes / (1024.0 * 1024.0), jx, jy);
}

// =====================================================================================
// B 形态 步骤2：B 侧把共享 buffer 解成纹理（供之后的降采样 / NR / 上采样用）
//   全部懒创建 + 全在 bform 开关下；bform=0 时一行都不执行
// =====================================================================================
static ComPtr<ID3D12Resource> g_bTexM, g_bTexD;
static bool g_bTexReady = false;
static UINT g_bDecCnt = 0;

static bool BformBEnsure(ID3D12Device* dev) {
    if (g_bTexReady) return true;
    if (!dev || !g_bformReady || !g_upW || !g_upH) return false;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto mk = [&](UINT w, UINT h, DXGI_FORMAT f, ComPtr<ID3D12Resource>& r, const char* nm,
                  D3D12_RESOURCE_FLAGS fl = D3D12_RESOURCE_FLAG_NONE) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h;
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = f; td.SampleDesc.Count = 1;
        td.Flags = fl;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
        if (FAILED(hr)) { Log("BFORM-B: 建 %s 纹理失败 0x%08X", nm, (unsigned)hr); return false; }
        return true;
    };
    if (!mk(g_upW, g_upH, g_upFmtC, g_bTexC, "色", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;
    if (!mk(g_upMw, g_upMh, g_upFmtM, g_bTexM, "MV")) return false;
    if (g_upDw && g_upDh) { if (!mk(g_upDw, g_upDh, g_upFmtD, g_bTexD, "深度")) return false; }
    g_bTexReady = true;
    Log("BFORM-B: B 侧纹理就绪 | 色 %ux%u fmt=%d | MV %ux%u fmt=%d | 深度 %ux%u fmt=%d",
        g_upW, g_upH, (int)g_upFmtC, g_upMw, g_upMh, (int)g_upFmtM, g_upDw, g_upDh, (int)g_upFmtD);
    return true;
}

// ★ v4fix2：把"这一轮解出的槽位身份"发布出去（只有令牌校验通过后才调用）。
//   为什么需要：出画闸门必须能回答"这次 Present 画的是不是【本代的新帧】"。
//   槽位令牌（epoch/attempt/frame）是 A 写 B 读的唯一身份来源；解码成功 = 该身份确实被消费了。
//   "同一个槽被连续读两轮"不算新帧（B 侧可能因为等待/防饿死门重读同一格），所以用
//   (frame, epoch) 去重后再推进 g_bDecSeq —— 预热计数按 g_bDecSeq 推进，天然每帧只算一次。
static void BformPublishDecodeIdentity(int slot) {
    const LONG ep = (LONG)(UINT)g_slotTag[slot].epoch;
    const LONG at = (LONG)(UINT)g_slotTag[slot].attempt;
    const LONG fr = (LONG)g_slotTag[slot].frame;
    const LONG prevFr = InterlockedCompareExchange(&g_bDecFrame, 0, 0);
    const LONG prevEp = InterlockedCompareExchange(&g_bDecEpoch, 0, 0);
    InterlockedExchange64(&g_bDecTick, (LONGLONG)GetTickCount64());
    if (fr == prevFr && ep == prevEp) return;          // 同一格重读：不是新帧
    InterlockedExchange(&g_bDecEpoch, ep);
    InterlockedExchange(&g_bDecAttempt, at);
    InterlockedExchange(&g_bDecFrame, fr);
    InterlockedIncrement(&g_bDecSeq);
}

// 记在 B 的命令列表上：共享 buffer 当前槽 → 三张本地纹理
// ★ v4fix2：返回 bool（false = 本轮没有解出有效新帧：未开启 / 未就绪 / 令牌不匹配 / 资源失败）。
static bool BformBDecode(ID3D12GraphicsCommandList* cl, int slot) {
    if (!g_bformOn || !g_bformReady || !cl || !g_upBufW) return false;
    // ★ v4 P7：消费端令牌三字段校验（epoch + attempt + frame 下限）。不匹配 ⇒ 本槽不消费。
    //   两个 B 分支（WorkerMain 的 NGX 准备块 / lmxxf 准备块）都从这里解码，所以单点即可覆盖，
    //   不让"上一代/上一批次"的槽被当成新帧解出来（UI 开关那几帧最容易踩）。
    if (!UiTokenValid(slot)) {
        static int tv = 0;
        if ((tv++ % 60) == 0) LogWarn("TOKEN: 槽 %d 令牌不匹配（epoch/attempt/frame）→ 本轮跳过解码，累计 %d", slot, tv);
        return false;
    }
    if (!BformBEnsure(g_worker.dev.Get())) return false;
    // step2wm r2：一个槽只允许成功消费一次，且只在 NGX + C4 下生效（lmxxf 恢复基线：同一槽可重读）。
    // 身份走 tag->frame：valid 已是 1（UiTokenValid 刚通过），frame 在 valid 清 0 之前不会被改写。
    {
        UINT64 ready = g_slotReadyFv[slot];
        UINT64 comp = (g_xqFe2) ? g_xqFe2->GetCompletedValue() : 0;
        int fValid = (g_xqFe2 != nullptr) ? 1 : 0;
        BtTrace("DECODE_BEGIN", g_tlFrame, "decode",
                "iter=%ld slot=%d tag=%d ready=%llu producer_fence=0x%llx side=g completed=%llu f_valid=%d worker_queue=0x%llx wait_enqueued=%d",
                g_tlIter, slot, (int)g_slotTag[slot].frame, (unsigned long long)ready,
                BtPtr(g_xqFe2 ? g_xqFe2.Get() : nullptr), (unsigned long long)comp, fValid,
                BtPtr(g_worker.q ? g_worker.q.Get() : nullptr), g_tlDecodeWaitEnqueued);
    }
    const UINT64 base = (UINT64)slot * g_upSlotBytes;

    auto one = [&](ID3D12Resource* tex, DXGI_FORMAT fmt, UINT w, UINT h, UINT pitch, UINT64 off) {
        Barrier(cl, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = tex;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = g_upBufW.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Offset = base + off;
        s.PlacedFootprint.Footprint.Format = fmt;
        s.PlacedFootprint.Footprint.Width = w;
        s.PlacedFootprint.Footprint.Height = h;
        s.PlacedFootprint.Footprint.Depth = 1;
        s.PlacedFootprint.Footprint.RowPitch = pitch;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        Barrier(cl, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    };
    // ★ B 侧读共享 buffer 前必须把它转成 COPY_SOURCE —— 跨适配器资源的"状态"是各设备各记一份的，
    //    A 那边转成 COMMON 不代表 B 这边也在 COMMON。少了这句会读到非法状态（表现是极慢/异常）。
    Barrier(cl, g_upBufW.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    one(g_bTexC.Get(), g_upFmtC, g_upW, g_upH, g_upPitchC, g_upOffC);
    if (g_upMw && g_upMh)
        one(g_bTexM.Get(), g_upFmtM, g_upMw, g_upMh, g_upPitchM, g_upOffM);
    if (g_upDw && g_upDh && g_bTexD)
        one(g_bTexD.Get(), g_upFmtD, g_upDw, g_upDh, g_upPitchD, g_upOffD);
    Barrier(cl, g_upBufW.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

    // ★ v4fix2：拷贝命令都排进 list 之后登记本轮身份 → 供出画闸门判定"这一次 Present 是不是新帧"
    BformPublishDecodeIdentity(slot);
    R28ShShown(slot);   // r28 块1：屏上帧号 shownT / maxShown / backward（只记账，倒退时打一行）

    if ((g_bDecCnt++ % 300) == 0)
        Log("BFORM-B: 已解出第 %u 次（槽 %d，%ux%u，帧 %d，epoch %u）", g_bDecCnt, slot, g_upW, g_upH,
            (int)g_slotTag[slot].frame, (UINT)g_slotTag[slot].epoch);
    return true;
}

// ★ 2026-10-31 N2：槽位解出的 MV → 共享纹理 W.vel
//   为什么必须拷：W.vel 建的时候带 shared（核心要把它 import 到自己那条 Vulkan 设备上做原地采样），
//   而 g_bTexM 是普通 DEFAULT 纹理、不能被 import ⇒ 只能抄一份。
//   A 形态里同一拷贝的源是跨卡 buffer；B 形态下源换成槽位解出的 g_bTexM。
static bool BformSlotMvToShared(ID3D12GraphicsCommandList* cl, ID3D12Resource* velDst,
                                D3D12_RESOURCE_STATES curDst) {
    if (!cl || !velDst || !g_bTexM || !g_upMw || !g_upMh) return false;
    if (g_upMw != kNetW || g_upMh != kNetH) return false;         // 形状不一致就不拷（避免非法拷贝）
    if (g_upFmtM != DXGI_FORMAT_R16G16_FLOAT) return false;       // W.vel 固定 RG16F
    Barrier(cl, g_bTexM.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cl, velDst, curDst, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(velDst, g_bTexM.Get());
    Barrier(cl, velDst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bTexM.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    static int c = 0;
    if ((c++ % 300) == 0) Log("BFORM-B(NGX): 槽位MV已装进共享纹理 W.vel（第 %d 次）", c);
    return true;
}

// ---- B 形态 步骤3：B 侧自己把收到的 pColor 降采样到 1080p，直接喂给 NR ----
//   这样 A 就不用再传 1080p（省 8.3MB/帧 + 一条通道）
static const char* kBformDS = R"(
cbuffer PpB : register(b0) { float gPpOn; float gPpGain; float gPpCurve; float gPpContrast; float gPpSat; };
Texture2D<float4>   gS : register(t0);
SamplerState        gM : register(s0);
RWTexture2D<float4> gD : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gD.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    // ★ 与 A 侧 DownCS 对齐：2×2 区域平均（源比目标大约 1.16 倍，单点采样会混叠）
    //   原来是单点双线性，导致 B 喂给网络的小图和 A 不是同一张 →
    //   同一场景 A 的 |答案−喂入| ≈7~9、B 只有 2~6（差 3~4 倍）
    float2 t = float2(0.25 / float(w), 0.25 / float(h)) * 1.16;
    float4 c  = gS.SampleLevel(gM, uv + float2(-t.x, -t.y), 0);
    c += gS.SampleLevel(gM, uv + float2( t.x, -t.y), 0);
    c += gS.SampleLevel(gM, uv + float2(-t.x,  t.y), 0);
    c += gS.SampleLevel(gM, uv + float2( t.x,  t.y), 0);
    gD[id.xy] = float4(PpApply(c.rgb * 0.25, gPpOn, gPpGain, gPpCurve, gPpContrast, gPpSat), 1.0);
}
)";
static ComPtr<ID3D12RootSignature> g_bDsRs;
static ComPtr<ID3D12PipelineState> g_bDsPso;
static ComPtr<ID3D12Resource> g_bDs1080;
static bool g_bDsReady = false;

// ★ 共用的小工具堆/根签名：DS 与 UP 都用"描述符表"绑纹理
//   （根描述符 SRV/UAV 只能描述 buffer，绑 Texture2D 会让 PSO 创建失败 —— 已踩）
static ComPtr<ID3D12DescriptorHeap> g_bAuxHeap;   // 0=DS源 1=DS目标 2=UP源 3=UP目标
static ComPtr<ID3D12RootSignature>  g_bAuxRs;
static UINT g_bAuxInc = 0;
static bool g_bAuxReady = false;

static bool BformBAuxEnsure(ID3D12Device* dev) {
    if (g_bAuxReady) return true;
    if (!dev) return false;
    D3D12_DESCRIPTOR_RANGE rgS{};
    rgS.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rgS.NumDescriptors = 1;
    rgS.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE rgU{};
    rgU.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rgU.NumDescriptors = 1;
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
    rp[2].Constants.RegisterSpace = 0;
    rp[2].Constants.Num32BitValues = 5;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3; rd.pParameters = rp;
    rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        Log("BFORM-B: AUX 根签名失败: %s", err ? (char*)err->GetBufferPointer() : "?"); return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_bAuxRs)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_bAuxHeap)))) return false;
    g_bAuxInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_bAuxReady = true;
    return true;
}

static bool BformBEnsureDS(ID3D12Device* dev) {
    if (g_bDsReady) return true;
    if (!dev) return false;
    if (!BformBAuxEnsure(dev)) return false;
    g_bDsRs = g_bAuxRs;
    ComPtr<ID3DBlob> cs, e2;
    std::string bdsSrc = std::string(kPreprocessHLSL) + kBformDS;
    if (FAILED(D3DCompile(bdsSrc.data(), bdsSrc.size(), "bds.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
        Log("BFORM-B: DS 编译失败: %s", e2 ? (char*)e2->GetBufferPointer() : "?"); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_bDsRs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    {   HRESULT hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_bDsPso));
        if (FAILED(hr)) { Log("BFORM-B: DS PSO 失败 0x%08X", (unsigned)hr); return false; }
    }

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kNetW; td.Height = kNetH;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_bDs1080)))) {
        Log("BFORM-B: DS 纹理失败"); return false;
    }
    g_bDsReady = true;
    Log("BFORM-B: 本地降采样就绪（%dx%d）", kNetW, kNetH);
    return true;
}

// B 侧：收到的 pColor → 1080p → 写进 NR 的输入纹理
static bool BformBDownsample(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst1080) {
    if (!g_bDsReady || !g_bTexC || !dst1080) return false;
    Barrier(cl, g_bTexC.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetComputeRootSignature(g_bDsRs.Get());
    cl->SetPipelineState(g_bDsPso.Get());
    {   // 走描述符表（0=源 SRV，1=目标 UAV）
        auto mkS = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)i * g_bAuxInc;
            g_worker.dev->CreateShaderResourceView(r, &sd, h);
        };
        auto mkU = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)i * g_bAuxInc;
            g_worker.dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
        };
        mkS(g_bTexC.Get(), g_upFmtC, 0);
        mkU(g_bDs1080.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);
        ID3D12DescriptorHeap* hs[] = { g_bAuxHeap.Get() };
        cl->SetDescriptorHeaps(1, hs);
        D3D12_GPU_DESCRIPTOR_HANDLE g = g_bAuxHeap->GetGPUDescriptorHandleForHeapStart();
        cl->SetComputeRootDescriptorTable(0, g);
        D3D12_GPU_DESCRIPTOR_HANDLE g1 = g; g1.ptr += g_bAuxInc;
        cl->SetComputeRootDescriptorTable(1, g1);
    }
    EnsurePreprocessLoadedFromIni();
    float pp[5] = {
        g_ppOn ? 1.0f : 0.0f,
        (g_ppExpMode == 0) ? 1.0f : exp2f((float)g_ppBias),
        (float)g_ppCurve,
        (float)g_ppContrast,
        (float)g_ppSat
    };
    cl->SetComputeRoot32BitConstants(2, 5, pp, 0);
    cl->Dispatch((kNetW + 7) / 8, (kNetH + 7) / 8, 1);
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    // 注意：dst1080（NR 输入纹理）照抄现有做法，不额外加状态转换（跨 API 导入，沿用原约定）
    cl->CopyResource(dst1080, g_bDs1080.Get());
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bTexC.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    static int c = 0;
    if ((c++ % 300) == 0) Log("BFORM-B: 已本地降采样 %d 次", c);
    return true;
}

// ---- B 形态 步骤4：B 侧残差合成（把 A 的 ResidCS 搬到 B，答案改从纹理取）----
//   输入：pColor(收到) + MV(收到) + 喂进 NR 的 1080p + NR 答案(纹理)
//   输出：合成后的 pColor（供 S5 上采样用）；时域状态在 B 本地
static const char* kBformResid = R"(
Texture2D<float4>   gSrc : register(t0);
Texture2D<float4>   gRes : register(t1);
Texture2D<float4>   gMV  : register(t2);
Texture2D<float4>   gFed : register(t3);
Texture2D<float4>   gAns : register(t4);
SamplerState        gSmp : register(s0);
RWTexture2D<float4> gDst : register(u0);
RWTexture2D<float4> gSt  : register(u1);
cbuffer P : register(b0)
{
    // ★ 必须与 A 侧 SetPass 上传的 20 个常量【逐位对齐】（A 的 kHLSL 顺序）。
    //   曾经这里只声明 8 个、且删掉了 edge/smooth/temporal，导致后面的
    //   gain/sat/alpha/varGain/mvScale/mvConf/gateK 全部塌陷错位：
    //   MV 缩放被喂成 alpha、方差门限被喂成 sat ⇒ 残差被重投影到错误位置后又抵消掉。
    float gLimit; float gEdge; float gSmooth; float gGain;
    float gSat; float gAlpha; float gVarGain; float gTemporal;
    float gMvScale; float gMvConf; float gMvIdx; float gConfIdx;
    float gGateK; float gGateIdx; float gMode; float gForce;
    float gLowCut; float gLowR; float gPad2; float gPad3;
};
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 c = gSrc.Load(int3((int)id.x, (int)id.y, 0));
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);

    float3 a = gAns.SampleLevel(gSmp, uv, 0).rgb;
    float3 f = gFed.SampleLevel(gSmp, uv, 0).rgb;
    float3 e = a - f;

    // 【低频扣除】把残差的"局部均值"减掉，只保留高频细节修正。
    //   为什么：网络输出偶尔整体偏亮/偏暗（同帧实测差过 20+ 灰阶），
    //   全额叠上去会让整帧变亮变暗 → 表现为全图亮暗闪。
    //   做法：8 个方向 + 自己共 9 点，在大半径 gLowR 上估均值（很便宜）。
    if (gLowCut > 0.5) {
        uint nw, nh; gFed.GetDimensions(nw, nh);
        float2 tx = float2(1.0 / float(nw), 1.0 / float(nh)) * gLowR;
        float3 se = e;
        [unroll] for (int kk = 0; kk < 8; ++kk) {
            float ang = (float)kk * 0.7853982;
            float2 off = float2(cos(ang), sin(ang)) * tx;
            float3 a2 = gAns.SampleLevel(gSmp, saturate(uv + off), 0).rgb;
            float3 f2 = gFed.SampleLevel(gSmp, saturate(uv + off), 0).rgb;
            se += (a2 - f2);
        }
        e -= se * (1.0 / 9.0);
    }

    // 代理后处理关闭：保留当前 NR 残差，不读取或混合历史。
        // ---- 探针 5（运行时切换；只作用于 B 侧合成）----
    if (gMode > 0.5 && gMode < 1.5) { gDst[id.xy] = float4(1.0, 0.0, 1.0, 1.0); return; }  // 1=纯洋红
    if (gMode > 2.5 && gMode < 3.5) { gDst[id.xy] = float4(a, 1.0); return; }               // 3=直接放答案
    if (gMode > 1.5) { gDst[id.xy] = float4(saturate(c.rgb + e + gForce), c.a); return; }   // 2=强制叠加
gDst[id.xy] = float4(saturate(c.rgb + e), c.a);
}
)";
// ★ 2026-10-31 F8 照搬到 B 形态：三横带诊断（写进 g_bComp，出画链路不变）
//   上 = 答案 / 中 = 残差灰阶 / 下 = 底图 + 残差，与 A 形态 TriCS 同口径
//   绑定沿用残差合成的根签名：t0=底图(4K) t3=喂入(1080p) t4=答案  u0=输出
static const char* kBformTri = R"(
Texture2D<float4>   gSrc : register(t0);
Texture2D<float4>   gFed : register(t3);
Texture2D<float4>   gAns : register(t4);
SamplerState        gSmp : register(s0);
RWTexture2D<float4> gDst : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float  fy = float(id.y) / float(h);
    float3 c = gSrc.Load(int3((int)id.x, (int)id.y, 0)).rgb;
    float3 f = gFed.SampleLevel(gSmp, uv, 0).rgb;
    float3 a = gAns.SampleLevel(gSmp, uv, 0).rgb;
    float3 e = a - f;
    float  el = dot(e, float3(0.2126, 0.7152, 0.0722));
    float  g  = saturate(0.18 + el * 8.0);
    float3 o;
    if (fy < 0.33333)      o = a * 2.0;
    else if (fy < 0.66666) o = float3(g, g, g);
    else                   o = saturate(c + e);
    gDst[id.xy] = float4(o, 1.0);
}
)";

// ★ 2026-10-31 F7 照搬到 B 形态：跑马灯黑条（画在 B 侧解出的 4K 底图上）
//   与 A 侧 MarkCS 【同口径】：整幅换成「白底 + 黑条 + 条内白数字」，
//   不保留游戏原画面 —— 判错配时底图必须干净，残差里混进别相位的条才一眼可见。
//   （要看真实画面按 F7 关掉即可；A 侧跨卡槽位里的内容始终是真实最终画面，不受影响）
//   绑定：u0 = 目标，b0 = 20 个常量（与合成/三横带共用根签名）
static const char* kBformMark = R"(
cbuffer P : register(b0) { float gc[20]; };
RWTexture2D<float4> gDst : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float phase = gc[19];                       // 0/.25/.5/.75
    float bw    = gc[18];                       // 条宽比例
    int   digit = (int)clamp(gc[0], 1.0, 8.0);  // 1..8
    // ★ 2026-10-31 自证实验：条位置【直接由数字推算】，不再用独立的相位常量。
    //   这样"数字"与"条位置"在同一行代码里绑定，不可能出现"数字变、条不动"。
    //   若仍不动 ⇒ 屏幕上那个数字不是本 shader 画的，问题在呈现层。
    phase = float(digit - 1) / 8.0;             // 数字 3 → 第 3 格 … 数字 8 → 第 8 格
    bw    = 0.125;                              // 8 格刚好排满一屏
    if (!(bw > 0.02) || bw > 0.9) bw = 0.25;    // 兜底：常量异常时也保证有条
    if (!(phase >= 0.0) || phase > 0.99) phase = 0.0;
    float x0 = phase * float(w), x1 = x0 + bw * float(w);
    float px = float(id.x), py = float(id.y);
    if (px < x0 || px >= x1) {
        // gc[17] > 0.5 = 叠加模式（画在合成结果上）：条外不动，只叠一条参考条
        if (gc[17] > 0.5) return;
        gDst[id.xy] = float4(1.0, 1.0, 1.0, 1.0);   // 底图模式：条外涂白底（与 A 侧一致）
        return;
    }
    float3 col = float3(0.0, 0.0, 0.0);         // 条内先黑，再印白数字
    int m = 127;
    if (digit == 1) m = 6;
    else if (digit == 2) m = 91;
    else if (digit == 3) m = 79;
    else if (digit == 4) m = 102;
    else if (digit == 5) m = 109;
    else if (digit == 6) m = 125;
    else if (digit == 7) m = 7;
    float thirdH = float(h) / 3.0;
    float gw = bw * float(w) * 0.55;
    float gx0 = x0 + (bw * float(w) - gw) * 0.5;
    for (int t = 0; t < 3; ++t) {
        float gy0 = float(t) * thirdH + thirdH * 0.22;
        float gh  = thirdH * 0.56;
        if (px >= gx0 && px < gx0 + gw && py >= gy0 && py < gy0 + gh) {
            float lx = (px - gx0) / gw, ly = (py - gy0) / gh;
            float T = 0.16;
            bool sA = (ly < T)        && (lx > T*0.8) && (lx < 1.0 - T*0.8);
            bool sD = (ly > 1.0 - T)  && (lx > T*0.8) && (lx < 1.0 - T*0.8);
            bool sG = (abs(ly-0.5) < T*0.6) && (lx > T*0.8) && (lx < 1.0 - T*0.8);
            bool sF = (lx < T)        && (ly > T*0.8) && (ly < 0.5 - T*0.3);
            bool sB = (lx > 1.0 - T)  && (ly > T*0.8) && (ly < 0.5 - T*0.3);
            bool sE = (lx < T)        && (ly > 0.5 + T*0.3) && (ly < 1.0 - T*0.8);
            bool sC = (lx > 1.0 - T)  && (ly > 0.5 + T*0.3) && (ly < 1.0 - T*0.8);
            bool on = (sA && (m & 1))  || (sB && (m & 2))  || (sC && (m & 4))  || (sD && (m & 8))
                   || (sE && (m & 16)) || (sF && (m & 32)) || (sG && (m & 64));
            if (on) col = float3(1.0, 1.0, 1.0);
        }
    }
    gDst[id.xy] = float4(col, 1.0);
}
)";

static ComPtr<ID3D12RootSignature> g_bRs;
static ComPtr<ID3D12PipelineState> g_bPso;
static ComPtr<ID3D12PipelineState> g_bTriPso;   // ★ F8：三横带诊断 PSO（与合成共用根签名）
static ComPtr<ID3D12PipelineState> g_bMarkPso;  // ★ F7：跑马灯 PSO（与合成共用根签名）
static ComPtr<ID3D12DescriptorHeap> g_bHeap;
static UINT g_bInc = 0;
static ComPtr<ID3D12Resource> g_bComp[2], g_bState;
static bool g_bStateCleared = false;
static UINT g_bCompCnt = 0;

// B heap: 0..5 input SRVs, 6=legacy PP UAV, 7=state, 8=F7 base, 9/10=compose UAVs.
static bool BformBEnsureComp(ID3D12Device* dev) {
    if (g_bCompReady) return true;
    if (!dev || !g_bformReady || !g_upW) return false;

    // ★ 每张表必须用【不同的寄存器】t0..t4 / u0 / u1；
    //   全部用 t0 会让根签名序列化直接失败（0x80070057，已踩）
    D3D12_DESCRIPTOR_RANGE rgS[5]{};
    D3D12_DESCRIPTOR_RANGE rgU[2]{};
    for (int i = 0; i < 5; ++i) {
        rgS[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rgS[i].NumDescriptors = 1;
        rgS[i].BaseShaderRegister = (UINT)i;
    }
    for (int i = 0; i < 2; ++i) {
        rgU[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        rgU[i].NumDescriptors = 1;
        rgU[i].BaseShaderRegister = (UINT)i;
    }
    D3D12_ROOT_PARAMETER rp[8]{};
    for (int i = 0; i < 5; ++i) {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[i].DescriptorTable.NumDescriptorRanges = 1;
        rp[i].DescriptorTable.pDescriptorRanges = &rgS[i];
        rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[5].DescriptorTable.NumDescriptorRanges = 1;
    rp[5].DescriptorTable.pDescriptorRanges = &rgU[0];
    rp[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[6].DescriptorTable.NumDescriptorRanges = 1;
    rp[6].DescriptorTable.pDescriptorRanges = &rgU[1];
    rp[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[7].Constants.ShaderRegister = 0;
    rp[7].Constants.Num32BitValues = 20;
    rp[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 8; rd.pParameters = rp;
    rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        Log("BFORM-B: 合成根签名失败"); return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_bRs)))) return false;

    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kBformResid, strlen(kBformResid), "bres.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
        Log("BFORM-B: 合成 shader 编译失败: %s", e2 ? (char*)e2->GetBufferPointer() : "?"); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_bRs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_bPso)))) { Log("BFORM-B: 合成 PSO 失败"); return false; }

    {   // ★ F8：三横带诊断 PSO（同一个根签名，只换 shader；编译失败只是没有诊断视图）
        ComPtr<ID3DBlob> ct, et;
        if (SUCCEEDED(D3DCompile(kBformTri, strlen(kBformTri), "btri.hlsl", nullptr, nullptr, "main", "cs_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ct, &et))) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC td3{};
            td3.pRootSignature = g_bRs.Get();
            td3.CS.pShaderBytecode = ct->GetBufferPointer();
            td3.CS.BytecodeLength = ct->GetBufferSize();
            if (FAILED(dev->CreateComputePipelineState(&td3, IID_PPV_ARGS(&g_bTriPso)))) g_bTriPso.Reset();
        } else {
            Log("BFORM-B: 三横带 shader 编译失败: %s", et ? (char*)et->GetBufferPointer() : "?");
        }
    }

    {   // ★ F7：跑马灯 PSO（同一个根签名，只换 shader）
        ComPtr<ID3DBlob> cm, em;
        if (SUCCEEDED(D3DCompile(kBformMark, strlen(kBformMark), "bmark.hlsl", nullptr, nullptr, "main", "cs_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cm, &em))) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC md3{};
            md3.pRootSignature = g_bRs.Get();
            md3.CS.pShaderBytecode = cm->GetBufferPointer();
            md3.CS.BytecodeLength = cm->GetBufferSize();
            if (FAILED(dev->CreateComputePipelineState(&md3, IID_PPV_ARGS(&g_bMarkPso)))) g_bMarkPso.Reset();
        } else {
            Log("BFORM-B: 跑马灯 shader 编译失败: %s", em ? (char*)em->GetBufferPointer() : "?");
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 11;  // separate descriptor locations for the two compose targets
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_bHeap)))) { Log("BFORM-B: 合成堆失败"); return false; }
    g_bInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto mkuav = [&](UINT w, UINT h, DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& r, const char* nm) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = fmt; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)))) {
            Log("BFORM-B: 合成 %s 纹理失败", nm); return false;
        }
        return true;
    };
    // 合成输出固定 8bit：底图可能是 10bit（pOutput 原生），但合成/出画这条链保持 8bit
    //   （交换链也因此保持 8bit，避开"10bit swapchain + DComp + 8bit 桌面"的闪烁问题）
    ComPtr<ID3D12Resource> outputs[2], state;
    for (auto& output : outputs)
        if (!mkuav(g_upW, g_upH, DXGI_FORMAT_B8G8R8A8_UNORM, output, "输出")) return false;
    if (!mkuav(kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, state, "状态")) return false;
    for (int i = 0; i < 2; ++i) {
        g_bComp[i].Swap(outputs[i]);
        g_bCompStamp[i] = {};
    }
    g_bState.Swap(state);

    D3D12_CPU_DESCRIPTOR_HANDLE c = g_bHeap->GetCPUDescriptorHandleForHeapStart();
    auto at = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)i * g_bInc; return h; };
    { D3D12_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_B8G8R8A8_UNORM; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      for (int i = 0; i < 2; ++i) dev->CreateUnorderedAccessView(g_bComp[i].Get(), nullptr, &ud, at(9 + i)); }
    { D3D12_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      dev->CreateUnorderedAccessView(g_bState.Get(), nullptr, &ud, at(7)); }

    g_bCompReady = true;
    Log("BFORM-B: 残差合成就绪 | 输出 %ux%u | 状态 %ux%u", g_upW, g_upH, kNetW, kNetH);
    return true;
}

// 在 B 的列表上：合成 pColor + 残差 → g_bComp
static bool BformBCompose(ID3D12GraphicsCommandList* cl, ID3D12Resource* ansTex, int slot) {
    if (!g_bCompReady || !ansTex || !g_bTexC || !g_bTexM || !g_bDs1080) return false;
    BtTrace("COMPOSE_RECORD", g_tlFrame, "compose",
            "iter=%ld dst=0x%llx input=0x%llx list=0x%llx slot=%d",
            g_tlIter, BtPtr(g_bComp[slot].Get()), BtPtr(ansTex), BtPtr(cl), slot);
    {
        UINT64 comp = (g_bpFe) ? g_bpFe->GetCompletedValue() : 0;
        int valid = (g_bpFe != nullptr) ? 1 : 0;
        BtTrace("COMPOSE_BEGIN_SAMPLE", g_tlFrame, "compose",
                "iter=%ld present_fence=0x%08llX side=w present_value=%llu completed=%llu valid=%d",
                g_tlIter, BtPtr(g_bpFe ? g_bpFe.Get() : nullptr),
                (unsigned long long)g_bCompStamp[slot].lastPresentFv, (unsigned long long)comp, valid);
    }
    {   // 【诊断·一次性】答案纹理描述 vs 我们建 SRV 时写死的格式（这里两个都在作用域里）
        static bool l1 = false;
        if (!l1) {
            l1 = true;
            D3D12_RESOURCE_DESC ad = ansTex->GetDesc();
            Log("DIAG-PO2: 答案 fmt=%d %ux%u array=%u mip=%u | 我们的SRV=R8G8B8A8_UNORM | 合成底色 fmt=%d %ux%u",
                (int)ad.Format, (UINT)ad.Width, (UINT)ad.Height, ad.DepthOrArraySize, ad.MipLevels,
                (int)g_upFmtC, g_upW, g_upH);
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_bHeap->GetCPUDescriptorHandleForHeapStart();
    auto cpu = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)i * g_bInc; return h; };
    auto mksrv = [&](ID3D12Resource* r, DXGI_FORMAT f, int i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g_worker.dev->CreateShaderResourceView(r, &sd, cpu(i));
    };
    mksrv(g_bTexC.Get(), g_upFmtC, 0);
    mksrv(g_bTexC.Get(), g_upFmtC, 1);          // 占位
    mksrv(g_bTexM.Get(), g_upFmtM, 2);
    mksrv(g_bDs1080.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 3);
    mksrv(ansTex, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
    mksrv(g_bTexC.Get(), g_upFmtC, 5);          // 占位

    Barrier(cl, g_bTexC.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bTexM.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, ansTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bComp[slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cl, g_bState.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* hs[] = { g_bHeap.Get() };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(g_bRs.Get());
    cl->SetPipelineState(g_bPso.Get());
    auto gpu = [&](int i) { D3D12_GPU_DESCRIPTOR_HANDLE h = g_bHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * g_bInc; return h; };
    cl->SetComputeRootDescriptorTable(0, gpu(0));
    cl->SetComputeRootDescriptorTable(1, gpu(1));
    cl->SetComputeRootDescriptorTable(2, gpu(2));
    cl->SetComputeRootDescriptorTable(3, gpu(3));
    cl->SetComputeRootDescriptorTable(4, gpu(4));
    cl->SetComputeRootDescriptorTable(5, gpu(9 + slot));
    cl->SetComputeRootDescriptorTable(6, gpu(7));
    // 保持 A/B 常量布局一致；后处理常量中性且不使用历史状态。
    float k[20] = { 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f,
                    MvScaleUv(), 0, 0, 0, 0, 0,
                    (float)g_diag5Mode, (g_diag5Mode == 2) ? 0.2f : 0.0f,
                    (g_diag5Mode == 4) ? 1.0f : 0.0f, (g_diag5Mode == 4) ? 12.0f : 0.0f, 0, 0 };
    cl->SetComputeRoot32BitConstants(7, 20, k, 0);
    cl->Dispatch((g_upW + 7) / 8, (g_upH + 7) / 8, 1);

    Barrier(cl, g_bComp[slot].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bState.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, ansTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bTexM.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bTexC.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    DumpFlush(2);   // 先把上一帧的合成结果落盘
    if (g_dumpMask & 4) DumpRecord(cl, g_worker.dev.Get(), 2, g_bComp[slot].Get(), D3D12_RESOURCE_STATE_COMMON);
    if ((g_bCompCnt++ % 300) == 0) Log("BFORM-B: 已合成第 %u 次（%ux%u）", g_bCompCnt, g_upW, g_upH);
    return true;
}

// ★ 2026-10-31 F8 照搬到 B 形态：三横带诊断（写进 g_bComp，出画链路完全不变）
//   用途：肉眼判"答案 / 残差 / 底图"三者是不是同一帧 —— 底图带跑马灯时，
//   若答案或残差来自别的相位，三条带里的黑条位置会不一致（帧错配定位方法文档同法）。
static bool BformBTri(ID3D12GraphicsCommandList* cl, ID3D12Resource* ansTex, int slot) {
    if (!g_bCompReady || !g_bTriPso || !ansTex || !g_bTexC || !g_bDs1080 || !g_bTexM) return false;
    BtTrace("COMPOSE_RECORD", g_tlFrame, "compose",
            "iter=%ld dst=0x%llx input=0x%llx list=0x%llx slot=%d f8=1",
            g_tlIter, BtPtr(g_bComp[slot].Get()), BtPtr(ansTex), BtPtr(cl), slot);
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_bHeap->GetCPUDescriptorHandleForHeapStart();
    auto cpu = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)i * g_bInc; return h; };
    auto gpu = [&](int i) { D3D12_GPU_DESCRIPTOR_HANDLE h = g_bHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * g_bInc; return h; };
    auto mksrv = [&](ID3D12Resource* r, DXGI_FORMAT f, int i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g_worker.dev->CreateShaderResourceView(r, &sd, cpu(i));
    };
    mksrv(g_bTexC.Get(), g_upFmtC, 0);
    mksrv(g_bTexC.Get(), g_upFmtC, 1);
    mksrv(g_bTexM.Get(), g_upFmtM, 2);
    mksrv(g_bDs1080.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 3);
    mksrv(ansTex, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
    mksrv(g_bTexC.Get(), g_upFmtC, 5);

    Barrier(cl, g_bTexC.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, ansTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bComp[slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* hs[] = { g_bHeap.Get() };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(g_bRs.Get());
    cl->SetPipelineState(g_bTriPso.Get());
    for (int i = 0; i < 5; ++i) cl->SetComputeRootDescriptorTable(i, gpu(i));
    cl->SetComputeRootDescriptorTable(5, gpu(9 + slot));
    cl->SetComputeRootDescriptorTable(6, gpu(7));
    float k[20] = { 0 };
    cl->SetComputeRoot32BitConstants(7, 20, k, 0);
    cl->Dispatch((g_upW + 7) / 8, (g_upH + 7) / 8, 1);

    Barrier(cl, g_bComp[slot].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, ansTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bTexC.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    static UINT tc = 0;
    if ((tc++ % 300) == 0) Log("BFORM-B: 三横带诊断 %u 次（%ux%u）", tc, g_upW, g_upH);
    return true;
}

// ★ 2026-10-31 F7：跑马灯通用画法（同一套 shader 两种模式）
//   overlay = false：画在底图 g_bTexC 上（白底+黑条+数字）⇒ 网络喂入与合成底图都带图案
//   overlay = true ：画在合成结果 g_bComp 上（只叠条与数字，条外不动）⇒ 屏幕上一定有一条会动的参考条
//   判读：屏幕上的参考条 = "当前帧"；若底图受残差影响出现另一条更淡的条，就是帧错配。
static bool BformBMarkInto(ID3D12GraphicsCommandList* cl, ID3D12Resource* target,
                           int frameIdx, bool overlay, int slot = 0) {
    if (!cl || !target || !g_bMarkPso || !g_bCompReady || !g_bHeap) return false;
    D3D12_RESOURCE_STATES stUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Both the base and the selected compose output enter/leave in COMMON.
    const D3D12_RESOURCE_STATES stBack = D3D12_RESOURCE_STATE_COMMON;

    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = overlay ? DXGI_FORMAT_B8G8R8A8_UNORM : g_upFmtC;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    const int markUav = overlay ? (9 + slot) : 8;
    if (!overlay) {
        D3D12_CPU_DESCRIPTOR_HANDLE ch = g_bHeap->GetCPUDescriptorHandleForHeapStart();
        ch.ptr += (SIZE_T)markUav * g_bInc;
        g_worker.dev->CreateUnorderedAccessView(target, nullptr, &ud, ch);
    }

    Barrier(cl, target, stBack, stUav);
    ID3D12DescriptorHeap* hs[] = { g_bHeap.Get() };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(g_bRs.Get());
    cl->SetPipelineState(g_bMarkPso.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE gp = g_bHeap->GetGPUDescriptorHandleForHeapStart();
    gp.ptr += (SIZE_T)markUav * g_bInc;
    cl->SetComputeRootDescriptorTable(5, gp);           // u0 = 目标
    // ★ 相位/数字必须用【每画一次 +1 的计数器】，不能用面单帧号 g_slotTag[..].frame：
    //   面单在"颜色拷贝成功后才发布"，实测按 4 的倍数在跳 ⇒
    //   数字（mod 8）会变、黑条（mod 3→4）不动（2026-10-31 实测现象，根因就在这里）。
    static UINT mgSeq = 0;
    const int seq = (int)(mgSeq++);
    float k[20] = { 0 };
    k[0]  = (float)((seq & 7) + 1);                     // 数字 1~8，每次画 +1
    k[18] = 0.25f;                                      // 条宽 25%
    k[19] = (float)(seq & 3) * 0.25f;                   // 相位 0/.25/.5/.75，每次画右移一格
    k[17] = overlay ? 1.0f : 0.0f;                      // 叠加模式标志
    cl->SetComputeRoot32BitConstants(7, 20, k, 0);
    cl->Dispatch((g_upW + 7) / 8, (g_upH + 7) / 8, 1);
    UavBarrier(cl, target);
    Barrier(cl, target, stUav, stBack);
    static UINT mc = 0;
    static UINT mqDbg = 0;
    if (mqDbg < 64) {   // ★ 证据用：前 64 次逐帧打印，确认相位序列与面单帧号的关系（纯日志）
        Log("MARQ%u: 画序=%d 相位=%d 数字=%d | 面单帧=%d", mqDbg, seq, seq & 3, (seq & 7) + 1, frameIdx);
        ++mqDbg;
    } else if ((mc++ % 120) == 0) {
        Log("BFORM-B: 跑马灯%s已画 %u 次（帧 %d 相位 %d）", overlay ? "(叠加)" : "(底图)", mc, frameIdx, frameIdx & 3);
    }
    return true;
}
static bool BformBMarkSlot(ID3D12GraphicsCommandList* cl, int frameIdx) {
    g_bMarkFrame = frameIdx;
    return BformBMarkInto(cl, g_bTexC.Get(), frameIdx, false);
}
static bool BformBOverlay(ID3D12GraphicsCommandList* cl, int frameIdx, int slot) {
    return BformBMarkInto(cl, g_bComp[slot].Get(), frameIdx, true, slot);
}

// ---- B 形态 步骤5a：B 侧上采样（先用 Catmull-Rom 空间放大，打通链路再说画质）----
//   1440p 合成结果 → 4K。等链路通了再换真 XeSS on B / FSR4（那有"一进程两实例"的不确定性）
static const char* kBformUp = R"(
Texture2D<float4>   gS : register(t0);
SamplerState        gM : register(s0);
RWTexture2D<float4> gD : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gD.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    uint sw, sh; gS.GetDimensions(sw, sh);
    float2 tc = uv * float2(sw, sh) - 0.5;
    float2 f  = frac(tc);
    int2   i  = int2(floor(tc));
    // Catmull-Rom 的 12 抽头（4x4 邻域里的 3x3 有效块）
    float4 acc = float4(0,0,0,0);
    float  wsum = 0.0;
    [unroll] for (int yy = -1; yy <= 2; ++yy) {
        [unroll] for (int xx = -1; xx <= 2; ++xx) {
            int2 q = clamp(i + int2(xx, yy), int2(0,0), int2((int)sw - 1, (int)sh - 1));
            // ★ 抽头与权重必须一一对应：w(-1)↔xx=-1, w(0)↔xx=0, w(1)↔xx=1, w(2)↔xx=2
            //   曾经整体右移一格（xx=0 用 w(-1)），等价于把成品整体偏移 +1 源像素并糊掉
            float wx = (xx == -1) ? (-0.5f*f.x*f.x*f.x + f.x*f.x - 0.5f*f.x)
                     : (xx == 0) ? ( 1.5f*f.x*f.x*f.x - 2.5f*f.x*f.x + 1.0f)
                     : (xx == 1) ? (-1.5f*f.x*f.x*f.x + 2.0f*f.x*f.x + 0.5f*f.x)
                                 : ( 0.5f*f.x*f.x*f.x - 0.5f*f.x*f.x);
            float wy = (yy == -1) ? (-0.5f*f.y*f.y*f.y + f.y*f.y - 0.5f*f.y)
                     : (yy == 0) ? ( 1.5f*f.y*f.y*f.y - 2.5f*f.y*f.y + 1.0f)
                     : (yy == 1) ? (-1.5f*f.y*f.y*f.y + 2.0f*f.y*f.y + 0.5f*f.y)
                                 : ( 0.5f*f.y*f.y*f.y - 0.5f*f.y*f.y);
            float wgt = wx * wy;
            acc += gS.Load(int3(q, 0)) * wgt;
            wsum += wgt;
        }
    }
    gD[id.xy] = acc / max(wsum, 1e-5);
}
)";
static ComPtr<ID3D12RootSignature> g_bUpRs;
static ComPtr<ID3D12PipelineState> g_bUpPso;
static UINT g_bUpCnt = 0;

static bool BformBEnsureUp(ID3D12Device* dev) {
    if (g_bUpReady) return true;
    if (!dev || !g_outW || !g_outH) return false;
    if (!BformBAuxEnsure(dev)) return false;
    g_bUpRs = g_bAuxRs;                 // 与 DS 共用"描述符表 + 静态采样器"的根签名
    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kBformUp, strlen(kBformUp), "bup.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
        Log("BFORM-B: UP 编译失败: %s", e2 ? (char*)e2->GetBufferPointer() : "?"); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_bUpRs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_bUpPso)))) { Log("BFORM-B: UP PSO 失败"); return false; }

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = g_outW; td.Height = g_outH;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = g_upFmtC; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_bUp4k)))) {
        Log("BFORM-B: UP 纹理失败"); return false;
    }
    g_bUpReady = true;
    Log("BFORM-B: 上采样就绪 | %ux%u -> %ux%u", g_upW, g_upH, g_outW, g_outH);
    return true;
}

// ===== 诊断：五源同块对照（原生 pColor / B 合成 / 喂入 1080p / 答案 1080p / B 成品 4K）=====
//   目的：一次重启就能回答四个问题 ——
//     ① 答案和喂入差多少（网络到底改了多少）
//     ② B 的合成结果和原生差多少（残差有没有叠上）
//     ③ B 的成品和原生差多少（最终画面上还差多少）
//     ④ 三者的均值是否量级一致（排除格式/通道错位）
static const UINT kDiagBlk = 16;                 // 每块 16×16 像素
static const UINT kDiagPitch = 256;              // 行距必须 256 对齐
static const UINT kDiagBlkBytes = 16 * 256;      // 一块占的字节
static ComPtr<ID3D12Resource> g_diagRb;
static ComPtr<ID3D12CommandQueue> g_diagQ;
static ComPtr<ID3D12CommandAllocator> g_diagAl;
static ComPtr<ID3D12GraphicsCommandList> g_diagCl;
static ComPtr<ID3D12Fence> g_diagFe;
static UINT64 g_diagFv = 0;
static bool g_diagReady = false;

static bool BformDiagEnsure(ID3D12Device* dev) {
    if (g_diagReady) return true;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 6 * kDiagBlkBytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_diagRb)))) return false;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_diagQ)))) return false;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&g_diagAl)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_diagAl.Get(), nullptr, IID_PPV_ARGS(&g_diagCl)))) return false;
    g_diagCl->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_diagFe)))) return false;
    g_diagReady = true;
    Log("DIAG-B: 就绪");
    return true;
}

static double BlkMean(const unsigned char* p, UINT pitch);   // 前沿声明（BlkStd 要用）
static double BlkStd(const unsigned char* p, UINT pitch) {
    double m = BlkMean(p, pitch), s = 0.0;
    for (UINT y = 0; y < kDiagBlk; ++y)
        for (UINT x = 0; x < kDiagBlk; ++x) {
            const unsigned char* q = p + (size_t)y * pitch + (size_t)x * 4;
            double v = (q[0] + q[1] + q[2]) / 3.0;
            s += (v - m) * (v - m);
        }
    return sqrt(s / (double)(kDiagBlk * kDiagBlk));
}
static double BlkMean(const unsigned char* p, UINT pitch) {
    double s = 0.0;
    for (UINT y = 0; y < kDiagBlk; ++y)
        for (UINT x = 0; x < kDiagBlk; ++x) {
            const unsigned char* q = p + (size_t)y * pitch + (size_t)x * 4;
            s += (q[0] + q[1] + q[2]) / 3.0;
        }
    return s / (double)(kDiagBlk * kDiagBlk);
}

// 两块同尺寸区域的"平均绝对差"（逐通道平均后取均值，单位 = 8bit 灰度级）
static double BlkDiff(const unsigned char* a, UINT pa, const unsigned char* b, UINT pb) {
    double s = 0.0;
    for (UINT y = 0; y < kDiagBlk; ++y)
        for (UINT x = 0; x < kDiagBlk; ++x) {
            const unsigned char* qa = a + (size_t)y * pa + (size_t)x * 4;
            const unsigned char* qb = b + (size_t)y * pb + (size_t)x * 4;
            s += (fabs((double)qa[0] - qb[0]) + fabs((double)qa[1] - qb[1]) + fabs((double)qa[2] - qb[2])) / 3.0;
        }
    return s / (double)(kDiagBlk * kDiagBlk);
}

// 把 4 块 16×16 拷进回读缓冲（挂在 worker 自己的 list 上，WaitW 之后才读）
static bool g_diagPending = false;
static void BformDiagEnqueue(Worker& W, ID3D12Resource* ans, int slot, bool upWritten) {
    static int tick = 0;
    if (!g_bformOn) return;
    const int m = tick++ % 150;
    if (m != 0 && m != 1) return;   // 每 150 帧【连续采两帧】：用于"答案延迟"测量（第 2 帧比第 1 帧的喂入）
    if (!g_bCompReady || !g_bUpReady || !g_bTexC || !g_bComp[slot] || !g_bUp4k || !g_bDs1080) {
        Log("DIAG4: 资源未就绪，跳过（comp=%d up=%d）", (int)g_bCompReady, (int)g_bUpReady);
        return;
    }
    if (!BformDiagEnsure(W.dev.Get()) || !g_diagRb) { Log("DIAG4: 回读缓冲创建失败"); return; }
    ID3D12GraphicsCommandList* cl = W.cl.Get();
    auto blk = [&](ID3D12Resource* tex, DXGI_FORMAT fmt, UINT sx, UINT sy, UINT64 off, D3D12_RESOURCE_STATES st) {
        if (!tex) return;
        Barrier(cl, tex, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = g_diagRb.Get();
        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        d.PlacedFootprint.Offset = off;
        d.PlacedFootprint.Footprint.Format = fmt;
        d.PlacedFootprint.Footprint.Width = kDiagBlk;
        d.PlacedFootprint.Footprint.Height = kDiagBlk;
        d.PlacedFootprint.Footprint.Depth = 1;
        d.PlacedFootprint.Footprint.RowPitch = kDiagPitch;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = tex; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
        D3D12_BOX box{ sx, sy, 0, sx + kDiagBlk, sy + kDiagBlk, 1 };
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
        Barrier(cl, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
    };
    const UINT nxs = (g_upW  > kDiagBlk + 2) ? (g_upW  / 2 - kDiagBlk / 2) : 0;
    const UINT nys = (g_upH  > kDiagBlk + 2) ? (g_upH  / 2 - kDiagBlk / 2) : 0;
    const UINT sxs = (kNetW  > kDiagBlk + 2) ? (kNetW  / 2 - kDiagBlk / 2) : 0;
    const UINT sys = (kNetH  > kDiagBlk + 2) ? (kNetH  / 2 - kDiagBlk / 2) : 0;
    const UINT oxs = (g_outW > kDiagBlk + 2) ? (g_outW / 2 - kDiagBlk / 2) : 0;
    const UINT oys = (g_outH > kDiagBlk + 2) ? (g_outH / 2 - kDiagBlk / 2) : 0;
    blk(g_bTexC.Get(),   g_upFmtC, nxs, nys, 0 * kDiagBlkBytes, D3D12_RESOURCE_STATE_COMMON);
    blk(g_bComp[slot].Get(), DXGI_FORMAT_B8G8R8A8_UNORM, nxs, nys, 1 * kDiagBlkBytes, D3D12_RESOURCE_STATE_COMMON);
    blk(g_bDs1080.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, sxs, sys, 2 * kDiagBlkBytes, D3D12_RESOURCE_STATE_COMMON);
    // ★ 2026-10-31：4K 后置模式不走上采样，g_bUp4k 停在 COMMON ——
    //   这里若按 NON_PIXEL_SHADER_RESOURCE 起跳就是非法转换（会掉设备），所以 4K 下跳过这一块。
    if (!g_bform4k && upWritten)
        blk(g_bUp4k.Get(), g_upFmtC, oxs, oys, 3 * kDiagBlkBytes,
            g_backendNgx ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // ★ 第 5 块直接取【本帧的答案纹理 po】：不再用 CPU 回读的那份（那份现在晚一轮）
    if (ans) blk(ans, DXGI_FORMAT_R8G8B8A8_UNORM, sxs, sys, 4 * kDiagBlkBytes, D3D12_RESOURCE_STATE_COMMON);
    // ★ 第 6 块：TAA 的输出。用来判断 TAA 有没有真的在混历史（输出≈输入 = 空转）
    if (!g_backendNgx && g_taaOn && g_taaValid && g_taaOut)
        blk(g_taaOut.Get(), g_upFmtC, oxs, oys, 5 * kDiagBlkBytes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_diagPending = true;
}

static void BformDiagReadback(Worker& W) {
    if (!g_diagPending || !g_diagRb) return;
    g_diagPending = false;
    D3D12_RANGE rr{ 0, 6 * kDiagBlkBytes };
    void* m = nullptr;
    if (FAILED(g_diagRb->Map(0, &rr, &m)) || !m) { Log("DIAG4: 回读 Map 失败"); return; }
    const unsigned char* pN = (const unsigned char*)m + 0 * kDiagBlkBytes;
    const unsigned char* pC = (const unsigned char*)m + 1 * kDiagBlkBytes;
    const unsigned char* pF = (const unsigned char*)m + 2 * kDiagBlkBytes;
    const unsigned char* pU = (const unsigned char*)m + 3 * kDiagBlkBytes;
    const unsigned char* pA = (const unsigned char*)m + 4 * kDiagBlkBytes;   // 本帧答案（GPU 侧直接取）
    const unsigned char* pT = (const unsigned char*)m + 5 * kDiagBlkBytes;   // TAA 输出（仅 bform=8 有效）

    double mN = BlkMean(pN, kDiagPitch);
    double mC = BlkMean(pC, kDiagPitch);
    double mF = BlkMean(pF, kDiagPitch);
    double mU = BlkMean(pU, kDiagPitch);
    double mA = BlkMean(pA, kDiagPitch);
    double dAF = BlkDiff(pA, kDiagPitch, pF, kDiagPitch);     // 网络改了多少（同帧，无错配）
    double dCN = BlkDiff(pC, kDiagPitch, pN, kDiagPitch);     // B 合成 vs 原生
    double dUN = BlkDiff(pU, kDiagPitch, pN, kDiagPitch);     // B 成品 vs 原生
    // TAA 到底动了多少：输出 vs 它的输入（Catmull-Rom 结果）
    //   ≈0  → TAA 在空转（历史被门限/钳制全丢了，或初始化标志一直是"无历史"）
    //   有量 → TAA 真在混合历史，那么观感没改善就是"混合没帮上忙"（该考虑换 FSR4）
    double dTU = (g_taaOn && g_taaValid) ? BlkDiff(pT, kDiagPitch, pU, kDiagPitch) : -1.0;
    // 【降噪指标】块内标准差 σ：TAA 若真在累积，输出 σ 应明显低于输入 σ（噪点被平均掉）
    double sU = (g_taaOn && g_taaValid) ? BlkStd(pU, kDiagPitch) : -1.0;   // 输入（Catmull-Rom 结果）
    double sT = (g_taaOn && g_taaValid) ? BlkStd(pT, kDiagPitch) : -1.0;   // TAA 输出
    double sC = BlkStd(pC, kDiagPitch);                                    // B 合成结果（参考）
    double mT  = (g_taaOn && g_taaValid) ? BlkMean(pT, kDiagPitch) : -1.0;

    // 【答案延迟测量】DIAG4 每 150 帧连续采两次，第二次用第一次的"喂入块"跟本帧答案比：
    //   若 |答案 − 上轮喂入| 明显小于 |答案 − 本轮喂入| ⇒ 答案属于上一帧（滞后一帧）
    //   若两者接近            ⇒ 答案就是本帧的（没有滞后）
    //   这块采样点在画面中心，两次采样是相邻两帧，所以"上轮喂入"就是前一帧同一位置。
    static unsigned char prevFed[kDiagBlk * kDiagPitch];
    static bool  havePrevFed = false;
    static bool  diagSecond = false;
    double dAFprev = -1.0;
    if (diagSecond && havePrevFed) dAFprev = BlkDiff(pA, kDiagPitch, prevFed, kDiagPitch);
    memcpy(prevFed, pF, sizeof(prevFed));
    havePrevFed = true;
    diagSecond = !diagSecond;

    D3D12_RANGE nw{ 0, 0 };
    g_diagRb->Unmap(0, &nw);
    g_diagCnt++;
    Log("DIAG4[%d] gframe=%d stage=%d: 均值 原生=%.1f 喂入=%.1f 答案=%.1f 合成=%.1f 成品=%.1f TAA=%.1f | |答案-喂入|=%.2f |答案-上轮喂入|=%.2f |合成-原生|=%.2f |成品-原生|=%.2f |TAA变化|=%.2f |σ: 合成=%.2f 上采样=%.2f TAA=%.2f",
        W.processed, g_frame, g_bformStage, mN, mF, mA, mC, mU, mT, dAF, dAFprev, dCN, dUN, dTU, sC, sU, sT);
}
static bool BformBUpscale(ID3D12GraphicsCommandList* cl, int slot) {
    if (!g_bUpReady || !g_bComp[slot]) return false;
    Barrier(cl, g_bComp[slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bUp4k.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetComputeRootSignature(g_bUpRs.Get());
    cl->SetPipelineState(g_bUpPso.Get());
    {   // 描述符表：2=源 SRV，3=目标 UAV（与 DS 共用同一个堆）
        auto mkS = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)i * g_bAuxInc;
            g_worker.dev->CreateShaderResourceView(r, &sd, h);
        };
        auto mkU = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)i * g_bAuxInc;
            g_worker.dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
        };
        mkS(g_bComp[slot].Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 2);
        mkU(g_bUp4k.Get(), g_upFmtC, 3);
        ID3D12DescriptorHeap* hs[] = { g_bAuxHeap.Get() };
        cl->SetDescriptorHeaps(1, hs);
        D3D12_GPU_DESCRIPTOR_HANDLE g = g_bAuxHeap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += (SIZE_T)2 * g_bAuxInc;
        cl->SetComputeRootDescriptorTable(0, g);
        D3D12_GPU_DESCRIPTOR_HANDLE g1 = g; g1.ptr += g_bAuxInc;
        cl->SetComputeRootDescriptorTable(1, g1);
    }
    cl->Dispatch((g_outW + 7) / 8, (g_outH + 7) / 8, 1);
    Barrier(cl, g_bUp4k.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bComp[slot].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (g_backendNgx)
        Barrier(cl, g_bUp4k.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if ((g_bUpCnt++ % 300) == 0) Log("BFORM-B: 已上采样第 %u 次（%ux%u）", g_bUpCnt, g_outW, g_outH);
    return true;
}

// ---- B 形态 步骤5b：TAA-lite 时域上采样（治噪点 / 锯齿）----
//   输入：上采样后的 4K 当前帧 + 上一帧的 4K 输出（历史）+ 运动矢量（1080p）
//   做法：MV 重投影 → 邻域钳制（防拖影）→ 与历史混合（alpha 越小越稳）
//   输出：4K → 出画；同时写进另一张历史，下一帧两张对调
//   注：本版【不做 jitter 补偿】（先看时域累积本身的效果），jitter 走共享 buffer 头部是下一步
static const char* kBformTAA = R"(
Texture2D<float4>   gCur  : register(t0);
Texture2D<float4>   gHist : register(t1);
Texture2D<float4>   gMV   : register(t2);
SamplerState        gSmp  : register(s0);
RWTexture2D<float4> gDst  : register(u0);
RWTexture2D<float4> gHO   : register(u1);
cbuffer P : register(b0)
{
    float gAlpha;     // 混合系数：越小越稳（越容易拖影）
    float gClampK;    // 邻域钳制放宽系数：越小越防拖影
    float gMvScale;   // MV 像素→uv 缩放（沿用残差那套）
    float gInit;      // 1 = 历史无效，直接输出当前帧
};
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float3 c = gCur.Load(int3((int)id.x, (int)id.y, 0)).rgb;
    float3 outc = c;

    if (gInit < 0.5) {
        // 邻域钳制：当前帧 3×3 的 min/max，历史必须落在里面（抑制拖影）
        float3 mn = c, mx = c;
        for (int j = -2; j <= 2; ++j) {      // 5×5 邻域（3×3 时钳制窗口太窄，TAA 等于空转）
            for (int i = -2; i <= 2; ++i) {
                int2 q = clamp(int2(id.xy) + int2(i, j), int2(0, 0), int2((int)w - 1, (int)h - 1));
                float3 sm = gCur.Load(int3(q, 0)).rgb;
                mn = min(mn, sm); mx = max(mx, sm);
            }
        }
        float3 ext = (mx - mn) * 0.5 * gClampK;
        mn -= ext; mx += ext;
        // MV 重投影：把历史对齐到当前像素
        float2 mv = gMV.SampleLevel(gSmp, uv, 0).rg * gMvScale;
        float2 puv = saturate(uv - mv);
        float3 hcol = gHist.SampleLevel(gSmp, puv, 0).rgb;
        hcol = clamp(hcol, mn, mx);
        outc = lerp(c, hcol, gAlpha);
    }
    gDst[id.xy] = float4(outc, 1.0);
    gHO[id.xy]  = float4(outc, 1.0);
}
)";
static ComPtr<ID3D12Resource> g_taaHist[2];
static ComPtr<ID3D12RootSignature> g_taaRs;
static ComPtr<ID3D12PipelineState> g_taaPso;
static ComPtr<ID3D12DescriptorHeap> g_taaHeap;
static UINT g_taaInc = 0;
static int  g_taaIdx = 0;

// ★ 待清理：lmxxf 后端分支。2077 线默认后端为 NGX（xess_proxy.backend.txt=ngx），本段默认不会执行，属无用代码，后续统一清理；在此之前不要再改这里的逻辑。
static bool BformTaaEnsure(ID3D12Device* dev) {
    if (g_taaReady) return true;
    if (!dev || !g_outW || !g_outH) return false;
    const DXGI_FORMAT fmt = g_upFmtC;

    g_taaHist[0] = MakeTex(dev, g_outW, g_outH, fmt, D3D12_RESOURCE_STATE_COMMON, true);
    g_taaHist[1] = MakeTex(dev, g_outW, g_outH, fmt, D3D12_RESOURCE_STATE_COMMON, true);
    g_taaOut     = MakeTex(dev, g_outW, g_outH, fmt, D3D12_RESOURCE_STATE_COMMON, true);
    if (!g_taaHist[0] || !g_taaHist[1] || !g_taaOut) { Log("BFORM-TAA: 建纹理失败"); return false; }

    // 根签名：t0/t1/t2 三张 SRV 表 + u0/u1 两张 UAV 表 + 4 个 32 位常量（b0）
    D3D12_DESCRIPTOR_RANGE rgS[3]{}, rgU[2]{};
    for (int i = 0; i < 3; ++i) {
        rgS[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rgS[i].NumDescriptors = 1; rgS[i].BaseShaderRegister = (UINT)i;
    }
    for (int i = 0; i < 2; ++i) {
        rgU[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        rgU[i].NumDescriptors = 1; rgU[i].BaseShaderRegister = (UINT)i;
    }
    D3D12_ROOT_PARAMETER rp[6]{};
    for (int i = 0; i < 3; ++i) {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[i].DescriptorTable.NumDescriptorRanges = 1;
        rp[i].DescriptorTable.pDescriptorRanges = &rgS[i];
    }
    for (int i = 0; i < 2; ++i) {
        rp[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[3 + i].DescriptorTable.NumDescriptorRanges = 1;
        rp[3 + i].DescriptorTable.pDescriptorRanges = &rgU[i];
    }
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[5].Constants.Num32BitValues = 4;
    rp[5].Constants.ShaderRegister = 0;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 6; rd.pParameters = rp;
    rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        Log("BFORM-TAA: 根签名失败: %s", err ? (char*)err->GetBufferPointer() : "?"); return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&g_taaRs)))) { Log("BFORM-TAA: CreateRootSignature 失败"); return false; }

    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kBformTAA, strlen(kBformTAA), "btaa.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
        Log("BFORM-TAA: 编译失败: %s", e2 ? (char*)e2->GetBufferPointer() : "?"); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_taaRs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_taaPso)))) { Log("BFORM-TAA: PSO 失败"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 5;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_taaHeap)))) return false;
    g_taaInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    g_taaReady = true;
    Log("BFORM-TAA: TAA-lite 就绪 | %ux%u fmt=%d | alpha=%.2f clampK=%.2f（本版未做 jitter 补偿）",
        g_outW, g_outH, (int)fmt, (float)g_taaAlpha, (float)g_taaClampK);
    return true;
}

// 出画源：TAA 生效时用 TAA 输出，否则用原始上采样结果
static ID3D12Resource* BformPresentSrc(int slot, bool direct4k) {
    if (direct4k) return g_bCompReady ? g_bComp[slot].Get() : nullptr;
    if (!g_backendNgx && g_taaOn && g_taaReady && g_taaValid) return g_taaOut.Get();
    return g_bUp4k.Get();
}

// ★ 待清理：lmxxf 后端分支。2077 线默认后端为 NGX（xess_proxy.backend.txt=ngx），本段默认不会执行，属无用代码，后续统一清理；在此之前不要再改这里的逻辑。
static bool BformTaaRun(ID3D12GraphicsCommandList* cl) {
    if (!g_taaReady || !cl || !g_bUp4k || !g_bTexM) return false;
    ID3D12Device* dev = g_worker.dev.Get();
    if (!dev) return false;
    const int prev = g_taaIdx, next = 1 - g_taaIdx;

    auto cpu = [&](int i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_taaHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * g_taaInc; return h;
    };
    auto mksrv = [&](ID3D12Resource* r, DXGI_FORMAT f, int i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(r, &sd, cpu(i));
    };
    auto mkuav = [&](ID3D12Resource* r, DXGI_FORMAT f, int i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(r, nullptr, &ud, cpu(i));
    };
    mksrv(g_bUp4k.Get(), g_upFmtC, 0);             // 当前帧
    mksrv(g_taaHist[prev].Get(), g_upFmtC, 1);     // 历史
    mksrv(g_bTexM.Get(), g_upFmtM, 2);             // 运动矢量
    mkuav(g_taaOut.Get(), g_upFmtC, 3);            // 输出
    mkuav(g_taaHist[next].Get(), g_upFmtC, 4);     // 下一帧历史

    Barrier(cl, g_taaHist[prev].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bTexM.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_taaOut.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cl, g_taaHist[next].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* hs[] = { g_taaHeap.Get() };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(g_taaRs.Get());
    cl->SetPipelineState(g_taaPso.Get());
    auto gpu = [&](int i) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = g_taaHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * g_taaInc; return h;
    };
    for (int i = 0; i < 5; ++i) cl->SetComputeRootDescriptorTable(i, gpu(i));
    float k[4] = { (float)g_taaAlpha, (float)g_taaClampK, MvScaleUv(), g_taaValid ? 0.0f : 1.0f };
    cl->SetComputeRoot32BitConstants(5, 4, k, 0);
    cl->Dispatch((g_outW + 7) / 8, (g_outH + 7) / 8, 1);

    Barrier(cl, g_taaOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_taaHist[next].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_taaHist[prev].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bTexM.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    g_taaIdx = next;
    g_taaValid = true;
    static UINT cnt = 0;
    if ((cnt++ % 300) == 0) Log("BFORM-TAA: 已时域累积 %u 次（%ux%u）", cnt, g_outW, g_outH);
    return true;
}

// ---- B 形态 步骤6：B 侧出画（合成交换链 + DComp 视觉贴到游戏窗口上）----
//   数据源 = g_bUp4k（B 自己上采样出来的 4K）
//   关键：只在 worker 的 GPU 工作【完全落地之后】才拷贝+Present，
//         避免"跨队列读写同一资源"（S1 第一次闪退就是栽在这）。
static bool g_bpOn = false;
static ComPtr<IDCompositionDevice> g_bpDcomp;
static ComPtr<IDCompositionTarget> g_bpTarget;
static ComPtr<IDCompositionVisual> g_bpVisual;
static ComPtr<IDXGISwapChain3>     g_bpSc;
static ComPtr<ID3D12CommandQueue>  g_bpQ;
static ComPtr<ID3D12CommandAllocator> g_bpAl, g_bpAl2;
static int g_bpAi = 0;
static UINT64 g_bpAlFv[2] = {};
static ComPtr<ID3D12GraphicsCommandList> g_bpCl, g_bpCl2;
static UINT   g_bpCnt = 0;
static DXGI_FORMAT g_bpFmt = DXGI_FORMAT_UNKNOWN;
static ComPtr<IDCompositionScaleTransform> g_bpScale;   // 覆盖层缩放（跟随窗口客户区）
static UINT g_bpCw = 0, g_bpCh = 0;                     // 建立缩放时用的客户区尺寸
// 出画节奏统计（判断"卡卡的"是不是因为出画间隔不匀）
static double g_bpT0 = 0;
static double g_bpSumMs = 0, g_bpMinMs = 1e9, g_bpMaxMs = 0;
static int    g_bpShort = 0, g_bpLong = 0;   // <8ms（一刷新内出两帧）/ >25ms（漏帧）

// 覆盖层可见性跟随 stage：stage>=6 显示 B 的输出；切回 A（stage<6）就把视觉内容清空。
// 不这么做的话，切回 A 之后 DComp 会留一张冻结的 B 画面盖在游戏窗口上，看不到 A。
// ★ v4 P17：函数签名不变，内部改成 desired/applied 状态机（原 static cur 缓存并入
//   g_overlayDesired / g_overlayApplied）。好处：Commit 失败时 applied 不更新，
//   下一次调用自动重试（设计 §11 B8：旧的"初始化/Commit 失败不重试"被解掉）。
static void BformPresentShow(bool show) {
    InterlockedExchange(&g_overlayDesired, show ? 1 : 0);
    ApplyOverlay();
}

static bool BformPresentInit() {
    if (g_bpReady) return true;
    if (g_bpFailed) return false;
    if (!g_worker.dev || !g_bUp4k) { return false; }
    // 自己去找窗口（以前这里是"直接放弃 → 只能靠用户按 Del 开菜单顺带取到句柄"）
    if (!ResolveGameHwnd()) { static int n = 0; if ((n++ % 300) == 0) Log("BFORM-P: 还没定位到游戏主窗口，稍后重试（无需开菜单）"); return false; }

    // 用 B 的适配器
    LUID luid = g_worker.dev->GetAdapterLuid();
    ComPtr<IDXGIFactory4> fac4;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac4)))) { Log("BFORM-P: 工厂失败"); g_bpFailed = true; return false; }
    ComPtr<IDXGIAdapter1> ad;
    if (FAILED(fac4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad))) || !ad) {
        Log("BFORM-P: 按 LUID 找不到 B 适配器"); g_bpFailed = true; return false;
    }

    // 自己的 DIRECT 队列（Present 必须在建链的那条队列上）
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_worker.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_bpQ)))) { Log("BFORM-P: 建队列失败"); g_bpFailed = true; return false; }
    if (FAILED(g_worker.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_bpAl)))) { g_bpFailed = true; return false; }
    if (FAILED(g_worker.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_bpAl2)))) { g_bpFailed = true; return false; }
    if (FAILED(g_worker.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bpAl.Get(), nullptr, IID_PPV_ARGS(&g_bpCl)))) { g_bpFailed = true; return false; }
    if (FAILED(g_bpCl->Close()) ||
        FAILED(g_worker.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bpAl2.Get(), nullptr, IID_PPV_ARGS(&g_bpCl2))) ||
        FAILED(g_bpCl2->Close())) { g_bpFailed = true; return false; }
    if (FAILED(g_worker.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_bpFe)))) { g_bpFailed = true; return false; }

    // 合成交换链：格式与 g_bUp4k 一致，才能直接 CopyResource
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_outW; sd.Height = g_outH;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // 固定 8bit（与合成输出一致；底图 10bit 也不影响这里）
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = fac4->CreateSwapChainForComposition(g_bpQ.Get(), &sd, nullptr, &sc1);
    if (FAILED(hr) || !sc1) { Log("BFORM-P: 合成交换链失败 0x%08X（B 可能没有输出）", (unsigned)hr); g_bpFailed = true; return false; }
    if (FAILED(sc1.As(&g_bpSc)) || !g_bpSc) { Log("BFORM-P: QI 失败"); g_bpFailed = true; return false; }
    g_bpSc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    g_bpFmt = g_upFmtC;

    // DComp 设备（D3D12 设备不能当 IDXGIDevice，失败则用同卡 D3D11 设备）
    hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&g_bpDcomp));
    if (FAILED(hr) || !g_bpDcomp) {
        ComPtr<ID3D11Device> d11; D3D_FEATURE_LEVEL fl{};
        HRESULT h2 = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                       nullptr, 0, D3D11_SDK_VERSION, &d11, &fl, nullptr);
        ComPtr<IDXGIDevice> dx;
        if (FAILED(h2) || !d11 || FAILED(d11.As(&dx)) || !dx) { Log("BFORM-P: D3D11 设备失败"); g_bpFailed = true; return false; }
        hr = DCompositionCreateDevice(dx.Get(), IID_PPV_ARGS(&g_bpDcomp));
        if (FAILED(hr) || !g_bpDcomp) { Log("BFORM-P: DComp 失败 0x%08X", (unsigned)hr); g_bpFailed = true; return false; }
    }
    hr = g_bpDcomp->CreateTargetForHwnd(g_gameHwnd, TRUE, &g_bpTarget);
    if (FAILED(hr) || !g_bpTarget) { Log("BFORM-P: CreateTargetForHwnd 失败 0x%08X", (unsigned)hr); g_bpFailed = true; return false; }
    hr = g_bpDcomp->CreateVisual(&g_bpVisual);
    if (FAILED(hr) || !g_bpVisual) { Log("BFORM-P: CreateVisual 失败 0x%08X", (unsigned)hr); g_bpFailed = true; return false; }
    // ★ step1：Init 只建对象，不把交换链挂上视觉。覆盖层显示只许 ApplyOverlay/Show 路径完成。
    //   账本保持 applied=0、desired=0，与"内容未挂上"一致（不要在这里把 applied 写成 1）。
    g_bpVisual->SetContent(nullptr);
    {
        // 覆盖层缩放 = 客户区 / B 出画尺寸。
        // ★ 现在可能在"进游戏之前"就初始化（不再等菜单），窗口之后还可能改分辨率/窗口模式
        //   → 缩放要能跟着改，见 BformPresentRetarget()
        RECT cr{}; GetClientRect(g_gameHwnd, &cr);
        UINT cw = (UINT)(cr.right - cr.left), chh = (UINT)(cr.bottom - cr.top);
        g_bpCw = cw; g_bpCh = chh;
        if (SUCCEEDED(g_bpDcomp->CreateScaleTransform(&g_bpScale)) && g_bpScale) {
            g_bpScale->SetScaleX(cw ? (float)cw / (float)g_outW : 1.0f);
            g_bpScale->SetScaleY(chh ? (float)chh / (float)g_outH : 1.0f);
            g_bpVisual->SetTransform(g_bpScale.Get());
        }
    }
    hr = g_bpTarget->SetRoot(g_bpVisual.Get());
    if (FAILED(hr)) { Log("BFORM-P: SetRoot 失败 0x%08X", (unsigned)hr); g_bpFailed = true; return false; }
    // ★ step1：删掉这里单独的 Commit，以及下面那帧没清屏的假黑帧 Present。
    //   内容还没挂上，Commit/Present 都不该在 Init 里做。显示只走 ApplyOverlay。

    g_bpReady = true;
    Log("BFORM-P: 出画就绪 | %ux%u fmt=%d（B 用自己的交换链贴到游戏窗口）", g_outW, g_outH, (int)g_bpFmt);
    return true;
}

// 窗口客户区变了（改分辨率 / 切窗口模式 / 早点初始化时窗口还没定尺寸）→ 跟上缩放
static void BformPresentRetarget() {
    if (!g_bpReady || !g_bpScale || !g_gameHwnd || !g_bpDcomp) return;
    RECT cr{}; if (!GetClientRect(g_gameHwnd, &cr)) return;
    UINT cw = (UINT)(cr.right - cr.left), chh = (UINT)(cr.bottom - cr.top);
    if (!cw || !chh || (cw == g_bpCw && chh == g_bpCh)) return;
    g_bpCw = cw; g_bpCh = chh;
    const float sx = (float)cw / (float)g_outW, sy = (float)chh / (float)g_outH;
    g_bpScale->SetScaleX(sx);
    g_bpScale->SetScaleY(sy);
    // ★ step1 返工：Commit 放进 g_ovLock。调用者（BformPresentFrame）不持有该锁，不会自锁。
    //   只加锁，不改其他逻辑，不在这里更新 g_overlayApplied。
    AcquireSRWLockExclusive(&g_ovLock);
    g_bpDcomp->Commit();
    ReleaseSRWLockExclusive(&g_ovLock);
    Log("BFORM-P: 窗口客户区变为 %ux%u → 覆盖层缩放 %.3fx%.3f", cw, chh, sx, sy);
}

// 只在 worker 的 GPU 工作完全落地之后调用
static bool BformPresentFrame(int slot, UINT64 composeFv, bool keepalive) {
    if (!g_bpReady || g_bpSyncBroken || !composeFv) return false;
    BformPresentRetarget();
    const bool direct4k = g_bform4k;
    if (g_backendNgx) {
        const auto& stamp = g_bCompStamp[slot];
        if (!UiGateAllowsNr() || !g_bformOn || stamp.uiEpoch != BformUiEpoch() ||
            stamp.modeEpoch != InterlockedCompareExchange(&g_r28Epoch, 0, 0) || stamp.direct4k != direct4k ||
            composeFv != stamp.composeFv || (!direct4k && composeFv != g_bUpComposeFv)) return false;
    }
    ID3D12Resource* src = BformPresentSrc(slot, direct4k);
    if (!src) return false;
    UINT bi = g_bpSc->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> bb;
    if (FAILED(g_bpSc->GetBuffer(bi, IID_PPV_ARGS(&bb))) || !bb) return false;
    const UINT64 done = g_bpFe->GetCompletedValue();
    if (done == UINT64_MAX) { NrHardFail("B1: present fence device removed"); return false; }
    int batch = g_bpAi ^ 1;
    if (g_bpAlFv[batch] > done) batch ^= 1;
    if (g_bpAlFv[batch] > done) return false;  // both lists in flight; no CPU wait or slot bookkeeping
    ID3D12CommandAllocator* al = batch ? g_bpAl2.Get() : g_bpAl.Get();
    ID3D12GraphicsCommandList* cl = batch ? g_bpCl2.Get() : g_bpCl.Get();
    if (FAILED(al->Reset()) || FAILED(cl->Reset(al, nullptr))) return false;
    const D3D12_RESOURCE_STATES srcState = (direct4k || g_backendNgx)
        ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = src;
    b.Transition.StateBefore = srcState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    b.Transition.pResource = bb.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cl->ResourceBarrier(1, &b);
    cl->CopyResource(bb.Get(), src);
    b.Transition.pResource = src;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = srcState;
    cl->ResourceBarrier(1, &b);
    b.Transition.pResource = bb.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cl->ResourceBarrier(1, &b);
    if (FAILED(cl->Close())) return false;
    // ★ GPU 侧同步（不占 CPU）：出画队列必须等主队列把上采样算完，否则可能拷到写了一半或上一帧的画面。
    //   以前这里完全没有同步，靠"能跑"。
    const UINT64 wcomp = g_worker.fe->GetCompletedValue();
    const HRESULT waitHr = wcomp == UINT64_MAX ? DXGI_ERROR_DEVICE_REMOVED : g_bpQ->Wait(g_worker.fe.Get(), composeFv);
    BtTrace("PRESENT_WAIT", g_tlFrame, "present",
            "iter=%ld src=0x%llx wait_fence=0x%llx side=w wait_value=%llu wait_hr=0x%08lX sample=%llu sample_valid=%d slot=%d keepalive=%d",
            g_tlIter, BtPtr(src), BtPtr(g_worker.fe.Get()), (unsigned long long)composeFv,
            (unsigned long)waitHr, (unsigned long long)wcomp, wcomp != UINT64_MAX ? 1 : 0, slot, keepalive ? 1 : 0);
    if (FAILED(waitHr)) {
        if (HrIsDeviceLost(waitHr)) NrHardFail("B1: present worker wait failed (device lost)");
        return false;
    }
    ID3D12CommandList* l[] = { cl };
    BtTrace("PRESENT_EXECUTE", g_tlFrame, "present",
            "iter=%ld src=0x%llx queue=0x%llx list=0x%llx present_fence=0x%llx side=w",
            g_tlIter, BtPtr(src), BtPtr(g_bpQ.Get()), BtPtr(cl), BtPtr(g_bpFe.Get()));
    g_bpQ->ExecuteCommandLists(1, l);
    const UINT64 presentFv = g_bpFv + 1;
    const HRESULT sigHr = g_bpQ->Signal(g_bpFe.Get(), presentFv);
    BtTrace("PRESENT_SIGNAL", g_tlFrame, "present",
            "iter=%ld value=%llu hr=0x%08lX present_fence=0x%llx side=w",
            g_tlIter, (unsigned long long)presentFv, (unsigned long)sigHr, BtPtr(g_bpFe.Get()));
    if (FAILED(sigHr)) {
        // Execute already submitted a read. Without a known fence its source must never be reused.
        g_bpSyncBroken = true;
        NrHardFail("B1: present copy submitted without a fence");
        return false;
    }
    g_bpFv = presentFv;
    g_bpAi = batch;
    g_bpAlFv[batch] = presentFv;
    if (direct4k) g_bCompStamp[slot].lastPresentFv = presentFv;
    else if (g_backendNgx) g_bUpLastPresentFv = presentFv;
    HRESULT hrPres = g_bpSc->Present(0, 0);
    BtTrace("PRESENT_RETURN", g_tlFrame, "present", "iter=%ld hr=0x%08lX", g_tlIter, (unsigned long)hrPres);
    // ★ v4 P20（设计 §8.2）：Present 的设备级失败 → 统一走 NrHardFail（切 OFF + 清 C4 + 卸覆盖层）。
    //   本函数是【我们自己那条 B 出画交换链】的 Present：失败即说明出画链已断。
    //   但单次失败可能只是瞬时（窗口被遮/最小化），所以：设备失联立即硬错；普通失败连续 30 次才硬错。
    static LONG presFailRun = 0;
    if (FAILED(hrPres)) {
        const LONG run = InterlockedIncrement(&presFailRun);
        if (HrIsDeviceLost(hrPres) || run >= 30) {
            InterlockedExchange(&presFailRun, 0);
            NrHardFail("Present failed (B)");
            return false;
        }
        if (run == 1 || (run % 10) == 0)
            LogError("B present 失败 hr=0x%08X（连续 %ld 次；连续 30 次才判硬错）", (unsigned)hrPres, (long)run);
    } else {
        InterlockedExchange(&presFailRun, 0);            // 成功：清零连续失败计数
        // ★ P0① 修复（v4fix1）：2077 用 pColor 底图（g_baseFromPresent=0），游戏 Present 钩子
        //   未安装，OnPresentSuccess 靠 Detour_Present 永远不会被调用。
        //   在 B 自己的出画链 Present 实际成功处补调。
        // ★ v4fix2：改调 OnBPresentSuccess —— 该入口带"B 出画身份"语义（只有 BformBDecode 发布的
        //   有效新 epoch 新帧才会推进预热计数并驱动单一显示闸门），而不是"任何 Present 成功都算"。
        if (hrPres == S_OK && !keepalive) OnBPresentSuccess();
    }

    // 出画节奏统计：间隔不匀 = 肉眼"卡卡的"
    {
        static LARGE_INTEGER fq{};
        if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double t = (double)now.QuadPart * 1000.0 / (double)fq.QuadPart;
        if (g_bpT0 > 0) {
            double dt = t - g_bpT0;
            g_bpSumMs += dt;
            if (dt < g_bpMinMs) g_bpMinMs = dt;
            if (dt > g_bpMaxMs) g_bpMaxMs = dt;
            if (dt < 8.0)  ++g_bpShort;
            if (dt > 25.0) ++g_bpLong;
        }
        g_bpT0 = t;
    }
    if ((g_bpCnt++ % 300) == 0) {
        int n = 300;
        if (g_bpSumMs > 0)
            Log("BFORM-P: 产出 %u 帧 | 间隔 均值%.2fms 最小%.2f 最大%.2f | 过短(<8ms) %d 次 过长(>25ms) %d 次（每300帧）",
                g_bpCnt, g_bpSumMs / n, g_bpMinMs, g_bpMaxMs, g_bpShort, g_bpLong);
        g_bpSumMs = 0; g_bpMinMs = 1e9; g_bpMaxMs = 0; g_bpShort = 0; g_bpLong = 0;
    }
    return hrPres == S_OK;
}

// =====================================================================================
// 【B 形态 S7：两级流水】把"解码+降采样"与"合成+上采样+出画"拆到两条队列、两个缓冲
//   Stage1（worker 的 compute 队列）：解码 + 降采样 + NR        （处理帧 N）
//   Stage2（g_bpQ，DIRECT 队列）    ：合成 + 上采样 + 出画      （处理帧 N-1）
//   两段跑在不同缓冲组上，所以 Stage2(N-1) 不会被 Stage1(N) 覆盖。
//   目标：每帧吞吐从 19.3ms（串行）压到 max(NR+解码, 合成+上采样) ≈ 15ms
// =====================================================================================
static const int PP = 2;
static ComPtr<ID3D12Resource> g_ppTexC[PP], g_ppTexM[PP], g_ppDs[PP];
static ComPtr<ID3D12Resource> g_ppComp[PP], g_ppUp4k[PP];
static UINT g_ppCnt = 0;

// ★ 待清理：lmxxf 后端分支。2077 线默认后端为 NGX（xess_proxy.backend.txt=ngx），本段默认不会执行，属无用代码，后续统一清理；在此之前不要再改这里的逻辑。
static bool BformPPEnsure(ID3D12Device* dev) {
    if (g_ppReady) return true;
    if (!dev || !g_upW || !g_outW) return false;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto mk = [&](UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl,
                  ComPtr<ID3D12Resource>& r, const char* nm) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = f; td.SampleDesc.Count = 1; td.Flags = fl;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
        if (FAILED(hr)) { Log("BFORM-PP: 建 %s 失败 0x%08X", nm, (unsigned)hr); return false; }
        return true;
    };
    for (int i = 0; i < PP; ++i) {
        if (!mk(g_upW, g_upH, g_upFmtC, D3D12_RESOURCE_FLAG_NONE, g_ppTexC[i], "color")) return false;
        if (g_upMw && !mk(g_upMw, g_upMh, g_upFmtM, D3D12_RESOURCE_FLAG_NONE, g_ppTexM[i], "mv")) return false;
        if (!mk(kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, g_ppAns[i], "ans")) return false;
        if (!mk(kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, g_ppDs[i], "ds")) return false;
        if (!mk(g_upW, g_upH, g_upFmtC, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, g_ppComp[i], "comp")) return false;
        if (!mk(g_outW, g_outH, g_upFmtC, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, g_ppUp4k[i], "up4k")) return false;
    }
    g_ppReady = true;
    Log("BFORM-PP: 双缓冲就绪（%d 组）| 色 %ux%u | 合成 %ux%u | 成品 %ux%u",
        PP, g_upW, g_upH, g_upW, g_upH, g_outW, g_outH);
    return true;
}

// Stage1 记在 worker 的 compute 列表上：解码 + 降采样（目标本组缓冲）
// ★ 待清理：lmxxf 后端分支。2077 线默认后端为 NGX（xess_proxy.backend.txt=ngx），本段默认不会执行，属无用代码，后续统一清理；在此之前不要再改这里的逻辑。
static void BformPPStage1(ID3D12GraphicsCommandList* cl, int i, ID3D12Resource* nrColorDst,
                          ID3D12Resource* ansSnapDst, ID3D12Resource* po) {
    if (!g_ppReady) return;
    const UINT64 base = (UINT64)(g_frame % K) * g_upSlotBytes;
    Barrier(cl, g_upBufW.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto dec = [&](ID3D12Resource* tex, DXGI_FORMAT fmt, UINT w, UINT h, UINT pitch, UINT64 off) {
        if (!tex || !w || !h) return;
        Barrier(cl, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = tex; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = g_upBufW.Get(); s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Offset = base + off;
        s.PlacedFootprint.Footprint.Format = fmt;
        s.PlacedFootprint.Footprint.Width = w;
        s.PlacedFootprint.Footprint.Height = h;
        s.PlacedFootprint.Footprint.Depth = 1;
        s.PlacedFootprint.Footprint.RowPitch = pitch;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    };
    dec(g_ppTexC[i].Get(), g_upFmtC, g_upW, g_upH, g_upPitchC, g_upOffC);
    dec(g_ppTexM[i].Get(), g_upFmtM, g_upMw, g_upMh, g_upPitchM, g_upOffM);
    Barrier(cl, g_upBufW.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

    // 降采样 → NR 输入
    if (g_bDsReady && nrColorDst) {
        Barrier(cl, g_ppTexC[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto mkS = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT k) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)k * g_bAuxInc;
            g_worker.dev->CreateShaderResourceView(r, &sd, h);
        };
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        { D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
          h.ptr += (SIZE_T)1 * g_bAuxInc;
          g_worker.dev->CreateUnorderedAccessView(g_ppDs[i].Get(), nullptr, &ud, h); }
        mkS(g_ppTexC[i].Get(), g_upFmtC, 0);
        ID3D12DescriptorHeap* hs[] = { g_bAuxHeap.Get() };
        cl->SetDescriptorHeaps(1, hs);
        cl->SetComputeRootSignature(g_bAuxRs.Get());
        cl->SetPipelineState(g_bDsPso.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE g = g_bAuxHeap->GetGPUDescriptorHandleForHeapStart();
        cl->SetComputeRootDescriptorTable(0, g);
        D3D12_GPU_DESCRIPTOR_HANDLE g1 = g; g1.ptr += g_bAuxInc;
        cl->SetComputeRootDescriptorTable(1, g1);
        cl->Dispatch((kNetW + 7) / 8, (kNetH + 7) / 8, 1);
        Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyResource(nrColorDst, g_bDs1080.Get());
        Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
}

// Stage2：记在 g_bpQ（DIRECT 队列）上 —— 合成 + 上采样 + 出画，全部读【上一组】缓冲
//   与 Stage1(N) 并行运行；读的资源 Stage1 不会碰（不同组）
// ★ 待清理：lmxxf 后端分支。2077 线默认后端为 NGX（xess_proxy.backend.txt=ngx），本段默认不会执行，属无用代码，后续统一清理；在此之前不要再改这里的逻辑。
static bool BformPPStage2(int j) {
    if (!g_ppReady || !g_bpReady || g_bpSyncBroken || !g_ppComp[j] || !g_ppUp4k[j]) return false;
    const UINT64 done = g_bpFe->GetCompletedValue();
    if (done == UINT64_MAX || done < g_bpAlFv[0]) return false;
    if (FAILED(g_bpAl->Reset())) return false;
    if (FAILED(g_bpCl->Reset(g_bpAl.Get(), nullptr))) return false;
    ID3D12GraphicsCommandList* cl = g_bpCl.Get();
    auto dev = g_worker.dev.Get();

    // ---- 合成：ppTexC[j] + ppTexM[j] + ppDs[j] + ppAns[j] → ppComp[j] ----
    if (g_bCompReady) {
        auto cpuSrv = [&](ID3D12Resource* r, DXGI_FORMAT f, int k) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)k * g_bInc;
            dev->CreateShaderResourceView(r, &sd, h);
        };
        auto cpuUav = [&](ID3D12Resource* r, DXGI_FORMAT f, int k) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)k * g_bInc;
            dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
        };
        cpuSrv(g_ppTexC[j].Get(), g_upFmtC, 0);
        cpuSrv(g_ppTexC[j].Get(), g_upFmtC, 1);
        if (g_ppTexM[j]) cpuSrv(g_ppTexM[j].Get(), g_upFmtM, 2);
        if (g_ppDs[j])   cpuSrv(g_ppDs[j].Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 3);
        if (g_ppAns[j])  cpuSrv(g_ppAns[j].Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 4);
        cpuSrv(g_ppTexC[j].Get(), g_upFmtC, 5);
        cpuUav(g_ppComp[j].Get(), g_upFmtC, 6);
        cpuUav(g_bState.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 7);

        Barrier(cl, g_ppTexC[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (g_ppTexM[j]) Barrier(cl, g_ppTexM[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (g_ppDs[j])   Barrier(cl, g_ppDs[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (g_ppAns[j])  Barrier(cl, g_ppAns[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_ppComp[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cl, g_bState.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12DescriptorHeap* hs[] = { g_bHeap.Get() };
        cl->SetDescriptorHeaps(1, hs);
        cl->SetComputeRootSignature(g_bRs.Get());
        cl->SetPipelineState(g_bPso.Get());
        auto gpu = [&](int k) { D3D12_GPU_DESCRIPTOR_HANDLE h = g_bHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)k * g_bInc; return h; };
        for (int k = 0; k < 5; ++k) cl->SetComputeRootDescriptorTable(k, gpu(k));
        cl->SetComputeRootDescriptorTable(5, gpu(6));
        cl->SetComputeRootDescriptorTable(6, gpu(7));
        // 不使用历史状态。
        float kk[20] = { 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f,
                         MvScaleUv(), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        cl->SetComputeRoot32BitConstants(7, 20, kk, 0);
        cl->Dispatch((g_upW + 7) / 8, (g_upH + 7) / 8, 1);

        Barrier(cl, g_ppComp[j].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_bState.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, g_ppTexC[j].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (g_ppTexM[j]) Barrier(cl, g_ppTexM[j].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (g_ppDs[j])   Barrier(cl, g_ppDs[j].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (g_ppAns[j])  Barrier(cl, g_ppAns[j].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    // ---- 上采样：ppComp[j] → ppUp4k[j] ----
    if (g_bUpReady) {
        auto mkS = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT k) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)k * g_bAuxInc;
            dev->CreateShaderResourceView(r, &sd, h);
        };
        auto mkU = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT k) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)k * g_bAuxInc;
            dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
        };
        mkS(g_ppComp[j].Get(), g_upFmtC, 2);
        mkU(g_ppUp4k[j].Get(), g_upFmtC, 3);
        Barrier(cl, g_ppComp[j].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_ppUp4k[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* hs[] = { g_bAuxHeap.Get() };
        cl->SetDescriptorHeaps(1, hs);
        cl->SetComputeRootSignature(g_bAuxRs.Get());
        cl->SetPipelineState(g_bUpPso.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE g = g_bAuxHeap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += (SIZE_T)2 * g_bAuxInc;
        cl->SetComputeRootDescriptorTable(0, g);
        D3D12_GPU_DESCRIPTOR_HANDLE g1 = g; g1.ptr += g_bAuxInc;
        cl->SetComputeRootDescriptorTable(1, g1);
        cl->Dispatch((g_outW + 7) / 8, (g_outH + 7) / 8, 1);
        Barrier(cl, g_ppUp4k[j].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    // ---- 出画：ppUp4k[j] → 后缓冲 → Present ----
    {
        UINT bi = g_bpSc->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> bb;
        if (FAILED(g_bpSc->GetBuffer(bi, IID_PPV_ARGS(&bb))) || !bb) { g_bpCl->Close(); return false; }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.pResource = bb.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cl->ResourceBarrier(1, &b);
        cl->CopyResource(bb.Get(), g_ppUp4k[j].Get());
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cl->ResourceBarrier(1, &b);
    }
    if (FAILED(g_bpCl->Close())) return false;
    ID3D12CommandList* l[] = { g_bpCl.Get() };
    g_bpQ->ExecuteCommandLists(1, l);
    const UINT64 presentFv = g_bpFv + 1;
    if (FAILED(g_bpQ->Signal(g_bpFe.Get(), presentFv))) {
        g_bpSyncBroken = true;
        NrHardFail("B1: PP present copy submitted without a fence");
        return false;
    }
    g_bpAlFv[0] = g_bpFv = presentFv;
    const HRESULT hrPresPP = g_bpSc->Present(0, 0);
    // ★ v4fix2：流水线（stage 7）出画走的是这里而不是 BformPresentFrame，同样要把"有效出画"登记给
    //   单一显示闸门，否则 STARTING 的预热计数没人推进（会靠 UiGateTick 的兜底才恢复，慢一截）。
    if (SUCCEEDED(hrPresPP)) OnBPresentSuccess();

    // 节奏统计
    {
        static LARGE_INTEGER fq{};
        if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double t = (double)now.QuadPart * 1000.0 / (double)fq.QuadPart;
        static double t0 = 0, sum = 0, mn = 1e9, mx = 0;
        static int sh = 0, lg = 0;
        if (t0 > 0) {
            double dt = t - t0;
            sum += dt; if (dt < mn) mn = dt; if (dt > mx) mx = dt;
            if (dt < 8.0) ++sh; if (dt > 25.0) ++lg;
        }
        t0 = t;
        if ((g_ppCnt++ % 300) == 0) {
            if (sum > 0) Log("BFORM-PP: 产出 %u 帧 | 间隔 均值%.2fms 最小%.2f 最大%.2f | 过短 %d 过长 %d",
                            g_ppCnt, sum / 300.0, mn, mx, sh, lg);
            sum = 0; mn = 1e9; mx = 0; sh = 0; lg = 0;
        }
    }
    return true;
}

// =====================================================================================
// Add4K 热重编译：源码放在 xess_proxy.add4k.hlsl，改完保存 → 约 1 秒内自动生效
// -------------------------------------------------------------------------------------
// · 首次运行若文件不存在，会把内置模板写出来（方便直接改）
// · 编译失败 → 保留旧版本，把编译器报错写进日志（不会崩、不会黑屏）
// · 换 PSO 时旧的那个延迟一代再释放，避免正在飞的命令还引用它
// =====================================================================================
static const char* kAdd4KTemplate = R"HLSL(
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
)HLSL";

static std::string g_add4kPath;
static std::string g_add4kSrc;
static INT64       g_add4kStamp = 0;
static ComPtr<ID3D12PipelineState> g_psoAdd4KPrev;

static bool ReadTextFile(const std::string& p, std::string& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 20)) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(&out[0], 1, (size_t)n, f);
    fclose(f);
    out.resize(got);
    return got > 0;
}
static bool WriteTextFile(const std::string& p, const std::string& s) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "wb") != 0 || !f) return false;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}
static INT64 FileStamp(const std::string& p) {
    struct _stat64 st{};
    if (_stat64(p.c_str(), &st) != 0) return 0;
    return (INT64)st.st_mtime * 1000000 + (INT64)st.st_size;
}
static bool CompileAdd4K(ID3D12Device* dev, const std::string& src,
                         ID3D12PipelineState** out, std::string& errOut) {
    ComPtr<ID3DBlob> cs, er;
    if (FAILED(D3DCompile(src.c_str(), src.size(), "add4k.hlsl", nullptr, nullptr, "Add4K",
                          "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &er))) {
        errOut = er ? (char*)er->GetBufferPointer() : "(无编译日志)";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(out)))) {
        errOut = "CreateComputePipelineState 失败";
        return false;
    }
    return true;
}
static bool InitAdd4K(ID3D12Device* dev) {
    g_add4kPath = ModuleDir() + "xess_proxy.add4k.hlsl";
    INT64 st = FileStamp(g_add4kPath);
    if (!st) {
        if (WriteTextFile(g_add4kPath, kAdd4KTemplate)) Log("热编译: 已生成模板 %s", g_add4kPath.c_str());
        st = FileStamp(g_add4kPath);
    }
    if (!ReadTextFile(g_add4kPath, g_add4kSrc)) g_add4kSrc = kAdd4KTemplate;
    g_add4kStamp = st;
    std::string err;
    if (!CompileAdd4K(dev, g_add4kSrc, g_psoAdd4K.GetAddressOf(), err)) {
        Log("热编译: 初版失败，退回内置模板 —— %s", err.c_str());
        g_add4kSrc = kAdd4KTemplate;
        if (!CompileAdd4K(dev, g_add4kSrc, g_psoAdd4K.GetAddressOf(), err)) {
            Log("热编译: 模板也编译失败 —— %s", err.c_str());
            return false;
        }
    }
    Log("热编译: Add4K 就绪（%s，%zu 字节）", g_add4kPath.c_str(), g_add4kSrc.size());
    return true;
}
// 每 60 帧查一次文件是否变了
static void PollAdd4KReload() {
    if (g_add4kPath.empty()) return;
    INT64 st = FileStamp(g_add4kPath);
    if (!st || st == g_add4kStamp) return;
    g_add4kStamp = st;
    std::string src;
    if (!ReadTextFile(g_add4kPath, src) || src.empty()) return;
    std::string err;
    ComPtr<ID3D12PipelineState> np;
    if (!CompileAdd4K(g_dev.Get(), src, np.GetAddressOf(), err)) {
        Log("热编译: 失败，保留旧版本 —— %s", err.c_str());
        return;
    }
    g_psoAdd4KPrev.Reset();          // 释放上上一代（已过去两代，肯定不在飞）
    g_psoAdd4KPrev = g_psoAdd4K;     // 旧的那代延后一代释放
    g_psoAdd4K = np;
    g_add4kSrc = src;
    Log("热编译: Add4K 已更新（%zu 字节）", src.size());
}

// ---------- D0-OBS (observability only): MV identity probes ----------
//   raw pointer values only: no AddRef, no ComPtr, no keep-alive of the game's MV resource.
static ID3D12Resource* g_initMv  = nullptr;   // first MV ever bound into kSrvMvCur
static ID3D12Resource* g_boundMv = nullptr;   // MV currently bound into kSrvMvCur (last mkSrv)
static int            g_buildGen = 0;         // BuildResources rebuild generation counter

static bool BuildResources(ID3D12Device* dev, ID3D12Resource* color,
                           ID3D12Resource* mv, ID3D12Resource* dep, ID3D12Resource* outp) {
    // 2026-11-14 c4outp3：堆重建时清零 UAV 就绪标志，强制重新写描述符
    for (int i = 0; i < 3; ++i) g_outp8Ready[i] = false;
    g_mvPackReady = false;
    
    D3D12_RESOURCE_DESC cd = color->GetDesc();
    D3D12_RESOURCE_DESC od = outp->GetDesc();
    g_cw = (UINT)cd.Width; g_ch = cd.Height; g_cfmt = cd.Format;
    g_outW = (UINT)od.Width; g_outH = od.Height;
    Log("pColor %ux%u fmt=%d | pOutput %ux%u fmt=%d | 槽位=%d 延迟=%d 帧",
        g_cw, g_ch, (int)g_cfmt, g_outW, g_outH, (int)od.Format, K, g_kDelay);

    for (int i = 0; i < K; ++i) {
        g_slots[i].color = MakeTex(dev, g_cw, g_ch, g_cfmt, D3D12_RESOURCE_STATE_COPY_DEST, true);   // ★true=带 UAV（跑马灯要往上画）
        if (mv)  g_slots[i].mv    = MakeTex(dev, (UINT)mv->GetDesc().Width, mv->GetDesc().Height, mv->GetDesc().Format, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (dep) g_slots[i].depth = MakeTex(dev, (UINT)dep->GetDesc().Width, dep->GetDesc().Height, dep->GetDesc().Format, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (!g_slots[i].color) { Log("槽位纹理失败"); return false; }
        g_slots[i].answer.assign((size_t)kNetW * kNetH * 4, 0);
        g_slots[i].feedInput.assign((size_t)kNetW * kNetH * 4, 0);
    }
    g_netS = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g_pre  = MakeTex(dev, g_cw, g_ch, g_cfmt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g_resTex = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, false);
    g_resSmooth = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    if (g_resSmooth) {
        // 初始化为中性 0.5（无残差）
        Log("残差时域状态纹理已建");
    }
    if (!g_netS || !g_pre || !g_resTex || !g_resSmooth) { Log("资源创建失败"); return false; }

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    auto foot = [&](ID3D12Resource* r, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
        D3D12_RESOURCE_DESC d = r->GetDesc();
        D3D12_RESOURCE_DESC pl = d; pl.Flags = D3D12_RESOURCE_FLAG_NONE;
        UINT rows = 0; UINT64 rs = 0, tot = 0;
        dev->GetCopyableFootprints(&pl, 0, 1, 0, &fp, &rows, &rs, &tot);
        return tot;
    };
    g_bytesRes = foot(g_resTex.Get(), g_fpRes);
    g_bytesRb  = foot(g_netS.Get(), g_fpRb);

    auto mkbuf = [&](UINT64 n, bool upload) {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = n; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(upload ? &uh : &rh, D3D12_HEAP_FLAG_NONE, &bd,
            upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(r.GetAddressOf()));
        return r;
    };
    g_resUp = mkbuf(g_bytesRes, true);
    g_rb    = mkbuf(g_bytesRb, false);
    {   // 诊断用回读缓冲（残差 / 时域状态 各一张 1080p）
        D3D12_RESOURCE_DESC dd = g_resSmooth->GetDesc();
        D3D12_RESOURCE_DESC pl = dd; pl.Flags = D3D12_RESOURCE_FLAG_NONE;
        UINT rws = 0; UINT64 rps = 0;
        dev->GetCopyableFootprints(&pl, 0, 1, 0, &g_dumpFp, &rws, &rps, &g_dumpBytes);
        g_dumpRb1 = mkbuf(g_dumpBytes, false);
        g_dumpRb2 = mkbuf(g_dumpBytes, false);
        Log("诊断回读缓冲: %llu 字节 行距 %u", (unsigned long long)g_dumpBytes, g_dumpFp.Footprint.RowPitch);
    }

    D3D12_DESCRIPTOR_RANGE r0{}, r1{}, r2{}, r3{}, r4{}, ru{};
    r0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r0.NumDescriptors = 1; r0.BaseShaderRegister = 0;
    r1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r1.NumDescriptors = 1; r1.BaseShaderRegister = 1;
    r2.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r2.NumDescriptors = 1; r2.BaseShaderRegister = 2;
    r3.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r3.NumDescriptors = 1; r3.BaseShaderRegister = 3;
    r4.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r4.NumDescriptors = 1; r4.BaseShaderRegister = 4;
    ru.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ru.NumDescriptors = 1; ru.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE ru1{};
    ru1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ru1.NumDescriptors = 1; ru1.BaseShaderRegister = 1;
    D3D12_ROOT_PARAMETER rp[8] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &r0;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &r1;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &r2;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[3].DescriptorTable.NumDescriptorRanges = 1; rp[3].DescriptorTable.pDescriptorRanges = &r3;
    rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[4].DescriptorTable.NumDescriptorRanges = 1; rp[4].DescriptorTable.pDescriptorRanges = &r4;
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[5].DescriptorTable.NumDescriptorRanges = 1; rp[5].DescriptorTable.pDescriptorRanges = &ru;
    rp[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[6].DescriptorTable.NumDescriptorRanges = 1; rp[6].DescriptorTable.pDescriptorRanges = &ru1;
    rp[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[7].Constants.ShaderRegister = 0; rp[7].Constants.RegisterSpace = 0;
    rp[7].Constants.Num32BitValues = 20;
    rp[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sm{};
    sm.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sm.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sm.MaxLOD = D3D12_FLOAT32_MAX;
    sm.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 8; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &sm;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { Log("根签名失败"); return false; }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(g_rs.GetAddressOf())))) return false;

    struct { const char* n; ID3D12PipelineState** p; } ps[] = {
        { "DownCS", &g_psoDown }, { "ResidCS", &g_psoResid }, { "MarkCS", &g_psoMark }
        , { "TriCS", &g_psoTri }
    };
    std::string fullHlsl = std::string(kPreprocessHLSL) + kHLSL;
    for (auto& e : ps) {
        ComPtr<ID3DBlob> cs, er;
        if (FAILED(D3DCompile(fullHlsl.data(), fullHlsl.size(), "v9.hlsl", nullptr, nullptr, e.n, "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &er))) {
            Log("shader %s 编译失败: %s", e.n, er ? (char*)er->GetBufferPointer() : "?");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs.Get();
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(e.p)))) { Log("PSO %s 失败", e.n); return false; }
    }
    {   // MV 打包 PSO（用 g_rs 的表 0=SRV / 表 5=UAV）
        ComPtr<ID3DBlob> cs, er;
        if (FAILED(D3DCompile(kMvPackHLSL, strlen(kMvPackHLSL), "mvpack.hlsl", nullptr, nullptr,
                              "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &er))) {
            Log("MV 打包 shader 编译失败: %s", er ? (char*)er->GetBufferPointer() : "?");
        } else {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = g_rs.Get();
            pd.CS.pShaderBytecode = cs->GetBufferPointer();
            pd.CS.BytecodeLength = cs->GetBufferSize();
            if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoMvPack)))) Log("MV 打包 PSO 失败");
        }
    }
    {   // 4K 后置：10bit→8bit 转换 PSO（同样用 g_rs：表 0=SRV，表 5=UAV）
        ComPtr<ID3DBlob> cs, er;
        if (FAILED(D3DCompile(kBformFmt8, strlen(kBformFmt8), "fmt8.hlsl", nullptr, nullptr,
                              "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &er))) {
            Log("4K 后置 shader 编译失败: %s", er ? (char*)er->GetBufferPointer() : "?");
        } else {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = g_rs.Get();
            pd.CS.pShaderBytecode = cs->GetBufferPointer();
            pd.CS.BytecodeLength = cs->GetBufferSize();
            if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoFmt8)))) Log("4K 后置 PSO 失败");
        }
    }
    if (!InitAdd4K(dev)) Log("!! Add4K 不可用（resid4k=1 时会跳过 4K 叠加）");
    if (!BwInit(dev)) Log("!! 带宽测试不可用（不影响主流程）");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvA2Mv + 1u;   // append A2 UAV; all legacy offsets unchanged
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(g_heap.GetAddressOf())))) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto mkSrv = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
        if (!r) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)slot * g_inc;
        dev->CreateShaderResourceView(r, &sd, h);
    };
    auto mkUav = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
        if (!r) return;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)slot * g_inc;
        dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
    };
    for (int i = 0; i < K; ++i) mkSrv(g_slots[i].color.Get(), g_cfmt, (UINT)i);
    {   // S5-2：每个槽位一份"被喂进去的 1080p 输入"
        int made = 0;
        for (int i = 0; i < K; ++i) {
            g_fedTex[i] = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  D3D12_RESOURCE_STATE_COPY_DEST, false);
            if (g_fedTex[i]) { mkSrv(g_fedTex[i].Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvFed + i); made++; }
        }
        Log("S5-2: fed 纹理 %d/%d 已建（SRV 槽 %d~%d）", made, K, kSrvFed, kSrvFed + K - 1);
    }
    for (int i = 0; i < K; ++i) if (g_slots[i].mv) mkSrv(g_slots[i].mv.Get(), g_slots[i].mv->GetDesc().Format, kSrvMvBase + (UINT)i);
    mkSrv(g_resTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvRes);
    mkUav(g_netS.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvNetS);
    mkUav(g_pre.Get(), g_cfmt, kSrvPre);
    mkUav(g_resSmooth.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvSmooth);
    g_cpuResSmooth = c; g_cpuResSmooth.ptr += (SIZE_T)kSrvSmooth * g_inc;
    {   // B1：4K 叠加用的三张视图（outp 的 SRV、g_pre 的 SRV、g_pre4k 的 UAV）
        g_pre4k = MakeTex(dev, g_outW, g_outH, od.Format, D3D12_RESOURCE_STATE_COMMON, true);
        if (g_pre4k) {
            mkSrv(outp, od.Format, kSrvOutp);
            mkSrv(g_pre.Get(), g_cfmt, kSrvPreSrv);
            mkUav(g_pre4k.Get(), od.Format, kSrvPre4K);
            Log("B1: 4K 叠加资源就绪 gout=%ux%u fmt=%d（槽 %d/%d/%d）",
                g_outW, g_outH, (int)od.Format, (int)kSrvOutp, (int)kSrvPreSrv, (int)kSrvPre4K);
        } else {
            Log("B1: g_pre4k 创建失败，4K 叠加不可用（会自动退回原路径）");
        }
    }
    {   // MV 打包：1440p(8B) → 1080p RG16F(4B)，传输量 29.5MB → 8.3MB
        g_mvPacked = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R16G16_FLOAT,
                             D3D12_RESOURCE_STATE_COMMON, true);
        if (g_mvPacked && mv) {
            mkSrv(mv, mv->GetDesc().Format, kSrvMvCur);
            if (!g_initMv)  g_initMv  = mv;   // D0-OBS: first MV bound into kSrvMvCur (raw value only)
            g_boundMv = mv;                   // D0-OBS: MV currently bound into kSrvMvCur
            {
                D3D12_RESOURCE_DESC mdI = mv->GetDesc();
                Log("D0-OBS MVINIT: mv=%p %ux%u fmt=%d srvSlot=%u buildGen=%d first=%d",
                    (const void*)mv, (unsigned)mdI.Width, mdI.Height, (int)mdI.Format,
                    (unsigned)kSrvMvCur, g_buildGen, (g_initMv == mv) ? 1 : 0);
            }
            mkUav(g_mvPacked.Get(), DXGI_FORMAT_R16G16_FLOAT, kSrvMvPacked);
            g_mvPackReady = true;
            Log("MV打包: 就绪（%dx%d RG16F），原始MV格式 %d", kNetW, kNetH, (int)mv->GetDesc().Format);
        } else {
            Log("MV打包: 不可用（继续用原始 MV 传输）");
        }
    }
    Log("游戏侧资源就绪");
    // 注意：B 形态的跨卡 buffer 不能在这里建 —— 此时 g_worker.dev 还没创建（WorkerInit 在后面），
    //       会空指针崩在 OpenSharedHandle 上（已踩）。改为首次需要时懒加载（见 ProxyExecute）。
    return true;
}

// 把答案编码成残差（answer − feedInput），0.5 中性
static void BuildResidual(const Slot& S, std::vector<uint8_t>& enc) {
    enc.resize((size_t)kNetW * kNetH * 4);
    if (g_bypass) { memcpy(enc.data(), S.answer.data(), enc.size()); return; }   // 直出：上传答案本身
    for (size_t i = 0; i < enc.size(); ++i) {
        int dv = (int)S.answer[i] - (int)S.feedInput[i];
        int v = dv + 128;
        if (v < 0) v = 0; if (v > 255) v = 255;
        enc[i] = (uint8_t)v;
    }
}

static void ProxyImGuiDraw(ID3D12GraphicsCommandList* cl, ID3D12Resource* rt, bool wmEligible = false,
                           D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_COMMON);
// ---------- IAT hook：菜单打开时让游戏"看不到"键盘 ----------
typedef SHORT (WINAPI* PfnGaks)(int);
typedef BOOL  (WINAPI* PfnGks)(PBYTE);
static PfnGaks g_origGaks = nullptr;
static PfnGks  g_origGks = nullptr;
typedef UINT (WINAPI* PfnGrid)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static PfnGrid g_origGrid = nullptr;
static SHORT WINAPI HookGaks(int vk) { if (g_menuOpen) return 0; return g_origGaks ? g_origGaks(vk) : 0; }
static UINT WINAPI HookGrid(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    if (g_menuOpen) { if (size) *size = 0; return (UINT)-1; }
    return g_origGrid ? g_origGrid(h, cmd, data, size, hdr) : (UINT)-1;
}
static BOOL WINAPI HookGks(PBYTE st) {
    BOOL r = g_origGks ? g_origGks(st) : FALSE;
    if (g_menuOpen && st) memset(st, 0, 256);
    return r;
}
static bool PatchIATIn(HMODULE base, const char* dllName, const char* fnName, void* hook, void** orig) {
    if (!base) return false;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dll = (const char*)((BYTE*)base + imp->Name);
        if (_stricmp(dll, dllName) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;
        auto thunk = (IMAGE_THUNK_DATA*)((BYTE*)base + imp->FirstThunk);
        auto nameT = (IMAGE_THUNK_DATA*)((BYTE*)base + imp->OriginalFirstThunk);
        for (; thunk->u1.Function; ++thunk, ++nameT) {
            if (nameT->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto hint = (IMAGE_IMPORT_BY_NAME*)((BYTE*)base + nameT->u1.AddressOfData);
            if (strcmp((const char*)hint->Name, fnName) != 0) continue;
            DWORD oldProt = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt)) return false;
            if (orig && !*orig) *orig = (void*)thunk->u1.Function;
            thunk->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProt, &oldProt);
            return true;
        }
    }
    return false;
}
static void PatchInputIAT() {
    int nGaks = 0, nGks = 0, nGrid = 0, nMod = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                nMod++;
                HMODULE mb = (HMODULE)me.modBaseAddr;
                if (mb == g_self) continue;   // 不能 hook 自己：我们要用原始状态读鼠标/Del
                if (PatchIATIn(mb, "USER32.dll", "GetAsyncKeyState", (void*)HookGaks, (void**)&g_origGaks)) nGaks++;
                if (PatchIATIn(mb, "USER32.dll", "GetKeyboardState", (void*)HookGks, (void**)&g_origGks)) nGks++;
                if (PatchIATIn(mb, "USER32.dll", "GetRawInputData", (void*)HookGrid, (void**)&g_origGrid)) nGrid++;
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    Log("IAT hook: 模块 %d 个 | GetAsyncKeyState=%d GetKeyboardState=%d GetRawInputData=%d", nMod, nGaks, nGks, nGrid);
}

// =====================================================================================
// 【第 1 步·只观察】队列虚表 hook：截获"游戏那条 DIRECT 队列"的指针
// -------------------------------------------------------------------------------------
// 目的：将来要把 41.5MB 的跨卡拷贝挪到我们自己的 copy 队列上（游戏回到 60fps、画质不变），
//       而"排序"需要在游戏队列上 Signal 围栏 —— 必须先拿到队列对象。
//       本步【只记录 + 打日志】，不 Signal、不改任何时序 ⇒ 出问题删掉即恢复。
// 开关：xess_proxy.queuehook.txt 写 1 才安装（默认不装，可做 kill switch）
// 说明：ExecuteCommandLists 在 ID3D12CommandQueue 的虚表里位置固定（COM 接口顺序），
//       用我们自己创建的 DIRECT 队列即可拿到同一张虚表，不需要扫内存找地址。
// =====================================================================================
typedef void (*PFN_ECL)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static PFN_ECL        g_origECL      = nullptr;
static volatile LONG  g_eclCalls     = 0;
static volatile PVOID g_gameQueuePtr = nullptr;
static bool           g_eclInstalled = false;

static void XferMaybeKick(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists);   // 前向声明（定义在下方）


static void HookPresentECL(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static void HookECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    MvPairHit hits[8];
    const int nhit = (q != g_worker.q.Get() && q != g_bpQ.Get() && q != g_xqQ.Get())
        ? MvPairConsume(n, lists, hits) : 0;
    F9Id::AHits f9Hits;
    const auto a2Hits = A2In::ExecuteOriginal(q, n, lists, [&] {
        f9Hits = F9Id::ExecuteOriginal(q, n, lists, g_origECL, g_origECL == &HookPresentECL);
    }, g_origECL == &HookPresentECL);
    A2In::ForwardScope a2IndependentPost;
    F9Id::ForwardScope f9IndependentPost;       // 旧 MV/Xfer 及派生提交不继承本次转发上下文
    for (int i = 0; i < nhit; ++i) MvPairSignal(q, hits[i]);
    XferMaybeKick(q, n, lists);                  // 旧颜色路径的提交记号，与 MV 独立
    F9Id::AfterExecute(f9Hits);
    A2In::After(a2Hits);
    LONG c = InterlockedIncrement(&g_eclCalls);
    if (!g_gameQueuePtr) {
        g_gameQueuePtr = q;
        Log("QUEUEHOOK: 首次截获队列 %p（本次提交 %u 条 list）", (void*)q, n);
    }
    if ((c % 3000) == 0)
        Log("QUEUEHOOK[帧%d] 已观察 %ld 次提交 | 游戏队列=%p", (int)g_frame, c, g_gameQueuePtr);
}

static void InstallQueueHook() {
    if (g_eclInstalled || g_origECL) return;
    std::string p = ModuleDir() + "xess_proxy.queuehook.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "r") != 0 || !f) { Log("QUEUEHOOK: 未启用（没有 %s）", p.c_str()); return; }
    char b[8] = { 0 }; size_t rd = fread(b, 1, 7, f); fclose(f);
    if (rd == 0 || b[0] != '1') { Log("QUEUEHOOK: 未启用（文件内容不是 1）"); return; }
    if (!g_worker.q) { Log("QUEUEHOOK: worker 队列还没建好，放弃"); return; }

    void** vt = *(void***)g_worker.q.Get();
    if (!vt) { Log("QUEUEHOOK: 拿不到虚表，放弃"); return; }
    const int kIdx = 10;   // IUnknown(3) + ID3D12Object(4) + ID3D12DeviceChild(1) + Update/CopyTileMappings(2) = 10
    if (!vt[kIdx]) { Log("QUEUEHOOK: 槽位[%d] 为空，放弃", kIdx); return; }

    HMODULE hm = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)vt[kIdx], &hm) || !hm) { Log("QUEUEHOOK: 定位模块失败，放弃"); return; }
    char mod[MAX_PATH] = { 0 }; GetModuleFileNameA(hm, mod, MAX_PATH);
    Log("QUEUEHOOK: 虚表=%p 槽位[%d]=%p 所属模块=%s", (void*)vt, kIdx, vt[kIdx], mod);

    DWORD old = 0;
    if (!VirtualProtect(vt, sizeof(void*) * (kIdx + 1), PAGE_READWRITE, &old)) {
        Log("QUEUEHOOK: 改内存保护失败，放弃"); return;
    }
    g_origECL = (PFN_ECL)vt[kIdx];
    vt[kIdx] = (void*)&HookECL;
    VirtualProtect(vt, sizeof(void*) * (kIdx + 1), old, &old);
    g_eclInstalled = true;
    F9Id::observerReady.store(true);
    Log("QUEUEHOOK: 已安装（第 1 步：只观察，不改时序）");
}

// =====================================================================================
// 【C4 方案A · S1】present 队列 ECL 观察钩子（只 log，不改时序）
// -------------------------------------------------------------------------------------
// round-04 已定：present 队列(0x1be16e24d40) ≠ FSR dispatch 队列，且现 InstallQueueHook
// 用 g_worker.q 虚表只截到 dispatch 队列、没截到 present 队列 ⇒ present 队列是另一张虚表。
// S1 = 换挂 present 队列【自己的】ExecuteCommandLists 虚表槽10，只观察（无 Signal/无 compute）。
// 开关：无。addon 每帧 AmdNrSetPresentQueue 交指针，首次交到即装；失败无害。
// =====================================================================================
static volatile PVOID g_presentQueue         = nullptr;   // addon 交来的 present 队列原生指针（S0 填、S1 用）
static PFN_ECL        g_origPresentECL       = nullptr;
static volatile LONG  g_presentEclCalls      = 0;
static bool           g_presentHookInstalled  = false;

// ===== C4-S3：compute 转换(28/10→87) + 落槽（present 队列）=====
static volatile LONG  g_bbPending = 0;              // 1 = BbHandoff 已交 T_src，等 present flush 后 compute
static volatile PVOID g_bbTex = nullptr;            // addon 交来的 T_src 原生句柄
static volatile LONG  g_bbFrame = -1;               // 对应帧号
static ComPtr<ID3D12CommandAllocator>    g_bbAl[3];    // compute list 分配器（2026-11-13 方案2 C4三套轮转：单套会在上一帧没跑完时 Reset 失败 → 整帧丢弃）
static ComPtr<ID3D12GraphicsCommandList> g_bbCl[3];    // 我们的 compute 转换 list（提交到 present 队列；2026-11-13 方案2 C4三套轮转）
static int      g_bbAi = 0;                            // 3 套轮转指针（2026-11-13 方案2 C4三套轮转，沿用 g_xqAi 先例）
static UINT64   g_bbFvUsed[3] = { 0, 0, 0 };           // 每套上次登记的完成围栏值（2026-11-13 方案2 C4三套轮转，沿用 g_xqFvUsed 先例）
static ComPtr<ID3D12Fence>               g_bbFe;    // compute list 完成围栏（2026-11-13 方案2：仍只一个，不改成 3 个）
static UINT64        g_bbFv = 0;                      // 单调递增的信号计数（2026-11-13 方案2：仍只一个，不改成 3 个）
// r28 块2 延迟实现：g_bbFe 现在已定义（搜索"R28GetBbDone 实现"）
static UINT64 R28GetBbDone() { return g_bbFe ? g_bbFe->GetCompletedValue() : 0; }
// 2026-11-13 c4fix：C4 私有状态（只在 C4Kick 的重入守卫内读写）
static volatile LONG g_c4InKick = 0;                  // 阻断1：C4Kick 非阻塞重入守卫（0=空闲，1=有线程在 C4Kick 内）
static volatile LONG g_c4ReentrySkip = 0;             // 阻断1：守卫抢占失败次数（日志用）
static bool          g_bbBuilt = false;               // 阻断2：3 套 + fence 已整轮建成（替代"g_bbCl[2] 非空"作懒建判据，单套重建期间可安全置空）
static bool          g_bbBad[3] = { false, false, false };   // 阻断2：该套 list 失效（Close/Reset 失败），等其围栏完成后单独重建
static UINT64        g_bbSigOwed = 0;                 // 阻断3：Signal(g_bbFe) 失败时欠下的围栏值（0 = 不欠）
static ID3D12CommandQueue* g_bbSigOwedQ = nullptr;    // 阻断3：欠账发生在哪条队列（只在同一队列补打）
static volatile LONG g_c4DevLost = 0;                 // 阻断3：GetCompletedValue()==UINT64_MAX（设备已移除）→ C4 私有停用
// 2026-11-13 c4srv：T_src SRV 描述符缓存改为按 C4 套号 ai（0..2）索引，原来按 parity 只有 2 个。
//   g_bbLastHeap 记录该描述符写进的是哪个 g_heap：尺寸变化时 BuildResources 会新建 g_heap，新堆里该槽是空的，必须重写。
static ID3D12Resource*       g_bbLastTex[3]  = { nullptr, nullptr, nullptr };
static ID3D12DescriptorHeap* g_bbLastHeap[3] = { nullptr, nullptr, nullptr };

// =====================================================================================
// 【UI 自动开关 v4 · B1~B6 实现】
//   放在 C4 全局量之后：ClearC4State / C4GateAllows / C4TakePending 要直接读写 g_bbPending 一族。
//   本段之后的所有函数（HookPresentECL / C4Kick / BbHandoff / ProxyExecute / HotkeyThread）
//   都可以直接用这些设施；前面已声明过的（BformPresentShow / BformBDecode / WorkerMain 的 B 分支）
//   依赖 B0 段的前置声明。
// =====================================================================================

// ---------------- 日志分级（代码里原来只有 Log；这里按设计 §8 的调用点补三级薄封装） ----------------
static void LogFmtV(const char* tag, const char* fmt, va_list ap) {
    char buf[1024] = { 0 };
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    Log("%s%s", tag, buf);
}
// 设备级失联（不可恢复）。只有这类错误才允许把整条链判死（NrHardFail）；
// 普通 Present/Signal 失败（被遮挡、窗口最小化、WAS_STILL_DRAWING 等）不该关掉 NR。
static bool HrIsDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
}
static void LogInfo(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); LogFmtV("[UIGATE] ", fmt, ap); va_end(ap);
}
static void LogWarn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); LogFmtV("[UIGATE][WARN] ", fmt, ap); va_end(ap);
}
static void LogError(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); LogFmtV("[UIGATE][ERR] ", fmt, ap); va_end(ap);
}

// ---------------- 状态包读写（独占写 / 共享读） ----------------
static void UiGateFillSnap(UiGateStatePacket* snap) {
    AcquireSRWLockShared(&g_uiStateLock);
    *snap = g_uiState;
    ReleaseSRWLockShared(&g_uiStateLock);
}
static LONG UiGateReadState() {
    AcquireSRWLockShared(&g_uiStateLock);
    const LONG s = g_uiState.state;
    ReleaseSRWLockShared(&g_uiStateLock);
    return s;
}

// ---------------- B0/P3：硬错 ----------------
// v4 §8.1：第一次硬错记录原因 → 切 OFF → 关 C4 → 清理 → 卸覆盖层。之后所有入口都因 state==OFF 直通 real()。
static void NrHardFail(const char* reason) {
    if (InterlockedExchange(&g_hardFail, 1) != 0) return;   // 只处理第一次
    LogError("Hard fail: %s", reason ? reason : "-");
    AcquireSRWLockExclusive(&g_uiStateLock);
    g_uiState.state = UIG_OFF;
    g_uiState.epoch++;
    g_uiState.timestamp = GetTickCount64();
    ReleaseSRWLockExclusive(&g_uiStateLock);
    g_nrOn = false;                                   // 让既有早退路径也直接走 real()
    InterlockedExchange(&g_c4Enabled, 0);
    R28BumpEpoch(reason);                             // r28 块2：硬错 → 全环 SEALED
    ClearC4State();
    OverlayGateCancel("硬错");                         // ★ v4fix2：不允许再挂覆盖层
    HideOverlay();
}

// ---------------- 覆盖层（P16/P17/P18） ----------------
// 覆盖层内容就是 B 的 4K 画面（DComp 视觉贴游戏窗口）。所以"能不能贴"判断一次
// g_bpReady/g_bformOn/g_formStage：B 没在出画时贴上去只会是一张冻结旧画面（会盖住游戏菜单）。
static bool OverlayContentLive() {
    return g_bpReady && g_bpVisual && g_bpDcomp && g_bformOn && g_bformStage >= 6;
}
static void ApplyOverlay() {
    // ★ v4fix2：串行化「判定 → SetContent → Commit」。原来没有锁：热路径（ProxyExecute / worker）
    //   和心跳线程都会进来，一次晚到的 Show 能把刚生效的 Hide 覆盖掉（UI 已经打开、旧画面又冒出来）。
    //   现在整个决策与 Commit 在同一把独占锁里，且显示路径在锁内【重新校验身份】，晚到的旧回调作废。
    AcquireSRWLockExclusive(&g_ovLock);
    const LONG desired = InterlockedCompareExchange(&g_overlayDesired, 0, 0);
    const LONG applied = InterlockedCompareExchange(&g_overlayApplied, 0, 0);
    if (desired == applied) { ReleaseSRWLockExclusive(&g_ovLock); return; }   // 已同步
    if (!g_bpReady || !g_bpVisual || !g_bpDcomp) {
        // 对象没建好：不更新 applied → 下次调用自动重试（设计 §11 B8）
        static LONG w1 = 0;
        if (InterlockedIncrement(&w1) == 1) LogWarn("Overlay: 对象未就绪，desired=%ld 暂缓（之后自动重试）", desired);
        ReleaseSRWLockExclusive(&g_ovLock);
        return;
    }
    if (desired == 1 && !OverlayContentLive()) {
        // 期望显示但"没有可显示的实时内容"或"UI 闸门正在停止/关闭"：
        // 此时贴上去只会是一张冻结的旧 B 画面（正好盖住游戏菜单）。不更新 applied → 延后重试。
        static LONG w2 = 0;
        if (InterlockedIncrement(&w2) == 1) LogWarn("Overlay: 无实时内容或闸门未放行，延后到 B 恢复后自动挂载");
        ReleaseSRWLockExclusive(&g_ovLock);
        return;
    }
    if (desired == 1 && g_uiFeatureOn) {
        // ★ v4fix2 单一闸门的最后一关（锁内重读）：闸门武装时记的 epoch 必须仍是当前 epoch，
        //   且状态必须仍是 RUNNING/OBSERVE。STARTING / STOPPING / STOPPED / 换代 → 一律不挂。
        UiGateStatePacket s;
        UiGateFillSnap(&s);
        const LONG gateEp = InterlockedCompareExchange(&g_ovGateEpoch, 0, 0);
        if (s.state != UIG_RUNNING && s.state != UIG_OBSERVE) {
            static LONG w3 = 0;
            if (InterlockedIncrement(&w3) == 1)
                LogWarn("Overlay: 状态=%ld 非运行，放弃本次挂载（STARTING 贴上去就是旧画面）", (long)s.state);
            ReleaseSRWLockExclusive(&g_ovLock);
            return;
        }
        if ((LONG)s.epoch != gateEp) {
            static LONG w4 = 0;
            if (InterlockedIncrement(&w4) == 1)
                LogWarn("Overlay: epoch 已换代（武装=%ld 现在=%u），放弃本次挂载（旧回调晚到）", gateEp, s.epoch);
            ReleaseSRWLockExclusive(&g_ovLock);
            return;
        }
    }
    HRESULT hr = E_FAIL;
    if (desired == 1) hr = g_bpVisual->SetContent((IUnknown*)g_bpSc.Get());
    else              hr = g_bpVisual->SetContent(nullptr);
    if (SUCCEEDED(hr)) hr = g_bpDcomp->Commit();
    if (SUCCEEDED(hr)) {
        InterlockedExchange(&g_overlayApplied, desired);
        Log("BFORM-P: 覆盖层 %s（stage=%d）", desired ? "显示" : "卸载", g_bformStage);   // 保留原日志口径
        LogInfo("Overlay %s (desired=%ld applied=%ld)", desired ? "shown" : "hidden", desired, (long)desired);
    } else {
        LogError("Overlay Commit failed: 0x%08X（applied 不更新，下次重试）", (unsigned)hr);
    }
    ReleaseSRWLockExclusive(&g_ovLock);
}
static void ShowOverlay() { InterlockedExchange(&g_overlayDesired, 1); ApplyOverlay(); }
static void HideOverlay() { InterlockedExchange(&g_overlayDesired, 0); ApplyOverlay(); }

// ---------------- C4 闸门（P13/P14/P15） ----------------
// 双重闸门：BbHandoff 入口 + C4Kick 消费都看这里；pending 的读写一律在 g_c4Lock 独占下做。
static bool C4GateAllows() {
    if (!g_c4Enabled) return false;
    if (!g_uiFeatureOn) return true;                    // 特性未启用：保持旧行为（只由 g_c4On 决定）
    const LONG st = UiGateReadState();
    return !(st == UIG_OFF || st == UIG_STOPPING || st == UIG_STOPPED);
}
static bool C4TakePending() {
    bool got = false;
    AcquireSRWLockExclusive(&g_c4Lock);
    if (InterlockedCompareExchange(&g_bbPending, 0, 1) == 1) got = true;
    ReleaseSRWLockExclusive(&g_c4Lock);
    return got;
}
static void C4RestorePending() {
    AcquireSRWLockExclusive(&g_c4Lock);
    InterlockedCompareExchange(&g_bbPending, 1, 0);
    ReleaseSRWLockExclusive(&g_c4Lock);
}
static void C4DropPending() {
    AcquireSRWLockExclusive(&g_c4Lock);
    InterlockedExchange(&g_bbPending, 0);
    ReleaseSRWLockExclusive(&g_c4Lock);
}
static void ClearC4State() {
    AcquireSRWLockExclusive(&g_c4Lock);
    InterlockedExchange(&g_bbPending, 0);
    g_bbFrame = -1;
    // g_bbTex 是 addon 交来的原生句柄，本文件从未 AddRef 它
    // ⇒ 只能断引用，不能像设计伪代码那样 Release()（那会过度释放别人的资源）。
    g_bbTex = nullptr;
    ReleaseSRWLockExclusive(&g_c4Lock);
    LogInfo("C4 state cleared");
}

// ---------------- B2/P10：停止边界 ----------------
// 设计原意：打固定围栏等所有 GPU 任务落地。本链路不改 g_worker.fv（那是 worker 自己记账的），
// 只等"当前已登记的信号值"被 GPU 追平，有界 3 秒；超时只告警不硬错（不把慢机器判死）。
static bool CaptureStopBoundary() {
    ClearC4State();
    if (!g_worker.q || !g_worker.fe) { LogInfo("Stop boundary: worker 围栏未就绪，跳过等待"); return true; }
    const UINT64 need = g_worker.fv;
    if (!need) return true;
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        const UINT64 done = g_worker.fe->GetCompletedValue();
        if (done == UINT64_MAX) { NrHardFail("Fence device lost (stop boundary)"); return false; }
        if (done >= need) break;
        if ((GetTickCount64() - t0) > (ULONGLONG)g_uiDrainWarnMs) {
            LogWarn("Stop boundary: worker 围栏等待超时 %llu ms（completed=%llu need=%llu）→ 继续切 STOPPED",
                    (unsigned long long)(GetTickCount64() - t0), (unsigned long long)done, (unsigned long long)need);
            return false;
        }
        SwitchToThread();
    }
    LogInfo("Stop boundary: worker GPU 已排空（completed>=%llu）", (unsigned long long)need);
    return true;
}

// ---------------- B1/P5：租约 + 闸门 ----------------
// 无条件先 +1（与 UiGateAutoLeave 的 -1 严格配对，特性关掉时也不会漏账）；
// 特性未启用时不读状态包、不拿共享锁，行为与旧版完全一致。
static bool UiGateEnter(UiGateStatePacket* snap) {
    InterlockedIncrement(&g_uiState.inFlight);
    if (!g_uiFeatureOn) {
        if (snap) { snap->state = UIG_RUNNING; snap->epoch = 0; snap->attempt = 0; snap->minFrame = 0; }
        return true;
    }
    MemoryBarrier();                                    // 确保 +1 对状态读取可见（设计 §3.2 步骤 2）
    UiGateStatePacket s;
    UiGateFillSnap(&s);
    if (snap) *snap = s;
    const LONG st = s.state;
    if (st == UIG_OFF || st == UIG_STOPPING || st == UIG_STOPPED) {
        // 设计 §3.2 是在这里就地 -1 再 return false。本实现把 -1 交给调用方的
        // UiGateAutoLeave 析构统一做 —— 只有一处 -1，才不会出现"进入时减一次、退出又减一次"的负计数。
        return false;                                    // 调用方走 real()
    }
    if (st == UIG_OBSERVE) {
        // 设计 §2.1 说 OBSERVE「只记日志，不改行为」，§3.2 伪代码却写 OBSERVE 也直通 real() ——
        // 两处冲突。这里按 §2.1 实施（放行 = 不改变现役行为），否则观察模式会直接把 NR 关掉。
        static LONG obs = 0;
        const LONG o = InterlockedIncrement(&obs);
        if (o == 1 || (o % 600) == 0) LogInfo("OBSERVE：放行第 %ld 帧（本模式不改行为）", o);
    }
    return true;                                         // RUNNING / STARTING
}
static void UiGateLeave() {
    InterlockedDecrement(&g_uiState.inFlight);
}
// 热路径只读闸门：WorkerMain 的 B 分支、ProxyExecute 的 bPresenting 都用它
static bool UiGateAllowsNr() {
    if (!g_uiFeatureOn) return true;
    const LONG st = UiGateReadState();
    return !(st == UIG_OFF || st == UIG_STOPPING || st == UIG_STOPPED);
}

// ---------------- B2/P8：RUNNING → STOPPING ----------------
static void UiOpenDetected() {
    AcquireSRWLockExclusive(&g_uiStateLock);
    if (g_uiState.state != UIG_RUNNING) {               // 不在 RUNNING，忽略
        ReleaseSRWLockExclusive(&g_uiStateLock);
        return;
    }
    g_uiState.state = UIG_STOPPING;
    g_uiState.epoch++;
    g_uiState.timestamp = GetTickCount64();
    // inFlight 不改：等自然归零（设计 §5.1 步骤 2）
    const UINT ep = g_uiState.epoch;
    ReleaseSRWLockExclusive(&g_uiStateLock);
    InterlockedExchange(&g_c4Enabled, 0);               // P13：停止期间关 C4（BbHandoff / C4Kick 双重闸门）
    LogInfo("UI opened, state RUNNING -> STOPPING, epoch=%u", ep);
    // ★ P0② 修复（v4fix1）：原版只置 STOPPING 不 HideOverlay，导致 UI 已打开但旧 B 画面仍盖住菜单。
    //   进 STOPPING 即立即 desired=0：DComp SetContent(nullptr)+Commit 同步卸载，确认成功才 applied=0。
    //   ApplyOverlay 内 Commit 失败不更新 applied → 下次心跳自动重试（设计 §11 B8）。
    // ★ v4fix2：同时【撤销显示 pending】并让本代不再武装 —— 否则一个已在途的 show 会在
    //   UI 打开后把旧画面重新贴上来（"UI 已开、菜单被旧画面盖住"的另一条路径）。
    OverlayGateCancel("UI 打开：立即取消 pending 显示");
    InterlockedExchange(&g_uiPrimeLastSeq, -1);
    HideOverlay();
}

// ---------------- B2/P9：STOPPING → STOPPED ----------------
static void CheckDrained() {
    UiGateStatePacket snap;
    UiGateFillSnap(&snap);
    if (snap.state != UIG_STOPPING) return;
    const LONG lease = InterlockedCompareExchange(&g_uiState.inFlight, 0, 0);
    if (lease > 0) {                                    // 还有任务在飞：等待
        const ULONGLONG elapsed = GetTickCount64() - snap.timestamp;
        // ★ 逃生阀（实现补充，设计里没有）：若游戏其实已经恢复出帧（调用间隔很小）而租约一直归不了零，
        //   说明"UI 开了一下又关、但我们已进 STOPPING"；这时继续等只会永久停在 STOPPING（每帧 deny）。
        //   超过 5 秒且调用已恢复 → 直接补回 RUNNING（换代，让旧令牌一起过期）。
        const LONGLONG lastTick2 = InterlockedCompareExchange64(&g_g0LastTick, 0, 0);
        const LONGLONG gap2 = lastTick2 ? ((LONGLONG)GetTickCount64() - lastTick2) : 0;
        if (elapsed > 5000 && gap2 >= 0 && gap2 < (LONGLONG)g_uiOpenThrMs) {
            AcquireSRWLockExclusive(&g_uiStateLock);
            if (g_uiState.state == UIG_STOPPING) {
                g_uiState.state = UIG_RUNNING;
                g_uiState.epoch++;
                g_uiState.attempt++;
                g_uiState.minFrame = (UINT)(g_frame > 0 ? g_frame : 0);
                g_uiState.timestamp = GetTickCount64();
                const UINT epEsc = g_uiState.epoch;
                ReleaseSRWLockExclusive(&g_uiStateLock);
                InterlockedExchange(&g_c4Enabled, 1);
                LogWarn("STOPPING 排空超 %llu ms 但调用已恢复（gap=%lld ms）→ 逃生阀：直接回 RUNNING，epoch=%u",
                        (unsigned long long)elapsed, gap2, (unsigned)epEsc);
                return;
            }
            ReleaseSRWLockExclusive(&g_uiStateLock);
        }
        if (elapsed > (ULONGLONG)g_uiDrainWarnMs) {
            static ULONGLONG lastW = 0;
            const ULONGLONG now = GetTickCount64();
            if ((now - lastW) >= 1000) {
                lastW = now;
                LogWarn("STOPPING timeout %llu ms, inFlight=%d", (unsigned long long)elapsed, (int)lease);
            }
        }
        return;
    }
    // 租约已归零：先落状态（防并发重复进边界），再在锁外做 GPU 边界。
    // ★ 设计 §5.2 伪代码把 CaptureStopBoundary() 写在独占锁内：本链路边界最长等 3 秒，
    //   持锁会让游戏线程的 UiGateEnter 卡 3 秒（表现就是一次大卡顿），所以挪到锁外。
    AcquireSRWLockExclusive(&g_uiStateLock);
    if (!(g_uiState.state == UIG_STOPPING && g_uiState.inFlight == 0)) {
        ReleaseSRWLockExclusive(&g_uiStateLock);
        return;
    }
    g_uiState.state = UIG_STOPPED;
    g_uiState.timestamp = GetTickCount64();
    ReleaseSRWLockExclusive(&g_uiStateLock);
    const bool boundaryOk = CaptureStopBoundary();      // 清 C4 + 等 GPU 排空（P10）
    InterlockedExchange(&g_c4Enabled, 1);               // 开关回 1；STOPPED 状态本身仍拦住 C4
    // ★ P0② 修复（v4fix1）：原版在 STOPPED 后调 ShowOverlay()，这是照抄设计 §5.2 L313-323 的错误。
    //   覆盖层内容是 B 的实时画面；NR 已停时贴上去只会是冻结旧画面盖住游戏菜单（P0② 主因）。
    //   正确行为：STOPPED 保持 hidden（HideOverlay 已在进 STOPPING 时调过，desired=0）。
    //   覆盖层在 STARTING→RUNNING 预热完成后由 OnPresentSuccess 的单一闸门重新挂载。
    // ★ v4fix2：到 STOPPED 时把显示 pending 一并撤销（不允许带着旧武装进下一轮预热）。
    OverlayGateCancel("已排空到 STOPPED");
    LogInfo("Drained, state STOPPING -> STOPPED（boundary=%d）", boundaryOk ? 1 : 0);
}

// ---------------- B2/P11：STOPPED → STARTING ----------------
static void UiCloseDetected() {
    AcquireSRWLockExclusive(&g_uiStateLock);
    if (g_uiState.state != UIG_STOPPED) { ReleaseSRWLockExclusive(&g_uiStateLock); return; }
    const ULONGLONG elapsed = GetTickCount64() - g_uiState.timestamp;
    if (elapsed < (ULONGLONG)g_uiResumeCoolMs) {        // cooldown（设计 §5.3 步骤 2）
        ReleaseSRWLockExclusive(&g_uiStateLock);
        static ULONGLONG lastCoolLog = 0;               // 心跳 40ms 一次，这条日志节流到 1 秒一条
        const ULONGLONG nowT = GetTickCount64();
        if ((nowT - lastCoolLog) >= 1000) {
            lastCoolLog = nowT;
            LogInfo("Cooldown %llu ms（<%ld ms）, skip resume", (unsigned long long)elapsed, (long)g_uiResumeCoolMs);
        }
        return;
    }
    if (!g_nrOn || g_hardFail) {                        // NR 总开关 / 硬错（设计 §5.3 步骤 3）
        ReleaseSRWLockExclusive(&g_uiStateLock);
        static ULONGLONG lastSkipLog = 0;
        const ULONGLONG nowT2 = GetTickCount64();
        if ((nowT2 - lastSkipLog) >= 1000) {
            lastSkipLog = nowT2;
            LogInfo("g_nrOn=false 或 hardFail=1, skip resume");
        }
        return;
    }
    g_uiState.state = UIG_STARTING;
    g_uiState.epoch++;
    g_uiState.attempt++;
    g_uiState.primeLeft = g_uiPrimeFrames;              // 4 帧预热
    g_uiState.minFrame = (UINT)(g_frame > 0 ? g_frame : 0);
    g_uiState.timestamp = GetTickCount64();
    {
        // Publish the A boundary before UiGateEnter can observe STARTING.
        std::lock_guard<std::mutex> lk(g_worker.m);
        g_worker.aReset.Raise(A1UiResume, (int)g_uiState.minFrame);
    }
    // 设计 §5.3 步骤 4 还有一句 inFlight = 0。这里【不写】：此时若已有新一帧拿了租约，
    // 强行清零会让那个 -1 把计数打成负数（配对铁律）。CheckDrained 已保证进 STOPPED 时它是 0。
    const UINT ep = g_uiState.epoch, at = g_uiState.attempt;
    ReleaseSRWLockExclusive(&g_uiStateLock);
    InterlockedExchange(&g_c4Enabled, 1);
    // ★ v4fix2：进 STARTING = 重新预热。撤销上一代遗留的显示 pending、清掉"已计入预热的序号"，
    //   并保证覆盖层此刻是隐藏的（本轮必须先看到有效新帧 + fence 才允许再挂）。
    OverlayGateCancel("UI 关闭：重新预热");
    InterlockedExchange(&g_uiPrimeLastSeq, -1);
    InterlockedExchange(&g_ovGateShownEpoch, -1);
    HideOverlay();
    LogInfo("UI closed, state STOPPED -> STARTING, epoch=%u, attempt=%u", ep, at);
}

// ---------------- v4fix2：单一显示闸门（替代原 B2/P12 的"每次 Present 递减 primeLeft"） ----------------
// 原实现的两个毛病（已由现场日志证实）：
//   ① 对每一次 Present 成功都递减 primeLeft —— 包括 STARTING 期间那些"还没有新内容"的帧；
//   ② 递减到 0 就立刻 ShowOverlay —— 而那一刻贴上去的仍是进 UI 前最后一帧（旧交换链内容）。
// 现在的语义：
//   · 只有"有效新 epoch 新帧"的 B present 才推进预热计数（BformBDecode 已发布身份 + 解码序号）；
//   · 预热到 0 只把状态推进 RUNNING 并【武装】闸门，不显示；
//   · 闸门要再等【下一次】同 epoch 有效 B present，且它对应的出画 fence 已落地（非阻塞），
//     再重读 state/epoch/NR/stage/窗口 才 ApplyOverlay —— 即"多等一帧 compositor 裕量"。
static void OverlayGateArm(UINT epoch) {
    InterlockedExchange(&g_ovGateEpoch, (LONG)epoch);
    InterlockedExchange64(&g_ovGateFence, 0);
    InterlockedExchange64(&g_ovGateArmTick, (LONGLONG)GetTickCount64());
    InterlockedExchange(&g_ovGateArm, 1);
    LogInfo("Overlay gate: armed (epoch=%u) —— 等下一次同代有效 B present + fence 落地", epoch);
}
static void OverlayGateCancel(const char* why) {
    const LONG was = InterlockedExchange(&g_ovGateArm, 0);
    InterlockedExchange64(&g_ovGateFence, 0);
    if (was == 1) LogInfo("Overlay gate: cancelled（%s）", why ? why : "-");
}
// 非阻塞判定：任一条件不满足就保持隐藏并下帧再试（绝不阻塞游戏线程 / 不无界等待）。
static void OverlayGateTick() {
    if (!g_uiFeatureOn) return;
    if (InterlockedCompareExchange(&g_ovGateArm, 0, 0) != 1) return;
    UiGateStatePacket snap;
    UiGateFillSnap(&snap);
    // ★ 核心修复：STARTING 一律不挂（这正是"UI 退出后闪冻结旧画面"的入口）
    if (snap.state != UIG_RUNNING && snap.state != UIG_OBSERVE) {
        OverlayGateCancel("状态不是运行态");
        HideOverlay();
        return;
    }
    const LONG ep = InterlockedCompareExchange(&g_ovGateEpoch, 0, 0);
    if ((LONG)snap.epoch != ep) { OverlayGateCancel("epoch 已换代（旧回调晚到）"); return; }
    const LONGLONG armedRaw = InterlockedCompareExchange64(&g_ovGateArmTick, 0, 0);
    const ULONGLONG armed = (ULONGLONG)armedRaw;
    const ULONGLONG waited = armed ? (GetTickCount64() - armed) : 0;
    const LONGLONG needRaw = InterlockedCompareExchange64(&g_ovGateFence, 0, 0);
    if (needRaw == 0) {
        // 武装之后还没见到"有效新帧出画"：保持隐藏；有时限（无新内容不无界等待，下次有效帧会自动再武装）
        return;
    }
    if (g_bpFe) {
        const UINT64 done = g_bpFe->GetCompletedValue();
        if (done == UINT64_MAX) { NrHardFail("Fence device lost (overlay gate)"); return; }
        if (done < (UINT64)needRaw) {
            if (waited > (ULONGLONG)g_uiGateWaitMs) { OverlayGateCancel("出画 fence 超时未落地（保持隐藏）"); }
            else {
                static LONG hold = 0;
                if ((InterlockedIncrement(&hold) % 60) == 1)
                    LogInfo("Overlay gate: 等出画 fence 落地（done=%llu need=%llu 已等 %llu ms，保持隐藏）",
                            (unsigned long long)done, (unsigned long long)needRaw, (unsigned long long)waited);
            }
            return;
        }
    }
    // ---- 重读全部前置条件（不信任任何更早的快照）----
    if (!g_nrOn || g_hardFail)                    { OverlayGateCancel("NR 已关 / 硬错"); HideOverlay(); return; }
    if (!OverlayContentLive())                    { return; }
    if (!UiGateAllowsNr())                        { return; }
    if (!g_gameHwnd || !IsWindow(g_gameHwnd) || IsIconic(g_gameHwnd)) {
        OverlayGateCancel("窗口不可见（最小化/已销毁）");
        return;
    }
    InterlockedExchange(&g_ovGateArm, 0);                 // 一次性：挂上即撤销武装
    InterlockedExchange64(&g_ovGateFence, 0);
    InterlockedExchange(&g_ovGateShownEpoch, ep);
    LogInfo("Overlay gate: 条件满足 → 挂载（epoch=%ld fence=%lld 等 fence %llu ms）",
            ep, (long long)needRaw, (unsigned long long)waited);
    ShowOverlay();
}

// 预热推进 + 闸门武装/驱动（唯一入口）。fromB=false 表示"非 B 出画的 Present 成功"
// （游戏交换链的 Present 钩子；2077 用 pColor 底图时它根本不会被调用）。
static void UiPrimeStep(bool fromB) {
    if (!g_uiFeatureOn) return;
    UiGateStatePacket snap;
    UiGateFillSnap(&snap);
    if (snap.state == UIG_STARTING) {
        if (!fromB) return;                                   // 非 B 出画：不计入 B 的预热
        const LONG seq  = InterlockedCompareExchange(&g_bDecSeq, 0, 0);
        const LONG last = InterlockedCompareExchange(&g_uiPrimeLastSeq, 0, 0);
        const LONG epB  = InterlockedCompareExchange(&g_bDecEpoch, 0, 0);
        const LONG frB  = InterlockedCompareExchange(&g_bDecFrame, 0, 0);
        if (seq <= 0 || seq == last || frB < 0) return;        // 没有新的有效解码：本帧不计入预热
        if ((UINT)epB != snap.epoch) {                         // 换代之前的槽位：不计入
            static int pe = 0;
            if ((pe++ % 30) == 0)
                LogInfo("Prime: 解码身份 epoch=%ld ≠ 当前 %u → 本帧不计入预热", epB, snap.epoch);
            return;
        }
        if ((UINT)frB < snap.minFrame) return;                 // 帧号下限（令牌已校验，这里双保险）
        InterlockedExchange(&g_uiPrimeLastSeq, seq);
        const LONG left = InterlockedDecrement(&g_uiState.primeLeft);
        if (left > 0) { LogInfo("Priming %d/%d（有效新帧 seq=%ld frame=%ld）",
                                (int)(g_uiPrimeFrames - left), (int)g_uiPrimeFrames, seq, frB); return; }
        if (left < 0) { InterlockedExchange(&g_uiState.primeLeft, 0); return; }   // 别把计数打成负数
        // 预热完成 → 只推进状态机 + 武装闸门（★ 不再直接 ShowOverlay）
        AcquireSRWLockExclusive(&g_uiStateLock);
        if (g_uiState.state == UIG_STARTING && g_uiState.primeLeft == 0) {
            g_uiState.state = UIG_RUNNING;
            g_uiState.timestamp = GetTickCount64();
            const UINT ep2 = g_uiState.epoch;
            ReleaseSRWLockExclusive(&g_uiStateLock);
            LogInfo("Priming done（%d 帧有效新帧）, state STARTING -> RUNNING, epoch=%u", (int)g_uiPrimeFrames, ep2);
            OverlayGateArm(ep2);
        } else {
            ReleaseSRWLockExclusive(&g_uiStateLock);
        }
        return;
    }
    if (snap.state != UIG_RUNNING && snap.state != UIG_OBSERVE) {
        OverlayGateCancel("状态离开运行态");
        return;
    }
    if (!fromB) return;                                       // 非 B 出画：不驱动 B 覆盖层
    // RUNNING / OBSERVE：覆盖层当前没挂着、且本代还没显示过 → 重新武装（自愈，UI 反复开关也适用）
    // ★ 只在【未武装】时武装：已武装时绝不能重置 fence —— 否则每帧都把"要等的出画围栏"推到最新一帧，
    //   永远追不上（fence 目标随时间前进），覆盖层会一直挂不上。武装一次、fence 填一次，然后等它落地。
    const LONG shown   = InterlockedCompareExchange(&g_ovGateShownEpoch, 0, 0);
    const LONG applied = InterlockedCompareExchange(&g_overlayApplied, 0, 0);
    if (InterlockedCompareExchange(&g_ovGateArm, 0, 0) != 1 &&
        applied != 1 && shown != (LONG)snap.epoch) {
        OverlayGateArm(snap.epoch);
    }
    if (InterlockedCompareExchange(&g_ovGateArm, 0, 0) == 1 &&
        InterlockedCompareExchange64(&g_ovGateFence, 0, 0) == 0) {
        // 用【本次有效出画】的出画围栏值当 target：等它完成 = 这一帧确实已经落到交换链后缓冲里
        InterlockedExchange64(&g_ovGateFence, (LONGLONG)g_bpFv);
        InterlockedExchange64(&g_ovGateArmTick, (LONGLONG)GetTickCount64());
    }
    OverlayGateTick();
}

// ---------------- B2/P12：STARTING → RUNNING ----------------
// ★ v4fix2：签名保持不变（Detour_Present / 游戏交换链的调用点不动），语义改为"非 B 出画"：
//   它不再递减 B 的预热计数，也不再直接挂覆盖层 —— 显示只有 OverlayGateTick 一条出路。
static void OnPresentSuccess() { UiPrimeStep(false); }
// ★ v4fix2：B 自己那条出画链 Present 成功时调用（BformPresentFrame / BformPPStage2）
static void OnBPresentSuccess() {
    InterlockedExchange64(&g_bPresFv, (LONGLONG)g_bpFv);
    InterlockedExchange64(&g_bPresTick, (LONGLONG)GetTickCount64());
    UiPrimeStep(true);
}

// ---------------- B1/P6：令牌发布 ----------------
// 发布协议（设计 §4.2）：清 valid → 写全部字段 → MemoryBarrier → 置 valid。
static void UiPublishSlot(int slot, int frame, ID3D12Resource* outputTex, UINT64 readyFence, float jx, float jy) {
    if (slot < 0 || slot >= K) return;
    BformSlotTag* tag = &g_slotTag[slot];
    InterlockedExchange(&tag->valid, 0);                // 1. 先清发布门（防复用时被提前读）
    UINT ep = 0, at = 0;
    if (g_uiFeatureOn) {
        AcquireSRWLockShared(&g_uiStateLock);
        ep = g_uiState.epoch; at = g_uiState.attempt;
        ReleaseSRWLockShared(&g_uiStateLock);
    }
    tag->epoch = ep;
    tag->attempt = at;
    tag->frame = frame;
    tag->jx = jx; tag->jy = jy;                         // 面单原有意义（该帧 jitter），随包一起发布
    tag->readyFence = readyFence;
    tag->outputTex = outputTex;
    MemoryBarrier();                                    // 3. 字段全部可见后才开门
    InterlockedExchange(&tag->valid, 1);
    // step2wm r2：消费资格不再在这里清。它记的是"已消费的 frame 值"，
    // 新 frame 与旧记录不同就自然放行；同一 frame 被重复发布仍然只消费一次。
}

// ---------------- B1/P7：令牌校验（消费端） ----------------
static bool UiTokenValid(int slot) {
    if (!g_uiFeatureOn) return true;                    // 未启用：不做令牌校验（旧行为）
    if (slot < 0 || slot >= K) return false;
    BformSlotTag* tag = &g_slotTag[slot];
    if (InterlockedCompareExchange(&tag->valid, 1, 1) != 1) return false;   // 未发布
    MemoryBarrier();
    UiGateStatePacket snap;
    UiGateFillSnap(&snap);
    if (tag->epoch != snap.epoch || tag->attempt != snap.attempt) return false;    // 身份字段 1/2
    if ((INT)tag->frame < (INT)snap.minFrame) return false;                        // 身份字段 3
    return true;
}

// ---------------- B6：模式矩阵 / 心跳 ----------------
static void UiGateReloadConfig() {
    std::string cp = ModuleDir() + "xess_proxy.uigate.txt";
    FILE* cf = nullptr; char cb[64] = { 0 };
    int mv = -1; long tv = 0, cool = 0;
    if (fopen_s(&cf, cp.c_str(), "r") == 0 && cf) {
        if (fgets(cb, sizeof(cb), cf)) {
            if (sscanf_s(cb, "%d %ld %ld", &mv, &tv, &cool) < 1) mv = -1;
        }
        fclose(cf);
    }
    if (mv < 0) return;                                 // 文件不在/读失败：保持当前配置
    const LONG newMode = (mv == 1 || mv == 2) ? (LONG)mv : 0;
    if (tv >= 50 && tv <= 5000) InterlockedExchange(&g_uiOpenThrMs, (LONG)tv);
    if (cool >= 500 && cool <= 10000) InterlockedExchange(&g_uiResumeCoolMs, (LONG)cool);
    if (newMode == g_uiModeCfg && (newMode >= 1) == (g_uiFeatureOn != 0)) return;   // 没变化
    LogInfo("配置热更新: mode=%ld（0=关 1=观察 2=自动）阈值=%ldms 冷却=%ldms",
            (long)newMode, (long)g_uiOpenThrMs, (long)g_uiResumeCoolMs);
    InterlockedExchange(&g_uiModeCfg, newMode);
    InterlockedExchange(&g_uiFeatureOn, newMode >= 1 ? 1 : 0);
    InterlockedExchange(&g_uiAutoOn, newMode == 2 ? 1 : 0);
    // 同步 G0 观察探针的开关（它只在 mode==1 时做空档计算/打日志）
    InterlockedExchange(&g_g0Mode, newMode == 1 ? 1 : 0);
    InterlockedExchange(&g_g0ModeShared, newMode == 1 ? 1 : 0);
    InterlockedExchange64(&g_g0ThrMsShared, newMode == 1 ? (LONGLONG)g_uiOpenThrMs : 0);
    InterlockedExchange64(&g_g0ThrMs, newMode == 1 ? (LONGLONG)g_uiOpenThrMs : 0);
    AcquireSRWLockExclusive(&g_uiStateLock);
    if (newMode == 0) {
        g_uiState.state = UIG_OFF;
        g_uiState.timestamp = GetTickCount64();
    } else if (newMode == 1) {
        if (g_uiState.state != UIG_OBSERVE) {          // 2→1：退回观察（不改行为）
            g_uiState.state = UIG_OBSERVE;
            g_uiState.epoch++;
            g_uiState.timestamp = GetTickCount64();
        }
    } else {                                           // newMode == 2
        if (g_uiState.state == UIG_OFF || g_uiState.state == UIG_OBSERVE) {   // 0/1→2：进入自动
            g_uiState.state = g_nrOn ? UIG_RUNNING : UIG_OFF;
            g_uiState.epoch++;
            g_uiState.timestamp = GetTickCount64();
        }
    }
    ReleaseSRWLockExclusive(&g_uiStateLock);
}
static void UiGateInit(LONG mode, LONG thrMs) {
    if (thrMs >= 50 && thrMs <= 5000) InterlockedExchange(&g_uiOpenThrMs, thrMs);
    InterlockedExchange(&g_uiModeCfg, mode);
    InterlockedExchange(&g_uiFeatureOn, mode >= 1 ? 1 : 0);
    InterlockedExchange(&g_uiAutoOn, mode == 2 ? 1 : 0);
    if (mode >= 1) {
        AcquireSRWLockExclusive(&g_uiStateLock);
        // mode 1 = 观察；mode 2 = 自动。自动模式下若启动时 NR 本来就是关的
        // （xess_proxy.nrstart.txt=0）就落到 OFF，等 F10 再进 RUNNING，与旧语义一致。
        g_uiState.state = (mode == 1) ? UIG_OBSERVE : (g_nrOn ? UIG_RUNNING : UIG_OFF);
        g_uiState.epoch = 1;
        g_uiState.attempt = 0;
        g_uiState.primeLeft = 0;
        g_uiState.minFrame = 0;
        g_uiState.timestamp = GetTickCount64();
        ReleaseSRWLockExclusive(&g_uiStateLock);
        LogInfo("UI Gate v4 启用：mode=%ld（1=观察 2=自动）阈值=%ldms 冷却=%ldms 预热=%ld帧",
                (long)mode, (long)g_uiOpenThrMs, (long)g_uiResumeCoolMs, (long)g_uiPrimeFrames);
    } else {
        LogInfo("UI Gate v4 未启用（xess_proxy.uigate.txt 不存在或 mode=0）→ 完全直通，行为与旧版一致");
    }
}
static void UiGateTick() {
    const ULONGLONG now = GetTickCount64();
    {   // P22：配置热更新（1 秒一次；特性关着也要读 —— 否则运行中把文件建出来永远不会生效）
        static ULONGLONG lastCfg = 0;
        if ((now - lastCfg) >= 1000) { lastCfg = now; UiGateReloadConfig(); }
    }
    if (!g_uiFeatureOn) return;
    const LONG st = UiGateReadState();
    const LONGLONG lastTick = InterlockedCompareExchange64(&g_g0LastTick, 0, 0);
    LONGLONG gap = lastTick ? ((LONGLONG)now - lastTick) : 0;
    if (gap < 0) gap = 0;                                // 时间戳倒退按 0 处理
    if (!lastTick) return;                               // 还没开始调用：什么也不判
    if (g_uiAutoOn) {
        if (st == UIG_RUNNING && gap >= (LONGLONG)g_uiOpenThrMs) UiOpenDetected();
        else if (st == UIG_STOPPING) CheckDrained();
        else if (st == UIG_STOPPED && gap < (LONGLONG)g_uiOpenThrMs) UiCloseDetected();
    } else {
        // OBSERVE：只记日志，不改行为（设计 §2.1）
        static bool obsGap = false;
        if (st == UIG_OBSERVE && gap >= (LONGLONG)g_uiOpenThrMs) {
            if (!obsGap) { obsGap = true; LogInfo("OBSERVE: 检测到 UI 打开空档 %lld ms（本模式不改行为）", gap); }
        } else if (obsGap && gap < (LONGLONG)g_uiOpenThrMs) {
            obsGap = false;
            LogInfo("OBSERVE: 调用已恢复（gap=%lld ms），若为自动模式此处会走 STOPPED -> STARTING", gap);
        }
    }
    // ★ v4fix2 兜底①：STARTING 长时间没有"有效新帧"（例如 B 侧不再出画、或后端不同步）时，
    //   预热计数永远到不了 0 → 一直停在 STARTING → 覆盖层永不挂载（NR 看不见）。
    //   这里给一个【有界】降级：超时就直接推进 RUNNING 并武装闸门（闸门自己还会再等新帧 + fence）。
    if (st == UIG_STARTING) {
        UiGateStatePacket sp;
        UiGateFillSnap(&sp);
        if (sp.state == UIG_STARTING && sp.timestamp &&
            (now - sp.timestamp) > (ULONGLONG)g_uiPrimeDeadlineMs) {
            AcquireSRWLockExclusive(&g_uiStateLock);
            if (g_uiState.state == UIG_STARTING) {
                g_uiState.state = UIG_RUNNING;
                g_uiState.primeLeft = 0;
                g_uiState.timestamp = GetTickCount64();
                const UINT epD = g_uiState.epoch;
                ReleaseSRWLockExclusive(&g_uiStateLock);
                LogWarn("Priming 超时 %ld ms 仍无有效新帧 → 兜底推进 RUNNING（epoch=%u）", (long)g_uiPrimeDeadlineMs, epD);
                OverlayGateArm(epD);
            } else {
                ReleaseSRWLockExclusive(&g_uiStateLock);
            }
        }
    }
    // ★ v4fix2 兜底②：心跳里也做一次非阻塞闸门判定（出画 fence 可能在这期间落完，而游戏已经不出新帧）。
    OverlayGateTick();
}
static void UiGateHotkeyToggle() {
    if (!g_uiFeatureOn) {                                // 特性未启用：保持旧行为（F10 只切 NR）
        R28BumpEpoch("f10");                             // r28 块2（NEW-HIT4）：先 bump 再切状态，B 不能消费旧票
        g_nrOn = !g_nrOn;
        Log("*** F10: NR %s ***", g_nrOn ? "开" : "关");
        return;
    }

    AcquireSRWLockExclusive(&g_uiStateLock);
    if (g_uiState.state == UIG_OFF || !g_nrOn) {
        g_nrOn = true;
        g_uiState.state = UIG_RUNNING;
        g_uiState.epoch++;
        g_uiState.attempt = 0;
        g_uiState.primeLeft = 0;
        g_uiState.minFrame = (UINT)(g_frame > 0 ? g_frame : 0);
        g_uiState.timestamp = GetTickCount64();
        const UINT ep = g_uiState.epoch;
        ReleaseSRWLockExclusive(&g_uiStateLock);
        InterlockedExchange(&g_c4Enabled, 1);
        LogInfo("*** F10: UI Gate OFF -> RUNNING（epoch=%u）***", ep);
    } else {
        g_nrOn = false;
        g_uiState.state = UIG_OFF;
        g_uiState.epoch++;
        g_uiState.timestamp = GetTickCount64();
        ReleaseSRWLockExclusive(&g_uiStateLock);
        InterlockedExchange(&g_c4Enabled, 0);
        ClearC4State();
        OverlayGateCancel("F10 关 NR");        // ★ v4fix2：撤销显示 pending
        HideOverlay();
        LogInfo("*** F10: UI Gate -> OFF（NR 关，C4 已清，覆盖层已卸）***");
    }
}

static void C4Kick(ID3D12CommandQueue* q);          // 前向声明（定义在 XferKickColor 之后）

static void HookPresentECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    F9Id::AHits f9Hits;
    const auto a2Hits = A2In::ExecuteOriginal(q, n, lists, [&] {
        f9Hits = F9Id::ExecuteOriginal(q, n, lists, g_origPresentECL, g_origPresentECL == &HookECL);
    }, g_origPresentECL == &HookECL);
    A2In::ForwardScope a2IndependentPost;
    F9Id::ForwardScope f9IndependentPost;              // C4 派生提交拥有自己的调用链
    if (g_hotDiagOn) {
        const LONG c = InterlockedIncrement(&g_presentEclCalls);
        if (c == 1 || (c % 3000) == 0)
            Log("PRESENT-ECL[帧%d] 第 %ld 次提交 queue=%p n=%u", (int)g_frame, c, (void*)q, n);
    }
    // C4-S3：present 队列 flush 后，若有 pending 的 T_src，提交 compute 转换 + 落槽（每帧只处理一次）
    // ★ v4 P13：取 pending 改到 g_c4Lock 独占下做（原来裸 CAS，与 BbHandoff/ClearC4State 有竞态）
    if (q == (ID3D12CommandQueue*)g_presentQueue && C4TakePending())
        C4Kick(q);
    F9Id::AfterExecute(f9Hits);
    A2In::After(a2Hits);
}

static void InstallPresentQueueHook() {
    if (g_presentHookInstalled || g_origPresentECL || !g_presentQueue) return;
    void** vt = *(void***)g_presentQueue;
    if (!vt) { Log("PRESENT-HOOK: 拿不到 present 队列虚表，放弃"); return; }
    const int kIdx = 10;   // 同 InstallQueueHook：IUnknown(3)+ID3D12Object(4)+DeviceChild(1)+TileMappings(2)=10
    if (!vt[kIdx]) { Log("PRESENT-HOOK: 槽位[%d] 为空，放弃", kIdx); return; }

    HMODULE hm = nullptr;
    char mod[MAX_PATH] = { 0 };
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)vt[kIdx], &hm) && hm) GetModuleFileNameA(hm, mod, MAX_PATH);
    Log("PRESENT-HOOK: present 队列=%p 虚表=%p 槽位[%d]=%p 模块=%s",
        (void*)g_presentQueue, (void*)vt, kIdx, vt[kIdx], mod);

    DWORD old = 0;
    if (!VirtualProtect(vt, sizeof(void*) * (kIdx + 1), PAGE_READWRITE, &old)) {
        Log("PRESENT-HOOK: 改内存保护失败，放弃"); return;
    }
    g_origPresentECL = (PFN_ECL)vt[kIdx];
    vt[kIdx] = (void*)&HookPresentECL;
    VirtualProtect(vt, sizeof(void*) * (kIdx + 1), old, &old);
    g_presentHookInstalled = true;
    F9Id::observerReady.store(true);
    Log("PRESENT-HOOK: 已换挂 present 队列 ECL（观察钩子，不改时序）");
}

// =====================================================================================
// 【第 2 步】把 33.2MB 的 4K 颜色拷贝挪到我们自己的 COPY 队列
// -------------------------------------------------------------------------------------
// 要解决的两个问题：
//  ① 竞态：CPU 录帧 N 时 GPU 可能还在写帧 N-1 的缓冲 → 用 HookECL 在游戏提交后 Signal 内部围栏，
//     我们的拷贝队列 Wait 过它才拷（g_xqFe）
//  ② B 读太早：颜色比 MV 晚到 → 用【跨卡共享围栏】g_xqFe2 通知 B，B 解码前 Wait（GPU 侧等待）
// 开关：xess_proxy.queuehook.txt = 1（与第 1 步共用；写 0 即完全回到直拷路径）
// =====================================================================================

static bool XferEnsure() {
    if (g_xqQ) return true;
    if (!g_dev) { static int de=0; if(de++<3) Log("XFERDBG3: XferEnsure 第 %d 步失败", 2); return false; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_xqQ)))) { static int de=0; if(de++<3) Log("XFERDBG3: XferEnsure 第 %d 步失败", 4); return false; }
    for (int xi = 0; xi < 3; ++xi) {
        if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&g_xqAl[xi])))) { static int de=0; if(de++<3) Log("XFERDBG3: XferEnsure 第 %d 步失败", 6); return false; }
        if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_xqAl[xi].Get(), nullptr, IID_PPV_ARGS(&g_xqCl[xi])))) { static int de=0; if(de++<3) Log("XFERDBG3: XferEnsure 第 %d 步失败", 7); return false; }
        g_xqCl[xi]->Close();
    }
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_xqFe)))) return false;   // 内部记号围栏（同卡即可）
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&g_xqFe2)))) { static int de=0; if(de++<3) Log("XFERDBG3: XferEnsure 第 %d 步失败", 10); return false; }
    g_xferOwn = true;
    if (FAILED(g_dev->CreateSharedHandle(g_xqFe2.Get(), nullptr, GENERIC_ALL, nullptr, &g_xqShare))) return false;   // ★ 跨卡共享句柄（B 端用它 OpenSharedHandle）
    Log("XFER: 独立 copy 队列 + 跨卡共享围栏就绪（33.2MB 颜色改由我们的队列拷）");
    return true;
}

// 在我们的 COPY 队列上：等 vSig 过了 → 拷 g_outp8[parity] → 槽 slot 的颜色平面 → Signal 共享围栏
static bool XferKickColor(ID3D12Resource* src4k, UINT slot, UINT64 vSig) {
    if (!XferEnsure() || !src4k || !g_upBuf) { static int d0=0; if(d0++<3) Log("XFERDBG2: 早退 ensure/src/buf (xferOwn=%d src=%p buf=%p)", (int)g_xferOwn, (void*)src4k, (void*)g_upBuf.Get()); return false; }
    int ai = g_xqAi; g_xqAi = (g_xqAi + 1) % 3;
    // 该组上次的拷贝还没跑完 → 跳过并计数（这就是"半帧不更新"的来源，必须能看见）
    if (g_xqFvUsed[ai] && g_xqFe2 && g_xqFe2->GetCompletedValue() < g_xqFvUsed[ai])
    { InterlockedIncrement(&g_xferSkip); return false; }
    if (FAILED(g_xqAl[ai]->Reset()) || FAILED(g_xqCl[ai]->Reset(g_xqAl[ai].Get(), nullptr)))
    { InterlockedIncrement(&g_xferSkip); return false; }
    ID3D12GraphicsCommandList* cl = g_xqCl[ai].Get();
    D3D12_RESOURCE_BARRIER bb{};
    bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb.Transition.pResource = src4k;
    bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &bb);
    bb.Transition.pResource = g_upBuf.Get();
    bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    cl->ResourceBarrier(1, &bb);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_upBuf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = (UINT64)slot * g_upSlotBytes + g_upOffC;
    dst.PlacedFootprint.Footprint.Format = g_upFmtC;
    dst.PlacedFootprint.Footprint.Width = g_upW;
    dst.PlacedFootprint.Footprint.Height = g_upH;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g_upPitchC;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = src4k;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    bb.Transition.pResource = src4k;
    bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    cl->ResourceBarrier(1, &bb);
    bb.Transition.pResource = g_upBuf.Get();
    bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    cl->ResourceBarrier(1, &bb);
    if (FAILED(cl->Close())) { InterlockedIncrement(&g_xferSkip); return false; }
    HRESULT hrWait = g_xqQ->Wait(g_xqFe.Get(), vSig);
    ID3D12CommandList* lm[] = { cl };
    g_xqQ->ExecuteCommandLists(1, lm);
    UINT64 v2 = ++g_xqFv2;
    HRESULT hrSig = g_xqQ->Signal(g_xqFe2.Get(), v2);
    BtTrace("COPY_SUBMIT", g_frame, "copy",
            "slot=%u frameTag=%d copy_queue=0x%llx fence=0x%llx side=g signal_value=%llu vSig=%llu source_fence=0x%llx source_side=g signal_hr=0x%08lX wait_hr=0x%08lX",
            slot, (int)g_xferPendFrameTag, BtPtr(g_xqQ.Get()), BtPtr(g_xqFe2.Get()),
            (unsigned long long)v2, (unsigned long long)vSig, BtPtr(g_xqFe.Get()),
            (unsigned long)hrSig, (unsigned long)hrWait);
    g_xqFvUsed[ai] = v2;
    g_slotReadyFv[slot] = v2;
    // ★ 颜色拷贝成功后才发布面单；v4 P6：改走令牌发布协议（valid 门 + epoch/attempt/frame 三字段）
    UiPublishSlot((int)slot, (int)(g_xferPendFrameTag), src4k, v2, 0.0f, 0.0f);
    g_upLastSlot = (int)slot;
    LONG n = InterlockedIncrement(&g_xferKick);
    if ((n % 300) == 1)
        Log("XFER[帧%d] 已拷 %ld 次 | 跳过 %ld | 槽 %u（该槽记录帧 %d）| 就绪围栏=%llu", (int)g_frame, n, g_xferSkip, slot, (int)g_slotTag[slot].frame, (unsigned long long)v2);
    return true;
}

// =====================================================================================
// 【C4 方案A · S3】在 present 队列上跑 compute 转换(28/10→87) + 落槽
// -------------------------------------------------------------------------------------
// 时机：addon finish_effects 把后缓冲拷进 T_src 并调 AmdNrBbHandoff（置 pending），
//       ReShade 随后 flush 把捕获 list 提交到 present 队列 → HookPresentECL 触发 C4Kick。
//       C4Kick 在 present 队列（FIFO，晚于捕获）录 compute：T_src --g_psoFmt8--> g_outp8 --Copy--> 槽。
// 开关：xess_proxy.c4.txt = 1（默认关；另有 m1probe=0 + bform 开 才让 BbHandoff 走到这里）。
// =====================================================================================
// 2026-11-13 c4srv：描述符与 C4 套号 ai 绑定（原来按 parity 只有 2 个，3 套在途时同奇偶的旧提交
//   可能仍在 GPU 上读，新一帧就改写它）。第 ai 个描述符只被第 ai 套 list 读，调用方选套时已保证
//   该套 g_bbFvUsed[ai] 已被 g_bbFe 完成或尚未登记 ⇒ 此时改写安全（复用时机与 allocator 相同）。
static UINT C4BbSrcSlot(int ai) {
    return ai == 0 ? kSrvBbSrc0 : (ai == 1 ? kSrvBbSrc1 : kSrvBbSrc2);
}
static void C4EnsureBbSrv(ID3D12Resource* tex, int ai) {
    if (!tex || !g_dev || !g_heap) return;
    if (ai < 0 || ai >= 3) return;
    if (g_bbLastTex[ai] == tex && g_bbLastHeap[ai] == g_heap.Get()) return;
    g_bbLastTex[ai] = tex;
    g_bbLastHeap[ai] = g_heap.Get();
    D3D12_RESOURCE_DESC d = tex->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = d.Format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)C4BbSrcSlot(ai) * g_inc;
    g_dev->CreateShaderResourceView(tex, &sd, h);
    // T_src 可跟两张底图一起轮换。重绑照常；只在显式诊断时抽样记录，避免每次重绑刷盘。
    if (g_hotDiagOn) {
        static unsigned remap = 0;
        if ((remap++ % 300) == 0)
            Log("C4-S3: T_src SRV 就绪 ai=%d 槽=%u heap=%p %ux%u fmt=%d (%p)", ai, C4BbSrcSlot(ai), (void*)g_heap.Get(), (UINT)d.Width, (UINT)d.Height, (int)d.Format, (void*)tex);
    }
}

// ★ 2026-11-08 HOTDIAG：C4Kick 早退分类计数（默认关；游戏内 F6 开）。只在 g_hotDiagOn 时累计/打印，
//   同一类最多每 300 次一条；只计数+日志，不改变任何 return 条件/顺序或 GPU 调用。
static volatile LONG g_c4EarlyCnt[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const char* const kC4EarlyName[16] = {
    "gate", "xfer_ensure", "invalid_tex_frame_upbuf", "busy_fence",
    "outp8_not_ready", "bare_dev", "bb_alloc", "bb_list",
    "bb_fence", "reset_fail", "close_fail", "unknown",
    "reentry", "sig_owed", "dev_lost", "bad_set"          // 2026-11-13 c4fix 新增 12..15
};
static void C4DiagEarly(int why, const char* detail) {
    if (!g_hotDiagOn) return;   // 关：零日志、零计数（仅一次 volatile 读）
    if (why < 0 || why >= 16) why = 11;
    LONG n = InterlockedIncrement(&g_c4EarlyCnt[why]);
    if (n == 1 || (n % 300) == 0)
        Log("HOTDIAG C4EARLY reason=%s count=%ld detail=%s", kC4EarlyName[why], n, detail ? detail : "-");
}

// 2026-11-13 c4fix 阻断3：GetCompletedValue()==UINT64_MAX 表示设备已移除，不能当"已完成"。
//   置 C4 私有停用标志（不动 g_c4On：g_c4On 还决定别处 8bit 路径是否让位），只打一次日志。
static bool C4FenceDevLost(UINT64 done) {
    if (done != UINT64_MAX) return false;
    if (InterlockedExchange(&g_c4DevLost, 1) == 0)
        Log("C4-S3: g_bbFe->GetCompletedValue()=UINT64_MAX（设备已移除）→ C4 停用，不再提交（GetDeviceRemovedReason 见游戏侧）");
    C4DiagEarly(14, "dev_lost");
    NrHardFail("Fence device lost (C4)");   // ★ v4 P20：设备移除是硬错（不可恢复）→ 统一 OFF + 清状态
    return true;
}

// 只在 C4KickLocked 持有 g_c4InKick 时访问。持有公共对象引用避免地址复用命中旧结果；
// list 由 g_bbCl 持有，并在每个重建点主动失效对应记录。
struct C4HeapBinding {
    ID3D12GraphicsCommandList* list = nullptr;
    ComPtr<ID3D12DescriptorHeap> native;
    bool decided = false;
    unsigned probes = 0;
};
static struct C4HeapBindingCache {
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> dev;
    int buildGen = -1;
    C4HeapBinding sets[3];
} g_c4HeapBindings;

// 2026-11-13 c4fix 阻断2：单独重建失效的第 ai 套（调用方须已确认该套 g_bbFvUsed[ai] 已被 fence 完成）。
//   微软文档：Close 失败的 command list 永远不能再 Reset（Reset 返回原 Close 错误），只能释放重建。
//   做法：先释放坏 list → Reset allocator（失败则连 allocator 一起重建）→ 新建 list → 立即 Close。
//   全部成功才替换 g_bbCl[ai] 并清 g_bbBad[ai]；任何一步失败保持失效、返回 false（本帧跳过，下次轮到再试）。
//   只动这一套，绝不触碰另两套（它们可能仍在 GPU 上执行）。
static bool C4RebuildSet(ID3D12CommandQueue* q, int ai) {
    ComPtr<ID3D12Device> bareDev;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&bareDev))) || !bareDev) { Log("C4-S3: 重建 ai=%d 取裸设备失败", ai); return false; }
    g_c4HeapBindings.sets[ai] = {};                         // list 地址即使复用，也不能复用旧身份结论
    g_bbCl[ai].Reset();                                     // 坏 list 不可恢复，先放开（也解除它与 allocator 的关联）
    HRESULT hrA = g_bbAl[ai] ? g_bbAl[ai]->Reset() : E_POINTER;
    bool newAl = false;
    if (FAILED(hrA)) {
        ComPtr<ID3D12CommandAllocator> al;
        HRESULT hrNa = bareDev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
        if (FAILED(hrNa) || !al) { Log("C4-S3: 重建 ai=%d allocator Reset=0x%08X 且新建失败 0x%08X", ai, (unsigned)hrA, (unsigned)hrNa); return false; }
        g_bbAl[ai] = al; newAl = true;
    }
    ComPtr<ID3D12GraphicsCommandList> cl;
    HRESULT hrL = bareDev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bbAl[ai].Get(), nullptr, IID_PPV_ARGS(&cl));
    if (FAILED(hrL) || !cl) { Log("C4-S3: 重建 ai=%d 建 list 失败 0x%08X", ai, (unsigned)hrL); return false; }
    HRESULT hrC = cl->Close();
    if (FAILED(hrC)) { Log("C4-S3: 重建 ai=%d 新 list Close 失败 0x%08X（丢弃，保持失效）", ai, (unsigned)hrC); return false; }
    g_bbCl[ai] = cl;
    g_bbBad[ai] = false;
    Log("C4-S3: 重建 ai=%d 成功 list=%p allocReset=0x%08X newAlloc=%d", ai, (void*)cl.Get(), (unsigned)hrA, (int)newAl);
    return true;
}

// 2026-11-13 c4fix 阻断1：C4Kick = 非阻塞重入守卫 + C4KickLocked（原函数体，含懒建/录制/Execute/Signal/登记）。
//   HookPresentECL 的 CAS 成功即清 pending，AmdNrBbHandoff 随后可再置 1，另一条 ECL 线程就能在前一条
//   尚未退出时再进 C4Kick → g_bbAi 选同一套、并发 Reset/录制、g_bbFv/g_bbFvUsed 竞争、懒建重复执行。
//   守卫抢占失败：不等待（不能阻塞游戏提交线程），把 pending 用 CAS 0→1 恢复后返回：
//     · 理由：失败线程的 CAS 已把 Handoff 交来的新帧标记清掉，不恢复这一帧就永久丢了（B 端该槽不更新）；
//       恢复后由下一次 present 队列 ECL（仍在捕获之后，FIFO）处理最新 g_bbTex/g_bbFrame。
//     · 用 CAS 0→1 而非 Exchange：二者效果相同（都只会写 1），CAS 表明"只在已被清掉时补回"，不覆盖并发 Handoff。
//     · 代价：若在途线程已经读到了同一份 tex/frame，补回会让同一帧再提交一次（同内容覆写同一槽，多一次围栏），可接受。
static void C4KickLocked(ID3D12CommandQueue* q);
static void C4Kick(ID3D12CommandQueue* q) {
    // ★ v4 P15：C4Kick 闸门（第二道）。BbHandoff 已经拦过一次，这里再拦是为了覆盖
    //   "handoff 时还在 RUNNING、提交时已经切 STOPPING" 这一帧。禁用时把 pending 直接丢掉，
    //   不留给下一次 present 队列（否则恢复后会把一帧 UI 画面当 NR 底图 submit 上去）。
    if (!C4GateAllows()) {
        C4DropPending();
        C4DiagEarly(0, "ui_gate");
        return;
    }
    if (InterlockedCompareExchange(&g_c4InKick, 1, 0) != 0) {
        C4RestorePending();                                 // 补回被本线程 CAS 清掉的 pending（见上；v4：走锁）
        LONG n = InterlockedIncrement(&g_c4ReentrySkip);
        if (n == 1 || (n % 300) == 0) Log("C4-S3: KICK 重入被拒（已有线程在 C4Kick 内）次数=%ld，pending 已补回", n);
        C4DiagEarly(12, "reentry");
        return;
    }
    struct C4KickRelease { ~C4KickRelease() { InterlockedExchange(&g_c4InKick, 0); } } c4Release;   // RAII：所有出口释放
    C4KickLocked(q);
}

static void C4KickLocked(ID3D12CommandQueue* q) {
    // KICK 常态只留成功汇总；入口/早退详情继续通过 F6 HOTDIAG 查。
    if (!g_c4On || !q || !g_dev || !g_psoFmt8 || !g_heap) {
        C4DiagEarly(0, !g_c4On ? "c4_off" : !q ? "q_null" : !g_dev ? "dev_null" : !g_psoFmt8 ? "pso_null" : "heap_null");
        return;
    }
    // step2wm r2：水印读回/解码/失败隐藏只在 NGX + C4 下生效。lmxxf + C4 不写水印，
    // 走下面的基线路径：frame 用插件的 g_bbFrame，照常落槽、照常发布。
    if (g_c4DevLost) { C4DiagEarly(14, "dev_lost"); return; }       // 2026-11-13 c4fix 阻断3：设备已移除 → C4 停用
    // 2026-11-13 c4fix 阻断3：上次 Signal(g_bbFe) 失败欠下的围栏值，先在同一队列补打；补打成功才清零。
    //   补打失败 → 本帧直接返回，不 Reset 任何 allocator（否则三套都 Signal 失败后 used=1/2/3 而完成值永远 0，
    //   C4 永久停摆；此处补打的 Signal 排在该套 list 之后（同一 FIFO 队列），到达即说明该套已跑完）。
    if (g_bbSigOwed) {
        if (!g_bbFe || q != g_bbSigOwedQ) {
            static int ow = 0;
            if (ow++ < 5 || (ow % 300) == 0)
                Log("C4-S3: 欠围栏 %llu 无法补打（fe=%p 欠账队列=%p 当前队列=%p）→ 跳过本帧",
                    (unsigned long long)g_bbSigOwed, (void*)g_bbFe.Get(), (void*)g_bbSigOwedQ, (void*)q);
            C4DiagEarly(13, "sig_owed_queue");
            return;
        }
        HRESULT hrOwed = q->Signal(g_bbFe.Get(), g_bbSigOwed);
        static int os = 0;
        if (os++ < 20 || (os % 300) == 0)
            Log("C4-S3: 补打欠围栏 Signal(bbFe,%llu) hr=0x%08X", (unsigned long long)g_bbSigOwed, (unsigned)hrOwed);
        if (FAILED(hrOwed)) { C4DiagEarly(13, "sig_owed_retry_fail"); return; }
        g_bbSigOwed = 0; g_bbSigOwedQ = nullptr;
    }
    if (!XferEnsure()) { C4DiagEarly(1, "xfer_ensure"); return; }   // 建 g_xqFe2（共享围栏）
    ID3D12Resource* tex = (ID3D12Resource*)g_bbTex;
    if (!tex || !g_upBuf) { C4DiagEarly(2, !tex ? "tex_null" : "upbuf_null"); return; }
    if (!tex || !g_upBuf) { C4DiagEarly(2, !tex ? "tex_null" : "upbuf_null"); return; }
    // 2026-11-13 方案2 C4三套轮转：删除原来的"整体 busy 判定"（g_bbFv 是全局单值，上一帧没跑完就整帧跳过，
    //   就是 B 端画面冻住的原因之一）。改为先选套、只检查这一套的围栏。
    //   取套写法沿用 g_xqAi 先例（int ai = g_bbAi; …），唯一差别：闸门放行后才推进 g_bbAi ——
    //   早退时不推进索引，否则"跳过一帧"会连带跳过一套（3 套退化成 2 套）。
    // 2026-11-13 c4fix（可选改进）：从 g_bbAi 起有限扫描 3 套，取第一套"未登记或已被 fence 完成"的。
    //   正常轮转下 g_bbAi 就是最老的套，扫描第 1 个即命中；只有 Reset 失败/失效重建打乱顺序后才会落到后面的套。
    //   失效套（g_bbBad）同样参与：只有它已完成时才会被选中，选中后下面单独重建。
    //   GetCompletedValue 只取一次；UINT64_MAX = 设备已移除，单独识别（阻断3）。
    const UINT64 bbDone = g_bbFe ? g_bbFe->GetCompletedValue() : 0;
    if (C4FenceDevLost(bbDone)) return;
    int ai = -1;                                                  // 2026-11-13 方案2 C4三套轮转：本帧使用的套
    for (int k = 0; k < 3; ++k) {
        const int c = (g_bbAi + k) % 3;
        if (!g_bbFvUsed[c] || bbDone >= g_bbFvUsed[c]) { ai = c; break; }
    }
    if (ai < 0) { C4DiagEarly(3, "busy_fence"); return; }         // 三套都还没跑完才跳过（不推进索引）
    g_bbAi = (ai + 1) % 3;                                        // 闸门放行才推进（本帧占用该套；后续若早退，本帧无提交，推进无害）
    // 2026-11-14 c4outp3：确保 g_outp8[ai] 就绪（B8G8R8A8 + UAV，懒建；三缓冲与 allocator 对齐）
    if (!g_outp8Ready[ai]) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = g_outW; td.Height = g_outH;
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_outp8[ai]))) && g_outp8[ai]) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_B8G8R8A8_UNORM; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
            const UINT uavSlot = (ai == 0) ? kSrvOutp8 : (ai == 1) ? kSrvOutp8b : kSrvOutp8c;
            h.ptr += (SIZE_T)uavSlot * g_inc;
            g_dev->CreateUnorderedAccessView(g_outp8[ai].Get(), nullptr, &ud, h);
            g_outp8Ready[ai] = true;
            Log("C4-S3: g_outp8[%d] 就绪 %ux%u（c4outp3 三缓冲）", ai, g_outW, g_outH);
        }
    }
    if (!g_outp8Ready[ai] || !g_outp8[ai]) { C4DiagEarly(4, "outp8_not_ready"); return; }
    // ★ step2wm r2：发布时的 T 与颜色必须来自同一次采集。
    int frame = (int)g_frame - 1;   // nowm：直接用核心帧号，不依赖水印解码
    // r28 块2：r28=2 时 slot 由 R28Claim 决定（-1 = DUP/DROP_FULL/旧epoch）；r28<2 时原式不变
    int slot;
    bool noPublish = false;
    const bool r28Live = g_r28Mode_live();
    R28UnsubmittedClaim unsubmitted;
    if (r28Live) {
        slot = R28Claim(frame, &unsubmitted.gate);
        if (slot < 0) return;   // R28 申请失败：DUP/DROP_FULL/旧epoch，放弃本帧
        unsubmitted.slot = slot;
    } else {
        slot = (((frame % K) + K) % K);
    }
    // 确保 compute list + fence 就绪
    // ★ C4 修复（主因）：用 present 队列的【裸设备】建 list/fence，不要用 g_dev（ReShade 包装设备）——
    //   否则得到 ReShade 包装 list，提交给裸 present 队列 → 运行时把 wrapper 当内部 list 解析 → AV。
    //   资源（g_heap/g_psoFmt8/g_outp8/g_upBuf）本就是 native（ReShade 不包装 heap/resource/PSO/fence），裸 list 可直接用。
    // 2026-11-13 方案2 C4三套轮转：一次建满 3 套 allocator+list（每套建完立即 Close），fence 仍只建一个。
    //   判断条件用"第 3 套是否已建"（外加 fence 缺失）⇒ 任何一套创建失败时不会留下"建了一半就永远不再建"的
    //   状态：下次进来判断仍为 false，会整轮重建。
    // 2026-11-13 c4fix 阻断2：懒建判据改为 g_bbBuilt（不再用"g_bbCl[2] 非空"——单套重建期间会把某一套置空，
    //   不能因此触发整轮释放：另两套可能还在 GPU 上执行）。创建失败仍保持 g_bbBuilt=false，下次整轮重来。
    if (!g_bbBuilt) {
        for (int bi = 0; bi < 3; ++bi) {
            g_c4HeapBindings.sets[bi] = {};
            g_bbAl[bi].Reset(); g_bbCl[bi].Reset();   // 2026-11-13 方案2：重试前先放开上次建了一半的套（防重复建导致泄漏）
        }
        ComPtr<ID3D12Device> bareDev;
        if (FAILED(q->GetDevice(IID_PPV_ARGS(&bareDev))) || !bareDev) { C4DiagEarly(5, "bare_dev"); Log("C4-S3: 取 present 队列裸设备失败"); return; }
        UINT hr0[3] = { 0, 0, 0 };                                  // 2026-11-13 方案2：3 套的初始 Close 结果
        for (int bi = 0; bi < 3; ++bi) {                            // 2026-11-13 方案2：沿用 g_xqAl/g_xqCl 先例的"循环 3 次 Create+Close"
            if (FAILED(bareDev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_bbAl[bi])))) { C4DiagEarly(6, "bb_alloc"); Log("C4-S3: 建 allocator 失败 bi=%d", bi); return; }
            if (FAILED(bareDev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bbAl[bi].Get(), nullptr, IID_PPV_ARGS(&g_bbCl[bi])))) { C4DiagEarly(7, "bb_list"); Log("C4-S3: 建 list 失败 bi=%d", bi); return; }
            hr0[bi] = (UINT)g_bbCl[bi]->Close();                    // 2026-11-13 方案2：每套 list 建完立即 Close（Reset 前的合法状态）
            g_bbBad[bi] = false;                                    // 2026-11-13 c4fix：新建的套先视为有效
            if (FAILED((HRESULT)hr0[bi])) {                         // 2026-11-13 c4fix 阻断2：初建 Close 失败 → 该套失效（Close 失败的 list 永远不能再 Reset，只能重建）
                g_bbBad[bi] = true;
                Log("C4-S3: 初建 Close 失败 bi=%d hr=0x%08X → 标记失效，等轮到时单独重建", bi, hr0[bi]);
            }
            g_bbFvUsed[bi] = 0;                                     // 2026-11-13 方案2：新套尚未登记任何提交
        }
        if (FAILED(bareDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_bbFe)))) { C4DiagEarly(8, "bb_fence"); return; }
        g_bbBuilt = true;                                           // 2026-11-13 c4fix：整轮建成（个别套可能已标失效，由下面的单套重建处理）
        Log("C4-S3: 已用裸设备建 3 套 compute list + fence（bareDev=%p，list=%p/%p/%p，初始 Close hr=0x%08X/0x%08X/0x%08X）",
            (void*)bareDev.Get(), (void*)g_bbCl[0].Get(), (void*)g_bbCl[1].Get(), (void*)g_bbCl[2].Get(), hr0[0], hr0[1], hr0[2]);   // 2026-11-13 方案2：改为报告 3 套
    }
    // 2026-11-13 c4fix 阻断2：轮到失效套 → 其围栏已完成（上面的选套保证）才单独重建；重建失败跳过本帧。
    if (g_bbBad[ai]) {
        if (!C4RebuildSet(q, ai)) { C4DiagEarly(15, "bad_set_rebuild_fail"); return; }
    }
    C4EnsureBbSrv(tex, ai);                                        // 2026-11-13 c4srv：按套号 ai 写本套的 T_src SRV（原按 parity）
    {   // ★ F6 诊断：只抽样 Reset 生命周期，复用选套时的围栏快照。
        static unsigned rl = 0;
        const bool rlog = g_hotDiagOn && ((rl++ % 300) == 0);
        if (rlog) Log("C4-S3: KICK Reset 前 ai=%d al=%p cl=%p fe=%p fv=%llu fvUsed=%llu done=%llu frame=%d",
                      ai, (void*)g_bbAl[ai].Get(), (void*)g_bbCl[ai].Get(), (void*)g_bbFe.Get(),
                      (unsigned long long)g_bbFv, (unsigned long long)g_bbFvUsed[ai], (unsigned long long)bbDone, frame);
        HRESULT hrAl = g_bbAl[ai]->Reset();
        HRESULT hrCl = (SUCCEEDED(hrAl)) ? g_bbCl[ai]->Reset(g_bbAl[ai].Get(), nullptr) : hrAl;
        if (rlog) Log("C4-S3: KICK Reset 后 ai=%d hrAl=0x%08X hrCl=0x%08X（frame=%d）", ai, (unsigned)hrAl, (unsigned)hrCl, frame);
        if (FAILED(hrAl) || FAILED(hrCl)) {
            // 2026-11-13 c4fix 阻断2：删除原"无条件补救 Close"——对已关闭的 list 再 Close 返回 E_FAIL，
            //   对 Close 失败过的 list 也救不回（它永远不能再 Reset）。改为把该套标记失效，跳过本帧；
            //   下次轮到它且围栏已完成时由 C4RebuildSet 单独重建（allocator Reset 失败则连 allocator 一起重建）。
            g_bbBad[ai] = true;
            C4DiagEarly(9, "reset_fail");
            Log("C4-S3: KICK Reset 失败 ai=%d hrAl=0x%08X hrCl=0x%08X → 标记失效，等轮到时单独重建", ai, (unsigned)hrAl, (unsigned)hrCl);
            return;
        }
    }
    ID3D12GraphicsCommandList* cl = g_bbCl[ai].Get();              // 2026-11-13 方案2：录制用本套的 list
    const UINT srcSlot = C4BbSrcSlot(ai);                          // 2026-11-13 c4srv：读本套的 T_src SRV（原按 parity）
    const UINT uavSlot = (ai == 0) ? kSrvOutp8 : (ai == 1) ? kSrvOutp8b : kSrvOutp8c;  // 2026-11-14 c4outp3：UAV 按套号 ai 索引
    Barrier(cl, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_outp8[ai].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // ReShade 原生堆仍需通过四方 canonical COM identity 校验；只对相同对象复用结论。
    // Unknown（如临时 QI/GetDevice 失败）不缓存，沿用原 fallback；已确认的 mismatch 也走原 fallback。
    auto& cache = g_c4HeapBindings;
    ID3D12DescriptorHeap* const heap = g_heap.Get();
    ID3D12Device* const dev = g_dev.Get();
    if (cache.heap.Get() != heap || cache.queue.Get() != q || cache.dev.Get() != dev || cache.buildGen != g_buildGen) {
        for (auto& set : cache.sets) set = {};
        cache.heap = heap;
        cache.queue = q;
        cache.dev = dev;
        cache.buildGen = g_buildGen;
    }
    C4HeapBinding& binding = cache.sets[ai];
    if (binding.list != cl) {
        binding = {};
        binding.list = cl;
    }
    if (!binding.decided) {
        static const GUID kC4IidReShadeUnwrappedObject =
            { 0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };
        // canonical：对象若是 ReShade 代理，QI 私有 IID 得到 _orig；若 E_NOINTERFACE，视为对象本身即原生（别名限制：
        // 其他不转发该 IID 的包装层也会被视为“原生”，之后 IUnknown identity 比较不一致时落入 mismatch）。其它失败 => unknown。
        // 最后取 IID_IUnknown 作为 COM identity。out 为 AddRef 持有，ComPtr 配平。
        auto c4Canon = [](IUnknown* obj, ComPtr<IUnknown>& out) -> HRESULT {
            out.Reset();
            if (!obj) return E_POINTER;
            ComPtr<IUnknown> base;
            void* pBase = nullptr;
            HRESULT hrQ = obj->QueryInterface(kC4IidReShadeUnwrappedObject, &pBase);
            if (SUCCEEDED(hrQ) && pBase) {
                base.Attach(static_cast<IUnknown*>(pBase));
            } else {
                if (pBase) static_cast<IUnknown*>(pBase)->Release();
                if (hrQ != E_NOINTERFACE) return FAILED(hrQ) ? hrQ : E_FAIL;
                base = obj;
            }
            return base.As(&out);
        };
        void* pUnwrapped = nullptr;
        const HRESULT hrUnwrap = heap ? heap->QueryInterface(kC4IidReShadeUnwrappedObject, &pUnwrapped) : E_POINTER;
        ComPtr<ID3D12DescriptorHeap> c4Cand;
        if (SUCCEEDED(hrUnwrap) && pUnwrapped)
            c4Cand.Attach(static_cast<ID3D12DescriptorHeap*>(pUnwrapped));
        else if (pUnwrapped)
            static_cast<IUnknown*>(pUnwrapped)->Release();   // 防御：失败却给了指针时不泄漏，仍走原路径
        // 设备身份：heap 所属设备（候选原生 heap 的 GetDevice；无候选时用 g_heap 仅作日志）/ present 队列设备 / C4 list 设备 / g_dev
        ComPtr<ID3D12Device> c4HpDev, c4QDev, c4LDev;
        ID3D12DescriptorHeap* c4HpOwner = c4Cand ? c4Cand.Get() : heap;
        const HRESULT hrHpDev = c4HpOwner ? c4HpOwner->GetDevice(IID_PPV_ARGS(&c4HpDev)) : E_POINTER;
        const HRESULT hrQDev = q->GetDevice(IID_PPV_ARGS(&c4QDev));
        const HRESULT hrLDev = cl->GetDevice(IID_PPV_ARGS(&c4LDev));
        ComPtr<IUnknown> c4IdG, c4IdH, c4IdQ, c4IdL;
        const HRESULT hrIdG = c4Canon(dev, c4IdG);
        const HRESULT hrIdH = c4Canon(c4HpDev.Get(), c4IdH);
        const HRESULT hrIdQ = c4Canon(c4QDev.Get(), c4IdQ);
        const HRESULT hrIdL = c4Canon(c4LDev.Get(), c4IdL);
        const bool c4IdKnown = SUCCEEDED(hrIdG) && SUCCEEDED(hrIdH) && SUCCEEDED(hrIdQ) && SUCCEEDED(hrIdL)
                               && c4IdG && c4IdH && c4IdQ && c4IdL;
        const bool c4IdMatch = c4IdKnown && c4IdG.Get() == c4IdH.Get() && c4IdG.Get() == c4IdQ.Get() && c4IdG.Get() == c4IdL.Get();
        const bool c4UseUnwrapped = (c4Cand != nullptr) && c4IdMatch;
        if (c4UseUnwrapped) binding.native = c4Cand;
        // E_NOINTERFACE 是稳定结论；其余未知结果允许下次重试，不误记为通过或确定不匹配。
        binding.decided = (c4Cand && c4IdKnown) || (hrUnwrap == E_NOINTERFACE && !pUnwrapped);
        const unsigned probe = ++binding.probes;
        if (probe == 1 || binding.decided || (g_hotDiagOn && (probe % 300) == 0)) {
            Log("C4-PRESENT-TRIM: heap-unwrap ai=%d gen=%d heap=%p unwrapHr=0x%08X candidate=%p identity=%s path=%s cached=%d",
                ai, cache.buildGen, (void*)heap, (unsigned)hrUnwrap, (void*)c4Cand.Get(),
                c4IdKnown ? (c4IdMatch ? "match" : "mismatch") : "unknown",
                c4UseUnwrapped ? "native(unwrapped)" : "fallback(g_heap)", (int)binding.decided);
            Log("C4-S3: heap-unwrap 设备 raw/canon g_dev=%p/%p heapDev=%p/%p qDev=%p/%p listDev=%p/%p hr(heapDev,qDev,listDev)=0x%08X,0x%08X,0x%08X canonHr(g,h,q,l)=0x%08X,0x%08X,0x%08X,0x%08X",
                (void*)dev, (void*)c4IdG.Get(), (void*)c4HpDev.Get(), (void*)c4IdH.Get(),
                (void*)c4QDev.Get(), (void*)c4IdQ.Get(), (void*)c4LDev.Get(), (void*)c4IdL.Get(),
                (unsigned)hrHpDev, (unsigned)hrQDev, (unsigned)hrLDev,
                (unsigned)hrIdG, (unsigned)hrIdH, (unsigned)hrIdQ, (unsigned)hrIdL);
        }
    }
    ID3D12DescriptorHeap* hh[] = { binding.native ? binding.native.Get() : heap };
    cl->SetDescriptorHeaps(1, hh);
    SetPass(cl, g_psoFmt8.Get(), srcSlot, srcSlot, srcSlot, srcSlot, srcSlot, uavSlot, uavSlot, g_outW, g_outH);
    Barrier(cl, g_outp8[ai].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);   // 2026-11-14 c4outp3
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_outp8[ai].Get();   // 2026-11-14 c4outp3：拷贝源改用 ai 索引
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    // step2wm r2（H1）：落槽的颜色必须和已解出的 T 来自同一次采集。
    //   wmtOn 且解出了 T：源是 g_outp8Hold[ai]（录上一次读回时另存的那份），不是刚被 SetPass 覆盖的 g_outp8[ai]。
    //   lmxxf（wmtOn=false）：源就是 g_outp8[ai]，与基线一致。
    //   wmtOnly：本帧解不出，不碰任何槽（否则槽内容与旧面单帧号错配）。
    if (!noPublish) {
    Barrier(cl, g_upBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_upBuf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = (UINT64)slot * g_upSlotBytes + g_upOffC;
    dst.PlacedFootprint.Footprint.Format = g_upFmtC;
    dst.PlacedFootprint.Footprint.Width = g_upW;
    dst.PlacedFootprint.Footprint.Height = g_upH;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g_upPitchC;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    // r28 块4：MV 入槽 —— g_mvHist[frame%8] → g_upBuf(slot, g_upOffM)
    // 在录入 MV 拷贝前用同一帧的专用 fence 排 GPU Wait；失败则只发布颜色，B 取零 MV。
    if (r28Live) g_r28[slot].mvOk = 0;
    if (r28Live && InterlockedCompareExchange(&g_r28MvEn, 0, 0)) {
        const MvFenceRec mv = MvFenceFor((LONG)frame);
        bool mvReady = false;
        if (mv.v && g_upMw && g_upMh && g_upPitchM) {
            const UINT64 done = mv.fence->GetCompletedValue();
            const HRESULT hr = (done == UINT64_MAX) ? DXGI_ERROR_DEVICE_REMOVED
                : (done >= mv.v) ? S_OK : q->Wait(mv.fence, mv.v);
            mvReady = SUCCEEDED(hr);
            if (!mvReady) {
                const LONG fails = InterlockedIncrement(&g_mvPairCntWaitFail);
                if (fails <= 5 || fails % 300 == 0)
                    Log("MVPAIR: C4 Wait 失败 frame=%d v=%llu hr=0x%08X → 零 MV", frame, (unsigned long long)mv.v, (unsigned)hr);
            }
        }
        if (mvReady) {
            InterlockedIncrement(&g_mvPairCntC4Ok);
            D3D12_TEXTURE_COPY_LOCATION mvDst{};
            mvDst.pResource = g_upBuf.Get();
            mvDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            mvDst.PlacedFootprint.Offset = (UINT64)slot * g_upSlotBytes + g_upOffM;
            mvDst.PlacedFootprint.Footprint.Format = g_upFmtM;
            mvDst.PlacedFootprint.Footprint.Width  = g_upMw;
            mvDst.PlacedFootprint.Footprint.Height = g_upMh;
            mvDst.PlacedFootprint.Footprint.Depth  = 1;
            mvDst.PlacedFootprint.Footprint.RowPitch = g_upPitchM;
            D3D12_TEXTURE_COPY_LOCATION mvSrc{};
            mvSrc.pResource = g_mvHist[frame & 7].Get();
            mvSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            mvSrc.SubresourceIndex = 0;
            Barrier(cl, g_mvHist[frame & 7].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl->CopyTextureRegion(&mvDst, 0, 0, 0, &mvSrc, nullptr);
            Barrier(cl, g_mvHist[frame & 7].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            g_r28[slot].mvOk = 1;
        } else {
            InterlockedIncrement(&g_mvPairCntC4Rej);
        }
    }
    }
    if (!noPublish) Barrier(cl, g_upBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_outp8[ai].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    // 成功的 Close/提交细节仅在 F6 诊断时抽样；实际失败仍走原处理。
    static unsigned c4KickLogCount = 0;
    const bool c4KickRlog = g_hotDiagOn && ((c4KickLogCount++ % 300) == 0);
    if (c4KickRlog) Log("C4-S3: KICK 录制完毕，准备 Close（frame=%d）", frame);
    HRESULT hrClose = cl->Close();
    if (c4KickRlog) Log("C4-S3: KICK Close hr=0x%08X（frame=%d）", (unsigned)hrClose, frame);
    if (FAILED(hrClose)) {                                   // 2026-11-13 c4fix 阻断2：录制 Close 失败 → 该套失效（不可再 Reset，只能重建）
        g_bbBad[ai] = true;
        Log("C4-S3: KICK Close 失败 ai=%d hr=0x%08X → 标记失效，等轮到时单独重建", ai, (unsigned)hrClose);
        C4DiagEarly(10, "close_fail");
        return;
    }
    if (c4KickRlog) Log("C4-S3: KICK 录制完成，准备提交（frame=%d slot=%d）", frame, slot);
    ID3D12CommandList* ls[] = { cl };

    if (r28Live) {
        // 给切代封存记录本次写入的退休值；下一次 Signal(bbFe) 使用同一个值。
        g_r28[slot].bvProducer = g_bbFv + 1;
        unsubmitted.slot = -1;
    }
    q->ExecuteCommandLists(1, ls);                          // 提交到 present 队列（FIFO，晚于捕获）
    if (c4KickRlog) Log("C4-S3: KICK 已提交（frame=%d）", frame);
    UINT64 v2 = 0;
    HRESULT hrSigFe2 = S_OK;
    if (!noPublish) {   // step2wm：wmtOnly/noPublish 帧没写槽，不推进共享就绪围栏
        v2 = ++g_xqFv2;
        hrSigFe2 = q->Signal(g_xqFe2.Get(), v2);            // 共享围栏通知 B（★ HOTDIAG：保存返回值；调用顺序/参数不变）
    }
    UINT64 bv = ++g_bbFv;
    HRESULT hrSigBb = q->Signal(g_bbFe.Get(), bv);           // 自己的完成围栏（下帧复用分配器前查）
    // 2026-11-13 方案2 C4三套轮转：把本套登记为 bv。Signal 成功时这是 GPU 真正会到达的值；
    //   Signal 失败时也登记 bv、绝不置 0 —— 置 0 等于宣称"该套无在途提交"，下一轮闸门会放行并 Reset 一个
    //   可能仍在用的 allocator。
    // 2026-11-13 c4fix 阻断3：Signal 失败时没有任何后续机制会把完成值推过 bv（若三套都失败，used=1/2/3、
    //   完成值 0，之后永远 busy、再无提交和 Signal → C4 永久停摆）。因此把 bv 记为欠账 g_bbSigOwed，
    //   下一次 C4Kick 在 busy 判断之前先在同一队列补打 Signal(g_bbFe, 欠账)，成功才清零、失败则本帧不做任何 Reset。
    g_bbFvUsed[ai] = bv;                                     // 2026-11-13 方案2：登记本套的围栏值（选中该套时据此判 busy）
    if (FAILED(hrSigBb)) {
        if (bv > g_bbSigOwed) g_bbSigOwed = bv;              // 单调围栏：补打最大的欠账值即覆盖所有更小的
        g_bbSigOwedQ = q;
        Log("C4-S3: KICK Signal(bbFe,%llu) 失败 hr=0x%08X ai=%d → 记欠账，下次 KICK 先补打", (unsigned long long)bv, (unsigned)hrSigBb, ai);
    }
    if (c4KickRlog)
        Log("C4-S3: KICK Signal(bbFe) hr=0x%08X bv=%llu frame=%d", (unsigned)hrSigBb, (unsigned long long)bv, frame);
    // r28 块2 单一事务发布：先写令牌/诊断字段，再 CAS COLW→READY 或 SEALED
    // 注意：UiPublishSlot 两条路径都要调（令牌 valid 字段供 B 侧 UiTokenValid）
    g_slotReadyFv[slot] = v2;   // 诊断/r28<2 读；r28=2 也要写
    // 发布槽位
    UiPublishSlot((int)slot, frame, g_outp8[ai].Get(), v2, 0.0f, 0.0f);
    if (r28Live) {
        if (FAILED(hrSigFe2)) R28Seal(slot, bv);
        else                  R28PublishReady(slot, v2);
    }
    g_upLastSlot = slot;
    R28ShKick(frame, v2, true);   // r28 块1：影子记账 T / v2 / DUP / 成对 KICK（不改 slot、不改发布）
    if (g_hotDiagOn) {   // ★ HOTDIAG：成功 KICK 低频记录（每 300 次一条；Signal 失败首次及每 300 次一条）
        g_c4LastOkFrame = frame; g_c4LastOkSlot = slot;
        static volatile LONG okn = 0, sigFail = 0;
        LONG n = InterlockedIncrement(&okn);
        if (FAILED(hrSigFe2)) {
            LONG f = InterlockedIncrement(&sigFail);
            if (f == 1 || (f % 300) == 0)
                Log("HOTDIAG C4SIGFAIL n=%ld frame=%d v2=%llu hr=0x%08X fail=%ld",
                    n, frame, (unsigned long long)v2, (unsigned)hrSigFe2, f);
        }
        if (n == 1 || (n % 300) == 0)
            Log("HOTDIAG C4OK n=%ld frame=%d slot=%d v2=%llu hrSigFe2=0x%08X",
                n, frame, slot, (unsigned long long)v2, (unsigned)hrSigFe2);
    }
    static unsigned cn = 0;
    if ((cn++ % 300) == 0) Log("C4-S3: 已落槽 frame=%d slot=%d 就绪围栏=%llu", frame, slot, (unsigned long long)v2);
}

// 在钩子里判断：这次提交的 list 里是否含"我们录过转换的那条" → 是则 Signal + 启动我们队列上的颜色拷贝
// 改为"每次提交都打一个记号"：不再依赖 list 指针身份（实测游戏提交的 list 与我们录的不是同一对象）
//   记号只会往后排 ⇒ 只要等"最新一次提交的记号"，就能保证此前已入队的换算全都执行完
static void XferMaybeKick(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    static volatile LONG dq = 0; if (InterlockedIncrement(&dq) <= 3) Log("XFERDBG3: XferMaybeKick 被调用 | xqFe=%p xferOwn=%d", (void*)g_xqFe.Get(), (int)g_xferOwn);
    if (!g_xqFe) return;
    // 2026-11-19 mvpair：g_worker.q / g_bpQ 建在 worker 设备（9070）上，对游戏设备的 g_xqFe Signal 必然
    //   E_INVALIDARG（日志 0x80070057 实证），也从不提交含游戏 MV 拷贝的 list。直接跳过：不占 g_xqFv 号、
    //   不改 g_lastSigV（旧行为里失败的 v 永远不会到达，只是被后面更大的值顺带覆盖），因此无任何依赖受损。
    //   g_xqQ 在游戏设备上，旧颜色记号照旧；MV 由 HookECL 独立配对。
    if (q == g_worker.q.Get() || q == g_bpQ.Get()) return;
    UINT64 v = (UINT64)InterlockedIncrement64((volatile LONG64*)&g_xqFv);
    HRESULT hrSig = q->Signal(g_xqFe.Get(), v);
    {
        int om = 0;
        if (lists) for (UINT i = 0; i < n && !om; ++i)
            if ((ID3D12CommandList*)lists[i] == (ID3D12CommandList*)g_ourList) om = 1;
        BtTrace("QUEUE_SIGNAL", g_frame, "dispatch",
                "queue=0x%llx fence=0x%llx side=g value=%llu hr=0x%08lX n=%u our_list_match=%d",
                BtPtr(q), BtPtr(g_xqFe.Get()), (unsigned long long)v, (unsigned long)hrSig, n, om);
    }
    g_lastSigV = v;
}
// 游戏内共享可行性探测：游戏设备 ↔ worker 设备
// ===== S1：共享纹理基础（只创建，不使用；行为零变化）=====
static volatile bool g_useShared = false;         // 总开关：true 才走共享纹理路径
static bool g_shareOk = false;
static ComPtr<ID3D12Resource> g_shareIn[K],  g_shareInW[K];    // 1080p 输入（游戏侧/worker侧视图）
static ComPtr<ID3D12Resource> g_shareAns[K], g_shareAnsW[K];   // 1080p 答案

// ===== R2：跨卡共享 buffer（颜色进 / 答案出）=====
          // 头部 512B，载荷从 +512 开始

static bool MakeXBuf(UINT64 bytes, const char* tag,
                     ComPtr<ID3D12Heap>& hA, ComPtr<ID3D12Resource>& bA,
                     ComPtr<ID3D12Heap>& hB, ComPtr<ID3D12Resource>& bB) {
    const UINT64 ALIGN = 65536;
    UINT64 sz = ((bytes + ALIGN - 1) / ALIGN) * ALIGN;
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = sz;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Properties.CreationNodeMask = 1; hd.Properties.VisibleNodeMask = 1;
    hd.Alignment = ALIGN;
    hd.Flags = (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);
    HRESULT h = g_dev->CreateHeap(&hd, IID_PPV_ARGS(hA.GetAddressOf()));
    if (FAILED(h)) { Log("R2 %s: 建堆 0x%08X", tag, (unsigned)h); return false; }
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = sz; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    h = g_dev->CreatePlacedResource(hA.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(bA.GetAddressOf()));
    if (FAILED(h)) { Log("R2 %s: 游戏侧 placed 0x%08X", tag, (unsigned)h); return false; }
    HANDLE sh = nullptr;
    h = g_dev->CreateSharedHandle(hA.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
    if (FAILED(h)) { Log("R2 %s: 导出 0x%08X", tag, (unsigned)h); return false; }
    h = g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(hB.GetAddressOf()));
    CloseHandle(sh);
    if (FAILED(h)) { Log("R2 %s: worker 打开 0x%08X", tag, (unsigned)h); return false; }
    h = g_worker.dev->CreatePlacedResource(hB.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(bB.GetAddressOf()));
    if (FAILED(h)) { Log("R2 %s: worker 侧 placed 0x%08X", tag, (unsigned)h); return false; }
    Log("R2 %s: OK (%llu 字节)", tag, (unsigned long long)sz);
    return true;
}

static ID3D12Resource* A2EnsureMvScratch(bool release = false) {
    static ComPtr<ID3D12DescriptorHeap> bound;
    static ComPtr<ID3D12Resource> packed;
    if (release) { packed.Reset(); bound.Reset(); return nullptr; }
    if (bound.Get() == g_heap.Get() && packed) return packed.Get();
    packed = MakeTex(g_dev.Get(), kNetW, kNetH, DXGI_FORMAT_R16G16_FLOAT,
                     D3D12_RESOURCE_STATE_COMMON, true);
    if (!packed) { bound.Reset(); return nullptr; }
    bound = g_heap;
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = DXGI_FORMAT_R16G16_FLOAT; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(kSrvA2Mv) * g_inc;
    g_dev->CreateUnorderedAccessView(packed.Get(), nullptr, &u, h);
    return packed.Get();
}

static bool A2InitDirect() {
    ComPtr<IUnknown> gBase, wBase;
    ComPtr<ID3D12Device> gNative, wNative;
    if (FAILED(F9Id::Canon(g_dev.Get(), gBase, A2In::gameDevice)) || FAILED(gBase.As(&gNative)) ||
        FAILED(F9Id::Canon(g_worker.dev.Get(), wBase, A2In::workerDevice)) || FAILED(wBase.As(&wNative))) return false;
    bool newlyCreated[A2In::kSlots]{};
    unsigned readySlots = 0;
    auto rollback = [&] {
        for (int i = 0; i < A2In::kSlots; ++i) if (newlyCreated[i]) A2In::ReleaseSlotBuffers(A2In::slots[i]);
    };
    for (int i = 0; i < A2In::kSlots; ++i) {
        auto& p = A2In::slots[i];
        if (A2In::S(p.gate.load()) != A2In::Free) continue;
        if (p.bufA && p.bufW && p.mirror && p.marker && p.prodA && p.prodW) { ++readySlots; continue; }
        newlyCreated[i] = true;
        if (!MakeXBuf(A2In::ColorPlane() + A2In::MvPlane(), "A2 direct pair", p.heapA, p.bufA, p.heapW, p.bufW) ||
            !F9Id::Buffer(gNative.Get(), A2In::ColorPlane() + sizeof(F9Id::Stamp), false, p.mirror) ||
            !F9Id::Buffer(gNative.Get(), sizeof(F9Id::Stamp), true, p.marker)) { rollback(); return false; }
        auto hr = gNative->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                                       IID_PPV_ARGS(&p.prodA));
        HANDLE sh = nullptr;
        if (SUCCEEDED(hr)) hr = gNative->CreateSharedHandle(p.prodA.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
        if (SUCCEEDED(hr)) hr = wNative->OpenSharedHandle(sh, IID_PPV_ARGS(&p.prodW));
        if (sh) CloseHandle(sh);
        if (FAILED(hr)) { Log("A2-INPUT: cross-adapter producer fence setup hr=%08X", (unsigned)hr); rollback(); return false; }
        if (F9Id::DeviceId(p.prodA.Get()).Get() != A2In::gameDevice.Get() ||
            F9Id::DeviceId(p.bufA.Get()).Get() != A2In::gameDevice.Get() ||
            F9Id::DeviceId(p.bufW.Get()).Get() != A2In::workerDevice.Get()) { rollback(); return false; }
        ++readySlots;
    }
    return readySlots > 0;
}

static void XAdapterInit() {
    if (!g_dev || !g_worker.dev) return;
    LUID la = g_dev->GetAdapterLuid();
    LUID lb = g_worker.dev->GetAdapterLuid();
    const bool sameCard = (la.HighPart == lb.HighPart && la.LowPart == lb.LowPart);
    Log("R2: 游戏设备 LUID=%08X:%08X | worker LUID=%08X:%08X | 同卡=%d",
        (unsigned)la.HighPart, (unsigned)la.LowPart, (unsigned)lb.HighPart, (unsigned)lb.LowPart, (int)sameCard);
    const UINT64 PAY = AnsBytes();
    const bool a2Req = g_backendNgx && !sameCard && !g_cpuIn;
    A2In::requested.store(a2Req);
    // Avoid keeping unused legacy 16 MiB input/MV buffers when A2 direct path is requested.
    bool a = a2Req ? true : MakeXBuf(PAY, "颜色进", g_xHeapIn,  g_xBufIn,  g_xHeapInW,  g_xBufInW);   // RE2 原样：单段
    bool b = MakeXBuf(XHDR + PAY * (UINT64)K, "答案出(K段)", g_xHeapOut, g_xBufOut, g_xHeapOutW, g_xBufOutW);
    if (g_backendNgx && !a2Req) {   // legacy NGX only: extra motion-vector channel
        if (MakeXBuf(PAY, "MV in", g_xHeapVel, g_xBufVel, g_xHeapVelW, g_xBufVelW))
            Log("NGX: MV cross-card channel ready (%llu bytes)", (unsigned long long)PAY);
        else
            Log("!! NGX: MV cross-card channel failed -> MV not fed");
    }
    if (!g_xFence) {
        HRESULT h1 = g_dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(g_xFence.GetAddressOf()));
        HRESULT h2 = E_FAIL, h3 = E_FAIL;
        if (SUCCEEDED(h1)) {
            HANDLE sh = nullptr;
            h2 = g_dev->CreateSharedHandle(g_xFence.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
            if (SUCCEEDED(h2)) { h3 = g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(g_xFenceW.GetAddressOf())); CloseHandle(sh); }
        }
        Log("R2: shared fence create=0x%08X export=0x%08X open=0x%08X", (unsigned)h1, (unsigned)h2, (unsigned)h3);
    }
    g_xOk = a && b;
    if (a2Req) {
        ComPtr<IUnknown> gBase, wBase;
        F9Id::Canon(g_dev.Get(), gBase, A2In::gameDevice);
        F9Id::Canon(g_worker.dev.Get(), wBase, A2In::workerDevice);
        if (!g_bformOn) {
            A2In::initAttempted = true;
            A2In::initialized.store(g_xOk && g_xRead && g_xWrite && A2InitDirect());
            Log("A2-INPUT-DIRECT: %s; %d independent pair slots, MV_expected=feedFrame; drops use native FSR, no legacy fallback",
                A2In::initialized.load() ? "available (submit and fence required)" : "unavailable", A2In::kSlots);
        } else {
            A2In::initAttempted = false;
            A2In::initialized.store(false);
            Log("A2-INPUT-DIRECT: deferred while B-form active (on-demand allocation)");
        }
    }
    Log("R2 结果: buffer=%d net=%ux%u", (int)g_xOk, kNetW, kNetH);
    // S5-4：本地中转 buffer（普通 buffer 才能建 SRV）
    if (g_xOk && g_heap) {
        D3D12_HEAP_PROPERTIES lh{};
        lh.Type = D3D12_HEAP_TYPE_DEFAULT;
        lh.CreationNodeMask = 1; lh.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC ld{};
        ld.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ld.Width = AnsBytes(); ld.Height = 1; ld.DepthOrArraySize = 1; ld.MipLevels = 1;
        ld.Format = DXGI_FORMAT_UNKNOWN; ld.SampleDesc.Count = 1;
        ld.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        g_ansLocal.Reset();
        HRESULT hl = g_dev->CreateCommittedResource(&lh, D3D12_HEAP_FLAG_NONE, &ld,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(g_ansLocal.GetAddressOf()));
        if (SUCCEEDED(hl)) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.FirstElement = 0;
            sd.Buffer.NumElements = (UINT)(AnsBytes() / 4);
            sd.Buffer.StructureByteStride = 0;
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            D3D12_CPU_DESCRIPTOR_HANDLE dh = g_heap->GetCPUDescriptorHandleForHeapStart();
            dh.ptr += (SIZE_T)kSrvAns * g_inc;
            g_dev->CreateShaderResourceView(g_ansLocal.Get(), &sd, dh);
            Log("S5-4: 本地中转 buffer %llu 字节 + SRV（槽 %d）已建", (unsigned long long)AnsBytes(), (int)kSrvAns);
        } else {
            Log("S5-4: 本地 buffer 建失败 0x%08X", (unsigned)hl);
        }
    }
    if (g_xOk) {
        D3D12_HEAP_PROPERTIES uh{};
        uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        uh.CreationNodeMask = 1; uh.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC ud{};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = XHDR; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.SampleDesc.Count = 1;
        ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        g_xHdrUp.Reset();
        if (SUCCEEDED(g_dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &ud,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(g_xHdrUp.GetAddressOf())))) {
            void* pm = nullptr;
            D3D12_RANGE zr{ 0, 0 };
            if (SUCCEEDED(g_xHdrUp->Map(0, &zr, &pm)) && pm) {
                struct XH { unsigned int magic, ver, w, h, fmt, pitch, frame, pad; };
                XH* hh = (XH*)pm;
                hh->magic = 0x31524E58u; hh->ver = 1; hh->w = kNetW; hh->h = kNetH;
                hh->fmt = 28; hh->pitch = kNetW * 4; hh->frame = 0; hh->pad = 0;
                Log("R3: 头部已写 magic=XNR1 %ux%u pitch=%u 头部=%d B", kNetW, kNetH, kNetW * 4, (int)XHDR);
            }
        }
    }
}

static bool ShareInit() {
    if (!g_dev || !g_worker.dev) { Log("S1: 设备未就绪，跳过"); return false; }
    auto mkShared = [&](ComPtr<ID3D12Resource>& own, ComPtr<ID3D12Resource>& peer, const char* tag) -> bool {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kNetW; td.Height = kNetH; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT h = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(own.GetAddressOf()));
        if (FAILED(h)) { Log("S1: %s 建失败 0x%08X", tag, (unsigned)h); return false; }
        HANDLE sh = nullptr;
        h = g_dev->CreateSharedHandle(own.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
        if (FAILED(h)) { Log("S1: %s 导出失败 0x%08X", tag, (unsigned)h); return false; }
        h = g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(peer.GetAddressOf()));
        CloseHandle(sh);
        if (FAILED(h)) { Log("S1: %s 对端打开失败 0x%08X", tag, (unsigned)h); return false; }
        return true;
    };
    if (!g_useShared) { Log("S1: 共享纹理跳过（跨卡下不可用）"); return false; }
    int okIn = 0, okAns = 0;
    for (int i = 0; i < K; ++i) {
        if (mkShared(g_shareIn[i],  g_shareInW[i],  "输入纹理")) okIn++;
        if (mkShared(g_shareAns[i], g_shareAnsW[i], "答案纹理")) okAns++;
    }
    // 共享 fence 探测（S6 会用到）
    HRESULT hf = E_FAIL, hf2 = E_FAIL, hf3 = E_FAIL;
    {
        ComPtr<ID3D12Fence> f1, f2;
        hf = g_dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(f1.GetAddressOf()));
        if (SUCCEEDED(hf)) {
            HANDLE sh = nullptr;
            hf2 = g_dev->CreateSharedHandle(f1.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
            if (SUCCEEDED(hf2)) { hf3 = g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(f2.GetAddressOf())); CloseHandle(sh); }
        }
    }
    g_shareOk = (okIn == K && okAns == K);
    Log("S1: 共享纹理 输入 %d/%d 答案 %d/%d | 共享fence 建=0x%08X 导出=0x%08X 打开=0x%08X | g_useShared=%d",
        okIn, K, okAns, K, (unsigned)hf, (unsigned)hf2, (unsigned)hf3, (int)g_useShared);
    return g_shareOk;
}
static void TestSharedInGame() {
    if (!g_dev || !g_worker.dev) return;
    auto luidOf = [](ID3D12Device* d) -> LUID {
        LUID l{};
        Microsoft::WRL::ComPtr<IDXGIDevice> gd;
        if (d && SUCCEEDED(d->QueryInterface(__uuidof(IDXGIDevice), (void**)gd.GetAddressOf()))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
            if (SUCCEEDED(gd->GetAdapter(ad.GetAddressOf()))) { DXGI_ADAPTER_DESC dd{}; ad->GetDesc(&dd); l = dd.AdapterLuid; }
        }
        return l;
    };
    LUID la = luidOf(g_dev.Get());
    LUID lb = luidOf(g_worker.dev.Get());
    Log("共享探测: 游戏设备 LUID=%08X:%08X | worker LUID=%08X:%08X | 同卡=%d",
        (unsigned)la.HighPart, (unsigned)la.LowPart, (unsigned)lb.HighPart, (unsigned)lb.LowPart,
        (int)(la.HighPart == lb.HighPart && la.LowPart == lb.LowPart));

    // ① buffer：普通 SHARED
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 4u << 20; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        HRESULT h1 = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &bd,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
        HANDLE sh = nullptr;
        HRESULT h2 = SUCCEEDED(h1) ? g_dev->CreateSharedHandle(r.Get(), nullptr, GENERIC_ALL, nullptr, &sh) : h1;
        ComPtr<ID3D12Resource> r2;
        HRESULT h3 = SUCCEEDED(h2) ? g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(r2.GetAddressOf())) : h2;
        if (sh) CloseHandle(sh);
        Log("共享探测① buffer: 建=0x%08X 导出=0x%08X 对端打开=0x%08X", (unsigned)h1, (unsigned)h2, (unsigned)h3);
    }
    // ② 纹理：1920x1080 RGBA8（就是我们要传的颜色尺寸）
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 1920; td.Height = 1080; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        HRESULT h1 = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
        HANDLE sh = nullptr;
        HRESULT h2 = SUCCEEDED(h1) ? g_dev->CreateSharedHandle(r.Get(), nullptr, GENERIC_ALL, nullptr, &sh) : h1;
        ComPtr<ID3D12Resource> r2;
        HRESULT h3 = SUCCEEDED(h2) ? g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(r2.GetAddressOf())) : h2;
        if (sh) CloseHandle(sh);
        Log("共享探测② 纹理1920x1080: 建=0x%08X 导出=0x%08X 对端打开=0x%08X", (unsigned)h1, (unsigned)h2, (unsigned)h3);
    }
}
static bool ProxyImGuiInit();
// ---------------- S1 镜像叠层（定义在文件末尾；这里只前置声明） ----------------
static void MirrorTick(ID3D12GraphicsCommandList* cl, ID3D12Resource* outp,
                       D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_COMMON);

// B1 开关：xess_proxy.resid4k.txt 里写 1/0（每 300 帧读一次）
static void PollResid4K() {
    static int s_last = -1;
    std::string p = ModuleDir() + "xess_proxy.resid4k.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "r") != 0 || !f) return;
    char b[8] = { 0 };
    size_t n = fread(b, 1, 7, f);
    fclose(f);
    if (!n) return;
    int v = (b[0] == '1') ? 1 : 0;
    if (v == s_last) return;
    s_last = v;
    if ((v != 0) != g_resid4k) {
        g_resid4k = (v != 0);
        Log("B1: 残差落点 -> %s（帧 %d）", g_resid4k ? "4K(上采样后)" : "pColor(上采样前)", g_frame);
    }
}

// 可选：xess_proxy.cpuin.txt=1 → 输入改走 CPU 回读路径（诊断 GPU 快路径是否差一帧）
static void CpuInFromFile() {
    static bool done = false;
    if (done) return;
    done = true;
    std::string p = ModuleDir() + "xess_proxy.cpuin.txt";
    FILE* f = nullptr; char b[8] = { 0 };
    if (fopen_s(&f, p.c_str(), "r") == 0 && f) {
        if (fgets(b, sizeof(b), f) && b[0] == '1') { g_cpuIn = true; g_xRead = false; g_xWrite = false; }
        fclose(f);
    }
    Log("输入路径: %s", g_cpuIn ? "CPU 回读（cpuin=1）" : "GPU 共享缓冲快路径");
}

// 可选：游戏目录放 xess_proxy.nrstart.txt，内容 0 → 初始不接管 NR（进场景后按 F10 再开）
static bool g_nrStartChecked = false;
static void NrStartFromFile() {
    if (g_nrStartChecked) return;
    g_nrStartChecked = true;
    std::string p = ModuleDir() + "xess_proxy.nrstart.txt";
    FILE* f = nullptr; char b[8] = { 0 };
    if (fopen_s(&f, p.c_str(), "r") == 0 && f) {
        if (fgets(b, sizeof(b), f) && b[0] == '0') { g_nrOn = false; Log("启动: xess_proxy.nrstart.txt=0 → NR 初始关闭（进场景后按 F10 开）"); }
        fclose(f);
    }
}

// ===== C4 方案A · S0：addon 把 present 队列原生指针交给核心（只记录，不改时序）=====
// 2077 交换链所属那条 present 队列（DIRECT）；S1 会换挂它的 ExecuteCommandLists 虚表。
// 开关：无。addon 每帧在 on_present 调 AmdNrSetPresentQueue(queue->get_native())，失败无害。
extern "C" __declspec(dllexport) void AmdNrSetPresentQueue(void* queue) {
    g_presentQueue = queue;
    InstallPresentQueueHook();   // S1：首次交到即换挂 ECL 虚表（观察钩子，幂等）
    static int n = 0;
    if ((n++ % 60) == 0) Log("C4-S0: present 队列 = %p（收到 %d 次）", queue, n);
}

// ===== C4 方案A · S2：三导出（NeedFinal 门 / BbHandoff 交接 / BbSlotFree 归还）=====
// S2 仍是观察级：只记录 + log，不录 compute（compute 在 S3）。
extern "C" __declspec(dllexport) int AmdNrNeedFinal() {
    // B 形态才需要 present 最终画面；A 形态走 pOutput。S3 会细化 stage 门槛。
    return g_bformOn ? 1 : 0;
}
extern "C" __declspec(dllexport) int AmdNrBbHandoff(void* cl, void* tex, unsigned frameTag) {
    // ★ v4 P13：BbHandoff 闸门（第一道）。UI 停止期间不再登记 pending，并清掉可能残留的旧 pending。
    //   返回 1 而不是 0：addon 侧把返回值当"交接是否被接受"，这里拒收只在内部生效，
    //   不能让它以为链路出错（否则 addon 会走自己的失败路径，行为反而不可控）。
    if (!C4GateAllows()) {
        C4DropPending();
        static int sk = 0;
        if ((sk++ % 60) == 0) Log("C4-S2: BbHandoff 被 UI 闸门拒绝（state 停止/禁用），本帧不登记 pending，累计 %d", sk);
        return 1;
    }
    // v4 P13：pending 三字段的写必须在 g_c4Lock 独占下（与 ClearC4State / C4TakePending 互斥）
    AcquireSRWLockExclusive(&g_c4Lock);
    g_bbTex = tex; g_bbFrame = (LONG)(g_frame - 1);   // nowm：与 C4KickLocked 保持一致，不用 addon 的 frameTag
    InterlockedExchange(&g_bbPending, 1);   // S3：置 pending，等 present flush 后 C4Kick compute
    ReleaseSRWLockExclusive(&g_c4Lock);
    static int n = 0;
    if ((n++ % 60) == 0) Log("C4-S2: BbHandoff tex=%p frame=%u cl=%p", tex, frameTag, cl);
    return 1;   // 成功（compute 由 C4Kick 在 present 队列上录）
}
extern "C" __declspec(dllexport) void AmdNrBbSlotFree(unsigned frameTag) {
    static int n = 0;
    if ((n++ % 60) == 0) Log("C4-S2: BbSlotFree frame=%u", frameTag);
}

// ★ 适配层唯一接缝：真派发（XeSS 构建 = g_xessTab[30]；FFX/FSR 构建由适配层注入 ffxDispatch）
extern "C" void* g_realDispatchFn = nullptr;
#include "a3_render.h"

static bool RebuildNetResolution(UINT wantW, UINT wantH, ID3D12Resource* color,
                                 ID3D12Resource* mvRes, ID3D12Resource* depRes, ID3D12Resource* outp) {
    Log("★ NR Scale 变化: %ux%u → %ux%u (mode=%d)，暂停 worker 并安全重建跨卡与 0.0.9 资源",
        kNetW, kNetH, wantW, wantH, (int)g_nrScaleMode);
    g_netRebuilding.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(g_worker.m);
        g_worker.pauseReq = true;
        if (g_worker.a3WakeEvent) SetEvent(g_worker.a3WakeEvent);
        g_worker.cv.notify_all();
        g_worker.pausedCv.wait_for(lk, std::chrono::milliseconds(2000),
                                   [&] { return g_worker.pausedAck || g_worker.quit; });
    }
    if (g_bpReady && g_bpFe && g_bpAlFv[0] > 0 && g_bpFe->GetCompletedValue() < g_bpAlFv[0]) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev) {
            if (SUCCEEDED(g_bpFe->SetEventOnCompletion(g_bpAlFv[0], ev))) WaitForSingleObject(ev, 1000);
            CloseHandle(ev);
        }
    }
    Sleep(80);
    if (g_bbFe) {
        UINT64 maxUsed = 0;
        for (int i = 0; i < 3; ++i) if (g_bbFvUsed[i] > maxUsed) maxUsed = g_bbFvUsed[i];
        if (maxUsed > 0 && g_bbFe->GetCompletedValue() < maxUsed) {
            g_bbFe->SetEventOnCompletion(maxUsed, nullptr);
            Sleep(16);
        }
    }
    A3::ResetForScale();
    A2In::ResetAll();
    A2EnsureMvScratch(true);
    g_xBufIn.Reset(); g_xBufInW.Reset(); g_xHeapIn.Reset(); g_xHeapInW.Reset();
    g_xBufOut.Reset(); g_xBufOutW.Reset(); g_xHeapOut.Reset(); g_xHeapOutW.Reset();
    g_xBufVel.Reset(); g_xBufVelW.Reset(); g_xHeapVel.Reset(); g_xHeapVelW.Reset();
    g_ansLocal.Reset(); g_xHdrUp.Reset(); g_xOk = false;
    for (auto& mh : g_mvHist) mh.Reset();
    g_bformReady = false; g_bTexReady = false;
    g_bTexC.Reset(); g_bTexM.Reset(); g_bTexD.Reset();
    g_upBuf.Reset(); g_upBufW.Reset(); g_upHeap.Reset(); g_upHeapW.Reset();
    g_bDsReady = false; g_bDs1080.Reset();
    g_bCompReady = false; g_bState.Reset(); g_bStateCleared = false;
    g_worker.color.Reset(); g_worker.colorFp16.Reset(); g_worker.vel.Reset();
    g_worker.up.Reset(); g_worker.upPtr = nullptr;
    g_worker.bMvZeroUp.Reset(); g_worker.rbPriv.Reset(); g_worker.bytesPriv = 0;

    kNetW = wantW; kNetH = wantH;
    ++g_buildGen;
    bool ok = WorkerAllocNetResources() && BuildResources(g_dev.Get(), color, mvRes, depRes, outp);
    if (ok) XAdapterInit();
    for (int i = 0; i < K; ++i) {
        g_slots[i].hasAnswer = false; g_slots[i].answerFrame = -1; g_slots[i].frame = -1; g_slots[i].captured = false;
    }
    g_dumpPhase = 0;
    {
        std::lock_guard<std::mutex> lk(g_worker.m);
        g_worker.aReset.Raise(A1Rebuild, g_frame);
        g_worker.pauseReq = false;
        g_worker.cv.notify_all();
    }
    g_netRebuilding.store(false, std::memory_order_release);
    Log("★ NR Scale 重建%s：%ux%u", ok ? "完成" : "失败", kNetW, kNetH);
    return ok;
}

extern "C" int ProxyExecute(void* ctx, void* cmdList, const void* params, float adapterMvScale,
                            const AdapterFrameDesc* adapterDesc) {
    using Fn = int(*)(void*, void*, const void*);
    const int okRc = adapterDesc ? adapterDesc->okCode : 0;
    const int errRc = adapterDesc ? adapterDesc->errCode : -1;
    // G0 探针：mode==0 时仍调用一次 GetTickCount64（数十纳秒）并做原子计数；日志只在 mode==1 由热键线程输出。
    // ★ G0 探针：入口第一处，在所有早退之前。无日志（统计交给热键线程）。
    {
        const ULONGLONG now = GetTickCount64();
        const LONGLONG prev = InterlockedExchange64(&g_g0LastTick, (LONGLONG)now);
        const LONGLONG iv = prev ? ((LONGLONG)now - prev) : 0;
        // 假设 ProxyExecute 单线程调用（与本文件 g_frame++ 等既有假设一致）；若多线程并发，LastTick 可能短暂倒退，仅影响观测。
        {   // 空档时长由生产侧精确计算：prev 与 now 同一次原子交换得到，不再分两次读
            const LONGLONG thr = InterlockedCompareExchange64(&g_g0ThrMsShared, 0, 0);
            if (prev != 0 && thr != 0 && iv >= thr) {   // thr==0 表示未初始化（含 mode≠1），跳过
                InterlockedExchange64(&g_g0GapLastMs, iv);
                InterlockedExchange64(&g_g0GapEndTick, (LONGLONG)now);
                InterlockedIncrement64(&g_g0GapCount);
            }
        }
        InterlockedIncrement64(&g_g0Seq);
        for (;;) {   // 原子 max：读 cur，interval<=cur 退出，否则 CAS，失败重试
            const LONGLONG cur = InterlockedCompareExchange64(&g_g0MaxIv, 0, 0);
            if (iv <= cur) break;
            if (InterlockedCompareExchange64(&g_g0MaxIv, iv, cur) == cur) break;
        }
        InterlockedIncrement64(&g_g0Count);
    }
    // 首个有效派发即生效；NR/UI 早退和旧存档加载均不影响契约。
    if (std::isfinite(adapterMvScale) && adapterMvScale != MvScaleUv())
        g_mvScale.store(adapterMvScale, std::memory_order_relaxed);
    {   // 读 ring 开关（一次性）
        static bool ringDone = false;
        if (!ringDone) {
            ringDone = true;
            std::string rp = ModuleDir() + "xess_proxy.ring.txt";
            FILE* rf = nullptr; char rb[8] = { 0 };
            if (fopen_s(&rf, rp.c_str(), "r") == 0 && rf) {
                if (fgets(rb, sizeof(rb), rf) && rb[0] == '0') g_ringOn = false;
                fclose(rf);
            }
            g_feedLagEff = kFeedLag;   // RE2 原样：固定 2
            Log("跨卡缓冲寻址: %s ; 喂帧滞后(有效)=%d", g_ringOn ? "K 段环" : "单段(RE2)", g_feedLagEff);
        }
    }
    NrStartFromFile();          // ★ 可选：xess_proxy.nrstart.txt=0 → 初始不接管 NR（进场景后按 F10 再开）
    CpuInFromFile();            // ★ 可选：xess_proxy.cpuin.txt=1 → 走 CPU 回读路径（诊断差一帧问题）
    R28ReadCfg();               // r28 块1：开机读一次 xess_proxy.r28.txt，在 g_r28Mode_live() 检查之前调
    Fn real = (Fn)(g_realDispatchFn ? g_realDispatchFn : g_xessTab[30]);
    {   // 诊断：确认 ProxyExecute 是否被调用、被哪个条件拦下
        static int dbg = 0;
        if (dbg < 6) {
            ++dbg;
            Log("DIAG-PE[%d]: params=%p cmdList=%p disabled=%d nrOn=%d real=%p",
                dbg, params, cmdList, (int)g_disabled, (int)g_nrOn, (void*)real);
        }
    }
    // CoreParams +60: latch the scene cut even if this dispatch cannot publish a worker input.
    unsigned gameReset = 0;
    if (params) memcpy(&gameReset, (const char*)params + 60, sizeof(gameReset));
    if (gameReset) {
        std::lock_guard<std::mutex> lk(g_worker.m);
        g_worker.aReset.Raise(A1GameReset, g_frame);
    }
    if (!params || !cmdList) { InterlockedIncrement64(&g_g0EarlyNull); return real ? real(ctx, cmdList, params) : errRc; }
    if (g_disabled || !g_nrOn) {
        A3::StopDisplay(g_frame);
        InterlockedIncrement64(&g_g0EarlyOff);
        BformPresentShow(false);   // NR 关掉 → 先卸覆盖层，否则会留一张冻结的 B 画面（F10 "像没生效"）
        return real ? real(ctx, cmdList, params) : errRc;
    }
    // ★ v4 P5：本帧租约 + UI 闸门（设计 §3.2）。闸门拒绝（OFF / STOPPING / STOPPED）⇒
    //   本帧不发布任务，直接走 real()（= A 形态 / 真 XeSS 出画，画面不会断）。
    //   UiGateAutoLeave 是 RAII：本函数后面所有 return 都会自动把租约 -1（+1/-1 严格配对）。
    UiGateStatePacket uiSnap;
    const bool uiGateOk = UiGateEnter(&uiSnap);
    UiGateAutoLeave uiLease;
    if (!uiGateOk) {
        A3::StopDisplay(g_frame);
        if (A2In::requested.load())
            A2In::OnUiStopped(uiSnap.state == UIG_OFF || uiSnap.state == UIG_STOPPED);
        InterlockedIncrement64(&g_g0EarlyOff);
        BformPresentShow(false);
        return real ? real(ctx, cmdList, params) : errRc;
    }
    InterlockedIncrement64(&g_g0Pass);   // G0：正常往下走（早退之后，不改控制流）

    // ★ 启动时自动载入用户存档（xess_proxy.saved.ini）
    //   以前只有菜单里的「恢复上次保存」会读它 → 每次启动都退回代码默认值，
    //   用户调好的参数（例如拖尾调优的 gateK=15 / mvConf=2.0）第二天就白调了。
    //   放在"记录起始参数"之前，这样「撤销改动」回到的是【载入后的值】，符合直觉。
    {
        static bool autoLoaded = false;
        if (!autoLoaded) {
            autoLoaded = true;
            EnsurePreprocessLoadedFromIni();
            Params ap = CaptureParams();
            if (LoadParamsFromFile(ap)) {
                ApplyParams(ap);
                Log("启动: 已自动载入上次保存的设置（xess_proxy.saved.ini）→ gateK=%.1f mvConf=%.1f gain=%.2f limit=%.2f",
                    (float)g_gateK, (float)g_mvConf, (float)g_gain, (float)g_limit);
            } else {
                Log("启动: 没有用户存档（xess_proxy.saved.ini），使用代码默认值");
            }
        }
    }

    // 记一次“本次启动的起始参数”，供菜单“撤销改动”回退
    if (!g_sessionStartValid) {
        g_sessionStart = CaptureParams();
        g_sessionStartValid = true;
        Log("菜单: 已记录本次启动的初始参数（撤销用）");
    }

    const unsigned __int64* q = (const unsigned __int64*)params;
    ID3D12Resource* color = (ID3D12Resource*)q[0];
    ID3D12Resource* mvRes = (ID3D12Resource*)q[1];
    ID3D12Resource* depRes = (ID3D12Resource*)q[2];
    ID3D12Resource* outp  = (ID3D12Resource*)q[5];
    g_outpRes = outp;                     // ★ 4K 后置模式要用（取 XeSS 上采样之后的画面）
    const auto bindCurrentOutpSrv = [&] {
        static ID3D12Resource* lastOutp = nullptr;
        static int lastBuildGen = -1;
        if (!outp || (lastOutp == outp && lastBuildGen == g_buildGen)) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = outp->GetDesc().Format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)kSrvOutp * g_inc;
        g_dev->CreateShaderResourceView(outp, &sd, h);
        lastOutp = outp;
        lastBuildGen = g_buildGen;
    };

    // ★ 目标探测：color/output 一旦换资源或改尺寸，槽位内容就会"写不回去"（屏幕只剩游戏画面）
    {
        static const void* lastC = nullptr; static const void* lastO = nullptr;
        static unsigned lastCW = 0, lastCH = 0, lastOW = 0, lastOH = 0;
        static const void* lastMV = nullptr; static const void* lastDP = nullptr;
        D3D12_RESOURCE_DESC dc{}, dop{};
        if (color) dc = color->GetDesc();
        if (outp)  dop = outp->GetDesc();
        D3D12_RESOURCE_DESC dmv{}, ddep{}; if (mvRes) dmv = mvRes->GetDesc(); if (depRes) ddep = depRes->GetDesc();
        if (color != lastC || outp != lastO || dc.Width != lastCW || dc.Height != lastCH
            || dop.Width != lastOW || dop.Height != lastOH
            || mvRes != lastMV || depRes != lastDP) {
            Log("TARGET: color=%p %ux%u fmt=%d flags=0x%X | output=%p %ux%u fmt=%d | 槽位尺寸=%ux%u fmt=%d",
                (const void*)color, (unsigned)dc.Width, dc.Height, (int)dc.Format, (unsigned)dc.Flags,
                (const void*)outp, (unsigned)dop.Width, dop.Height, (int)dop.Format, g_cw, g_ch, (int)g_cfmt);
            Log("D0-OBS TARGETMV: mv=%p %ux%u fmt=%d | dep=%p %ux%u fmt=%d | initMv=%p boundMv=%p | color=%p output=%p",
                (const void*)mvRes, mvRes ? (unsigned)dmv.Width : 0u, mvRes ? dmv.Height : 0u, mvRes ? (int)dmv.Format : -1,
                (const void*)depRes, depRes ? (unsigned)ddep.Width : 0u, depRes ? ddep.Height : 0u, depRes ? (int)ddep.Format : -1,
                (const void*)g_initMv, (const void*)g_boundMv, (const void*)color, (const void*)outp);
            lastMV = mvRes; lastDP = depRes;
            lastC = color; lastO = outp; lastCW = (unsigned)dc.Width; lastCH = dc.Height;
            lastOW = (unsigned)dop.Width; lastOH = dop.Height;
        }
    }

    // ---------- 诊断（与形态无关，前 5 帧一次）：jitter + MV 契约 + 游戏原始 MV 尺寸 ----------
    //   偏移按 intel/xess 的 xess_d3d12_execute_params_t：
    //   +0 pColor +8 pVelocity +16 pDepth +24 pExposure +32 pResponsiveMask +40 pOutput
    //   +48 jitterX +52 jitterY +56 exposureScale +60 resetHistory +64 inputWidth +68 inputHeight
    //   注：XeSS 没有 MV 缩放参数；RE 引擎 MV 是归一化(NDC)单位，像素 = 值 × (W/2)
    if (color) {
        static int mvdbg = 0;
        if (mvdbg < 5) {
            const unsigned char* pb = (const unsigned char*)params;
            float jx0 = 0, jy0 = 0, ev0 = 0; unsigned iw = 0, ih = 0;
            memcpy(&jx0, pb + 48, 4); memcpy(&jy0, pb + 52, 4);
            memcpy(&ev0, pb + 56, 4);
            memcpy(&iw, pb + 64, 4);  memcpy(&ih, pb + 68, 4);
            D3D12_RESOURCE_DESC cd = color->GetDesc();
            D3D12_RESOURCE_DESC md{}; int mf = -1; unsigned mw = 0, mh = 0;
            if (mvRes) { md = mvRes->GetDesc(); mw = (unsigned)md.Width; mh = (unsigned)md.Height; mf = (int)md.Format; }
            Log("DIAG-MV: jitter=(%.6f,%.6f) |len|=%.6f  契约[-0.5,0.5]", jx0, jy0, sqrtf(jx0*jx0 + jy0*jy0));
            Log("DIAG-MV: 契约 input=%ux%u expo=%.4f reset=%u | XeSS 无 MV 缩放参数（单位由引擎定）", iw, ih, ev0, gameReset);
            Log("DIAG-MV: 游戏颜色 %ux%u fmt=%d | 游戏MV %ux%u fmt=%d | 打包 %dx%d | 空间比例=%.4f | 归一化像素=值×%.1f",
                (unsigned)cd.Width, (unsigned)cd.Height, (int)cd.Format, mw, mh, mf, kNetW, kNetH,
                (mw && kNetW) ? ((float)kNetW / (float)mw) : 0.0f, iw ? iw / 2.0f : 0.0f);
            ++mvdbg;
        }
    }
    bool did4kUpscale = false;            // 4K 后置：本帧是否已在"敲铃前"提前做过 XeSS 上采样
    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmdList;
    float jx = 0, jy = 0;
    memcpy(&jx, (const char*)params + 48, 4);
    memcpy(&jy, (const char*)params + 52, 4);

    static bool inited = false, gaveUp = false;
    if (gaveUp) return real ? real(ctx, cmdList, params) : errRc;
    if (!inited) {
        if (!color || !outp) return real ? real(ctx, cmdList, params) : errRc;
        if (FAILED(color->GetDevice(__uuidof(ID3D12Device), (void**)g_dev.GetAddressOf())) || !g_dev) {
            gaveUp = true; return real ? real(ctx, cmdList, params) : errRc;
        }
        {
            D3D12_RESOURCE_DESC cdInit = color->GetDesc();
            ComputeNetDims((int)g_nrScaleMode, (UINT)cdInit.Width, cdInit.Height, kNetW, kNetH);
            Log("启动 NR 推理尺寸: %ux%u (nr_scale_mode=%d)", kNetW, kNetH, (int)g_nrScaleMode);
        }
        if (!BuildResources(g_dev.Get(), color, mvRes, depRes, outp) || !WorkerInit()) {
            gaveUp = true; Log("!! 初始化失败，永久放弃"); return real ? real(ctx, cmdList, params) : errRc;
        }
        {   // Present 钩子：取最终画面（tonemap + UI 之后）作底图的开关
            std::string bp = ModuleDir() + "xess_proxy.base.txt";
            FILE* bf = nullptr; char bb[8] = { 0 };
            if (fopen_s(&bf, bp.c_str(), "r") == 0 && bf) {
                if (fgets(bb, sizeof(bb), bf) && bb[0] == '1') g_baseFromPresent = 1;
                fclose(bf);
            }
            Log("底图来源: %s", g_baseFromPresent ? "Present 最终画面" : "pColor 渲染画面（A 形态原样）");
            if (g_baseFromPresent) InstallVtablePresentHook();
        }
        {   // C4-S3 开关：present 队列 compute 转换 + 落槽（默认关；与旧 base 路径互斥）
            std::string cp = ModuleDir() + "xess_proxy.c4.txt";
            FILE* cf = nullptr; char cb[8] = { 0 };
            if (fopen_s(&cf, cp.c_str(), "r") == 0 && cf) {
                if (fgets(cb, sizeof(cb), cf) && cb[0] == '1') g_c4On = 1;
                fclose(cf);
            }
            if (g_c4On) Log("C4-S3: 已启用（xess_proxy.c4.txt=1，present 队列 compute 转换 + 落槽）");
        }
        ProxyImGuiInit();
        TestSharedInGame();
        ShareInit();
        XAdapterInit();
        g_worker.th = std::thread(WorkerMain);
        inited = true;
        Log("初始化完成（方案 B：槽位%d / 喂帧滞后%d / 呈现延迟%d / 余量%d 帧）", K, kFeedLag, (int)g_kDelay, (int)g_kDelay - kFeedLag);
    }

    bool rebuiltAResources = false;
    // ★ 自适应：目标尺寸/格式变化（游戏内切超分档位、切全屏、改分辨率）→ 重建尺寸相关资源
    if (inited && color && outp) {
        static UINT lw = 0, lh = 0, low = 0, loh = 0; static DXGI_FORMAT lf = DXGI_FORMAT_UNKNOWN;
        D3D12_RESOURCE_DESC dc2 = color->GetDesc();
        D3D12_RESOURCE_DESC do2 = outp->GetDesc();
        if (lw == 0) { lw = g_cw; lh = g_ch; lf = g_cfmt; low = g_outW; loh = g_outH; }
        const bool chg = ((UINT)dc2.Width != lw) || (dc2.Height != lh) || (dc2.Format != lf)
                      || ((UINT)do2.Width != low) || (do2.Height != loh);
        if (chg) {
            Log("★ 目标变化: 尺寸 %ux%u fmt=%d / 输出 %ux%u → %ux%u fmt=%d / %ux%u，开始重建尺寸相关资源",
                lw, lh, (int)lf, low, loh, (unsigned)dc2.Width, dc2.Height, (int)dc2.Format,
                (unsigned)do2.Width, do2.Height);
            Sleep(80);   // 给 GPU 一点时间排空上一帧的引用（务实做法）
            // 2026-11-14 c4outp3：等待所有 C4 提交完成，确保堆重建前无 C4Kick 在飞
            if (g_bbFe) {
                UINT64 maxUsed = 0;
                for (int i = 0; i < 3; ++i) if (g_bbFvUsed[i] > maxUsed) maxUsed = g_bbFvUsed[i];
                if (maxUsed > 0) {
                    UINT64 done = g_bbFe->GetCompletedValue();
                    if (done < maxUsed) {
                        Log("c4outp3: 等待 C4 fence 达到 %llu（当前 %llu）", (unsigned long long)maxUsed, (unsigned long long)done);
                        g_bbFe->SetEventOnCompletion(maxUsed, nullptr);
                        Sleep(16);  // 务实等待
                    }
                }
            }
            ++g_buildGen;   // D0-OBS: BuildResources generation on the size-change rebuild path
            Log("D0-OBS BUILDGEN: gen=%d mv=%p dep=%p",
                g_buildGen, (const void*)mvRes, (const void*)depRes);
            if (!BuildResources(g_dev.Get(), color, mvRes, depRes, outp)) {
                Log("!! 重建失败 → 永久放弃 NR（游戏继续用自己的上采样）");
                gaveUp = true; return real ? real(ctx, cmdList, params) : errRc;
            }
            // 答案身份全部失效，避免"新尺寸画面配旧帧答案"
            for (int i = 0; i < K; ++i) {
                g_slots[i].hasAnswer = false; g_slots[i].answerFrame = -1; g_slots[i].frame = -1; g_slots[i].captured = false;
            }
            {
                std::lock_guard<std::mutex> lk(g_worker.m);
                for (int& f : g_worker.ansFrame) f = -1;
                g_worker.aReset.Raise(A1Rebuild, g_frame);
            }
            rebuiltAResources = true;
            lw = g_cw; lh = g_ch; lf = g_cfmt; low = g_outW; loh = g_outH;
            Log("重建完成：%ux%u fmt=%d（网络输入为 %ux%u）", g_cw, g_ch, (int)g_cfmt, kNetW, kNetH);
        }
    }
    // ★ Stage 3 NR Scale：菜单切换倍率或相对倍率下主渲染尺寸变动 → 暂停 worker 后安全重建
    if (inited && color && outp) {
        UINT wantW = 1920, wantH = 1080;
        ComputeNetDims((int)g_nrScaleMode, g_cw, g_ch, wantW, wantH);
        if (wantW != kNetW || wantH != kNetH) {
            if (!RebuildNetResolution(wantW, wantH, color, mvRes, depRes, outp)) {
                Log("!! NR Scale 重建失败 → 永久放弃 NR");
                gaveUp = true; return real ? real(ctx, cmdList, params) : errRc;
            }
            rebuiltAResources = true;
        }
    }

    auto tA = Clock::now();
    if ((g_frame % 300) == 0) PollResid4K();          // B1 开关（文件）
    if ((g_frame % 60) == 0) {
        if (!g_gameHwnd) ResolveGameHwnd();           // ★ 不依赖菜单：主动定位游戏主窗口（B 出画要用）
        BformPollFile();                              // B 形态开关（文件，每 60 帧读一次）
        Diag5PollFile();                              // 探针 5 模式（文件，每 60 帧读一次）
        DumpPollFile();                                // 抓帧导出（文件开关）
        TaaPollFile();                                // TAA 的 alpha/clampK（文件，每 60 帧读一次）
        // ★ 第 1 步·只观察：尝试截获游戏队列指针（需 xess_proxy.queuehook.txt = 1）
        {
            static bool qhTried = false;
            if (!qhTried && g_worker.q) { qhTried = true; InstallQueueHook(); }
        }
        // B 形态懒初始化：必须等 g_worker.dev 建好之后再建跨卡 buffer
        if (g_bformOn && !g_bformReady && color && mvRes && depRes)
            BformInit(g_dev.Get(), color, mvRes, depRes);
        // ★ 看门狗：stage7 若让 worker 卡死，游戏会一直挂着。超过 3 秒没心跳就自动降回 stage 6
        //   （降级后 A 会继续出画、只是没有 NR；不会让游戏永久卡死）
        if (g_bformStage >= 7 && g_workerBeat) {
            ULONGLONG nowTick = GetTickCount64();
            if (nowTick - g_workerBeat > 3000) {
                Log("!! 看门狗：worker 超过 3 秒没心跳（stage7 疑似卡死）→ 自动降回 stage 6");
                g_bformStage = 6;
                g_ppHavePrev = false;
            }
        }
    }
    if ((g_frame % 60) == 0) PollAdd4KReload();       // Add4K 热重编译（改 hlsl 保存即生效）
    // 覆盖层可见性跟随"B 是否真的出画"。
    // ★ v4fix2（本修复的关键一处）：热路径【只许卸载，不许显示】。
    //   原来这里直接 BformPresentShow(g_bformOn && g_bformStage >= 6 && g_bpReady && UiGateAllowsNr())：
    //   UiGateAllowsNr 为了不饿死 STARTING 的预热而放行 STARTING，于是 UI 一关就立刻把覆盖层挂上，
    //   而此刻交换链里还是进 UI 前最后一帧 → 玩家看到"UI 退出后先闪一下冻结的旧画面"。
    //   现在：
    //     · 特性未启用（uigate mode=0）：保持旧行为，直接跟随（默认配置下 B 形态照常出画）；
    //     · 特性启用：这里只负责"该隐藏就隐藏"，显示只有 OnPresentSuccess → OverlayGateTick 一条出路。
    //   ★ step2wm blockB 补充：两个分支在 g_wmtHidden 为真时都不显示覆盖层。
    {
        const bool wantShow = (g_bformOn && g_bformStage >= 6 && g_bpReady && UiGateAllowsNr());
        if (!g_uiFeatureOn) BformPresentShow(wantShow);
        else if (!wantShow) {
            // 本代"已显示过"的记要清掉：否则 stage 掉回 6 以下再回来时，同一代里不会再武装
            // （shown==epoch + applied==0 会被判成"本代已经显示过"）。
            InterlockedExchange(&g_ovGateShownEpoch, -1);
            BformPresentShow(false);
        }
    }
    ID3D12DescriptorHeap* heaps[] = { g_heap.Get() };
    cl->SetDescriptorHeaps(1, heaps);

    {   // ★ D5 增强：派发间隔（纯观测）——游戏停帧 / 切 UI 时这里会出现 >200ms 的空档
        static ULONGLONG prevDispatch = 0, lastGapLog = 0;
        const ULONGLONG nowDisp = GetTickCount64();
        if (prevDispatch) {
            const ULONGLONG gap = nowDisp - prevDispatch;
            if (gap > 200 && (nowDisp - lastGapLog) > 500) {
                lastGapLog = nowDisp;
                BtTrace("DISPATCH_GAP", g_frame, "dispatch",
                        "gap_ms=%llu frame=%d", (unsigned long long)gap, (int)g_frame);
            }
        }
        prevDispatch = nowDisp;
    }
    const int N = g_frame++;
    Slot& cur = g_slots[N % K];
    const int presentFrame = N - g_kDelay;

    // ---------- ① [CPU] 取答案（阻塞）----------
    //   ★ 只有【B 真的在出画】时才让 A 偷懒（省掉这 30ms 等待）；B 没接管（没按 Del、
    //     覆盖层没建起来）时 A 必须照常合成，否则会"两头都没有 NR"的静默失败（10-28 踩过）
    const bool bPresenting = (g_bformOn && g_bformStage >= 6 && g_bpReady && UiGateAllowsNr());
    const bool ngxB4k = g_backendNgx && g_bformOn && g_bform4k;
    const bool skipAPixels = ngxB4k && g_c4On && g_bformReady && bPresenting;
    const bool a2Direct = A2In::requested;
    const bool useA3 = a2Direct && !g_bformOn && adapterDesc;
    if (!useA3) A3::StopDisplay(N);
    bool a2MvValid = !a2Direct;
    if (a2Direct && cur.mv && mvRes) {
        const auto src = mvRes->GetDesc(), dst = cur.mv->GetDesc();
        a2MvValid = src.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && src.Width == dst.Width &&
                    src.Height == dst.Height && src.Format == dst.Format && src.MipLevels == dst.MipLevels &&
                    src.DepthOrArraySize == dst.DepthOrArraySize && src.SampleDesc.Count == 1 && dst.SampleDesc.Count == 1;
    }
    const bool aCaptureValid = !g_backendNgx || (color && (!cur.mv || mvRes) && (!cur.depth || depRes) && a2MvValid);
    static bool prevNgxB4k = false, prevSkipAPixels = false;
    static int aChainFrom = 0, aChainGen = 0, aReadbackFrame = -1;
    static uint64_t aChainResetEpoch = 0;
    bool newAChain = false;
    if (g_backendNgx) {
        std::lock_guard<std::mutex> lk(g_worker.m);
        auto& reset = g_worker.aReset;
        newAChain = aChainResetEpoch != reset.epoch || ngxB4k != prevNgxB4k ||
                    skipAPixels != prevSkipAPixels || rebuiltAResources || (!skipAPixels && !aCaptureValid) ||
                    (useA3 && A3::lastContext != adapterDesc->contextToken);
        if (newAChain) {
            aChainResetEpoch = reset.epoch;
            aChainGen = ++reset.chainGen;
        }
    }
    if (newAChain) {
        // B→A（以及中途停填、尺寸重建）：旧 A 任务即使稍后交付也不属于新一代。
        aChainFrom = aCaptureValid ? N : N + 1;
        aReadbackFrame = -1;
        for (Slot& slot : g_slots) { slot.hasAnswer = false; slot.feedFrame = -1; }
        if (g_stateWarmup < 4) g_stateWarmup = 4;
        if (ngxB4k != prevNgxB4k || skipAPixels != prevSkipAPixels)
            Log("BSTOP-A: 帧%d A代=%d B4K=%d 停A像素=%d", N, aChainGen, (int)ngxB4k, (int)skipAPixels);
    }
    F9Id::ARecord f9A(cl, g_dev.Get(), N, aChainGen, aChainResetEpoch,
                       g_backendNgx && !g_bformOn, F9Id::observerReady.load());
    A2In::Producer a2(cl); // observes same native list rerecord only for old-slot reclamation
    if (a2Direct) {
        A2In::Reap(N - g_feedLagEff, aChainGen, aChainResetEpoch, false, N);
        if (skipAPixels) {
            if (A2In::TryReleaseInactive(N - g_feedLagEff, aChainGen, aChainResetEpoch, N))
                A2EnsureMvScratch(true);
        } else if (aCaptureValid && !A2In::initialized.load() && !A2In::initAttempted && !A2In::broken.load() &&
                   (!g_bformOn || (N - aChainFrom >= K && !bPresenting))) {
            A2In::initAttempted = true;
            if (g_xOk && g_xRead && g_xWrite && A2InitDirect()) {
                A2In::initialized.store(true);
                Log("A2-INPUT-DIRECT: on-demand init OK (%d independent pair slots)", A2In::kSlots);
            }
        }
    }
    prevNgxB4k = ngxB4k;
    prevSkipAPixels = skipAPixels;
    if (useA3)
        return A3::Render(ctx, cl, *adapterDesc, real, params, {N, aChainGen, aChainResetEpoch}, a2, f9A);
    // 给 ResidCS 的本帧/前帧/偏移喂图填满一整圈，再允许 A 合成。
    const bool aHistoryReady = !g_backendNgx || (N - aChainFrom >= K && presentFrame >= aChainFrom);
    if (presentFrame >= 0 && !bPresenting && aHistoryReady) {
        Slot& P = g_slots[presentFrame % K];
        auto w0 = Clock::now();
        {
            std::unique_lock<std::mutex> lk(g_worker.m);
            // 身份化等待：必须等到【属于 presentFrame 这一帧内容】的答案，而不是"随便最新的一份"。
            // 注意：这里【绝对不能 return】—— 它下面还有"喂下一帧给 worker"，早退会饿死 worker，
            // 于是永远等不到答案（踩过一次：NR 全程失效）。失败只是本帧不合成。
            const int ansSlot = presentFrame % K;
            const auto hasCurrentAnswer = [&] {
                return (int)g_worker.ansFrame.size() == K && g_worker.ansFrame[ansSlot] == presentFrame &&
                    (!g_backendNgx || (g_worker.aReset.Current(aChainGen, aChainResetEpoch) &&
                        P.feedFrame == presentFrame && g_worker.ansAGen[ansSlot] == aChainGen));
            };
            // A2 only short-circuits a frame known never offered, or a sealed-off lane.
            // The existing bounded answer wait and identity predicate remain for real requests.
            const auto a2CannotAnswer = [&] {
                return a2Direct && (P.feedFrame != presentFrame || A2In::broken.load() ||
                                     A2In::Rejected(presentFrame, P.a2OfferSeq));
            };
            bool gotId = !a2CannotAnswer() && g_worker.cvOut.wait_for(lk, std::chrono::milliseconds(30),
                        [&] { return !g_worker.ok || a2CannotAnswer() || hasCurrentAnswer(); });
            if (gotId && !a2CannotAnswer() && hasCurrentAnswer()) {
                memcpy(P.answer.data(), g_worker.ansBuf[presentFrame % K].data(), P.answer.size());
                P.hasAnswer = true;
            } else {
                P.hasAnswer = false;
                static int missCnt = 0;
                if ((missCnt++ % 60) == 0)
                    Log("!! 等不到第 %d 帧的答案（槽%d 现有=%d ok=%d）本帧不合成，累计 %d 次",
                        presentFrame, presentFrame % K, g_worker.ansFrame[presentFrame % K],
                        (int)g_worker.ok, missCnt);
            }
            {

                // 静止检测：和上一次呈现的那一帧比较（抽样），决定时域混合强度
                {
                    Slot& Q = g_slots[(presentFrame - 1 + K * 4) % K];
                    if (Q.hasAnswer && Q.feedInput.size() == P.feedInput.size()) {
                        size_t cnt = 0, changed = 0;
                        for (size_t i = 0; i < P.feedInput.size(); i += 64) {
                            int d = (int)P.feedInput[i] - (int)Q.feedInput[i];
                            if (d < 0) d = -d;
                            if (d > 16) changed++;   // 噪声不会超过 16，真运动才会
                            cnt++;
                        }
                        float motion = cnt ? (float)changed / (float)cnt : 0.0f;
                        // 连续过渡：运动量在下界→用静止 alpha；上界→1.0（不用历史）；中间平滑插值
        // 旧写法是二元硬切换 (motion < 0.02 ? static : 1.0)，
        // 运动量在 0.02 附近抖动时 alpha 会在 0.02 ↔ 1.0 之间反复跳 → 画面闪烁
        float denom = g_motionHi - g_motionLo;
        if (denom < 1e-4f) denom = 1e-4f;
        float mt = (motion - g_motionLo) / denom;
        mt = (mt < 0.0f) ? 0.0f : ((mt > 1.0f) ? 1.0f : mt);
        mt = mt * mt * (3.0f - 2.0f * mt);   // smoothstep
        // 运动时不再到 1.0，而是到 g_motionAlpha；实际历史量再由 shader 的 conf/gate 决定
        g_alpha = g_temporalStatic + (g_motionAlpha - g_temporalStatic) * mt;
                        if (presentFrame < 8) g_alpha = 1.0f;   // 前几帧让状态纹理自愈（初始内容是垃圾）
                        static int logN = 0;
                        if ((logN++ % 150) == 0) Log("alpha: 运动量 %.4f -> alpha %.2f (区间 %.2f~%.2f)", motion, g_alpha, g_motionLo, g_motionHi);
                    }
                }
            }
        }
        double wms = std::chrono::duration<double, std::milli>(Clock::now() - w0).count();
        g_sumWait += wms; g_nWait++;
        if (g_nWait % 150 == 0)
            Log("stats: 帧%d 等待均值 %.1fms 槽位[%d]答案=%d", N, g_sumWait / g_nWait, presentFrame % K, (int)P.hasAnswer);
        {   // ★ 速率表：填槽速度 / worker 消费 / 合成出画
            static long long t0 = 0; static int n0 = 0, w0 = 0, c0 = 0, k0 = 0, m0 = 0;
            static long p0 = 0, q0 = 0, u0 = 0, s0 = 0;
            LARGE_INTEGER f, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&now);
            if (t0 == 0) { t0 = now.QuadPart; n0 = N; w0 = (int)g_worker.processed; c0 = (int)g_cntCompose; k0 = (int)g_cntSkip; m0 = (int)g_cntMiss; }
            else if ((N - n0) >= 120) {
                double dt = (double)(now.QuadPart - t0) / (double)f.QuadPart;
                double dn = (double)(N - n0), dw = (double)((int)g_worker.processed - w0);
                Log("RATE: %.1fs | 填槽 %.1f/s | worker %.1f/s | 合成 %.1f/s (+%d) | 跳过提交 +%d | 缺答案 +%d | 涂标 +%ld (跳过 %ld / 无UAV %ld / 无PSO %ld)",
                    dt, dn / dt, dw / dt, (double)((int)g_cntCompose - c0) / dt, (int)g_cntCompose - c0,
                    (int)g_cntSkip - k0, (int)g_cntMiss - m0,
                    (long)(g_cntPaint - p0), (long)(g_cntPaintSkip - q0), (long)(g_cntPaintNoUav - u0), (long)(g_cntPaintNoPso - s0));
                p0 = g_cntPaint; q0 = g_cntPaintSkip; u0 = g_cntPaintNoUav; s0 = g_cntPaintNoPso;
                t0 = now.QuadPart; n0 = N; w0 = (int)g_worker.processed; c0 = (int)g_cntCompose; k0 = (int)g_cntSkip; m0 = (int)g_cntMiss;
            }
        }
        if ((N % 30) == 0)
            Log("TRACE-CMP: 帧%d presentFrame=%d 槽=%d 该槽ansFrame=%d hasAnswer=%d fedSlot=%d",
                N, presentFrame, presentFrame % K, g_worker.ansFrame[presentFrame % K], (int)P.hasAnswer,
                (presentFrame + K - 0) % K);
    }

    // ---------- ② [CPU] Map 上一帧的回读 → 喂 worker（slot[N-1]）----------
    // ★ 余量文件调：xess_proxy.margin.txt = 1/2/3（每 60 帧重读）→ kDelay = kFeedLag + margin
    {
        static int lastMargin = -1; static int mTick = 0;
        if ((mTick++ % 60) == 0) {
            std::string mp2 = ModuleDir() + "xess_proxy.margin.txt";
            FILE* mf2 = nullptr; char mb[8] = { 0 };
            if (fopen_s(&mf2, mp2.c_str(), "r") == 0 && mf2) {
                if (fgets(mb, sizeof(mb), mf2)) {
                    int mv = atoi(mb);
                    if (mv < 1) mv = 1;
                    if (mv > K - 1 - g_feedLagEff) mv = K - 1 - g_feedLagEff;
                    g_kDelay = g_feedLagEff + mv;
                    if (mv != lastMargin) { lastMargin = mv; Log("余量(文件): %d 帧 → kDelay=%d（K=%d）", mv, (int)g_kDelay, K); }
                }
                fclose(mf2);
            }
        }
    }
    g_frameNow = (int)N;   // Present 钩子用它给画面副本编号；与 A 回读/唤醒无关
    if (g_baseFromPresent == 1) DoPendingGrab();
    if (N >= kFeedLag) {
        const int feedFrame = N - g_feedLagEff;
        const int feedSlot = ((feedFrame % K) + K) % K;
        Slot& F = g_slots[feedSlot];
        bool hasAPixels = false;
        A2In::Token offer;
        if (a2Direct) {
            F.a2OfferSeq = 0;
            if (!skipAPixels && feedFrame >= aChainFrom && F.captured && F.frame == feedFrame) {
                offer = A2In::Offer({feedFrame, aChainGen, aChainResetEpoch}, F.feedInput.data(), F.feedInput.size(), N);
                if (offer) { F.feedFrame = feedFrame; F.a2OfferSeq = offer.seq; hasAPixels = true; }
            } else {
                A2In::DiagOfferNone({feedFrame, aChainGen, aChainResetEpoch}, N, skipAPixels ? "a_pixels_skipped" : "source_not_in_current_capture");
            }
        } else if (!skipAPixels && (!g_backendNgx || (feedFrame >= aChainFrom && aReadbackFrame == feedFrame))) {
            D3D12_RANGE rr{ 0, (SIZE_T)g_bytesRb };
            void* m = nullptr;
            if (SUCCEEDED(g_rb->Map(0, &rr, &m)) && m) {
                UINT pitch = g_fpRb.Footprint.RowPitch;
                for (UINT y = 0; y < kNetH; ++y)
                    memcpy(&F.feedInput[(size_t)y * kNetW * 4], (unsigned char*)m + (size_t)y * pitch, (size_t)kNetW * 4);
                D3D12_RANGE nw{ 0, 0 };
                g_rb->Unmap(0, &nw);
                F.feedFrame = feedFrame;
                hasAPixels = true;
            }
        }

        // B 仍在这条 cv 上等活；帧身份和通知不依赖 A 像素。
        if (hasAPixels || g_backendNgx) {
            std::lock_guard<std::mutex> lk(g_worker.m);
            if (g_worker.hasIn) A2In::Cancel(g_worker.inA2);
            if (offer && !g_worker.aReset.Current(aChainGen, aChainResetEpoch)) {
                A2In::Cancel(offer); offer = {}; hasAPixels = false; F.feedFrame = -1; F.a2OfferSeq = 0;
            }
            g_worker.inA2 = offer;
            g_worker.inA3 = false;
            if (hasAPixels && !a2Direct) {
                std::vector<uint8_t> tmp(kNetW * kNetH * 4);
                // ★ SUBCHK：量【真正提交给网络的那张图】的图案占比（不写盘、每 30 帧一次）
                if ((N % 30) == 0) {
                    const uint8_t* rowS = F.feedInput.data() + (size_t)(kNetH / 2) * kNetW * 4;
                    size_t dS = 0, tS = 0;
                    for (UINT x = 0; x < kNetW; x += 32) {
                        if (rowS[(size_t)x * 4 + 0] < 16 && rowS[(size_t)x * 4 + 1] < 16 && rowS[(size_t)x * 4 + 2] < 16) ++dS;
                        ++tS;
                    }
                    Log("SUBCHK: 帧%d 提交槽%d inFrame=%d 该图黑占比=%d%%", N, feedSlot,
                        feedFrame, tS ? (int)(100.0 * (double)dS / (double)tS) : -1);
                }
                for (UINT y = 0; y < kNetH; ++y)
                    memcpy(&tmp[(size_t)y * kNetW * 4], &F.feedInput[(size_t)y * kNetW * 4], (size_t)kNetW * 4);
                g_worker.inBuf.swap(tmp);
                if ((N % 30) == 0) Log("TRACE-SUB: 帧%d 提交槽=%d inFrame=%d", N, feedSlot, feedFrame);
            }
            g_worker.inIndex = feedSlot;
            g_worker.inFrame = feedFrame;   // = N-2，与 RE2 一致
            g_worker.f9Dispatch = N;
            g_worker.inHasAPixels = hasAPixels;
            g_worker.inAGen = aChainGen;
            g_worker.inAResetEpoch = aChainResetEpoch;
            g_worker.hasIn = true;
            if (g_worker.a3WakeEvent) SetEvent(g_worker.a3WakeEvent);
            g_worker.cv.notify_one();
        }
    }

    // ---------- ③ [GPU] 拷本帧原始 pColor / MV / depth 进槽位 ----------
    if (!skipAPixels && aCaptureValid) {
        if (color) {
            Barrier(cl, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, cur.color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(cur.color.Get(), color);
            Barrier(cl, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        if (mvRes && cur.mv) {
            Barrier(cl, mvRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl->CopyResource(cur.mv.Get(), mvRes);
            Barrier(cl, mvRes, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        if (depRes && cur.depth) {
            Barrier(cl, depRes, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl->CopyResource(cur.depth.Get(), depRes);
            Barrier(cl, depRes, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_READ);
        }
        cur.jx = jx; cur.jy = jy; cur.frame = N; cur.captured = true;
    }

    // ---------- 3.4 [GPU] NGX backend only: pack MV -> cross-card buffer ----------
    // Reuses the B-form MvPack pass (game MV format -> 1080p RG16F). Runs only when backend=ngx
    // and not in B form. Same "lag by 2 frames" contract as colour, so no extra fence is needed.
    if (!a2Direct && g_backendNgx && !g_bformOn && g_xBufVel && g_mvPackReady && g_psoMvPack && mvRes) {
        {   // the game may rotate its MV resource -> rebuild the SRV once when the pointer changes
            static ID3D12Resource* lastMv = nullptr;
            if (lastMv != mvRes) {
                lastMv = mvRes;
                D3D12_RESOURCE_DESC md0 = mvRes->GetDesc();
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = md0.Format;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
                h.ptr += (SIZE_T)kSrvMvCur * g_inc;
                g_dev->CreateShaderResourceView(mvRes, &sd, h);
                Log("NGX: MV source bound (SRV) %ux%u fmt=%d", (UINT)md0.Width, md0.Height, (int)md0.Format);
            }
        }
        Barrier(cl, mvRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_mvPacked.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SetPass(cl, g_psoMvPack.Get(), kSrvMvCur, kSrvMvCur, kSrvMvCur, kSrvMvCur, kSrvMvCur,
                kSrvMvPacked, kSrvMvPacked, kNetW, kNetH);
        Barrier(cl, g_mvPacked.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, mvRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12_TEXTURE_COPY_LOCATION vd{};
        vd.pResource = g_xBufVel.Get();
        vd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        vd.PlacedFootprint.Offset = 0;
        vd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
        vd.PlacedFootprint.Footprint.Width = kNetW;
        vd.PlacedFootprint.Footprint.Height = kNetH;
        vd.PlacedFootprint.Footprint.Depth = 1;
        vd.PlacedFootprint.Footprint.RowPitch = kNetW * 4;
        D3D12_TEXTURE_COPY_LOCATION vv{};
        vv.pResource = g_mvPacked.Get();
        vv.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        vv.SubresourceIndex = 0;
        f9A.Tap(F9Id::AMv, g_mvPacked.Get(), N, N, F9Id::Shared);
        cl->CopyTextureRegion(&vd, 0, 0, 0, &vv, nullptr);
        Barrier(cl, g_mvPacked.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    // ---------- ③.5 [GPU] 【B 形态·一趟方案】把本帧原始 pColor/MV/深度 单向传给 B ----------
    // 一趟方案：A 只做"送原料"，NR / 残差合成 / 上采样 / 出画 全在 B 上做。
    // 好处：少一整次跨卡往返（B→A 的答案回传整条通道可以去掉）、延迟更低、同步退化成单向流。
    if (g_bformOn && g_bformReady && color && mvRes && depRes) {
        if (g_mvPackReady && g_psoMvPack) {
            // 先把 MV 打包成 1080p RG16F（29.5MB → 8.3MB），再把打包结果传过去
            Barrier(cl, mvRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, g_mvPacked.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // D1-FIX: rebuild kSrvMvCur when the game rotates its MV (same-size identity change).
            //   Mirrors the A-form rebuild (the `static ID3D12Resource* lastMv` block) into B form.
            {
                static ID3D12Resource* lastMvB = nullptr;
                if (lastMvB != mvRes) {
                    lastMvB = mvRes;
                    D3D12_RESOURCE_DESC md1 = mvRes->GetDesc();
                    D3D12_SHADER_RESOURCE_VIEW_DESC sd1{};
                    sd1.Format = md1.Format;
                    sd1.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    sd1.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    sd1.Texture2D.MipLevels = 1;
                    D3D12_CPU_DESCRIPTOR_HANDLE h1 = g_heap->GetCPUDescriptorHandleForHeapStart();
                    h1.ptr += (SIZE_T)kSrvMvCur * g_inc;
                    g_dev->CreateShaderResourceView(mvRes, &sd1, h1);
                    g_boundMv = mvRes;   // keep D0 probe's boundMv truthful
                    Log("D1-FIX: MV SRV rebound mv=%p fmt=%d", (const void*)mvRes, (int)md1.Format);
                }
            }
            // D0-OBS: pure logging (no GPU call, no ordering/param change)
            {
                static unsigned s_d0n = 0; static const void* s_d0last = nullptr;
                ++s_d0n;
                if (s_d0n == 1 || s_d0last != (const void*)mvRes || (s_d0n % 300) == 0) {
                    s_d0last = (const void*)mvRes;
                    D3D12_RESOURCE_DESC mdP = mvRes->GetDesc();
                    Log("D0-OBS MVPACK: frame=%d n=%u curMv=%p initMv=%p boundMv=%p stale=%d staleBound=%d mv=%ux%u fmt=%d",
                        (int)g_frame, s_d0n, (const void*)mvRes, (const void*)g_initMv, (const void*)g_boundMv,
                        (mvRes != g_initMv) ? 1 : 0, (mvRes != g_boundMv) ? 1 : 0,
                        (unsigned)mdP.Width, mdP.Height, (int)mdP.Format);
                }
            }
            SetPass(cl, g_psoMvPack.Get(), kSrvMvCur, kSrvMvCur, kSrvMvCur, kSrvMvCur, kSrvMvCur,
                    kSrvMvPacked, kSrvMvPacked, kNetW, kNetH);
            Barrier(cl, g_mvPacked.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, mvRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            
            // r28 块4：MV 本地暂存环 —— 把本帧 g_mvPacked 拷进 g_mvHist[N&7]，登记帧号给 XferMaybeKick
            // nowm 后帧标签与 C4 的 g_frame-1 一致；登记 N，不再沿用水印时代的 N+1。
            if (g_r28Mode_live() && InterlockedCompareExchange(&g_r28MvEn, 0, 0)) {
                if (!g_mvHist[0] && g_dev) {
                    for (int i = 0; i < 8; ++i)
                        g_mvHist[i] = MakeTex(g_dev.Get(), kNetW, kNetH, DXGI_FORMAT_R16G16_FLOAT,
                                              D3D12_RESOURCE_STATE_COMMON, false);
                    Log("R28B4: MV暂存环就绪（8×%dx%d RG16F）", kNetW, kNetH);
                }
                const int mvRegT = N;
                if (g_mvHist[mvRegT & 7]) {
                    Barrier(cl, g_mvHist[mvRegT & 7].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                    cl->CopyResource(g_mvHist[mvRegT & 7].Get(), g_mvPacked.Get());
                    Barrier(cl, g_mvHist[mvRegT & 7].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                    // 2026-11-19 mvpair：登记 {帧号, 本 cl 身份}；只有含本 cl 的那次 ExecuteCommandLists 才能认领
                    MvPairRegister((LONG)mvRegT, cl);
                }
            }

            if (!g_bform4k) {
                BformCopy(cl, color, g_mvPacked.Get(), depRes, jx, jy);
            } else {
                // ★ 4K 后置：B 形态下 A 不合成 ⇒ XeSS 可以提前到"敲铃之前"，顺序：
                //   ⑥ XeSS(本帧 2K 原图 → 4K) → 转 8bit → 拷进槽位 →（之后照常敲铃）
                //   这样 B 拿到的是【本帧】完整的 4K，零额外延迟。
                char b4[256]{};
                const size_t copyBytes = g_realDispatchFn ? 72u : sizeof(b4);
                if (params) memcpy(b4, params, copyBytes);
                if (g_jitterScale != 1.0f) {
                    float jx4 = 0, jy4 = 0;
                    memcpy(&jx4, b4 + 48, 4); memcpy(&jy4, b4 + 52, 4);
                    jx4 *= g_jitterScale; jy4 *= g_jitterScale;
                    memcpy(b4 + 48, &jx4, 4); memcpy(b4 + 52, &jy4, 4);
                }
                const int rc4 = real ? real(ctx, cl, b4) : errRc;
                did4kUpscale = true;
                // ★ B 形态下菜单要能被看见：B 显示的就是这张 4K 底图，
                //   所以把 ImGui 画进 pOutput 再转换 → 菜单自然跟着底图跨卡上屏。
                //   （原来菜单只画在 A 的输出上，被 B 的覆盖层盖住 → B 形态看不见菜单）
                ProxyImGuiDraw(cl, outp);
                    const int par8 = (int)(g_frame & 1);   // 双缓冲：本帧写哪一份
                    XferEnsure();   // ★ 必须先建好围栏：否则钩子不 Signal → 拷永远不触发（逻辑死锁）
                    // ★ 旧转换要追踪轮换的 pOutput；C4 转换自己绑定 T_src，且不登记 A 阶段抓帧。
                    if (!g_c4On) {
                        bindCurrentOutpSrv();
                        DumpFlush(0);
                    }
                // C4 有自己的三套分配；只有旧转换或 base=1 抓图需要按奇偶预建。
                if ((!g_c4On || g_baseFromPresent == 1) && !g_outp8Ready[par8]) {
                    D3D12_HEAP_PROPERTIES hp8{}; hp8.Type = D3D12_HEAP_TYPE_DEFAULT;
                    D3D12_RESOURCE_DESC td8{};
                    td8.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    td8.Width = g_outW; td8.Height = g_outH;
                    td8.DepthOrArraySize = 1; td8.MipLevels = 1;
                    td8.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td8.SampleDesc.Count = 1;
                    td8.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                    if (SUCCEEDED(g_dev->CreateCommittedResource(&hp8, D3D12_HEAP_FLAG_NONE, &td8,
                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_outp8[par8]))) && g_outp8[par8]) {
                        D3D12_UNORDERED_ACCESS_VIEW_DESC ud8{};
                        ud8.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        ud8.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                        D3D12_CPU_DESCRIPTOR_HANDLE h8 = g_heap->GetCPUDescriptorHandleForHeapStart();
                        h8.ptr += (SIZE_T)(par8 ? kSrvOutp8b : kSrvOutp8) * g_inc;   // ★ 两张缓冲各用自己的 UAV
                        g_dev->CreateUnorderedAccessView(g_outp8[par8].Get(), nullptr, &ud8, h8);
                        g_outp8Ready[par8] = true;
                        Log("BFORM: 4K 后置转换目标就绪 %ux%u（8bit）", g_outW, g_outH);
                    }
                }
                // ★ 直接拷 pOutput（原生 10bit）：-- 实验失败，已回退（见下）
                // ★ 优先用 Present 抓到的【最终画面】（tonemap + UI 之后）作 4K 底图
                ID3D12Resource* grab = nullptr; int gFrm = -1, gw = 0, gh = 0, gf = 0;
                if (g_baseFromPresent == 1) {
                    const int gs = ((g_frameNow % K) + K) % K;
                    if ((int)g_imgSlots.size() > gs && g_imgSlots[gs] && g_imgFrame[gs] != 0) {
                        grab = g_imgSlots[gs].Get(); grab->AddRef();
                        D3D12_RESOURCE_DESC gd = grab->GetDesc();
                        gFrm = g_imgFrame[gs]; gw = (int)gd.Width; gh = (int)gd.Height; gf = (int)gd.Format;
                    }
                }
                if (grab && g_outp8Ready[par8] && (UINT)gw == g_outW && (UINT)gh == g_outH) {
                    Barrier(cl, g_outp8[par8].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                    cl->CopyResource(g_outp8[par8].Get(), grab);
                    Barrier(cl, g_outp8[par8].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    static int gLogN = 0;
                    if ((gLogN++ % 120) == 0) Log("B4K: 底图来自 Present 最终画面（帧%d %dx%d fmt=%d）", gFrm, gw, gh, gf);
                    grab->Release(); grab = nullptr;
                    goto after_convert;
                }
                if (grab) { grab->Release(); grab = nullptr; }
                if (!g_c4On && g_outp8Ready[par8] && g_psoFmt8) {   // 8bit 转换路径（C4 启用时让位给 present 队列的 C4Kick）
                    Barrier(cl, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    Barrier(cl, g_outp8[par8].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    ID3D12DescriptorHeap* hh9[] = { g_heap.Get() };
                    cl->SetDescriptorHeaps(1, hh9);
                    SetPass(cl, g_psoFmt8.Get(), kSrvOutp, kSrvOutp, kSrvOutp, kSrvOutp, kSrvOutp,
                            (par8 ? kSrvOutp8b : kSrvOutp8), (par8 ? kSrvOutp8b : kSrvOutp8), g_outW, g_outH);
                    Barrier(cl, g_outp8[par8].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    // ★ 4 坑 FIFO：每帧登记一个待拷；只要"又发生过提交"就逐个启动，任何一帧都不会被覆盖
                    for (int pi = 0; pi < 4; ++pi) {
                        if (!g_pend[pi].valid) continue;
                        if (g_lastSigV <= g_pend[pi].sig) continue;     // 该帧的转换还没提交完
                        if (!g_pend[pi].valid) continue;
                        int pp = g_pend[pi].parity; UINT ps = (UINT)g_pend[pi].slot; int pf = g_pend[pi].frameTag;
                        if (!g_outp8[pp]) { g_pend[pi].valid = false; continue; }
                        g_pend[pi].valid = false;
                        g_xferPendFrameTag = pf;
                        XferKickColor(g_outp8[pp].Get(), ps, g_lastSigV);
                    }
                    // 登记本帧（取写指针那一坑；若仍在使用中，说明积压了，计数并覆盖最旧的）
                    {
                        LONG w = InterlockedIncrement(&g_pendW) & 3;
                        if (g_pend[w].valid) { InterlockedIncrement(&g_xferSkip); }
                        g_pend[w].slot = (int)(g_frame % K);
                        g_pend[w].parity = par8;
                        g_pend[w].sig = g_lastSigV;
                        g_pend[w].frameTag = (int)g_frame;
                        g_pend[w].valid = true;
                    }
                    g_ourList = cl;




                    BformCopy(cl, g_outp8[par8].Get(), g_mvPacked.Get(), depRes, 0.0f, 0.0f);  // 内部跳颜色
                    // ★ 4K 底图内容抽检：读 1 行 8bit，均值接近 0 说明底图仍是黑的
                    if ((g_frame % 60) == 0) {
                        static ComPtr<ID3D12Resource> rbChk;
                        if (!rbChk && g_dev) {
                            D3D12_HEAP_PROPERTIES hpC{}; hpC.Type = D3D12_HEAP_TYPE_READBACK;
                            D3D12_RESOURCE_DESC bdC{};
                            bdC.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                            bdC.Width = (UINT64)g_outW * 4; bdC.Height = 1; bdC.DepthOrArraySize = 1; bdC.MipLevels = 1;
                            bdC.Format = DXGI_FORMAT_UNKNOWN; bdC.SampleDesc.Count = 1; bdC.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                            g_dev->CreateCommittedResource(&hpC, D3D12_HEAP_FLAG_NONE, &bdC, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rbChk.GetAddressOf()));
                        }
                        if (rbChk) {
                            D3D12_TEXTURE_COPY_LOCATION dC{}; dC.pResource = rbChk.Get();
                            dC.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                            dC.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                            dC.PlacedFootprint.Footprint.Width = g_outW;
                            dC.PlacedFootprint.Footprint.Height = 1;
                            dC.PlacedFootprint.Footprint.Depth = 1;
                            dC.PlacedFootprint.Footprint.RowPitch = g_outW * 4;
                            D3D12_TEXTURE_COPY_LOCATION sC{}; sC.pResource = g_outp8[par8].Get();
                            sC.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sC.SubresourceIndex = 0;
                            D3D12_BOX bx{}; bx.left = 0; bx.top = g_outH / 2; bx.front = 0; bx.right = g_outW; bx.bottom = g_outH / 2 + 1; bx.back = 1;
                            cl->CopyTextureRegion(&dC, 0, 0, 0, &sC, &bx);

                        }
                    }
                    Barrier(cl, g_outp8[par8].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                    DumpRecord(cl, g_dev.Get(), 0, g_outp8[par8].Get(), D3D12_RESOURCE_STATE_COMMON);
                }
            after_convert:;
            }
        } else {
            if (!g_bform4k) BformCopy(cl, color, mvRes, depRes, jx, jy);
        }
    }

    // F7 跑马灯诊断已关闭（A/B 双形态一致保持纯净画面）
    static int& mqOn = *(int*)&g_marqueeOn;
    if (mqOn < 0) mqOn = 0;

    // A2 records both planes on the same list that actually captured slot[N]. The
    // worker still requests N at dispatch N+2; no changes to presentation or feed lag.
    if (a2Direct && !skipAPixels && aCaptureValid && cur.frame == N && cur.captured && cur.color && cur.mv &&
        g_netS && g_psoDown && g_heap && (A2In::initialized.load() || !g_bformOn)) {
        g_curJitX = cur.jx; g_curJitY = cur.jy;
        const auto base = (UINT)(N % K);
        if (mqOn && !g_bformOn)
            MarkSlot(cl, cur.color.Get(), N, 0u, 0u,
                     D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        else
            Barrier(cl, cur.color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        SetPass(cl, g_psoDown.Get(), base, kSrvRes, kSrvRes, kSrvRes, kSrvRes, kSrvNetS, kSrvSmooth, kNetW, kNetH);
        Barrier(cl, cur.color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(cl, g_netS.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (g_fedTex[base]) {
            auto dst = A2In::Tex(g_fedTex[base].Get()), src = A2In::Tex(g_netS.Get());
            Barrier(cl, g_fedTex[base].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        auto* a2Mv = A2In::initialized.load() && !A2In::broken.load() ? A2EnsureMvScratch() : nullptr;
        if (g_psoMvPack && a2Mv && g_ansLocal) {
            if (auto* p = a2.Reserve({N, aChainGen, aChainResetEpoch})) {
                const auto slotMv = kSrvMvBase + base; // immutable descriptor for this captured texture
                Barrier(cl, cur.mv.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cl, a2Mv, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                SetPass(cl, g_psoMvPack.Get(), slotMv, slotMv, slotMv, slotMv, slotMv,
                        kSrvA2Mv, kSrvA2Mv, kNetW, kNetH);
                Barrier(cl, cur.mv.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                Barrier(cl, a2Mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                p->resources[0] = cur.color; p->resources[1] = cur.mv;
                p->resources[2] = g_netS; p->resources[3] = a2Mv; p->resources[4] = g_fedTex[base];
                p->descriptors = g_heap;
                f9A.Tap(F9Id::AColor, g_netS.Get(), N, N, F9Id::A2Pair);
                f9A.Tap(F9Id::AMv, a2Mv, N, N, F9Id::A2Pair);
                a2.Seal(g_netS.Get(), a2Mv);
                Barrier(cl, a2Mv, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
        } else if (A2In::initialized.load() || !g_bformOn) { ++A2In::missing; }
        Barrier(cl, g_netS.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // ---------- ④ [GPU] legacy (non-A2): slot[N-1] → 1080p/readback ----------
    if (!a2Direct && !skipAPixels && N >= 1 && (!g_backendNgx ||
        (N > aChainFrom && g_slots[(N - 1) % K].captured && g_slots[(N - 1) % K].frame == N - 1))) {
        Slot& F = g_slots[(N - 1) % K];
        g_curJitX = F.jx; g_curJitY = F.jy;   // 去抖动必须用【这一帧被降采样的槽位】的 jitter
        // ★ 跑马灯：喂入之前先把【槽位底图】涂成相位黑条 ⇒ 喂入的图必然是跑马灯
        //   开关：xess_proxy.marquee.txt = 1（关掉时行为与原来完全一致）
        {
            // ★ 2026-10-31：B 形态下这张图（A 侧槽位底图）既不上屏也不喂网络 —— 画它等于
            //   "第二次画图"，按要求只在【喂入之前的那张底图】上画一次（B 侧 BformBMarkSlot）。
            if (mqOn && !g_bformOn)
                MarkSlot(cl, F.color.Get(), (int)(N - 1), 0u, 0u,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            else
                Barrier(cl, F.color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        SetPass(cl, g_psoDown.Get(), (UINT)((N - 1) % K), kSrvRes, kSrvRes, kSrvRes, kSrvRes, kSrvNetS, kSrvSmooth, kNetW, kNetH);
        Barrier(cl, F.color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        {   // ★ 只涂一次：喂入图必须来自"已涂标的底图"，这里不再补涂（否则全链路涂两次）
            //   若实测喂入图无图案，说明"槽位涂标 → DownCS 采样"这一环有问题，要修那一环，而不是补涂
            static bool loggedOnce2 = false;
            if (!loggedOnce2) { loggedOnce2 = true; Log("跑马灯: 只在喂入前涂一次（不做二次补涂）"); }
        }
        Barrier(cl, g_netS.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION rd{}; rd.pResource = g_rb.Get();
        rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; rd.PlacedFootprint = g_fpRb;
        D3D12_TEXTURE_COPY_LOCATION rs{}; rs.pResource = g_netS.Get();
        rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; rs.SubresourceIndex = 0;
        cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
        aReadbackFrame = N - 1;   // 只登记实际录制了读回的帧，不从旧 g_rb 推测
        {   // S5-2：把这一帧的输入存进它自己的槽位（供之后算残差）
            int fi = (N - 1 + K) % K;
            if (g_fedTex[fi]) {
                D3D12_TEXTURE_COPY_LOCATION fd{};
                fd.pResource = g_fedTex[fi].Get();
                fd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                fd.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION fs{};
                fs.pResource = g_netS.Get();
                fs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                fs.SubresourceIndex = 0;
                Barrier(cl, g_fedTex[fi].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyTextureRegion(&fd, 0, 0, 0, &fs, nullptr);
            }
        }
        if (g_xWrite && g_xOk && g_xBufIn) {
            Barrier(cl, g_xBufIn.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION xd{};
            xd.pResource = g_xBufIn.Get();
            xd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            {   // ★按槽号写：与 inIndex 同一个槽，worker 按 idx 读才配对
                const int wi = 0;   // RE2 原样：单段
                xd.PlacedFootprint.Offset = XHDR + (UINT64)wi * AnsBytes();   // 每槽字节
            }
            xd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            xd.PlacedFootprint.Footprint.Width = kNetW;
            xd.PlacedFootprint.Footprint.Height = kNetH;
            xd.PlacedFootprint.Footprint.Depth = 1;
            xd.PlacedFootprint.Footprint.RowPitch = kNetW * 4;
            D3D12_TEXTURE_COPY_LOCATION xs{};
            xs.pResource = g_netS.Get();
            xs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            xs.SubresourceIndex = 0;
            f9A.Tap(F9Id::AColor, g_netS.Get(), N - 1, N - 1, F9Id::Shared);
            cl->CopyTextureRegion(&xd, 0, 0, 0, &xs, nullptr);
            Barrier(cl, g_xBufIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            static int cnt = 0;
            if ((cnt++ % 300) == 0) Log("R3: 已把 1080p 写入跨卡 buffer（第 %d 次）", cnt + 1);
        }
        if (!(g_xWrite && g_xOk && g_xBufIn))
            f9A.Tap(F9Id::AColor, g_netS.Get(), N - 1, N - 1, F9Id::CpuUpload);
        Barrier(cl, g_netS.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // ---------- ⑤ [GPU] 合成：slot[presentFrame] + 残差 → pColor ----------
    Slot* P = (presentFrame >= 0) ? &g_slots[presentFrame % K] : nullptr;
    bool composed = false;
    // ★ 2026-10-31 ②：B 形态正在出画时，A 侧不再合成 —— 原来它拿着【陈旧答案】空转
    //   （B 出画期间 ① 被跳过，hasAnswer 停在旧值 ⇒ 每帧白做一次合成，还污染 DIAGA 读数）。
    if (P && P->hasAnswer && !bPresenting && aHistoryReady) {
        // B1：第一次合成前把时域状态显式清成中性 0.5（不再依赖预热帧覆盖）
        static bool stateCleared = false;
        if (!stateCleared && g_cpuResSmooth.ptr) {
            stateCleared = true;
            D3D12_GPU_DESCRIPTOR_HANDLE gh = g_heap->GetGPUDescriptorHandleForHeapStart();
            gh.ptr += (SIZE_T)kSrvSmooth * g_inc;
            const FLOAT neutral[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
            cl->ClearUnorderedAccessViewFloat(gh, g_cpuResSmooth, g_resSmooth.Get(), neutral, 0, nullptr);
            Log("B1: 时域状态已清为中性 0.5（均值应为 127）");
        }
        // 状态纹理预热：前 12 次合成强制 alpha=1，把初值（0=强压暗）彻底覆盖掉
        if (g_stateWarmup > 0) { g_alpha = 1.0f; --g_stateWarmup; }
        // 【诊断】A 侧同口径：|答案 − 喂入|（与 B 侧 DIAG4 的"|答案-喂入|"是同一个定义）
        //   两边都是 1080p RGBA8 裸字节，且按帧身份配对，所以 A/B 这两个数可以直接比
        {
            static int aCnt = 0;
            if ((aCnt++ % 150) == 0 && !P->answer.empty() && P->answer.size() == P->feedInput.size()) {
                const size_t n = P->answer.size();
                double s = 0.0; size_t cnt = 0;
                for (size_t i = 0; i + 16 <= n; i += 16) {   // 每 4 个像素取 1 个，取 RGB 三通道
                    for (int c = 0; c < 3; ++c) {
                        int d = (int)P->answer[i + c] - (int)P->feedInput[i + c];
                        s += (d < 0) ? -d : d;
                    }
                    cnt += 3;
                }
                double mean = cnt ? s / (double)cnt : 0.0;
                Log("DIAGA[%d]: |答案-喂入| 均值 = %.2f/255（%.1f%%）← A 形态，与 B 的 DIAG4 同口径",
                    N, mean, mean * 100.0 / 255.0);
            }
        }
        std::vector<uint8_t> enc;
        BuildResidual(*P, enc);
        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        if (SUCCEEDED(g_resUp->Map(0, &nr, &m)) && m) {
            UINT pitch = g_fpRes.Footprint.RowPitch;
            for (UINT y = 0; y < kNetH; ++y)
                memcpy((unsigned char*)m + (size_t)y * pitch, &enc[(size_t)y * kNetW * 4], (size_t)kNetW * 4);
            D3D12_RANGE nw{ 0, (SIZE_T)g_bytesRes };
            g_resUp->Unmap(0, &nw);

            D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = g_resTex.Get();
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = g_resUp.Get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = g_fpRes;
            cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            Barrier(cl, g_resTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            {
                ID3D12Resource* fr = g_fedTex[presentFrame % K].Get();
                if (fr) Barrier(cl, fr, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            if (g_ansLocal && g_xBufOut) {   // S5-4：答案从共享 buffer 拷到本地（供 SRV 读）
                Barrier(cl, g_xBufOut.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cl, g_ansLocal.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyBufferRegion(g_ansLocal.Get(), 0, g_xBufOut.Get(), XHDR + (UINT64)(presentFrame % K) * AnsBytes(), AnsBytes());
                Barrier(cl, g_ansLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                f9A.Tap(F9Id::AAnswer, g_ansLocal.Get(), presentFrame, presentFrame, F9Id::CopyOnly);
                Barrier(cl, g_xBufOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                static bool loggedS54 = false;
                if (!loggedS54) { loggedS54 = true; Log("S5-4: 答案已拷到本地 buffer（可建 SRV）"); }
            }
            // GPU 残差路径下 t1 改绑【上一帧槽位颜色】，供颜色一致性验证使用
    UINT srvPrevCol = g_useGpuResid ? (UINT)((presentFrame + K - 1) % K) : kSrvRes;
    // ★ 答案 ↔ 喂入 的配对偏移（2077 实测：答案比喂入晚 2 帧）
    //   写 xess_proxy.fedoff.txt = 0/1/2/3 可现场调，不必重建
    static int fedOff = -1; static int fedOffTick = 0; static int fedOffLive = -1;
    if (fedOff < 0 || (++fedOffTick % 60) == 0) {   // ★每 60 帧重读一次 → 游戏内改文件即可实时扫
        std::string fp2 = ModuleDir() + "xess_proxy.fedoff.txt";
        FILE* ff = nullptr; char fb[8] = { 0 };
        if (fedOff < 0) fedOff = 2;
        if (fopen_s(&ff, fp2.c_str(), "r") == 0 && ff) {
            if (fgets(fb, sizeof(fb), ff)) { int v = atoi(fb); if (v >= 0 && v < (int)K) fedOff = v; }
            fclose(ff);
        }
        if (fedOff != fedOffLive) {
            fedOffLive = fedOff;
            Log("配对偏移: 答案↔喂入 fedOff=%d（残差改用 %d 帧前的喂入图）", fedOff, fedOff);
        }
    }
    // ★★ 整体滞后 L：底图 / 喂入 / MV 一起挪到【答案真正对应的那一帧】
    //   原因（2077 实测）：运行时交回的答案内容是更早某次作业的输出，
    //   我们却拿当前帧的底图/喂入去配它 ⇒ 残差混入画面差异 = 运动抖动残影。
    //   文件 xess_proxy.lag.txt = 0..5，每 60 帧重读（游戏内可扫）
    static int lagL = -1; static int lagTick = 0; static int lagLive = -1;
    if (lagL < 0 || (++lagTick % 60) == 0) {
        std::string lp = ModuleDir() + "xess_proxy.lag.txt";
        FILE* lf = nullptr; char lb[8] = { 0 };
        if (lagL < 0) lagL = 0;
        if (fopen_s(&lf, lp.c_str(), "r") == 0 && lf) {
            if (fgets(lb, sizeof(lb), lf)) { int v = atoi(lb); if (v >= 0 && v < (int)K) lagL = v; }
            fclose(lf);
        }
        if (lagL != lagLive) { lagLive = lagL; Log("整体滞后: L=%d（底图/喂入/MV 统一用 %d 帧前）", lagL, lagL); }
    }
    const UINT slotBase = (UINT)((presentFrame + K - lagL) % K);

    // ★ 合成前抽检【合成真正用的那个槽】：它的喂入图（feedInput）有没有我们的图案
    //   以及它的答案帧号是否等于该槽应有的帧号 —— 用来定位"答案配到未涂标喂入"
    if ((N % 30) == 0) {
        Slot& QB = g_slots[slotBase];
        if (QB.feedInput.size() >= (size_t)kNetW * kNetH * 4) {
            const uint8_t* rowB = QB.feedInput.data() + (size_t)(kNetH / 2) * kNetW * 4;
            size_t darkB = 0, totB = 0;
            for (UINT x = 0; x < kNetW; x += 32) {
                if (rowB[(size_t)x * 4 + 0] < 16 && rowB[(size_t)x * 4 + 1] < 16 && rowB[(size_t)x * 4 + 2] < 16) ++darkB;
                ++totB;
            }
            const int pctB = totB ? (int)(100.0 * (double)darkB / (double)totB) : -1;
            int ansF = -1;
            { std::lock_guard<std::mutex> lk3(g_worker.m); ansF = g_worker.ansFrame[slotBase]; }
            Log("CMPCHK: 帧%d 槽%d 该槽喂入黑占比=%d%% 该槽ansFrame=%d presentFrame=%d %s",
                N, slotBase, pctB, ansF, presentFrame, (pctB < 6 ? "★ 合成用的喂入没图案" : ""));

            // ★ RESID：量化"网络到底改了多少" —— mean|答案 − 喂入|（0~255 灰阶）
            {
                std::lock_guard<std::mutex> lk4(g_worker.m);
                const std::vector<uint8_t>& A = g_worker.ansBuf[slotBase];
                const std::vector<uint8_t>& Fd = QB.feedInput;
                if (A.size() >= (size_t)kNetW * kNetH * 4 && Fd.size() >= (size_t)kNetW * kNetH * 4) {
                    double sum = 0; int mx = 0; size_t cnt = 0, nz = 0;
                    for (UINT y = 0; y < kNetH; y += 8) {
                        const uint8_t* pa = A.data() + (size_t)y * kNetW * 4;
                        const uint8_t* pf = Fd.data() + (size_t)y * kNetW * 4;
                        for (UINT x = 0; x < kNetW; x += 8) {
                            for (int c = 0; c < 3; ++c) {
                                const int d = (int)pa[(size_t)x * 4 + c] - (int)pf[(size_t)x * 4 + c];
                                const int ad = d < 0 ? -d : d;
                                sum += ad; if (ad > mx) mx = ad; if (ad > 0) ++nz; ++cnt;
                            }
                        }
                    }
                    const double mean = cnt ? sum / (double)cnt : -1.0;
                    Log("RESID: 帧%d 槽%d mean|答案-喂入|=%.2f/255 (max=%d, 非零比例=%.1f%%)",
                        N, slotBase, mean, mx, cnt ? 100.0 * (double)nz / (double)cnt : -1.0);
                }
            }
        }
    }

    // ★ 底图行探针：抓 slotBase 的中间一行（1920x1），下一帧在 CPU 上判定黑条位置/是否变回原图
    // ★ 底图行探针：每 30 帧抓一次（与 F9 帧身份诊断无关）
    if ((N % 30) == 0) {
        if (!g_rowRb) {
            D3D12_HEAP_PROPERTIES hp3{}; hp3.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd3{};
            bd3.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd3.Width = (UINT64)kNetW * 8; bd3.Height = 1; bd3.DepthOrArraySize = 1; bd3.MipLevels = 1;
            bd3.Format = DXGI_FORMAT_UNKNOWN; bd3.SampleDesc.Count = 1; bd3.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            g_dev->CreateCommittedResource(&hp3, D3D12_HEAP_FLAG_NONE, &bd3, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(g_rowRb.GetAddressOf()));
        }
        if (g_rowRb) {
            ID3D12Resource* baseTex = g_slots[slotBase].color.Get();
            if (baseTex) {
                Barrier(cl, baseTex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION rd3{}; rd3.pResource = g_rowRb.Get();
                rd3.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                rd3.PlacedFootprint.Offset = 0;
                rd3.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                {   // ★ 宽度必须 ≤ 网络宽：2077 换档位后 g_cw 可能是 2259 > 1920
                    const UINT rw = (g_cw < kNetW) ? g_cw : kNetW;
                    if (rw < 8) { /* 尺寸异常，本轮跳过 */ }
                    rd3.PlacedFootprint.Footprint.Width = rw;
                    g_rowW = rw;
                }
                rd3.PlacedFootprint.Footprint.Height = 1;
                rd3.PlacedFootprint.Footprint.Depth = 1;
                rd3.PlacedFootprint.Footprint.RowPitch = kNetW * 8;
                D3D12_TEXTURE_COPY_LOCATION rs3{}; rs3.pResource = baseTex;
                rs3.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; rs3.SubresourceIndex = 0;
                D3D12_BOX box{}; box.left = 0; box.top = g_ch / 2; box.front = 0;
                box.right = g_rowW;
                cl->CopyTextureRegion(&rd3, 0, 0, 0, &rs3, &box);
                Barrier(cl, baseTex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                g_rowPendingSlot = (int)slotBase;
                g_rowPendingFrame = N;
            }
        }
    }
    const UINT srvPrevCol2 = g_useGpuResid ? (UINT)((slotBase + K - 1) % K) : kSrvRes;
    const UINT srvFedPair = (UINT)(kSrvFed + ((slotBase + K - fedOff) % K));
    const UINT srvMvPair  = (UINT)(kSrvMvBase + slotBase);
    SetPass(cl, g_psoResid.Get(), slotBase, srvPrevCol2, srvMvPair, srvFedPair, (UINT)kSrvAns, kSrvPre, kSrvSmooth, g_cw, g_ch);
     f9A.AnswerPath(g_useGpuResid && !g_bypass ? F9Id::GpuResidual : F9Id::CpuResidual);

    // ★ 槽位内容抽检：从 CPU 的 feedInput 取一行，统计"近黑像素占比"
    //   我们的图案 = 白底 + 1/4 宽黑条 ⇒ 黑占比 ≈ 25%
    //   游戏原图 ⇒ 黑占比通常 < 2%
    if ((N % 60) == 0) {
        Slot& Q2 = g_slots[((N - 1) % K + K) % K];
        if (!Q2.feedInput.empty()) {
            const UINT yy = kNetH / 2;
            const uint8_t* row = Q2.feedInput.data() + (size_t)yy * kNetW * 4;
            size_t dark = 0, total = 0;
            for (UINT x = 0; x < kNetW; x += 4) {
                if (row[(size_t)x * 4 + 0] < 16 && row[(size_t)x * 4 + 1] < 16 && row[(size_t)x * 4 + 2] < 16) ++dark;
                ++total;
            }
            Log("SLOTCHK: 帧%d 槽%d 喂入图 黑占比=%.1f%%（≈25%%=我们的图案，≈0%%=游戏原图）",
                N, ((N - 1) % K + K) % K, total ? 100.0 * (double)dark / (double)total : -1.0);
        }
    }

    // ★ 槽位抽检（每帧都查，不落盘）+ 突变自动抓图
    //   每帧算一次"喂入图的黑占比"（≈25% = 我们的图案，≈0% = 游戏原图）
    //   · 比例突变 ≥10%  ⇒ 打印 SLOTCHG，并在该帧 + 后续 2 帧自动落盘图片
    //   · 手工指令：xess_proxy.slotdump.txt = 组数 [间隔帧]（0 = 停）
    {
        static int lastPct = -1, dumpRemain = 0, dumpLastReq = -1, reqTick = 0, dumpGap = 15;
        static int burst = 0;

        // ★ 底图行探针：把上一帧抓的那一行读回来（FP16），判"黑条在哪个 1/4"以及是否为我们的图案
        if (g_rowPendingSlot >= 0 && g_rowRb) {
            void* pv = nullptr; D3D12_RANGE rr3{ 0, (SIZE_T)kNetW * 8 };
            if (SUCCEEDED(g_rowRb->Map(0, &rr3, &pv)) && pv) {
                const uint16_t* h16 = (const uint16_t*)pv;
                size_t dark[4] = { 0,0,0,0 }, tot[4] = { 0,0,0,0 };
                const UINT rw2 = (g_rowW > 0 && g_rowW <= kNetW) ? g_rowW : kNetW;
                for (UINT x = 0; x < rw2; ++x) {
                    const int q = (int)((UINT64)x * 4 / rw2);
                    if (q > 3) continue;
                    const float r = HalfToFloat(h16[(size_t)x * 4 + 0]);
                    const float g2 = HalfToFloat(h16[(size_t)x * 4 + 1]);
                    const float b = HalfToFloat(h16[(size_t)x * 4 + 2]);
                    const float lum = 0.2126f * r + 0.7152f * g2 + 0.0722f * b;
                    if (lum < 0.02f) ++dark[q];
                    ++tot[q];
                }
                Log("BASEROW: 帧%d 槽%d 四段黑占比 %.0f%% / %.0f%% / %.0f%% / %.0f%%（≈25%% 在一段 = 我们的图案）",
                    g_rowPendingFrame, g_rowPendingSlot,
                    tot[0] ? 100.0 * dark[0] / tot[0] : -1.0, tot[1] ? 100.0 * dark[1] / tot[1] : -1.0,
                    tot[2] ? 100.0 * dark[2] / tot[2] : -1.0, tot[3] ? 100.0 * dark[3] / tot[3] : -1.0);
                {   // 落盘成 raw（BGRA8，便于 raw2img 转换）＋ txt
                    std::string dir3 = ModuleDir() + "slotdump\\";
                    CreateDirectoryA(dir3.c_str(), nullptr);
                    char path3[MAX_PATH];
                    sprintf_s(path3, "%sbase_row_%d.raw", dir3.c_str(), g_rowPendingFrame);
                    FILE* f3 = nullptr;
                    if (fopen_s(&f3, path3, "wb") == 0 && f3) {
                        std::vector<uint8_t> row((size_t)kNetW * 4);
                        for (UINT x = 0; x < kNetW; ++x) {
                            auto q8 = [](float v) { int i = (int)(v * 255.0f + 0.5f); return (uint8_t)(i < 0 ? 0 : (i > 255 ? 255 : i)); };
                            const float r = HalfToFloat(h16[(size_t)x * 4 + 0]);
                            const float g2 = HalfToFloat(h16[(size_t)x * 4 + 1]);
                            const float b = HalfToFloat(h16[(size_t)x * 4 + 2]);
                            row[(size_t)x * 4 + 0] = q8(b);
                            row[(size_t)x * 4 + 1] = q8(g2);
                            row[(size_t)x * 4 + 2] = q8(r);
                            row[(size_t)x * 4 + 3] = 255;
                        }
                        fwrite(row.data(), 1, row.size(), f3);
                        fclose(f3);
                        sprintf_s(path3, "%sbase_row_%d.txt", dir3.c_str(), g_rowPendingFrame);
                        if (fopen_s(&f3, path3, "wb") == 0 && f3) {
                            const UINT rw4 = (g_rowW > 0 && g_rowW <= kNetW) ? g_rowW : kNetW;
                            fprintf(f3, "w=%u h=1 pitch=%u\n", rw4, rw4 * 4); fclose(f3);
                        }
                    }
                }
                D3D12_RANGE nw3{ 0, 0 }; g_rowRb->Unmap(0, &nw3);
            }
            g_rowPendingSlot = -1;
        }

        // 每帧轻量抽检（横中线上每 32 像素取一点，几乎不耗）
        const int sl2 = ((N - 1) % K + K) % K;
        Slot& Qc = g_slots[sl2];
        if (Qc.feedInput.size() >= (size_t)kNetW * kNetH * 4) {
            const uint8_t* row = Qc.feedInput.data() + (size_t)(kNetH / 2) * kNetW * 4;
            size_t dark = 0, tot = 0;
            for (UINT x = 0; x < kNetW; x += 32) {
                if (row[(size_t)x * 4 + 0] < 16 && row[(size_t)x * 4 + 1] < 16 && row[(size_t)x * 4 + 2] < 16) ++dark;
                ++tot;
            }
            const int pct = tot ? (int)(100.0 * (double)dark / (double)tot) : -1;
            if (lastPct < 0) { lastPct = pct; Log("抽检: 起始 黑占比=%d%%（≈25%%=我们的图案）", pct); }
            else if (pct >= 0) {
                static int burstUsed = 0, lastBurstAt = -100000;
                if (abs(pct - lastPct) >= 10) {
                    Log("SLOTCHG: 帧%d 槽%d 黑占比 %d%% → %d%%", N, sl2, lastPct, pct);
                    lastPct = pct;
                }
                if (pct < 6 && burstUsed < 4 && (N - lastBurstAt) >= 120) {
                    ++burstUsed; lastBurstAt = N; burst = 3;
                    Log("SLOTCHG: ★ 图案消失（黑占比 %d%% < 6%%），自动抓 3 帧（第 %d/4 次）", pct, burstUsed);
                }
            }
        }

        // 手工指令（每 30 帧看一次）
        if ((reqTick++ % 30) == 0) {
            std::string dp = ModuleDir() + "xess_proxy.slotdump.txt";
            FILE* df = nullptr; char db[16] = { 0 };
            int req = 0;
            if (fopen_s(&df, dp.c_str(), "r") == 0 && df) {
                if (fgets(db, sizeof(db), df)) {
                    int gap = 0;
                    if (sscanf_s(db, "%d %d", &req, &gap) >= 1 && gap >= 5 && gap <= 600) dumpGap = gap;
                }
                fclose(df);
            }
            if (req <= 0) dumpLastReq = -1;
            else if (req != dumpLastReq) {
                dumpLastReq = req; dumpRemain = req;
                Log("抽检指令: 收到 %d 组（每 %d 帧一组），开始落盘", req, dumpGap);
            }
        }

        // 落盘（手工指令 或 突变突发）
        const bool wantBurst = (burst > 0);
        const bool wantManual = (dumpRemain > 0 && (N % dumpGap) == 0);
        if (wantBurst || wantManual) {
            if (wantBurst) --burst;
            if (wantManual) --dumpRemain;
            std::string dir = ModuleDir() + "slotdump\\";
            CreateDirectoryA(dir.c_str(), nullptr);
            auto dumpOne = [&](const char* tag, const std::vector<uint8_t>& src) {
                if (src.size() < (size_t)kNetW * kNetH * 4) return;
                char path[MAX_PATH];
                sprintf_s(path, "%sslotdump_%s_%s%d.raw", dir.c_str(), wantBurst ? "chg_" : "", tag, N);
                FILE* f = nullptr;
                if (fopen_s(&f, path, "wb") == 0 && f) {
                    std::vector<uint8_t> row((size_t)kNetW * 4);
                    for (UINT y = 0; y < kNetH; ++y) {
                        const uint8_t* s = src.data() + (size_t)y * kNetW * 4;
                        for (UINT x = 0; x < kNetW; ++x) {
                            row[(size_t)x * 4 + 0] = s[(size_t)x * 4 + 2];
                            row[(size_t)x * 4 + 1] = s[(size_t)x * 4 + 1];
                            row[(size_t)x * 4 + 2] = s[(size_t)x * 4 + 0];
                            row[(size_t)x * 4 + 3] = 255;
                        }
                        fwrite(row.data(), 1, row.size(), f);
                    }
                    fclose(f);
                    sprintf_s(path, "%sslotdump_%s_%s%d.txt", dir.c_str(), wantBurst ? "chg_" : "", tag, N);
                    if (fopen_s(&f, path, "wb") == 0 && f) { fprintf(f, "w=%u h=%u pitch=%u\n", kNetW, kNetH, kNetW * 4); fclose(f); }
                    Log("抽检落盘: slotdump_%s%s%d.raw（槽%d）", wantBurst ? "chg_" : "", tag, N, sl2);
                }
            };
            dumpOne("fed", Qc.feedInput);
            { std::lock_guard<std::mutex> lk(g_worker.m); dumpOne("ans", g_worker.ansBuf[sl2]); }
        }
    }
    // F8 三横带诊断视图已关闭（与 A 形态 A3 一致）
    {
        static int& tbOn = *(int*)&g_triOn;
        if (tbOn < 0) tbOn = 0;
        if (false && tbOn) {
            TriPass(cl, (int)(presentFrame % 4), g_cw, g_ch, srvFedPair, (UINT)kSrvAns, slotBase);
            if (g_psoTri && g_pre) f9A.AnswerPath(F9Id::TriBands);
        }
    }
            {
                ID3D12Resource* fr = g_fedTex[presentFrame % K].Get();
                if (fr) Barrier(cl, fr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            }
            Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(cl, g_resTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

            // g_pre → pColor
            // B1（g_resid4k）：不改"配对"，仍喂**同一个槽位帧**给上采样器，
            //                 但 pColor 里只放【槽位原图】，残差 e 留到上采样后用 Add4K 叠到 4K 上。
            if (!g_resid4k) {
                Barrier(cl, g_pre.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cl, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyResource(color, g_pre.Get());
                Barrier(cl, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, g_pre.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            } else {
                Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cl, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyResource(color, P->color.Get());
                Barrier(cl, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                static int l4k = 0;
                if ((l4k++ % 300) == 0) Log("B1: 帧%d pColor ← 槽位%d 原图（残差留到 4K 叠加）", N, presentFrame % K);
            }
            composed = true; ++g_cntCompose;
        }
    }
    // ---------- ⑥ 调真 XeSS（替换成该槽位的 MV/深度/jitter）----------
    int rc = okRc;
    if (!did4kUpscale) {   // 4K 后置模式下，XeSS 已经提前在"敲铃之前"调过了（见 ② 区），这里不再调
        char buf[256]{};
        const size_t copyBytes = g_realDispatchFn ? 72u : sizeof(buf);
        if (params) memcpy(buf, params, copyBytes);
        if (composed && P) {
            if (P->mv)    *(void**)(buf + 8)  = P->mv.Get();
            if (P->depth) *(void**)(buf + 16) = P->depth.Get();
            memcpy(buf + 48, &P->jx, 4);
            memcpy(buf + 52, &P->jy, 4);

        }
        // jitter 缩放实验：0=完全不抖（静止最稳，无时域增益），1=原样
        if (g_jitterScale != 1.0f) {
            float jx = 0, jy = 0;
            memcpy(&jx, buf + 48, 4); memcpy(&jy, buf + 52, 4);
            jx *= g_jitterScale; jy *= g_jitterScale;
            memcpy(buf + 48, &jx, 4); memcpy(buf + 52, &jy, 4);
        }
        rc = real ? real(ctx, cmdList, buf) : errRc;
    }

    // ---------- ⑥.2 【bform=9】把 XeSS 刚产出的 4K 画面拷进跨卡槽位（同帧，零额外延迟）----------
    //   为什么能同帧：XeSS 是我们自己在这个命令列表里调用的（⑥），所以拷贝可以排在它后面。
    //   若改成"下一帧再从 pOutput 取"，就会白晚一帧。
    if (!g_c4On && !did4kUpscale && g_bform4k && g_bformOn && g_bformStage >= 6 && g_outpRes && g_psoFmt8 && UiGateAllowsNr()) {
        if (!g_outp8Ready[0]) {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = g_outW; td.Height = g_outH;
            td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_outp8[0]))) && g_outp8[0]) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
                h.ptr += (SIZE_T)kSrvOutp8 * g_inc;
                g_dev->CreateUnorderedAccessView(g_outp8[0].Get(), nullptr, &ud, h);
                g_outp8Ready[0] = true;
                Log("BFORM: 4K 后置转换目标就绪 %ux%u（8bit）", g_outW, g_outH);
            }
        }
        if (g_outp8Ready[0]) {
            Barrier(cl, g_outpRes.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, g_outp8[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap* hh9[] = { g_heap.Get() };
            cl->SetDescriptorHeaps(1, hh9);
            SetPass(cl, g_psoFmt8.Get(), kSrvOutp, kSrvOutp, kSrvOutp, kSrvOutp, kSrvOutp,
                    kSrvOutp8, kSrvOutp8, g_outW, g_outH);
            Barrier(cl, g_outpRes.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, g_outp8[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            // ★ 注意：这里【不】拷进槽位！拷贝挪到下一帧开头（敲铃之前），见 BformCopy 调用处。
            //   原因：worker 在 A 敲铃时就会去解码槽位，如果拷贝发生在敲铃之后，
            //   B 会读到"正在被写"的槽位 → 每帧抖动 = 闪。
        }
    }

    // ---------- ⑥.5 [B1] 残差在 4K 上叠加：outp += bilinear(g_pre − 槽位原图) ----------
    if (g_resid4k && composed && P && outp && g_pre4k && g_psoAdd4K) {
        bindCurrentOutpSrv();   // B+C4 不更新 kSrvOutp；A Add4K 自己绑定本帧的输出
        Barrier(cl, g_pre.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_pre4k.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* h4[] = { g_heap.Get() };
        cl->SetDescriptorHeaps(1, h4);
        SetPass(cl, g_psoAdd4K.Get(), kSrvOutp, kSrvPreSrv,
                (UINT)(presentFrame % K), kSrvPre4K, kSrvPre4K,
                kSrvPre4K, kSrvPre4K, g_outW, g_outH);
        Barrier(cl, g_pre4k.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, outp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(outp, g_pre4k.Get());
        Barrier(cl, outp, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, g_pre4k.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(cl, g_pre.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        static int l4k2 = 0;
        if ((l4k2++ % 300) == 0) Log("B1: 帧%d 已把残差叠到 4K（outp %ux%u）", N, g_outW, g_outH);
    }

    if (!did4kUpscale) ProxyImGuiDraw(cl, outp, !g_bformOn && real && g_realDispatchFn && rc == okRc);   // 4K 模式下已在 ⑥ 之后提前画过（要跟着底图跨卡上屏）
    MirrorTick(cl, outp);       // S1：镜像叠层（默认关；关时立即返回）
    if ((g_frame % 300) == 0) BwPollFile();
    BwTick();                   // 带宽测试（默认关；关时立即返回）

    A2In::Stats(N);
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - tA).count();
    if ((N % 150) == 0)
        Log("stats: 帧%d 提交 %.2fms worker=%.1fms #%d 合成=%d", N, ms, g_worker.lastMs, g_worker.processed, (int)composed);
    return rc;
}


// ---------------- ImGui 覆盖层（Del 唤出，键盘导航） ----------------
static ComPtr<ID3D12DescriptorHeap> g_imguiSrvHeap;
static ComPtr<ID3D12DescriptorHeap> g_imguiRtvHeap;
static bool g_imguiInited = false, g_imguiFail = false;
static WNDPROC g_origWndProc = nullptr;

// 菜单打开时把键鼠消息吞掉，不传给游戏（OptiScaler 的做法）
static LRESULT CALLBACK ProxyWndProc(HWND, UINT, WPARAM, LPARAM);   // 前置声明
// 按进程 ID + 面积找游戏主窗口（不依赖"前台窗口"，初始化时游戏可能还不是前台）
struct FindWndCtx { DWORD pid; HWND best; long bestArea; };
static BOOL CALLBACK FindWndProc(HWND h, LPARAM lp) {
    auto* c = (FindWndCtx*)lp;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h)) return TRUE;
    RECT r{}; if (!GetClientRect(h, &r)) return TRUE;
    long area = (r.right - r.left) * (r.bottom - r.top);
    if (area > c->bestArea) { c->bestArea = area; c->best = h; }
    return TRUE;
}
static HWND FindGameWindowOnce() {
    FindWndCtx c{ GetCurrentProcessId(), nullptr, 0 };
    EnumWindows(FindWndProc, (LPARAM)&c);
    return c.best;
}
// 只定位句柄、不装 WndProc：B 形态出画需要它，但装 WndProc（吞键鼠）只在开菜单时才需要。
// 以前两者绑在一起、且唯一入口是 Del → 不开菜单 B 就没有窗口可用。
static HWND ResolveGameHwnd() {
    if (g_gameHwnd && IsWindow(g_gameHwnd)) return g_gameHwnd;
    HWND h = FindGameWindowOnce();
    if (!h) return nullptr;
    RECT r{};
    if (!GetClientRect(h, &r)) return nullptr;
    const long cw = r.right - r.left, ch = r.bottom - r.top;
    if (cw < 640 || ch < 360) return nullptr;   // 太小的窗口（启动器/提示框）不是主渲染窗口
    g_gameHwnd = h;
    char cls[128] = { 0 }, ttl[256] = { 0 };
    GetClassNameA(h, cls, 128); GetWindowTextA(h, ttl, 256);
    Log("WND: 已定位游戏主窗口 类=%s 标题=%s 客户区=%ldx%ld（不依赖菜单）", cls, ttl, cw, ch);
    return g_gameHwnd;
}
// ---- 光标争夺：IAT 挂钩 ClipCursor / SetCursorPos（菜单开着时屏蔽游戏的中置/裁剪）----
static BOOL(WINAPI* g_origClipCursor)(const RECT*) = nullptr;
static BOOL(WINAPI* g_origSetCursorPos)(int, int) = nullptr;
static BOOL WINAPI HookClipCursor(const RECT* r) {
    if (g_menuOpen) return TRUE;                      // 菜单开着：游戏想把光标锁回中心，直接拒
    return g_origClipCursor ? g_origClipCursor(r) : TRUE;
}
static BOOL WINAPI HookSetCursorPos(int x, int y) {
    if (g_menuOpen) return TRUE;                      // 菜单开着：不许游戏把光标搬走
    return g_origSetCursorPos ? g_origSetCursorPos(x, y) : TRUE;
}

static bool PatchIatOne(HMODULE mod, const char* dllName, const char* fnName, void* newFn, void** oldFn) {
    if (!mod || !newFn) return false;
    auto* dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)mod + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dll = (const char*)((BYTE*)mod + imp->Name);
        if (_stricmp(dll, dllName) != 0) continue;
        auto* oft = (IMAGE_THUNK_DATA*)((BYTE*)mod + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* ft  = (IMAGE_THUNK_DATA*)((BYTE*)mod + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)((BYTE*)mod + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fnName) != 0) continue;
            DWORD old = 0;
            if (VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
                if (oldFn) *oldFn = (void*)ft->u1.Function;
                ft->u1.Function = (ULONG_PTR)newFn;
                VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
                return true;
            }
        }
    }
    return false;
}

// ================= 进程内 Present 钩子（取最终画面：tonemap + UI 之后）=================
//   复用本文件已有的 PatchIatOne：IAT 钩住 dxgi 的建交换链函数 → 拿到交换链 → 钩 vtable Present
static const int SLOT_Present = 8, SLOT_Present1 = 22;
static void** g_pSCOrig = nullptr;      // 原 Present 表
static void** g_pSCNew  = nullptr;      // 我们的表
static std::vector<IDXGISwapChain*> g_scList;   // 已钩到的交换链（防重复）
static ID3D12CommandAllocator*   g_presAl = nullptr;
static ID3D12GraphicsCommandList* g_presCl = nullptr;

// ===== 向同进程的 dxgi.dll（我们的 present_grab）索取"最终画面" =====
//   成功返回 ID3D12Resource*（已 AddRef，调用方负责 Release），失败返回 nullptr
typedef void* (__cdecl* PFN_PRESENTGRABGET)(int*, int*, int*, int*);
static ID3D12Resource* PresentGrabQuery(int* frameOut, int* w, int* h, int* fmt) {
    static PFN_PRESENTGRABGET fn = nullptr; static bool tried = false;
    static bool ok = false;
    if (!tried) {
        tried = true;
        HMODULE m = GetModuleHandleW(L"dxgi.dll");
        if (m) fn = (PFN_PRESENTGRABGET)GetProcAddress(m, "PresentGrabGet");
        ok = (fn != nullptr);
        Log("Present 抓图: dxgi.dll=%p PresentGrabGet=%p（%s）", (void*)m, (void*)fn, ok ? "可用" : "不可用");
    }
    if (!fn) return nullptr;
    void* r = fn(frameOut, w, h, fmt);
    static int qn = 0;
    if ((qn++ % 120) == 0)
        Log("PresentGrabQuery: tex=%p frame=%d %dx%d fmt=%d", r, frameOut ? *frameOut : -1,
            w ? *w : 0, h ? *h : 0, fmt ? *fmt : 0);
    return (ID3D12Resource*)r;
}

// ===== 共享 vtable 钩子：不依赖游戏怎么建交换链 =====
//   自建一条 64x64 的假交换链（同一个 dxgi 实现 ⇒ 同一个类虚表），
//   直接把虚表内存里的 Present/Present1 改掉 ⇒ 游戏那条交换链也会走进来
static void InstallVtablePresentHook() {
    static bool done = false;
    if (done || !g_dev) return;
    done = true;

    static HWND dw = nullptr;
    if (!dw) {
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NRSharedVtblWnd";
        RegisterClassExW(&wc);
        dw = CreateWindowExW(0, L"NRSharedVtblWnd", L"", WS_OVERLAPPEDWINDOW,
                             0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    }
    if (!dw) { Log("vtable 钩子: 建窗口失败"); return; }

    HMODULE sys = LoadLibraryW(L"C:\\Windows\\System32\\dxgi.dll");
    if (!sys) { Log("vtable 钩子: 载入系统 dxgi 失败"); return; }
    auto cf1 = (HRESULT(WINAPI*)(REFIID, void**))GetProcAddress(sys, "CreateDXGIFactory1");
    if (!cf1) { Log("vtable 钩子: 无 CreateDXGIFactory1"); return; }

    ComPtr<IDXGIFactory2> f2;
    if (FAILED(cf1(IID_PPV_ARGS(&f2))) || !f2) { Log("vtable 钩子: 建工厂失败"); return; }

    ComPtr<ID3D12CommandQueue> q;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) { Log("vtable 钩子: 建队列失败"); return; }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 64; sd.Height = 64;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(f2->CreateSwapChainForHwnd(q.Get(), dw, &sd, nullptr, nullptr, &sc1))) {
        Log("vtable 钩子: 建假交换链失败"); return;
    }

    void** vt = *(void***)sc1.Get();
    if (!g_pSCOrig) {
        g_pSCOrig = (void**)malloc(sizeof(void*) * 32);
        memcpy(g_pSCOrig, vt, sizeof(void*) * 32);
    }
    // ★ 收窄补丁：只碰 Present(8)；Present1(22) 仅在确认对象是 IDXGISwapChain1 时才写
    //   上次崩溃最可能就是写了 32 个槽 / 越界写到相邻函数指针
    ComPtr<IDXGISwapChain1> probe;
    const bool hasV1 = SUCCEEDED(sc1->QueryInterface(IID_PPV_ARGS(&probe))) && probe;
    bool okP = false, okP1 = false;
    DWORD old = 0;
    if (VirtualProtect(&vt[SLOT_Present], sizeof(void*), PAGE_READWRITE, &old)) {
        vt[SLOT_Present] = (void*)&Detour_Present;
        okP = true;
    }
    if (hasV1 && VirtualProtect(&vt[SLOT_Present1], sizeof(void*), PAGE_READWRITE, &old)) {
        vt[SLOT_Present1] = (void*)&Detour_Present1;
        okP1 = true;
    }
    Log("vtable 钩子: Present=%d Present1=%d (IDXGISwapChain1=%d) 原表=%p",
        (int)okP, (int)okP1, (int)hasV1, (void*)g_pSCOrig);
}

// ★ 线程安全改造：Present 线程只登记"要抓哪条交换链 + 帧号"，
//   真正的 GetBuffer/CopyResource 挪到渲染线程（ProxyExecute）里做
static volatile void* g_pendingSC = nullptr;
static volatile int  g_pendingFrame = -1;

static void GrabBackbuffer(IDXGISwapChain* sc) {
    if (!g_baseFromPresent || g_cw == 0) return;
    g_pendingSC = (void*)sc;
    g_pendingFrame = g_frameNow;
    return;
}

// 在渲染线程调用：把上一帧登记的交换链后缓冲拷进槽位副本
static void DoPendingGrab() {
    IDXGISwapChain* sc = (IDXGISwapChain*)g_pendingSC;
    if (!sc || !g_dev || g_cw == 0) return;
    ComPtr<ID3D12Resource> bb;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) return;
    D3D12_RESOURCE_DESC d = bb->GetDesc();
    if ((UINT)d.Width != g_cw || d.Height != g_ch) return;      // 尺寸没对齐则跳过（切档位瞬间）
    const int slot = ((g_frameNow % K) + K) % K;
    // 该槽的画面副本（首次按后缓冲的格式/尺寸建）
    if ((int)g_imgSlots.size() <= slot) g_imgSlots.resize(K);
    if (!g_imgSlots[slot]) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = d; td.Flags = D3D12_RESOURCE_FLAG_NONE;
        td.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        td.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        td.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(g_imgSlots[slot].GetAddressOf())))) return;
    }
    if (!g_presAl) {
        if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_presAl)))) return;
        if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_presAl, nullptr, IID_PPV_ARGS(&g_presCl)))) return;
        g_presCl->Close();
    }
    g_presAl->Reset();
    g_presCl->Reset(g_presAl, nullptr);
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_presCl->ResourceBarrier(1, &b);
    }
    g_presCl->CopyResource(g_imgSlots[slot].Get(), bb.Get());
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        g_presCl->ResourceBarrier(1, &b);
    }
    g_presCl->Close();
    ID3D12CommandList* ls[] = { g_presCl };
    // 用我们自己的拷贝队列执行（不与游戏队列抢顺序）
    if (g_xqQ) { g_xqQ->ExecuteCommandLists(1, ls); }
    g_imgFrame[slot] = g_frameNow;
}

static HRESULT STDMETHODCALLTYPE Detour_Present(IDXGISwapChain* self, UINT sync, UINT flags);
static HRESULT STDMETHODCALLTYPE Detour_Present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp);

static void HookOneSwapChain(IDXGISwapChain* sc) {
    if (!sc || !g_dev) return;
    for (auto* p : g_scList) if (p == sc) return;
    if (!g_pSCOrig) {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
            void** vtbl = *(void***)sc1;
            g_pSCOrig = (void**)malloc(sizeof(void*) * 32);
            memcpy(g_pSCOrig, vtbl, sizeof(void*) * 32);
            g_pSCNew = (void**)malloc(sizeof(void*) * 32);
            memcpy(g_pSCNew, g_pSCOrig, sizeof(void*) * 32);
            g_pSCNew[SLOT_Present]  = (void*)&Detour_Present;
            g_pSCNew[SLOT_Present1] = (void*)&Detour_Present1;
            sc1->Release();
        }
    }
    if (!g_pSCOrig || !g_pSCNew) return;
    *(void***)sc = g_pSCNew;      // 换掉该交换链对象的 vtable 指针
    g_scList.push_back(sc);
    Log("Present 钩子: 已挂到交换链 %p（共 %d 条）", (void*)sc, (int)g_scList.size());
}

static HRESULT STDMETHODCALLTYPE Detour_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    __try {
        ID3D12Resource* t = nullptr;   // 裸指针：__try 里不能有带析构的局部对象
        if (self && SUCCEEDED(self->GetBuffer(0, IID_PPV_ARGS(&t))) && t) { t->Release(); GrabBackbuffer(self); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    const HRESULT hrPres = ((Fn)g_pSCOrig[SLOT_Present])(self, sync, flags);
    // ★ v4 P12/P20：Present 成功 → 递减预热计数（STARTING → RUNNING）。
    //   这里调的是【游戏自己那条交换链】的 Present：一般失败不算我们的硬错（可能只是被遮挡/窗口最小化），
    //   只有设备级失联（REMOVED/RESET/HUNG）才走 NrHardFail。
    if (SUCCEEDED(hrPres))      OnPresentSuccess();
    else if (HrIsDeviceLost(hrPres)) NrHardFail("Present failed (device lost)");
    return hrPres;
}
static HRESULT STDMETHODCALLTYPE Detour_Present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    __try {
        ID3D12Resource* t = nullptr;
        if (self && SUCCEEDED(self->GetBuffer(0, IID_PPV_ARGS(&t))) && t) { t->Release(); GrabBackbuffer(self); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    const HRESULT hrPres = ((Fn)g_pSCOrig[SLOT_Present1])(self, sync, flags, pp);
    if (SUCCEEDED(hrPres))           OnPresentSuccess();   // ★ v4 P12
    else if (HrIsDeviceLost(hrPres)) NrHardFail("Present1 failed (device lost)");   // ★ v4 P20
    return hrPres;
}

// dxgi 建交换链的 IAT 桩（拿到交换链立刻挂钩）
typedef HRESULT(WINAPI* PFN_CSCFHWND)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
static PFN_CSCFHWND g_origCSCF = nullptr;
static HRESULT WINAPI Hook_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* out, IDXGISwapChain1** pp) {
    HRESULT hr = g_origCSCF ? g_origCSCF(f, dev, hwnd, d, fd, out, pp) : E_FAIL;
    if (SUCCEEDED(hr) && pp && *pp) { __try { HookOneSwapChain((IDXGISwapChain*)*pp); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return hr;
}
// 另外两种建交换链入口（2077 可能走其中任一条）
typedef HRESULT(WINAPI* PFN_CSC)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(WINAPI* PFN_CSCFC)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGISwapChain1**);
static PFN_CSC   g_origCSC   = nullptr;
static PFN_CSCFC g_origCSCFC = nullptr;
static HRESULT WINAPI Hook_CreateSwapChain(IDXGIFactory* f, IUnknown* d, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** pp) {
    HRESULT hr = g_origCSC ? g_origCSC(f, d, desc, pp) : E_FAIL;
    if (SUCCEEDED(hr) && pp && *pp) { __try { HookOneSwapChain(*pp); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return hr;
}
static HRESULT WINAPI Hook_CreateSwapChainForComposition(IDXGIFactory2* f, IUnknown* d, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGISwapChain1** pp) {
    HRESULT hr = g_origCSCFC ? g_origCSCFC(f, d, desc, pp) : E_FAIL;
    if (SUCCEEDED(hr) && pp && *pp) { __try { HookOneSwapChain((IDXGISwapChain*)*pp); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return hr;
}
// ================= 工厂入口钩子（2077 走 COM 虚表，必须钩工厂创建）=================
static const int SLOT_CreateSwapChain = 10, SLOT_CreateSwapChainForHwnd = 15, SLOT_CreateSwapChainForComposition = 24;
static void** g_facOrig = nullptr;
static void** g_facNew  = nullptr;
static std::vector<void*> g_facList;

typedef HRESULT(WINAPI* PFN_DXGI_CF1)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_DXGI_CF2)(UINT, REFIID, void**);
static PFN_DXGI_CF1 g_origCF1 = nullptr;
static PFN_DXGI_CF2 g_origCF2 = nullptr;

static HRESULT WINAPI Detour_FacCSC(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** pp) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    HRESULT hr = ((Fn)g_facOrig[SLOT_CreateSwapChain])(self, dev, desc, pp);
    if (SUCCEEDED(hr) && pp && *pp) { __try { HookOneSwapChain(*pp); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return hr;
}
static HRESULT WINAPI Detour_FacCSCFHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* out, IDXGISwapChain1** pp) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    HRESULT hr = ((Fn)g_facOrig[SLOT_CreateSwapChainForHwnd])(self, dev, hwnd, d, fd, out, pp);
    if (SUCCEEDED(hr) && pp && *pp) { __try { HookOneSwapChain((IDXGISwapChain*)*pp); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return hr;
}
static HRESULT WINAPI Detour_FacCSCFComp(IDXGIFactory2* self, IUnknown* dev,
    const DXGI_SWAP_CHAIN_DESC1* d, IDXGISwapChain1** pp) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGISwapChain1**);
    HRESULT hr = ((Fn)g_facOrig[SLOT_CreateSwapChainForComposition])(self, dev, d, pp);
    if (SUCCEEDED(hr) && pp && *pp) { __try { HookOneSwapChain((IDXGISwapChain*)*pp); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return hr;
}

static void PatchFactory(void* fac) {
    if (!fac) return;
    for (auto* q : g_facList) if (q == fac) return;
    if (!g_facOrig) {
        void** vtbl = *(void***)fac;
        g_facOrig = (void**)malloc(sizeof(void*) * 32);
        memcpy(g_facOrig, vtbl, sizeof(void*) * 32);
        g_facNew = (void**)malloc(sizeof(void*) * 32);
        memcpy(g_facNew, g_facOrig, sizeof(void*) * 32);
        g_facNew[SLOT_CreateSwapChain]              = (void*)&Detour_FacCSC;
        g_facNew[SLOT_CreateSwapChainForHwnd]       = (void*)&Detour_FacCSCFHwnd;
        g_facNew[SLOT_CreateSwapChainForComposition]= (void*)&Detour_FacCSCFComp;
    }
    *(void***)fac = g_facNew;
    g_facList.push_back(fac);
    Log("Factory hook: patched %p (total %d)", fac, (int)g_facList.size());
}

static HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void** pp) {
    if (!g_origCF1) { HMODULE m = LoadLibraryW(L"dxgi.dll"); if (m) g_origCF1 = (PFN_DXGI_CF1)GetProcAddress(m, "CreateDXGIFactory1"); }
    if (!g_origCF1) return E_FAIL;
    HRESULT hr = g_origCF1(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) { __try { PatchFactory(*pp); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return hr;
}
static HRESULT WINAPI Hook_CreateDXGIFactory2(UINT flags, REFIID riid, void** pp) {
    if (!g_origCF2) { HMODULE m = LoadLibraryW(L"dxgi.dll"); if (m) g_origCF2 = (PFN_DXGI_CF2)GetProcAddress(m, "CreateDXGIFactory2"); }
    if (!g_origCF2) return E_FAIL;
    HRESULT hr = g_origCF2(flags, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) { __try { PatchFactory(*pp); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return hr;
}

static void EnsurePresentHook() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE main = GetModuleHandleW(nullptr);
    const bool a = PatchIatOne(main, "dxgi.dll", "CreateDXGIFactory1", (void*)Hook_CreateDXGIFactory1, (void**)&g_origCF1);
    const bool b = PatchIatOne(main, "dxgi.dll", "CreateDXGIFactory2", (void*)Hook_CreateDXGIFactory2, (void**)&g_origCF2);
    Log("Present hook: CreateDXGIFactory1=%d CreateDXGIFactory2=%d", (int)a, (int)b);
}
static void EnsureCursorHook() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE main = GetModuleHandleW(nullptr);
    const bool a = PatchIatOne(main, "USER32.dll", "ClipCursor",   (void*)HookClipCursor,   (void**)&g_origClipCursor);
    const bool b = PatchIatOne(main, "USER32.dll", "SetCursorPos", (void*)HookSetCursorPos, (void**)&g_origSetCursorPos);
    Log("光标钩子: ClipCursor=%s SetCursorPos=%s（菜单开着时游戏无法把光标锁回中心）",
        a ? "已挂" : "未找到", b ? "已挂" : "未找到");
}

static void EnsureWndHook() {
    EnsureCursorHook();
    ResolveGameHwnd();
    if (g_gameHwnd && !g_origWndProc) {
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)ProxyWndProc);
        char cls2[128] = { 0 }, ttl2[256] = { 0 };
        GetClassNameA(g_gameHwnd, cls2, 128); GetWindowTextA(g_gameHwnd, ttl2, 256);
        RECT rr{}; GetClientRect(g_gameHwnd, &rr);
        Log("ImGui: hook 窗口 类=%s 标题=%s 客户区=%dx%d 原过程=%p", cls2, ttl2, (int)(rr.right-rr.left), (int)(rr.bottom-rr.top), (void*)g_origWndProc);
    }
}
static LRESULT CALLBACK ProxyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_menuOpen) {
        switch (m) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR: case WM_INPUT:
            return 0;   // 吞掉
        default: break;
        }
    }
    return g_origWndProc ? CallWindowProcW(g_origWndProc, h, m, w, l) : DefWindowProcW(h, m, w, l);
}
static bool g_menuVisible = false;

static bool ProxyImGuiInit() {
    if (g_imguiInited || g_imguiFail) return g_imguiInited;
    if (!g_dev) { g_imguiFail = true; return false; }
    __try {
        D3D12_DESCRIPTOR_HEAP_DESC sh{};
        sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sh.NumDescriptors = 8;
        sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(g_imguiSrvHeap.GetAddressOf())))) { g_imguiFail = true; return false; }
        D3D12_DESCRIPTOR_HEAP_DESC rh{};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = 1;
        if (FAILED(g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(g_imguiRtvHeap.GetAddressOf())))) { g_imguiFail = true; return false; }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();                 // ★ 上次漏掉这句 → 必然崩溃
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // 中文字体：默认字体没有汉字，必须从系统字体加载（失败则回退默认字体）
        {
            const char* cands[] = {
                "C:\\Windows\\Fonts\\msyh.ttc",  "C:\\Windows\\Fonts\\msyhbd.ttc",
                "C:\\Windows\\Fonts\\simhei.ttf","C:\\Windows\\Fonts\\simsun.ttc",
                "C:\\Windows\\Fonts\\Deng.ttf"
            };
            bool fontOk = false;
            for (const char* p : cands) {
                if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) continue;
                if (io.Fonts->AddFontFromFileTTF(p, 20.0f, nullptr,
                        io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) { fontOk = true; break; }
            }
            if (!fontOk) { io.Fonts->AddFontDefault(); Log("ImGui: 未找到中文字体，回退默认字体"); }
            else Log("ImGui: 中文字体已加载");
        }
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2((float)g_outW, (float)g_outH);
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        if (!ImGui_ImplDX12_Init(g_dev.Get(), 4, DXGI_FORMAT_R10G10B10A2_UNORM, g_imguiSrvHeap.Get(),
                                 g_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
                                 g_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart())) {
            g_imguiFail = true; Log("ImGui: 后端初始化失败（已禁用）"); return false;
        }
        ImGui::StyleColorsDark();
        g_baseStyle = ImGui::GetStyle();          // 保存未缩放的基准
        ImGui::GetStyle().ScaleAllSizes(1.6f);    // 基础缩放（界面缩放滑条在此之上再乘）
        io.FontGlobalScale = 1.0f;
        Log("ImGui: hook 推迟到打开菜单时再装（那时游戏一定是前台）");
        PatchInputIAT();
        g_imguiInited = true;
        Log("ImGui: 覆盖层就绪（Del 开关）");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_imguiFail = true;
        Log("ImGui: 初始化异常（已禁用）");
        return false;
    }
    return g_imguiInited;
}

// ★ 单独成函数：ProxyImGuiDraw 里有 SEH(__try)，在它里面放 std::string 等带析构的对象会触发 C2712
static void MenuSectionNrControls() {
    ImGui::TextDisabled("════ NR 网络控制（直接作用于神经网络）════");
    bool on = g_nrCtrlOn;
    if (ImGui::Checkbox("启用网络参数控制", &on)) { g_nrCtrlOn = on; g_ctrlDirty = true; }
    ImGui::SetItemTooltip("关闭时按出厂默认（Intensity=1.00, Style=0）运行；开启后以下参数即时发给 NR 网络");
    float fi = (float)g_nrIntensity;
    if (ImGui::SliderFloat("NR 网络强度 (Intensity)", &fi, 0.0f, 2.0f, "%.2f")) { g_nrIntensity = fi; g_ctrlDirty = true; }
    ImGui::SetItemTooltip("直接控制神经网络增强/去噪强度：0.00 = 无增强，1.00 = 默认，2.00 = 双倍强度");
    int st = (int)g_nrStyle;
    if (st < 0 || st > 2) st = 0;
    if (ImGui::Combo("风格 (Style)", &st, "0 默认 (Standard)\0" "1 自然 (Natural)\0" "2 电影 (Cinematic)\0")) {
        g_nrStyle = st; g_ctrlDirty = true;
    }
    ImGui::SetItemTooltip("0=默认标准，1=自然，2=电影；改完下一帧立即生效，不重建会话");
    float ft = (float)g_nrTone, fs = (float)g_nrStruct, fsk = (float)g_nrSkin;
    if (ImGui::SliderFloat("局部影调 (LocalTone)", &ft, 0.0f, 2.0f, "%.2f")) { g_nrTone = ft; g_ctrlDirty = true; }
    if (ImGui::SliderFloat("局部结构 (LocalStruct)", &fs, 0.0f, 2.0f, "%.2f")) { g_nrStruct = fs; g_ctrlDirty = true; }
    if (ImGui::SliderFloat("皮肤结构 (SkinStruct)", &fsk, -1.0f, 1.0f, "%.2f")) { g_nrSkin = fsk; g_ctrlDirty = true; }
    ImGui::SetItemTooltip("-1.00 = 不独立处理皮肤（默认）；0.00~1.00 需开启下方「自动人物掩码」才生效");
    bool am = g_nrAutoMask;
    if (ImGui::Checkbox("自动人物/皮肤掩码 (UseAutoMask)", &am)) { g_nrAutoMask = am; g_ctrlDirty = true; }
    if (!g_backendNgx) {
        float gr = (float)g_nrGrade;
        if (ImGui::SliderFloat("风格分级 (Grade)", &gr, 0.0f, 8.0f, "%.1f")) { g_nrGrade = gr; g_ctrlDirty = true; }
    }
    ImGui::TextDisabled("%s | %s（点下方“保存设置”可持久化到 xess_proxy.saved.ini）",
                        g_backendNgx ? "后端=NGX (dlssnr_core)" : "后端=mochizuki",
                        g_ctrlDirty ? "等待 worker 下发" : "已下发");
    ImGui::Separator();
    ImGui::TextDisabled("════ NR 推理分辨率倍率 (NR Scale · 移植自 mochizuki) ════");
    int sm = (int)g_nrScaleMode;
    if (sm < 0 || sm > 7) sm = 0;
    if (ImGui::Combo("推理尺寸档位 (NR Scale)", &sm,
                     "0 固定 1080p (1920x1080 · 默认经典)\0"
                     "1 50% 渲染分辨率\0"
                     "2 67% 渲染分辨率\0"
                     "3 75% 渲染分辨率\0"
                     "4 100% 渲染分辨率 (原生 1:1)\0"
                     "5 固定 720p (1280x720 · 高帧率)\0"
                     "6 固定 900p (1600x900 · 均衡)\0"
                     "7 固定 1440p (2560x1440 · 高精细)\0")) {
        g_nrScaleMode = sm;
        Log("菜单(NR Scale): 请求切换档位 %d", sm);
    }
    ImGui::SetItemTooltip("调节副卡 NR 网络推理分辨率（低于原图=提帧减带宽；等于渲染=原生 1:1 精细）；切换时安全重建跨卡与 0.0.9 会话");
    ImGui::TextDisabled("当前 NR 网络尺寸: %u x %u（相对基准 %u x %u · 随保存设置联动）", kNetW, kNetH, g_cw, g_ch);
    ImGui::Separator();
}

static void MenuSectionPreprocess() {
    auto ppPut = [](const char* k, const std::string& v) {
        if (IniSetValue("Preprocess", k, v)) Log("菜单(PP): %s = %s（已写回 ini，GPU 喂图实时生效）", k, v.c_str());
    };
    auto ppNum = [](float v) { char b[32]; snprintf(b, sizeof(b), "%.2f", v); return std::string(b); };
    EnsurePreprocessLoadedFromIni();

    if (ImGui::Checkbox("预处理总开关（Enabled）", &g_ppOn)) ppPut("Enabled", g_ppOn ? "1" : "0");
    ImGui::SetItemTooltip("直接作用于喂给 NR 网络的 1080p 图像（A/B 双形态实时生效），并随下方保存/撤销/恢复/默认按钮联动");
    int em = (g_ppExpMode >= 0 && g_ppExpMode < 3) ? g_ppExpMode : 1;
    if (ImGui::Combo("曝光模式 Exposure", &em, "off\0auto\0fixed\0")) { g_ppExpMode = em; ppPut("Exposure", kPpExpName[em]); }
    ImGui::SetItemTooltip("off = 不做 EV 增益；auto / fixed = 按下方曝光补偿 EV 调节喂图亮度");
    if (ImGui::SliderFloat("曝光补偿 EV", &g_ppBias, -8.0f, 8.0f, "%+.2f")) { /* 实时生效 */ }
    if (ImGui::IsItemDeactivatedAfterEdit()) ppPut("ExposureBias", ppNum(g_ppBias));
    ImGui::SetItemTooltip("每 1 档 = 2 倍亮度；实时作用于喂给 NR 的图");
    int cv = (g_ppCurve >= 0 && g_ppCurve < 7) ? g_ppCurve : 3;
    if (ImGui::Combo("显示曲线 Curve", &cv, "none\0neutral\0reinhard\0filmic\0gt\0aces\0agx\0")) { g_ppCurve = cv; ppPut("Curve", kPpCurveName[cv]); }
    ImGui::SetItemTooltip("none = 原样；neutral / reinhard / filmic / gt / aces / agx = 喂入网络前的色调映射曲线");
    if (ImGui::SliderFloat("对比 Contrast", &g_ppContrast, 0.5f, 2.0f, "%.2f")) { /* 实时生效 */ }
    if (ImGui::IsItemDeactivatedAfterEdit()) ppPut("Contrast", ppNum(g_ppContrast));
    ImGui::SetItemTooltip("围绕中灰 (0.18) 的对比，1.0 = 不变");
    if (ImGui::SliderFloat("饱和 Saturation", &g_ppSat, 0.05f, 2.0f, "%.2f")) { /* 实时生效 */ }
    if (ImGui::IsItemDeactivatedAfterEdit()) ppPut("Saturation", ppNum(g_ppSat));
    ImGui::SetItemTooltip("1.0 = 不变；这是喂给网络前的饱和，A/B 双形态实时生效");
    ImGui::SameLine();
    if (ImGui::Button("从 ini 重新读取")) { LoadPreprocessFromIni(); Log("菜单(PP): 已按 dlssnr-amd.ini 重读预处理参数"); }
    ImGui::TextDisabled("预处理状态: %s | 随下方“保存设置 / 撤销改动 / 恢复上次保存 / 恢复默认”联动保存", g_ppOn ? "开（实时作用于 GPU 喂图）" : "关");
}
static void MenuSectionNetwork009() {
    if (!g_backendNgx) return;
    static bool  netLoaded = false;
    static int   netModel = 0;       // 0=SMALL_EXP, 1=SMALL, 2=SILVER_OPT, 3=SILVER
    static int   netOptLayers = 3;   // 0=Quality, 1=Balanced, 2=Performance, 3=Ultra Performance
    static int   netHistPrec = 0;    // 0=16-bit, 1=32-bit
    static bool  netStyleGrade = true;
    static const char* kModelNames[4] = {
        "CC_SMALL_FP8_EXPERIMENTAL",
        "CC_SMALL_FP8",
        "CC_SILVER_AARDWOLD_OPTIMIZED",
        "CC_SILVER_AARDWOLD"
    };
    auto netLoad = []() {
        std::string t, v;
        if (!IniReadAll(ModuleDir() + "dlssnr-amd.ini", t)) return;
        if (IniGetValue(t, "Network", "Model", v)) {
            for (int i = 0; i < 4; ++i) {
                if (_stricmp(v.c_str(), kModelNames[i]) == 0) { netModel = i; break; }
            }
        }
        if (IniGetValue(t, "Network", "OptimizedLayers", v)) {
            if (_stricmp(v.c_str(), "quality") == 0 || v == "0") netOptLayers = 0;
            else if (_stricmp(v.c_str(), "balanced") == 0 || v == "1") netOptLayers = 1;
            else if (_stricmp(v.c_str(), "performance") == 0 || v == "2") netOptLayers = 2;
            else if (_stricmp(v.c_str(), "ultra performance") == 0 || v == "3") netOptLayers = 3;
        }
        if (IniGetValue(t, "Network", "History Precision", v)) {
            netHistPrec = (v == "32" || _stricmp(v.c_str(), "fp32") == 0 || _stricmp(v.c_str(), "rgba32f") == 0) ? 1 : 0;
        }
        if (IniGetValue(t, "Network", "Style Grade", v)) {
            netStyleGrade = (v != "0");
        }
    };
    auto netPut = [](const char* k, const std::string& v) {
        if (IniSetValue("Network", k, v)) Log("菜单(0.0.9): [Network] %s = %s（已写回 ini，核心后台热切换）", k, v.c_str());
    };
    if (!netLoaded) { netLoaded = true; netLoad(); }

    ImGui::TextDisabled("════ 0.0.9 网络模型与显存选项（dlssnr_core · A/B 双形态共用）════");
    int nm = netModel;
    if (ImGui::Combo("NR 网络模型 (Model)", &nm,
                     "CC_SMALL_FP8_EXPERIMENTAL (310.9 加速版 · 推荐)\0"
                     "CC_SMALL_FP8 (310.9 原版)\0"
                     "CC_SILVER_AARDWOLD_OPTIMIZED (310.8 优化版)\0"
                     "CC_SILVER_AARDWOLD (310.8 原版)\0")) {
        netModel = nm;
        netPut("Model", kModelNames[nm]);
    }
    ImGui::SetItemTooltip("切换后 0.0.9 核心会在副卡后台建新网并热切换，当前画面保持出图；若首次编译模板需数十秒");
    int ol = netOptLayers;
    if (ImGui::Combo("优化层档位 (OptimizedLayers)", &ol,
                     "0 Quality (全 FP8/FP16 · 无 int4)\0"
                     "1 Balanced (部分 int4)\0"
                     "2 Performance (+固定注意力)\0"
                     "3 Ultra Performance (最快 · 默认)\0")) {
        netOptLayers = ol;
        char buf[8]; snprintf(buf, sizeof(buf), "%d", ol);
        netPut("OptimizedLayers", buf);
    }
    ImGui::SetItemTooltip("仅对 CC_SILVER_AARDWOLD_OPTIMIZED 生效；切换后后台重建网络热生效");
    int hp = netHistPrec;
    if (ImGui::Combo("时域历史精度 (History Precision)", &hp,
                     "16-bit (RGBA16F · 显存减半 · 默认)\0"
                     "32-bit (RGBA32F · 原版高精度)\0")) {
        netHistPrec = hp;
        netPut("History Precision", hp == 0 ? "16" : "32");
    }
    ImGui::SetItemTooltip("改后即时生效并重置一次网络历史");
    bool sg = netStyleGrade;
    if (ImGui::Checkbox("风格后分级 (Style Grade)", &sg)) {
        netStyleGrade = sg;
        netPut("Style Grade", sg ? "1" : "0");
    }
    ImGui::SetItemTooltip("控制 Natural(1) / Cinematic(2) 风格是否按 NVIDIA 原版做后段调色（Standard 不受影响）");
    ImGui::SameLine();
    if (ImGui::Button("重读 [Network]")) { netLoad(); Log("菜单(0.0.9): 已按 dlssnr-amd.ini 重读网络配置"); }
    ImGui::Separator();
}
static void MenuSectionRuntimeForm() {
    ImGui::TextDisabled("════ 当前运行形态与最终呈现显卡（F3 切换）════");
    const bool isB = g_bformOn;
    const bool nrActive = g_nrOn;
    if (!isB) {
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.55f, 1.0f),
                           "当前形态: A 形态（由【游戏卡】负责呈现最终画面）");
        ImGui::TextDisabled("分工: 游戏卡渲染并合成最终帧输出 | 神经卡仅后台做 NR 推理 | NR 总开关(F10): %s",
                            nrActive ? "开启" : "关闭");
    } else {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f),
                           "当前形态: B 形态（由【神经卡】负责呈现最终画面）");
        ImGui::TextDisabled("分工: %s | 阶段 Stage %d (%s) | NR 总开关(F10): %s",
                            g_bform4k ? "B-4K 神经卡 DComp 独立覆盖" : "B 形态独立呈现",
                            g_bformStage,
                            g_bpReady ? "覆盖层已就绪" : "覆盖层准备中",
                            nrActive ? "开启" : "关闭");
    }
    if (ImGui::Button(isB ? "切换到 A 形态（游戏卡呈现）" : "切换到 B 形态（神经卡呈现）")) {
        std::string path = ModuleDir() + "xess_proxy.bform.txt";
        const char* want = isB ? "0" : "9";
        FILE* fb = nullptr;
        if (fopen_s(&fb, path.c_str(), "w") == 0 && fb) { fwrite(want, 1, 1, fb); fclose(fb); }
        Log("*** 菜单切换: 请求切换到 %s 形态（%s）***",
            want[0] == '0' ? "A" : "B-4K",
            want[0] == '0' ? "游戏卡负责呈现最终画面" : "神经卡负责呈现最终画面");
    }
    ImGui::SetItemTooltip("A 形态：由游戏卡合成并呈现最终画面；B 形态：由神经卡合成并通过独立 DComp 覆盖层呈现最终画面（等效 F3）");
    ImGui::SameLine();
    ImGui::TextDisabled("也可直接按 F3 热切换（约 1 秒生效）");
    ImGui::Separator();
}
static void ProxyImGuiDraw(ID3D12GraphicsCommandList* cl, ID3D12Resource* rt, bool wmEligible,
                           D3D12_RESOURCE_STATES outputState) {
    if (!g_imguiInited || g_imguiFail || !rt) { return; }
    g_menuVisible = g_menuOpen;
    if (!g_menuVisible) { return; }
    __try {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)g_outW, (float)g_outH);
        io.DeltaTime = 1.0f / 60.0f;
        auto key = [&](ImGuiKey k, int vk) { io.AddKeyEvent(k, (GetAsyncKeyState(vk) & 0x8000) != 0); };
        key(ImGuiKey_UpArrow, VK_UP);     key(ImGuiKey_DownArrow, VK_DOWN);
        key(ImGuiKey_LeftArrow, VK_LEFT); key(ImGuiKey_RightArrow, VK_RIGHT);
        key(ImGuiKey_Enter, VK_RETURN);   key(ImGuiKey_Escape, VK_ESCAPE);
        key(ImGuiKey_Tab, VK_TAB);
        key(ImGuiKey_Delete, VK_DELETE);
        // 鼠标：屏幕坐标 → 客户区 → 显示尺寸
        POINT p{};
        if (GetCursorPos(&p) && g_gameHwnd) {
            ScreenToClient(g_gameHwnd, &p);
            RECT rc{}; GetClientRect(g_gameHwnd, &rc);
            float cw = (float)(rc.right - rc.left), ch = (float)(rc.bottom - rc.top);
            if (cw > 0 && ch > 0)
                io.AddMousePosEvent((float)p.x * (float)g_outW / cw, (float)p.y * (float)g_outH / ch);
        }
        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        io.MouseDrawCursor = true;

        // 界面缩放：值变了就重建样式（字体按 FontGlobalScale 缩放，不需要重建字体纹理）
        if (g_uiScale != g_uiApplied) {
            g_uiApplied = g_uiScale;
            ImGui::GetStyle() = g_baseStyle;
            ImGui::GetStyle().ScaleAllSizes(1.6f * g_uiApplied);
            io.FontGlobalScale = g_uiApplied;
        }
        ImGui_ImplDX12_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(900, 780), ImGuiCond_FirstUseEver);
        ImGui::Begin("跨卡 NR 参数（Del 关闭 · 鼠标操作 · 悬停看说明）", &g_menuVisible,
                     ImGuiWindowFlags_NoCollapse);   // 允许拖动(NoMove 未设)与缩放
        // 第一、二部分代理后处理已关闭；保留当前运行形态显示、网络参数控制、预处理和其余菜单。
        MenuSectionRuntimeForm();
        MenuSectionNetwork009();
        MenuSectionNrControls();
        MenuSectionPreprocess();

        ImGui::Separator();
        if (ImGui::Button("保存设置（独立备份）")) {
            SaveParamsToFile(CaptureParams());
        }
        ImGui::SetItemTooltip("把当前参数写进 xess_proxy.saved.ini。出厂默认在代码里，任何按钮都不会覆盖它");
        ImGui::SameLine();
        if (ImGui::Button("撤销改动")) {
            if (g_sessionStartValid) {
                ApplyParams(g_sessionStart);
                Log("菜单: 已撤销本次改动（回到本次启动时的参数）");
            } else {
                Log("菜单: 撤销不可用（还没记录启动状态）");
            }
        }
        ImGui::SetItemTooltip("丢弃本次游戏里所有未保存的调整，回到刚启动时的参数");
        ImGui::SameLine();
        if (ImGui::Button("恢复上次保存")) {
            Params p = CaptureParams();
            if (LoadParamsFromFile(p)) {
                ApplyParams(p);
                Log("菜单: 已恢复上次保存的设置");
            }
        }
        ImGui::SetItemTooltip("读取 xess_proxy.saved.ini 并应用；没有存档时只在日志里提示，不改动参数");
        ImGui::SameLine();
        if (ImGui::Button("恢复默认设置")) {
            ApplyParams(FactoryParams());
            Log("菜单: 已恢复默认设置（出厂值；你的存档文件保留）");
        }
        ImGui::SetItemTooltip("回到代码里的出厂默认值；不影响游戏实时 MV 比例，也不会删除 xess_proxy.saved.ini");
        ImGui::SameLine(); ImGui::TextDisabled("拖标题栏移动 · 拖右下角缩放");
        float us = (float)g_uiScale;
        if (ImGui::SliderFloat("界面缩放", &us, 0.6f, 2.5f, "%.2f")) g_uiScale = us;
        ImGui::SetItemTooltip("缩放整个菜单的文字与控件大小（不影响游戏画面）。默认 1.00");
        ImGui::Text("%.0f FPS | worker %.1f ms | 帧 %d", io.Framerate, g_worker.lastMs, g_frame);
        ImGui::TextWrapped("运行时状态: %s", g_statusLine[0] ? g_statusLine : "(还没拿到)");
        ImGui::End();
        ImGui::Render();

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_imguiRtvHeap->GetCPUDescriptorHandleForHeapStart();
        g_dev->CreateRenderTargetView(rt, nullptr, rtv);
        Barrier(cl, rt, outputState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{ 0, 0, (float)g_outW, (float)g_outH, 0, 1 };
        D3D12_RECT sc{ 0, 0, (LONG)g_outW, (LONG)g_outH };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
        Barrier(cl, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, outputState);
        // 恢复我们自己的描述符堆（ImGui 会改掉它；游戏随后会自设，但这里先还原）
        ID3D12DescriptorHeap* hs[] = { g_heap.Get() };
        cl->SetDescriptorHeaps(1, hs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_imguiFail = true;
        Log("ImGui: 绘制异常（已禁用）");
    }
}
// =====================================================================================
// S1 镜像叠层：把游戏已经出好的成品帧（outp）再叠一层到游戏窗口上
// -------------------------------------------------------------------------------------
// 目的：只测「自己拥有 present」本身的代价（合成 vs iFlip / 输入穿透 / 稳定性），
//       用来决定新形态（B 自己出画）值不值得做。
//   · 不改 NR 逻辑、不改 pColor/pOutput 内容、不碰游戏自己的交换链
//   · 用「合成交换链 + DComp 视觉」贴在游戏窗口的 DComp 目标上：不建独立窗口
//     → 不抢焦点、不进 alt-tab（present_probe4 已用绿屏验证过这条路可行）
//   · 节奏：本帧把成品拷进后备缓冲，下一帧才 Present（保证每次翻上去的都是写完的缓冲；
//     代价 +1 帧，对"合成代价"的测量没有影响）
//   · 任何一步失败 → 永久关闭本功能，游戏照常
// 开关：ini 键 mirror=1 / 菜单「镜像叠层」/ F11
// =====================================================================================
static ComPtr<IDCompositionDevice>         g_mirDev;
static ComPtr<IDCompositionTarget>         g_mirTarget;
static ComPtr<IDCompositionVisual>         g_mirVisual;
static ComPtr<IDCompositionScaleTransform> g_mirScale;
static ComPtr<IDXGISwapChain3>             g_mirSc;    // 需要 3 才有 SetColorSpace1 / GetCurrentBackBufferIndex
static bool g_mirPending = false;
static UINT g_mirW = 0, g_mirH = 0;

// 合成交换链只接受少数格式（本机实测：R8G8B8A8_UNORM 可用，R10G10B10A2_UNORM 直接
// 返回 DXGI_ERROR_INVALID_CALL）。所以两条路：
//   ① 交换链格式 == outp 格式 → CopyResource（零 shader，最省）
//   ② 否则退回 R8G8B8A8_UNORM → 一次全屏 blit（outp 当 SRV 采样，格式自动转换）
static bool  g_mirBlit = false;
static int   g_mirMode = 0;        // 1=DComp 合成视觉  2=自建置顶穿透窗口
static HWND  g_mirWnd  = nullptr;
// ★ D3D12 的 CreateSwapChain* 要求的 pDevice 是【命令队列】，不是 ID3D12Device
//   （传设备会稳定返回 DXGI_ERROR_INVALID_CALL 0x887A0001）
static ComPtr<ID3D12CommandQueue>   g_mirQueue;
// 阶段一（纯色块）：自己的分配器/列表/fence，全程自己的队列，绝不跨队列碰游戏的后备缓冲
static ComPtr<ID3D12CommandAllocator>      g_mirAlloc[2];
static ComPtr<ID3D12GraphicsCommandList>   g_mirList;
static ComPtr<ID3D12Fence>                 g_mirFence;
static UINT64 g_mirFenceVal = 0;
static UINT64 g_mirAllocFence[2] = { 0, 0 };   // 每个分配器上次被使用时的 fence 值
static int    g_mirAllocIdx = 0;
static bool   g_mirSolid = true;    // true=只画纯色（阶段一，安全） false=贴真画面（阶段二）
// 阶段二：outp 的快照（3 槽，游戏队列写 w，我们的队列只读 2 帧前那张 → 永不读写同一张）
static ComPtr<ID3D12Resource> g_mirSnap[3];
static UINT g_mirSnapW = 0, g_mirSnapH = 0;
static DXGI_FORMAT g_mirSnapFmt = DXGI_FORMAT_UNKNOWN;
static int  g_mirSnapWrite = 0;
static int  g_mirGrabCnt = 0;
static DXGI_FORMAT g_mirSwapFmt = DXGI_FORMAT_UNKNOWN;
static ComPtr<ID3D12RootSignature>  g_mirRs;
static ComPtr<ID3D12PipelineState>  g_mirPso;
static ComPtr<ID3D12DescriptorHeap> g_mirSrvHeap;
static ComPtr<ID3D12DescriptorHeap> g_mirRtvHeap;
static UINT  g_mirIncSrv = 0, g_mirIncRtv = 0;

static const char* kMirHLSL = R"(
Texture2D<float4> gSrc : register(t0);
SamplerState      gSmp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}
float4 PSMain(VSOut i) : SV_Target { return gSrc.Sample(gSmp, i.uv); }
)";

// ---- 备选路径：自建置顶穿透窗口 + CreateSwapChainForHwnd（合成交换链走不通时用）----
// 社区先例：Magpie / Lossless Scaling 双卡都是这条。代价：多一个窗口对象（但点击穿透、不进 alt-tab）
static LRESULT CALLBACK MirrorWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) return 0;
    return DefWindowProcW(h, m, w, l);
}

static bool MirrorBuildOwnWindow(IDXGIFactory2* fac, DXGI_FORMAT fmt) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MirrorWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CrossGpuNrMirrorWnd";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("MIR: RegisterClassExW 失败 %lu", GetLastError()); return false;
    }
    if (!g_mirWnd) {
        g_mirWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
            L"CrossGpuNrMirrorWnd", L"", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_mirWnd) { Log("MIR: CreateWindowExW 失败 %lu", GetLastError()); return false; }
    }
    RECT cr{}; GetClientRect(g_gameHwnd, &cr);
    POINT tl{ 0, 0 }; ClientToScreen(g_gameHwnd, &tl);
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    if (cw < 8 || ch < 8) { cw = (int)g_mirW; ch = (int)g_mirH; }
    SetWindowPos(g_mirWnd, HWND_TOPMOST, tl.x, tl.y, cw, ch, SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_mirW; sd.Height = g_mirH; sd.Format = fmt;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = fac->CreateSwapChainForHwnd(g_mirQueue.Get(), g_mirWnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr) || !sc1) { Log("MIR: CreateSwapChainForHwnd(自建窗口) 失败 0x%08X", (unsigned)hr); return false; }
    hr = sc1.As(&g_mirSc);
    if (FAILED(hr) || !g_mirSc) { Log("MIR: QI IDXGISwapChain3(自建窗口) 失败 0x%08X", (unsigned)hr); return false; }
    g_mirMode = 2;
    Log("MIR: 自建窗口就绪 | 交换链 %dx%d | 窗口 %dx%d @ (%ld,%ld)",
        (int)g_mirW, (int)g_mirH, cw, ch, tl.x, tl.y);
    return true;
}

static bool MirrorBuildBlit(ID3D12Device* dev, DXGI_FORMAT fmt) {    D3D12_DESCRIPTOR_RANGE r{};
    r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r.NumDescriptors = 1; r.BaseShaderRegister = 0; r.RegisterSpace = 0;
    r.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = 1;
    p.DescriptorTable.pDescriptorRanges = &r;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ss.ShaderRegister = 0; ss.RegisterSpace = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1; rs.pParameters = &p;
    rs.NumStaticSamplers = 1; rs.pStaticSamplers = &ss;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) { Log("MIR: 序列化根签名失败 0x%08X %s", (unsigned)hr, err ? (char*)err->GetBufferPointer() : ""); return false; }
    hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_mirRs));
    if (FAILED(hr)) { Log("MIR: CreateRootSignature 失败 0x%08X", (unsigned)hr); return false; }

    ComPtr<ID3DBlob> vs, ps, e2;
    hr = D3DCompile(kMirHLSL, strlen(kMirHLSL), "mir.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &e2);
    if (FAILED(hr)) { Log("MIR: 编译 VS 失败 %s", e2 ? (char*)e2->GetBufferPointer() : ""); return false; }
    hr = D3DCompile(kMirHLSL, strlen(kMirHLSL), "mir.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &e2);
    if (FAILED(hr)) { Log("MIR: 编译 PS 失败 %s", e2 ? (char*)e2->GetBufferPointer() : ""); return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_mirRs.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_mirPso));
    if (FAILED(hr)) { Log("MIR: CreateGraphicsPipelineState 失败 0x%08X", (unsigned)hr); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_mirSrvHeap));
    if (FAILED(hr)) { Log("MIR: 建 SRV 堆失败 0x%08X", (unsigned)hr); return false; }
    g_mirIncSrv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // 注意：RTV 堆由 MirrorBuildRtv 负责，这里【绝对不能】再建一次 ——
    //   重建会得到一个「没有 RTV 视图」的堆，纯色帧随即拿未初始化描述符去清屏 → 设备挂死（已崩过一次）
    return true;
}

// RTV 堆与后备缓冲视图（纯色块模式也要用，所以单独建、总是建）
static bool MirrorBuildRtv(ID3D12Device* dev) {
    if (!g_mirRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 4;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_mirRtvHeap));
        if (FAILED(hr) || !g_mirRtvHeap) { Log("MIR: 建 RTV 堆失败 0x%08X", (unsigned)hr); return false; }
        g_mirIncRtv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_mirRtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < 3; ++i) {
            ComPtr<ID3D12Resource> b;
            if (FAILED(g_mirSc->GetBuffer(i, IID_PPV_ARGS(&b))) || !b) { Log("MIR: 取后备缓冲 %u 失败", i); return false; }
            dev->CreateRenderTargetView(b.Get(), nullptr, h);
            h.ptr += g_mirIncRtv;
        }
    }
    return true;
}

// 我们自己的提交设施（纯色块模式全程用自己的队列，绝不跨队列碰游戏的后备缓冲）
static bool MirrorBuildSubmit(ID3D12Device* dev) {
    for (int i = 0; i < 2; ++i) {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_mirAlloc[i])))) {
            Log("MIR: CreateCommandAllocator %d 失败", i); return false;
        }
    }
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_mirAlloc[0].Get(), nullptr, IID_PPV_ARGS(&g_mirList)))) {
        Log("MIR: CreateCommandList 失败"); return false;
    }
    g_mirList->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_mirFence)))) {
        Log("MIR: CreateFence 失败"); return false;
    }
    g_mirFenceVal = 0;
    return true;
}

static bool MirrorBuild(ID3D12Resource* outp) {
    // 游戏窗口句柄只在「打开过菜单」时才会被填 → 这里自己补找一次
    if (!g_gameHwnd) g_gameHwnd = FindGameWindowOnce();
    if (!g_dev || !outp || !g_gameHwnd) {
        Log("MIR: 还缺东西（dev=%d outp=%d 窗口=%d）→ 本次跳过，可再试",
            g_dev ? 1 : 0, outp ? 1 : 0, g_gameHwnd ? 1 : 0);
        g_mirState = 3;          // 3 = 可重试（不永久放弃）
        return false;
    }
    // 重建前：先有界等待自己的 GPU 工作落地（≤200ms），否则释放仍在飞行中的对象会挂死
    if (g_mirFence && g_mirQueue && g_mirFenceVal) {
        if (g_mirFence->GetCompletedValue() < g_mirFenceVal) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev) {
                if (SUCCEEDED(g_mirFence->SetEventOnCompletion(g_mirFenceVal, ev)))
                    WaitForSingleObject(ev, 200);
                CloseHandle(ev);
            }
        }
    }
    // 重建前先彻底释放（同一个 HWND 反复建 target 不保险）
    g_mirSc.Reset(); g_mirVisual.Reset(); g_mirScale.Reset(); g_mirTarget.Reset(); g_mirDev.Reset();
    g_mirQueue.Reset();
    g_mirPending = false;

    D3D12_RESOURCE_DESC od = outp->GetDesc();
    g_mirW = (UINT)od.Width; g_mirH = od.Height;

    // ★ 两个坑都在这里：
    //   ① ID3D12Device【不实现 IDXGIDevice】（那是 D3D11 的用法，QI 会返回 E_NOINTERFACE）
    //      → 只能用 GetAdapterLuid() 反查 DXGI 适配器
    //   ② 交换链工厂必须来自该设备所在的那张卡，否则 CreateSwapChainForComposition
    //      返回 DXGI_ERROR_INVALID_CALL(0x887A0001)（本机两张卡，默认工厂常常不是游戏那张）
    HRESULT hr = S_OK;
    LUID devLuid = g_dev->GetAdapterLuid();

    ComPtr<IDXGIFactory4> fac4;
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&fac4));
    if (FAILED(hr) || !fac4) { Log("MIR: CreateDXGIFactory2 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }

    ComPtr<IDXGIAdapter1> mirAd;
    hr = fac4->EnumAdapterByLuid(devLuid, IID_PPV_ARGS(&mirAd));
    if (FAILED(hr) || !mirAd) { Log("MIR: EnumAdapterByLuid 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }
    DXGI_ADAPTER_DESC1 adDesc{};
    if (SUCCEEDED(mirAd->GetDesc1(&adDesc)))
        Log("MIR: 设备所在卡 = %ls (LUID %08X:%08X)", adDesc.Description,
            (unsigned)adDesc.AdapterLuid.HighPart, (unsigned)adDesc.AdapterLuid.LowPart);

    ComPtr<IDXGIFactory2> fac;
    hr = mirAd->GetParent(IID_PPV_ARGS(&fac));
    if (FAILED(hr) || !fac) { Log("MIR: 取适配器父工厂失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width  = g_mirW;
    sd.Height = g_mirH;
    sd.Format = od.Format;                 // 必须与 outp 一致（CopyResource 要求同格式同尺寸）
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.Scaling     = DXGI_SCALING_STRETCH;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;   // 不透明：整块盖住
    // 建我们自己的命令队列：CreateSwapChain* 的 pDevice 必须是队列
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        hr = g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_mirQueue));
        if (FAILED(hr) || !g_mirQueue) { Log("MIR: CreateCommandQueue 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }
    }

    // 先按 outp 的原生格式建（能成就能零 shader 直拷）；不成再退回 RGBA8 + blit
    ComPtr<IDXGISwapChain1> sc1;
    hr = fac->CreateSwapChainForComposition(g_mirQueue.Get(), &sd, nullptr, &sc1);
    g_mirBlit = false;
    if (FAILED(hr) || !sc1) {
        Log("MIR: 合成交换链不接受 fmt=%d（0x%08X）→ 退回 R8G8B8A8_UNORM + blit", (int)od.Format, (unsigned)hr);
        sc1.Reset();
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = fac->CreateSwapChainForComposition(g_mirQueue.Get(), &sd, nullptr, &sc1);
        g_mirBlit = true;
        if (FAILED(hr) || !sc1) {
            Log("MIR: RGBA8 合成交换链也失败 0x%08X（%ux%u）", (unsigned)hr, g_mirW, g_mirH);
            // 诊断：把环境状态钉死，便于判断是不是 DWM/独占全屏的问题
            {
                RECT wr{}; GetWindowRect(g_gameHwnd, &wr);
                RECT cr2{}; GetClientRect(g_gameHwnd, &cr2);
                RECT mr{}; GetWindowRect(GetDesktopWindow(), &mr);
                DWORD st = (DWORD)GetWindowLongPtrW(g_gameHwnd, GWL_STYLE);
                DWORD ex = (DWORD)GetWindowLongPtrW(g_gameHwnd, GWL_EXSTYLE);
                Log("MIR: 诊断 窗口=(%ld,%ld)-(%ld,%ld) 客户区=%ldx%ld 桌面=%ldx%ld style=0x%08X ex=0x%08X",
                    wr.left, wr.top, wr.right, wr.bottom,
                    cr2.right - cr2.left, cr2.bottom - cr2.top,
                    mr.right - mr.left, mr.bottom - mr.top, st, ex);
            }
            g_mirBlit = true;
            if (!MirrorBuildOwnWindow(fac.Get(), DXGI_FORMAT_R8G8B8A8_UNORM)) { g_mirState = 2; return false; }
            if (!MirrorBuildRtv(g_dev.Get())) { g_mirState = 2; return false; }
            if (!MirrorBuildSubmit(g_dev.Get())) { g_mirState = 2; return false; }
            if (!MirrorBuildBlit(g_dev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM)) { g_mirState = 2; return false; }
            g_mirState = 1;
            Log("MIR: 就绪 | 模式 自建窗口+blit | 输出 %ux%u", g_mirW, g_mirH);
            return true;
        }
    }
    hr = sc1.As(&g_mirSc);
    if (FAILED(hr) || !g_mirSc) { Log("MIR: QI IDXGISwapChain3 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }
    g_mirSc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);   // SDR；失败不致命
    if (!MirrorBuildRtv(g_dev.Get())) { g_mirState = 2; return false; }
    if (!MirrorBuildSubmit(g_dev.Get())) { g_mirState = 2; return false; }

    // DComp 设备：D3D12 设备不能当 IDXGIDevice 传进来。
    // 先试 nullptr（默认 DXGI 设备）；失败则在【同一张卡】上造一个 D3D11 设备来提供 IDXGIDevice。
    hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&g_mirDev));
    if (FAILED(hr) || !g_mirDev) {
        Log("MIR: DCompositionCreateDevice(nullptr) 失败 0x%08X → 改走同卡 D3D11 设备", (unsigned)hr);
        ComPtr<ID3D11Device> d11;
        D3D_FEATURE_LEVEL fl{};
        HRESULT h2 = D3D11CreateDevice(mirAd.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                       nullptr, 0, D3D11_SDK_VERSION, &d11, &fl, nullptr);
        ComPtr<IDXGIDevice> d11Dxgi;
        if (FAILED(h2) || !d11 || FAILED(d11.As(&d11Dxgi)) || !d11Dxgi) {
            Log("MIR: 建 D3D11 设备/取 IDXGIDevice 失败 0x%08X", (unsigned)h2);
            g_mirState = 2; return false;
        }
        hr = DCompositionCreateDevice(d11Dxgi.Get(), IID_PPV_ARGS(&g_mirDev));
        if (FAILED(hr) || !g_mirDev) {
            Log("MIR: DCompositionCreateDevice(D3D11) 失败 0x%08X", (unsigned)hr);
            g_mirState = 2; return false;
        }
    }

    hr = g_mirDev->CreateTargetForHwnd(g_gameHwnd, TRUE, &g_mirTarget);   // TRUE = 在窗口内容之上
    if (FAILED(hr) || !g_mirTarget) { Log("MIR: CreateTargetForHwnd 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }
    hr = g_mirDev->CreateVisual(&g_mirVisual);
    if (FAILED(hr) || !g_mirVisual) { Log("MIR: CreateVisual 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }

    g_mirVisual->SetContent(g_mirSc.Get());

    // 铺满客户区：输出分辨率 → 客户区 的等比缩放（客户区=4K 时就是 1:1）
    RECT rc{}; GetClientRect(g_gameHwnd, &rc);
    float cw = (float)(rc.right - rc.left), ch = (float)(rc.bottom - rc.top);
    float sx = 1.0f, sy = 1.0f;
    if (cw > 1 && ch > 1 && g_mirW && g_mirH) {
        sx = cw / (float)g_mirW; sy = ch / (float)g_mirH;
        if (SUCCEEDED(g_mirDev->CreateScaleTransform(&g_mirScale)) && g_mirScale) {
            g_mirScale->SetScaleX(sx);
            g_mirScale->SetScaleY(sy);
            g_mirScale->SetCenterX(0.0f);
            g_mirScale->SetCenterY(0.0f);
            g_mirVisual->SetTransform(g_mirScale.Get());
        }
    }

    hr = g_mirTarget->SetRoot(g_mirVisual.Get());
    if (FAILED(hr)) { Log("MIR: SetRoot 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }
    hr = g_mirDev->Commit();
    if (FAILED(hr)) { Log("MIR: Commit 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }

    if (g_mirBlit) {
        if (!MirrorBuildBlit(g_dev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM)) { g_mirState = 2; return false; }
    } else {
        // 直拷模式下也要备好 blit 管线（阶段二贴真画面要用，目标格式 = 交换链实际格式）
        if (!MirrorBuildBlit(g_dev.Get(), sd.Format)) { g_mirState = 2; return false; }
    }
    g_mirSwapFmt = sd.Format;

    g_mirMode = 1;
    g_mirState = 1;
    Log("MIR: 就绪 | 输出 %ux%u fmt=%d | 模式 %s | 客户区 %dx%d | 缩放 %.3f x %.3f",
        g_mirW, g_mirH, (int)od.Format, g_mirBlit ? "RGBA8+blit" : "直拷", (int)cw, (int)ch, sx, sy);
    return true;
}

// 文件开关：xess_proxy.mirror.txt 里写 1/0，每 ~30 帧读一次（纯 CPU，不碰 GPU）
// 用途：让外部脚本（DSH）能在不碰键盘的情况下做 A/B
static void MirrorPollFile() {
    static int s_last = -1;
    std::string p = ModuleDir() + "xess_proxy.mirror.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "r") != 0 || !f) return;
    char buf[16] = { 0 };
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (got == 0) return;
    int v = -1;
    for (size_t i = 0; i < got; ++i) {
        if (buf[i] == '0') { v = 0; break; }
        if (buf[i] == '1') { v = 1; break; }
        if (buf[i] == '2') { v = 2; break; }   // 2 = 纯色自检
    }
    if (v < 0 || v == s_last) return;
    s_last = v;
    bool wantOn = (v != 0);
    bool wantSolid = (v == 2);
    if (wantOn != g_mirOn || (wantOn && wantSolid != g_mirSolid)) {
        g_mirOn = wantOn;
        if (wantOn) g_mirSolid = wantSolid;
        Log("MIR: 文件开关 -> %s%s（帧 %d）", wantOn ? "开" : "关", wantOn ? (wantSolid ? "·纯色" : "·真画面") : "", g_frame);
    }
}

// ---- 阶段二：真画面 ----
// 分工：游戏队列只负责 outp → 快照；我们的队列只负责 快照 → 交换链 → Present。
// 两边都不碰对方飞行中的资源；快照 3 槽，读的是 2 帧前那张。
static bool MirrorEnsureSnap(ID3D12Device* dev, ID3D12Resource* outp) {
    if (!dev || !outp) return false;
    D3D12_RESOURCE_DESC od = outp->GetDesc();
    if (g_mirSnap[0] && (UINT)od.Width == g_mirSnapW && od.Height == g_mirSnapH && od.Format == g_mirSnapFmt) return true;
    for (int i = 0; i < 3; ++i) g_mirSnap[i].Reset();
    g_mirGrabCnt = 0; g_mirSnapWrite = 0;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = od;
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.MipLevels = 1;
    rd.DepthOrArraySize = 1;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    for (int i = 0; i < 3; ++i) {
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_mirSnap[i])))) {
            Log("MIR: 建快照纹理失败（第 %d 张）", i);
            for (int k = 0; k < 3; ++k) g_mirSnap[k].Reset();
            return false;
        }
    }
    g_mirSnapW = (UINT)od.Width; g_mirSnapH = od.Height; g_mirSnapFmt = od.Format;
    Log("MIR: 快照纹理 %ux%u fmt=%d ×3", g_mirSnapW, g_mirSnapH, (int)g_mirSnapFmt);
    return true;
}

// 记在【游戏的命令列表】上：outp → 快照[w]
static void MirrorGrab(ID3D12GraphicsCommandList* cl, ID3D12Resource* outp, D3D12_RESOURCE_STATES outputState) {
    if (!cl || !outp) return;
    if (!MirrorEnsureSnap(g_dev.Get(), outp)) return;
    int w = g_mirSnapWrite;
    Barrier(cl, outp, outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cl, g_mirSnap[w].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(g_mirSnap[w].Get(), outp);
    Barrier(cl, g_mirSnap[w].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cl, outp, D3D12_RESOURCE_STATE_COPY_SOURCE, outputState);
    g_mirSnapWrite = (w + 1) % 3;
    if (g_mirGrabCnt < 100) ++g_mirGrabCnt;
}

// 在【我们自己的队列】上：快照[w+2] → 交换链 → Present
static void MirrorPresentReal() {
    if (g_mirGrabCnt < 2) return;
    if (!g_mirSc || !g_mirQueue || !g_mirList || !g_mirFence || !g_mirRtvHeap || !g_mirPso || !g_mirSrvHeap) return;
    UINT64 done = g_mirFence->GetCompletedValue();
    int ai = -1;
    for (int k = 0; k < 2; ++k) { int i = (g_mirAllocIdx + k) % 2; if (done >= g_mirAllocFence[i]) { ai = i; break; } }
    if (ai < 0) return;
    g_mirAllocIdx = (ai + 1) % 2;
    auto& alloc = g_mirAlloc[ai];
    if (FAILED(alloc->Reset())) return;
    if (FAILED(g_mirList->Reset(alloc.Get(), nullptr))) return;

    int w = g_mirSnapWrite;                 // 本帧写入的槽
    int si = (w + 2) % 3;                   // 读 2 帧前那张（Grab 后再看，等价于上一帧写入的那张）
    if (!g_mirSnap[si]) { g_mirList->Close(); return; }

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = g_mirSnapFmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(g_mirSnap[si].Get(), &sv, g_mirSrvHeap->GetCPUDescriptorHandleForHeapStart());

    UINT bi = g_mirSc->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> bb;
    if (FAILED(g_mirSc->GetBuffer(bi, IID_PPV_ARGS(&bb))) || !bb) { g_mirList->Close(); return; }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_mirRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)bi * g_mirIncRtv;

    Barrier(g_mirList.Get(), bb.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12DescriptorHeap* hs[] = { g_mirSrvHeap.Get() };
    g_mirList->SetDescriptorHeaps(1, hs);
    g_mirList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_mirList->SetGraphicsRootSignature(g_mirRs.Get());
    g_mirList->SetGraphicsRootDescriptorTable(0, g_mirSrvHeap->GetGPUDescriptorHandleForHeapStart());
    g_mirList->SetPipelineState(g_mirPso.Get());
    g_mirList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT vp{ 0, 0, (float)g_mirW, (float)g_mirH, 0.0f, 1.0f };
    D3D12_RECT rcs{ 0, 0, (LONG)g_mirW, (LONG)g_mirH };
    g_mirList->RSSetViewports(1, &vp);
    g_mirList->RSSetScissorRects(1, &rcs);
    g_mirList->DrawInstanced(3, 1, 0, 0);
    Barrier(g_mirList.Get(), bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

    if (FAILED(g_mirList->Close())) return;
    ID3D12CommandList* ls[] = { g_mirList.Get() };
    g_mirQueue->ExecuteCommandLists(1, ls);
    UINT64 v = ++g_mirFenceVal;
    g_mirQueue->Signal(g_mirFence.Get(), v);
    g_mirAllocFence[ai] = v;
    g_mirSc->Present(0, 0);
    if ((g_frame % 300) == 0) Log("MIR: 真画面帧 %d（快照槽 %d）", g_frame, si);
}

// 阶段一：只清一块纯色。完全不读 outp、不碰游戏队列 → 排除资源状态与跨队列 Present 两个风险
static void MirrorSolidFrame() {
    if (!g_mirSc || !g_mirQueue || !g_mirList || !g_mirFence || !g_mirRtvHeap) return;
    // 只挑一个「GPU 已经用完」的分配器；两个都在飞就本帧跳过（绝不阻塞、绝不复用飞行中的对象）
    UINT64 done = g_mirFence->GetCompletedValue();
    int idx = -1;
    for (int k = 0; k < 2; ++k) {
        int i = (g_mirAllocIdx + k) % 2;
        if (done >= g_mirAllocFence[i]) { idx = i; break; }
    }
    if (idx < 0) return;
    g_mirAllocIdx = (idx + 1) % 2;
    auto& alloc = g_mirAlloc[idx];
    if (FAILED(alloc->Reset())) return;
    if (FAILED(g_mirList->Reset(alloc.Get(), nullptr))) return;
    UINT bi = g_mirSc->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> bb;
    if (FAILED(g_mirSc->GetBuffer(bi, IID_PPV_ARGS(&bb))) || !bb) { g_mirList->Close(); return; }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_mirRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)bi * g_mirIncRtv;
    Barrier(g_mirList.Get(), bb.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float col[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
    g_mirList->ClearRenderTargetView(rtv, col, 0, nullptr);
    Barrier(g_mirList.Get(), bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(g_mirList->Close())) return;
    ID3D12CommandList* ls[] = { g_mirList.Get() };
    g_mirQueue->ExecuteCommandLists(1, ls);
    UINT64 v = ++g_mirFenceVal;
    g_mirQueue->Signal(g_mirFence.Get(), v);
    g_mirAllocFence[idx] = v;
    g_mirSc->Present(0, 0);
    if ((g_frame % 300) == 0) Log("MIR: 纯色帧 %d（%ux%u）", g_frame, g_mirW, g_mirH);
}

static void MirrorTick(ID3D12GraphicsCommandList* cl, ID3D12Resource* outp, D3D12_RESOURCE_STATES outputState) {
    static int s_poll = 0;
    if ((s_poll++ % 30) == 0) MirrorPollFile();        // 文件开关（外部 A/B 用）

    // 每次「关→开」都允许重新尝试（否则一次失败就永久死了，见 22471 帧那次）
    static bool s_prevOn = false;
    if (g_mirOn && !s_prevOn && g_mirState == 2) { g_mirState = 0; Log("MIR: 重新开启，重试初始化"); }
    s_prevOn = g_mirOn;
    // 关闭态：把视觉内容摘掉（叠层消失，游戏照常出画），对象留着以便随时再开
    if (!g_mirOn) {
        if (g_mirState == 1) {
            if (g_mirVisual) g_mirVisual->SetContent(nullptr);
            if (g_mirDev) g_mirDev->Commit();
            if (g_mirWnd) ShowWindow(g_mirWnd, SW_HIDE);
            g_mirPending = false;
            g_mirState = 3;
            Log("MIR: 已关（叠层撤掉；F11 / 菜单可再开）");
        }
        return;
    }
    // ★ 关键：已经建好、只是被藏起来（state=3）时，【只挂回去】，绝不重建。
    //   重建要 Reset 交换链/队列/分配器，而上一轮的 GPU 工作可能还在飞 → 会挂死（已闪退一次）。
    if (g_mirState == 3 && g_mirSc) {
        if (g_mirMode == 1 && g_mirVisual) {
            g_mirVisual->SetContent(g_mirSc.Get());
            if (g_mirDev) g_mirDev->Commit();
        }
        if (g_mirMode == 2 && g_mirWnd) ShowWindow(g_mirWnd, SW_SHOWNOACTIVATE);
        g_mirState = 1;
        Log("MIR: 重新挂上（沿用已建好的交换链，未重建）");
    }
    if (g_mirState == 2) return;                       // 失败过，不再重试
    if (g_mirState != 1) { if (!MirrorBuild(outp)) return; }
    if (g_mirSolid) { MirrorSolidFrame(); return; }    // 阶段一：纯色块
    if (!cl || !outp) return;
    MirrorGrab(cl, outp, outputState);                 // 游戏队列：outp → 快照
    MirrorPresentReal();                               // 我们的队列：快照 → 交换链 → Present
    return;

    // 分辨率变了（切 XeSS 档位/窗口变化）→ 下一帧重建
    D3D12_RESOURCE_DESC od = outp->GetDesc();
    if ((UINT)od.Width != g_mirW || od.Height != g_mirH) {
        Log("MIR: 输出尺寸 %llux%u → %ux%u，准备重建", (unsigned long long)g_mirW, g_mirH, (UINT)od.Width, od.Height);
        if (g_mirVisual) g_mirVisual->SetContent(nullptr);
        g_mirState = 3; return;
    }

    // ① 先把上一帧已经写好的缓冲翻上去（纯 CPU 调用，不进命令流）
    if (g_mirMode == 2 && g_mirWnd) {          // 自建窗口：抽消息 + 保证可见
        MSG msg;
        while (PeekMessageW(&msg, g_mirWnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindowVisible(g_mirWnd)) ShowWindow(g_mirWnd, SW_SHOWNOACTIVATE);
    }
    if (g_mirPending) { g_mirSc->Present(0, 0); g_mirPending = false; }

    // ② 把本帧成品拷进当前后备缓冲（下一帧才 Present）
    ComPtr<ID3D12Resource> bb;
    if (FAILED(g_mirSc->GetBuffer(g_mirSc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb))) || !bb) return;

    if (!g_mirBlit) {
        Barrier(cl, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, bb.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(bb.Get(), outp);
        Barrier(cl, bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, outp, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    } else {
        // 格式不同（outp 的 fmt ≠ 交换链 fmt）→ 全屏 blit，顺便完成格式转换
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = od.Format;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        g_dev->CreateShaderResourceView(outp, &sv, g_mirSrvHeap->GetCPUDescriptorHandleForHeapStart());

        UINT bi = g_mirSc->GetCurrentBackBufferIndex();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_mirRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)bi * g_mirIncRtv;

        Barrier(cl, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(cl, bb.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12DescriptorHeap* mh[] = { g_mirSrvHeap.Get() };
        cl->SetDescriptorHeaps(1, mh);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cl->SetGraphicsRootSignature(g_mirRs.Get());
        cl->SetGraphicsRootDescriptorTable(0, g_mirSrvHeap->GetGPUDescriptorHandleForHeapStart());
        cl->SetPipelineState(g_mirPso.Get());
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VIEWPORT vp{ 0, 0, (float)g_mirW, (float)g_mirH, 0.0f, 1.0f };
        D3D12_RECT rcs{ 0, 0, (LONG)g_mirW, (LONG)g_mirH };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &rcs);
        cl->DrawInstanced(3, 1, 0, 0);

        Barrier(cl, bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, outp, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

        ID3D12DescriptorHeap* gh[] = { g_heap.Get() };     // 还原我们自己的堆（下面还要用）
        cl->SetDescriptorHeaps(1, gh);
    }
    g_mirPending = true;

    if ((g_frame % 300) == 0) Log("MIR: 帧 %d 已叠层 %ux%u（提交 %.2fms 参考）", g_frame, g_mirW, g_mirH, 0.0);
}

static DWORD WINAPI HotkeyThread(LPVOID) {
    bool p10 = false;
    // ★ v4 P21/P22：mode 语义扩展为 0=关 / 1=观察（只记日志）/ 2=自动（真开关 NR）。
    //   第三个数字是恢复冷却 ms（可省略，缺省 2000）。启动读一次，之后 UiGateTick 每秒热更新。
    int mode = 0; LONGLONG thrMs = 150; long coolMs = 2000;
    {
        std::string cp = ModuleDir() + "xess_proxy.uigate.txt";
        FILE* cf = nullptr; char cb[64] = { 0 };
        if (fopen_s(&cf, cp.c_str(), "r") == 0 && cf) {
            if (fgets(cb, sizeof(cb), cf)) {
                int mv = 0; long tv = 0, cv = 0;
                if (sscanf_s(cb, "%d %ld %ld", &mv, &tv, &cv) >= 1) {
                    mode = (mv == 1 || mv == 2) ? mv : 0;        // 其它值（含旧注释里的 2 之类）一律当 0
                    if (tv >= 50 && tv <= 5000) thrMs = tv;      // 越界用默认 150
                    if (cv >= 500 && cv <= 10000) coolMs = cv;   // 越界用默认 2000
                }
            }
            fclose(cf);
        }
    }
    InterlockedExchange(&g_uiResumeCoolMs, (LONG)coolMs);
    InterlockedExchange(&g_g0Mode, (mode == 1) ? 1L : 0L);
    InterlockedExchange(&g_g0ModeShared, (mode == 1) ? 1L : 0L);
    if (mode == 1) InterlockedExchange64(&g_g0ThrMsShared, thrMs);   // mode≠1 保持 0，入口跳过 gap 判断
    InterlockedExchange64(&g_g0ThrMs, (mode == 1) ? thrMs : 0);
    UiGateInit((LONG)mode, (LONG)thrMs);   // ★ v4 P21：特性开关 + 初始状态（mode=2 时 RUNNING）
    while (true) {
        UiGateTick();   // ★ v4 P21：心跳主循环（UI 打开 → STOPPING → 排空 → STOPPED → 关闭 → STARTING；含 P22 热更新）
        static bool pDel = false;
        // ★ 2077 每帧都会 ClipCursor 把光标锁回画面中心：菜单开着时必须每 tick 清一次，
        //   否则鼠标动不了（只在打开瞬间清一次没用）
        if (g_menuOpen) { ClipCursor(nullptr); ReleaseCapture(); }
        bool dDel = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (dDel && !pDel) {
            g_menuOpen = !g_menuOpen;
            if (g_menuOpen) {
                EnsureWndHook();
                while (ShowCursor(TRUE) < 0) {}
                ClipCursor(nullptr);
                // 打开瞬间把光标搬到游戏窗口中心一次（之后交给系统，菜单就能拖）
                static bool centered = false;
                if (!centered && g_gameHwnd) {
                    RECT cr{}; GetClientRect(g_gameHwnd, &cr);
                    POINT c{ (cr.right - cr.left) / 2, (cr.bottom - cr.top) / 2 };
                    ClientToScreen(g_gameHwnd, &c);
                    SetCursorPos(c.x, c.y);
                    centered = true;
                }
                if (!g_menuOpen) centered = false;
            }
            else { while (ShowCursor(FALSE) >= 0) {} }
            Log("*** Del: 参数菜单 %s ***", g_menuOpen ? "开" : "关");
        }
        pDel = dDel;
        static bool pF9 = false;
        bool dF9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        F9Id::Tick(dF9 && !pF9);
        pF9 = dF9;
        bool d10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        // ★ v4 P23：F10 走模式矩阵（特性未启用时内部退回旧行为：只切 g_nrOn，日志口径不变）
        if (d10 && !p10) { UiGateHotkeyToggle(); }
        p10 = d10;
        // F7/F8 诊断热键已停用（B 形态不再保留跑马灯/三横带功能，与 A 形态一致）
        static bool pF8 = false;
        bool dF8 = false;
        if (dF8 && !pF8) {
            if (g_triOn < 0) g_triOn = 1;
            g_triOn = g_triOn ? 0 : 1;
            Log("*** F8: 视图切换 → %s ***", g_triOn ? "三横带诊断（上答案/中残差/下底图）" : "正常 NR 画面");
        }
        pF8 = dF8;
        static bool pF7 = false;
        bool dF7 = false;
        if (dF7 && !pF7) {
            if (g_marqueeOn < 0) g_marqueeOn = 0;
            g_marqueeOn = g_marqueeOn ? 0 : 1;
            Log("*** F7: 黑条标记 %s ***", g_marqueeOn ? "开" : "关（画面里不再有标记，是真实 NR 结果）");
        }
        pF7 = dF7;
        static bool p11 = false;
        bool d11 = false;   // 已停用（改在菜单里调）：VK_F11
        if (d11 && !p11) { g_mirOn = !g_mirOn; Log("*** F11: 镜像叠层 %s ***", g_mirOn ? "开" : "关"); }
        // ★ F3：游戏内热切换 A 形态 ↔ B-4K 形态（改 bform.txt，轮询器 ~1 秒接手）
        //   ★ 2026-10-31：从「小键盘1/End（NumLock 关，VK_END）」改到这里，因为 VK_END 要让给
        //     dlssnr_core 的 NR Control（它用 GetAsyncKeyState 自己读 VK_END，不可配置）。
        //     这样两边不撞：NR Control = NumLock 关 + 小键盘1；A/B 切换 = F3。
        static bool pHk9 = false;
        bool dHk9 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;   // F3
        if (dHk9 && !pHk9) {
            std::string path = ModuleDir() + "xess_proxy.bform.txt";
            const char* want = g_bform4k ? "0" : "9";
            FILE* f9 = nullptr;
            if (fopen_s(&f9, path.c_str(), "w") == 0 && f9) { fwrite(want, 1, 1, f9); fclose(f9); }
            Log("*** F3: 请求切换到 %s 形态（%s）***", want[0] == 48 ? "A" : "B-4K",
                want[0] == 48 ? "NR@1440p + 真 XeSS + 游戏 present" : "真 XeSS 4K + B 端 4K NR + B present");
        }
        pHk9 = dHk9;
        p11 = d11;
        static bool p7 = false, p8 = false;
        bool d7 = false;   // 已停用（改在菜单里调）：VK_F7
        bool d8 = false;   // 已停用（改在菜单里调）：VK_F8
        if (d7 && !p7) { g_gain = (g_gain > 0.30f) ? g_gain - 0.06f : 0.24f; Log("*** F7: 残差增益 %.2f ***", g_gain); }
        if (d8 && !p8) { g_gain = (g_gain < 1.20f) ? g_gain + 0.06f : 1.26f; Log("*** F8: 残差增益 %.2f ***", g_gain); }
        p7 = d7; p8 = d8;
        static bool p9 = false;
        // F9 已交给 REFramework 开/关窗口 —— 我们不再占用（平滑改到菜单里调）
        p9 = false;
        static bool p5 = false, p6 = false;
        bool d5 = false;   // 已停用（改在菜单里调）：VK_F5
        bool d6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;   // ★ HOTDIAG：F6 = 热诊断开关（边沿触发；原「方差门限」轮换已停用）
        if (d5 && !p5) {
            static int m5 = 0;
            m5 = (m5 + 1) % 8;
            if (m5 == 0)      { g_temporalStatic = 1.0f;  g_stateWarmup = 40; Log("*** F5[0]: 时域稳定 关 ***"); }
            else if (m5 == 1) { g_temporalStatic = 0.60f; g_stateWarmup = 40; Log("*** F5[1]: 0.60 ***"); }
            else if (m5 == 2) { g_temporalStatic = 0.35f; g_stateWarmup = 40; Log("*** F5[2]: 0.35 ***"); }
            else if (m5 == 3) { g_temporalStatic = 0.02f; g_stateWarmup = 40; Log("*** F5[3]: 0.20 ***"); }
            else if (m5 == 4) { g_temporalStatic = 0.10f; g_stateWarmup = 40; Log("*** F5[4]: 0.10 ***"); }
            else if (m5 == 5) { g_temporalStatic = 0.05f; g_stateWarmup = 40; Log("*** F5[5]: 0.05 ***"); }
            else if (m5 == 6) { g_temporalStatic = 0.03f; g_stateWarmup = 40; Log("*** F5[6]: 0.03 ***"); }
            else              { g_temporalStatic = 0.02f; g_stateWarmup = 40; Log("*** F5[7]: 0.02（最强）***"); }
        }
        if (d6 && !p6) {   // ★ HOTDIAG：仅切换时写一行（带 HOTDIAG 前缀，便于 grep）；F7 跑马灯逻辑不动
            LONG hon = (g_hotDiagOn == 0) ? 1 : 0;
            InterlockedExchange(&g_hotDiagOn, hon);
            Log("HOTDIAG *** F6: 热诊断 %s ***", hon ? "开" : "关");
        }
        p5 = d5; p6 = d6;
        static bool p4 = false;
        bool d4 = false;   // 已停用（改在菜单里调）：VK_F4
        if (d4 && !p4) {
            static int m4 = 0; m4 = (m4 + 1) % 4;
            const float v[4] = { 0.0f, 1.0f, 2.0f, 6.0f };
            g_mvConf = v[m4]; g_mvConfIdx = m4;
            Log("*** F4[%d]: MV 置信衰减 %.1f ***", m4, g_mvConf);
        }
        static bool p2 = false;
        bool d2 = false;   // 已停用（改在菜单里调）：VK_F2
        if (d2 && !p2) {
            static int m2 = 0; m2 = (m2 + 1) % 5;
            const float v[5] = { 0.0f, 2.0f, 6.0f, 15.0f, 40.0f };
            g_gateK = v[m2]; g_gateIdx = m2;
            Log("*** F2[%d]: 逐像素门限 %.1f（0=关）***", m2, g_gateK);
        }
        p2 = d2;
        p4 = d4;
        {   // ★ D1-D5 增强：设备/围栏只读采样（最多 1 次/秒；只调 Get*，不 Signal/Wait/Execute/Reset）
            static ULONGLONG lastFs = 0;
            const ULONGLONG nowFs = GetTickCount64();
            if ((nowFs - lastFs) >= 1000) {
                lastFs = nowFs;
                static bool sessDevDone = false;
                if (!sessDevDone && g_dev && g_worker.dev && g_worker.fe) {
                    sessDevDone = true;
                    const LUID gl = g_dev->GetAdapterLuid();
                    const LUID wl = g_worker.dev->GetAdapterLuid();
                    char wdev[16]; wdev[0] = 0;
                    if (g_workerDevId) _snprintf_s(wdev, sizeof(wdev), _TRUNCATE, "0x%04X", g_workerDevId);
                    else               _snprintf_s(wdev, sizeof(wdev), _TRUNCATE, "-1");
                    BtTrace("SESSION_DEVICE", g_frame, "init",
                            "game_luid=0x%08lX%08lX game_devid=-1 worker_luid=0x%08lX%08lX worker_devid=%s game_fence=0x%llx game_fence_side=g worker_fence=0x%llx worker_fence_side=w",
                            (unsigned long)gl.HighPart, (unsigned long)gl.LowPart,
                            (unsigned long)wl.HighPart, (unsigned long)wl.LowPart, wdev,
                            BtPtr(g_xqFe.Get()), BtPtr(g_worker.fe.Get()));
                }
                if (g_worker.fe) {
                    const UINT64 wc = g_worker.fe->GetCompletedValue();
                    const UINT64 gc = g_xqFe ? g_xqFe->GetCompletedValue() : 0ull;
                    const long wr = g_worker.dev ? (long)g_worker.dev->GetDeviceRemovedReason() : -1L;
                    const long gr = g_dev ? (long)g_dev->GetDeviceRemovedReason() : -1L;
                    BtTrace("FENCE_SAMPLE", g_frame, "fence",
                            "worker_completed=%llu worker_removed_reason=0x%08lX game_completed=%llu game_removed_reason=0x%08lX worker_fence=0x%llx worker_fence_valid=%d game_fence=0x%llx game_fence_valid=%d",
                            (unsigned long long)wc, (unsigned long)wr,
                            (unsigned long long)gc, (unsigned long)gr,
                            BtPtr(g_worker.fe.Get()), (wc == UINT64_MAX) ? 0 : 1,
                            BtPtr(g_xqFe.Get()), (g_xqFe && gc != UINT64_MAX) ? 1 : 0);
                }
            }
        }
        {   // ★ G0 探针：UIGate 只观察（mode==1 才执行；不 Signal/Wait/Execute，不碰覆盖层/g_bformOn/发布/状态）
            static ULONGLONG lastSec = 0;
            static bool      inGap = false;
            static ULONGLONG gapStartTick = 0;   // 打 open 时记下的 tick（恢复调用写入的 lastTick）
            static ULONGLONG lastHold = 0;
            static LONGLONG  seenGapCount = 0;   // 上次看到的生产侧空档计数
            const ULONGLONG now = GetTickCount64();
            if (InterlockedCompareExchange(&g_uiModeCfg, 0, 0) == 1) {   // mode≠1：不做任何 G0 计算、不打 UIGATE 日志（v4：改读热更新后的值）
                const LONGLONG gapCount = InterlockedCompareExchange64(&g_g0GapCount, 0, 0);
                if (gapCount != seenGapCount) {   // 生产侧已精确算出空档结束
                    const LONGLONG grown = gapCount - seenGapCount;   // >1 = 两次采样间发生多次大空档
                    const LONGLONG lastMs = InterlockedCompareExchange64(&g_g0GapLastMs, 0, 0);
                    const LONGLONG endTick = InterlockedCompareExchange64(&g_g0GapEndTick, 0, 0);
                    BtTrace("UIGATE-OBS", g_frame, "g0",
                            "event=end total_gap_ms=%lld end_tick=%lld gaps=%lld",
                            lastMs, endTick, grown);
                    inGap = false;
                    seenGapCount = gapCount;
                }
                const LONGLONG lastTick = InterlockedCompareExchange64(&g_g0LastTick, 0, 0);
                const LONGLONG seq = InterlockedCompareExchange64(&g_g0Seq, 0, 0);
                LONGLONG gap = lastTick ? ((LONGLONG)now - lastTick) : 0;
                if (gap < 0) gap = 0;   // 时间戳倒退按 0 处理，不打 open
                if (lastTick) {
                    if (!inGap && gap >= thrMs) {   // 从 <threshold 跨到 >=threshold（观察，误差 ≤ 一个 40ms 轮询）
                        inGap = true; gapStartTick = (ULONGLONG)lastTick; lastHold = now;
                        BtTrace("UIGATE-OBS", g_frame, "g0",
                                "event=open gap_ms=%lld seq=%lld thr=%lld", gap, seq, thrMs);
                    } else if (inGap && (now - lastHold) >= 250) {   // 空档内每 250 ms
                        lastHold = now;
                        BtTrace("UIGATE-OBS", g_frame, "g0", "event=hold gap_ms=%lld", gap);
                    }
                }
                if ((now - lastSec) >= 1000) {   // 每 1 秒汇总
                    lastSec = now;
                    const LONGLONG disp = InterlockedExchange64(&g_g0Count, 0);
                    const LONGLONG maxiv = InterlockedExchange64(&g_g0MaxIv, 0);
                    const LONGLONG en = InterlockedExchange64(&g_g0EarlyNull, 0);
                    const LONGLONG eo = InterlockedExchange64(&g_g0EarlyOff, 0);
                    const LONGLONG ps = InterlockedExchange64(&g_g0Pass, 0);
                    BtTrace("UIGATE-SEC", g_frame, "g0",
                            "disp=%lld maxiv_ms=%lld early_null=%lld early_off=%lld pass=%lld bform=%d",
                            disp, maxiv, en, eo, ps, (int)g_bformOn);
                }
            }
        }
        BtFlush(BTRACE1_FLUSH_BATCH);
        FlushBtLogBatch();
        Sleep(40);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID lpvReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        {
            char exe[MAX_PATH] = { 0 };
            if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
                const char* s = strrchr(exe, '\\');
                const char* b = s ? (s + 1) : exe;
                if (_stricmp(b, "REDEngineErrorReporter.exe") == 0 || _stricmp(b, "CrashReporter.exe") == 0) {
                    g_disabled = true;
                    return TRUE;
                }
            }
        }
        OpenLog();
        Log("libxess proxy v9（方案 B：槽位队列 + 延迟呈现）attached");
        Log("D0-OBS: MV probe active (build 2077-BUI-G0OBS-v1-D0)");
        BtSetEmit(StageBtLogLine);
        BtSession("2077-BUI-G0OBS-v1", 2, "3C1A78FB8944E240D0F666CA2C74A30D33DE6F57970DE215B56C6321DAD1A1F", "fence-r5");
        FlushBtLogBatch();   // 启动 SESSION 仍立即落盘
        // 模型倍率必须在最早期设好：运行时在建会话时读取并缓存，晚于它初始化再设无效
        // （所以只在 worker 里设是不够的）。这里只碰进程环境块，不做文件 I/O、不调 LoadLibrary。
        SetEnvironmentVariableW(L"DLSS5_MULTI_PASS", L"1");   // 1 = Original 71-block；0.5 = Small
        Log("early: 已设 DLSS5_MULTI_PASS=1（请求 Original 71-block）");
        {
            std::string off = ModuleDir() + "xess_proxy_off.txt";
            if (GetFileAttributesA(off.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_disabled = true;
                Log("检测到 xess_proxy_off.txt：不接管");
            }
        }
#ifndef COGPU_ASI_BUILD
        InitForwarders();
#endif
        CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
        Log("转发就绪");
    } else if (reason == DLL_PROCESS_DETACH) {
        if (lpvReserved == nullptr) { A2In::Shutdown(); A2EnsureMvScratch(true); }
        else A2In::gameQueue.store(0);
    }
    return TRUE;
}
