// Included after kNetW/kNetH. F9-only; no GPU wait, no hot-path disk I/O.
#include <optional>

namespace F9Id {
enum Stage : unsigned { AColor, AMv, WColor, WMv, WAnswer, AAnswer, StageCount };
enum Path : unsigned { Shared = 1, CpuUpload, NoInputCopy, GpuResidual, CpuResidual, TriBands, CopyOnly, A2Pair };
enum Drop : unsigned { Busy, Full, NoHook, Setup, Shape, NoSubmit, SignalFail, Removed,
                       StampMismatch, Alias, MapFail, EvalFailed, NotProduced, Identity, GpuOutstanding, NoRerecord, DropCount };
static const char* const kStages[] = {"a_color", "a_mv", "w_color", "w_mv", "w_answer", "a_answer"};
static const char* const kDrops[] = {"busy", "full", "no_hook", "setup", "shape", "no_submit",
    "signal_fail", "device_removed", "stamp_mismatch", "replay_or_alias", "map_fail", "eval_failed", "not_produced",
    "identity_unverified", "gpu_outstanding", "rerecord_unobserved"};
static constexpr UINT kN = 8, kSpan = 8, kRow = kN * kSpan * 4, kStride = 512;
static constexpr UINT kABytes = 3 * kStride, kWBytes = 2 * kStride;
static constexpr unsigned kKeep = 384, kPhaseCount = 3;
static constexpr UINT kX[kN] = {135, 661, 1418, 382, 960, 1641, 556, 1217};
static constexpr UINT kY[kN] = {117, 261, 157, 495, 555, 647, 835, 918};
static_assert(kRow == D3D12_TEXTURE_DATA_PITCH_ALIGNMENT && kStride == D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
// One atomic snapshot: a record cannot combine a previous window with a new session.
static std::atomic<UINT64> mode{0}; // high bits run, low 3 bits: 0 off, 1 pre, 2 transit, 3 post, 4 drain
static std::atomic<UINT64> recordSeq{0}, submitSeq{0};
static std::atomic<UINT64> aDispatches{0};
static std::atomic<bool> observerReady{false};
static std::atomic<unsigned> drops[StageCount][DropCount]{};
static unsigned Phase() { return unsigned(mode.load() & 7); }
static UINT64 Session() { return mode.load() >> 3; }
static unsigned Active(UINT64 snapshot) { const unsigned p = unsigned(snapshot & 7); return p <= kPhaseCount ? p : 0; }
static void Count(Stage s, Drop d) { ++drops[s][d]; }
struct A3Meta { int inputSlot = -1, answerSlot = -1, fsrFrame = -1; UINT64 inputSeq = 0, answerSeq = 0; };
// Offset the absent marker so the large dormant row store remains zero-filled BSS.
struct A3Fields { unsigned input1 = 0, answer1 = 0, fsr1 = 0; UINT64 inputSeq = 0, answerSeq = 0; };
static A3Fields PackA3(A3Meta v) {
    return {unsigned(v.inputSlot) + 1, unsigned(v.answerSlot) + 1, unsigned(v.fsrFrame) + 1, v.inputSeq, v.answerSeq};
}
struct Meta { unsigned stage, path, part; int src, expected, dispatch, gen; UINT64 epoch, tick, run; A3Fields a3{}; };
struct Row { Meta m; UINT64 rec, sub, fv, hash; UINT_PTR list, queue, fence; };
struct Ring { Row rows[kKeep]; unsigned size = 0, next = 0; UINT64 overwritten = 0; };
static Ring rows[kPhaseCount][StageCount]; // HotkeyThread only
static void Keep(Meta m, UINT64 rec, UINT64 sub, UINT64 fv, UINT64 hash,
                 UINT_PTR list, UINT_PTR queue, UINT_PTR fence) {
    if (m.run != Session() || !m.part || m.part > kPhaseCount) return;
    auto& r = rows[m.part - 1][m.stage];
    r.rows[r.next] = {m, rec, sub, fv, hash, list, queue, fence};
    r.next = (r.next + 1) % kKeep;
    if (r.size < kKeep) ++r.size; else ++r.overwritten;
}
static UINT64 Hash(const BYTE* p) {
    UINT64 h = 14695981039346656037ull;
    for (UINT i = 0; i < kRow; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static bool Buffer(ID3D12Device* dev, UINT64 bytes, bool upload, ComPtr<ID3D12Resource>& r) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(r.ReleaseAndGetAddressOf())));
}
static void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
                       D3D12_RESOURCE_STATES st, D3D12_RESOURCE_STATES to) {
    if (st == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, st, to};
    cl->ResourceBarrier(1, &b);
}
static bool TextureShape(ID3D12Resource* src, DXGI_FORMAT fmt) {
    if (!src) return false;
    auto d = src->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Format == fmt &&
           d.Width == kNetW && d.Height == kNetH && d.SampleDesc.Count == 1;
}
static UINT PatchX(UINT i) {
    UINT x = (kX[i] * kNetW) / 1920u;
    return (kNetW > kSpan && x + kSpan <= kNetW) ? x : (kNetW > kSpan ? kNetW - kSpan : 0u);
}
static UINT PatchY(UINT i) {
    UINT y = (kY[i] * kNetH) / 1080u;
    return (kNetH > 0 && y < kNetH) ? y : (kNetH > 0 ? kNetH - 1u : 0u);
}
static void CopyTex(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT64 off,
                    ID3D12Resource* src, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES st) {
    Transition(cl, src, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint = {off, {fmt, kN * kSpan, 1, 1, kRow}};
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    for (UINT i = 0; i < kN; ++i) {
        const UINT px = PatchX(i), py = PatchY(i);
        D3D12_BOX box{px, py, 0, px + kSpan, py + 1, 1};
        cl->CopyTextureRegion(&d, i * kSpan, 0, 0, &s, &box);
    }
    Transition(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
}
static void CopyAnswer(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT64 off, ID3D12Resource* src) {
    constexpr auto st = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(cl, src, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT i = 0; i < kN; ++i)
        cl->CopyBufferRegion(dst, off + i * kSpan * 4, src,
            (UINT64(PatchY(i)) * kNetW + PatchX(i)) * 4, kSpan * 4);
    Transition(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
}

// MvPair's ReShade _orig contract + C4's canonical IUnknown validation. Do not
// identify devices by adapter LUID alone: two devices on one adapter need not interoperate.
static constexpr GUID kUnwrapped =
    { 0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };
static HRESULT Canon(IUnknown* obj, ComPtr<IUnknown>& base, ComPtr<IUnknown>& identity, bool* unwrapped = nullptr) {
    if (!obj) return E_POINTER;
    void* p = nullptr;
    const HRESULT hr = obj->QueryInterface(kUnwrapped, &p);
    if (unwrapped) *unwrapped = SUCCEEDED(hr) && p;
    if (SUCCEEDED(hr) && p) base.Attach(static_cast<IUnknown*>(p));
    else {
        if (p) static_cast<IUnknown*>(p)->Release();
        if (hr != E_NOINTERFACE) return FAILED(hr) ? hr : E_FAIL;
        base = obj;
    }
    return base.As(&identity);
}
static ComPtr<IUnknown> DeviceId(ID3D12DeviceChild* obj) {
    ComPtr<ID3D12Device> typed;
    ComPtr<IUnknown> given, base, id;
    if (!obj) return id;
    if (SUCCEEDED(obj->GetDevice(IID_PPV_ARGS(&typed))) && typed) Canon(typed.Get(), base, id);
    else if (SUCCEEDED(obj->GetDevice(IID_PPV_ARGS(&given)))) Canon(given.Get(), base, id);
    return id;
}
struct ListRoute {
    ComPtr<ID3D12CommandList> native;
    ComPtr<IUnknown> id;
    explicit ListRoute(ID3D12CommandList* list) {
        ComPtr<IUnknown> base;
        if (SUCCEEDED(Canon(list, base, id))) base.As(&native);
        if (!native) id.Reset();
    }
};
struct QueueRoute {
    ComPtr<ID3D12CommandQueue> native;
    ComPtr<IUnknown> dev, identity;
    bool verified = false, wrapped = false;
    explicit QueueRoute(ID3D12CommandQueue* q) {
        ComPtr<IUnknown> base;
        if (FAILED(Canon(q, base, identity, &wrapped)) || FAILED(base.As(&native))) return;
        dev = DeviceId(native.Get());
        const auto caller = native.Get() == q ? dev : DeviceId(q);
        verified = dev && caller.Get() == dev.Get();
    }
};

// Each native recording owns distinct buffers until both the real fence and a
// later valid recording into the same COM list prove completion and non-replay.
// Atomic control carries the generation, so a stalled observer cannot poison a reused slot.
struct Stamp { UINT64 a, b; };
static bool operator==(const Stamp& a, const Stamp& b) { return a.a == b.a && a.b == b.b; }
enum AState : UINT64 { AFree, ARecording, ASealed, ACalling, APending, AInspect, ADead };
static constexpr UINT64 kStateMask = 7, kAdvanced = 8, kBad = 16, kNoSubmit = 32, kSlowGpu = 64, kNoRerecord = 128,
                        kBadRoute = 256;
static constexpr unsigned kShift = 9;
struct ATicket {
    std::atomic<UINT64> gate{AFree};
    std::atomic<const void*> raw{nullptr}, native{nullptr}, id{nullptr}, devKey{nullptr};
    ComPtr<ID3D12Device> dev;
    ComPtr<IUnknown> deviceId, listIdentity;
    ComPtr<ID3D12CommandList> owner;
    ComPtr<ID3D12Resource> rb, upload, sources[3];
    ComPtr<ID3D12Fence> fe;
    UINT64 fv = 0, rec = 0, sub = 0, time = 0;
    Stamp stamp{};
    UINT_PTR queue = 0;
    Meta m[3]{};
    unsigned mask = 0;
    bool signalFailed = false;
};
static ATicket aTickets[64];
static std::atomic<unsigned> aWatched{0};
static AState State(UINT64 g) { return AState(g & kStateMask); }
static UINT64 Serial(UINT64 g) { return g >> kShift; }
static bool Watching(UINT64 g) {
    return !(g & kAdvanced) && State(g) >= ASealed && State(g) <= AInspect;
}
static bool Mark(ATicket& t, UINT64 snap, UINT64 flag) {
    const UINT64 seq = Serial(snap);
    while (Serial(snap) == seq && Watching(snap) && !(snap & flag))
        if (t.gate.compare_exchange_weak(snap, snap | flag)) return true;
    return false;
}
static bool Move(ATicket& t, UINT64 snap, AState next) {
    const UINT64 seq = Serial(snap);
    const auto from = State(snap);
    while (Serial(snap) == seq && State(snap) == from)
        if (t.gate.compare_exchange_weak(snap, (snap & ~kStateMask) | next)) return true;
    return false;
}
static void Note(ATicket& t, Drop why) {
    const UINT64 run = Session();
    for (unsigned i = 0; i < 3; ++i)
        if ((t.mask & (1u << i)) && t.m[i].run == run) Count(Stage(t.m[i].stage), why);
}
static void DeadA(ATicket& t, UINT64 snap, Drop why) {
    Note(t, why);
    Move(t, snap, ADead); // retain the buffers on uncertainty or device failure
    --aWatched;
}
static void ReleaseA(ATicket& t, UINT64 snap) { // only AInspect with both proofs
    for (auto& r : t.sources) r.Reset();
    t.raw.store(nullptr); t.native.store(nullptr); t.id.store(nullptr);
    t.owner.Reset(); t.listIdentity.Reset();
    --aWatched;
    t.gate.store((Serial(snap) << kShift) | AFree);
}
static bool PrepA(ATicket& t, ID3D12Device* dev, IUnknown* id, UINT64 seq) {
    if (t.deviceId.Get() != id) {
        t.rb.Reset(); t.upload.Reset(); t.fe.Reset(); t.fv = 0;
        t.dev = dev; t.deviceId = id;
    }
    if ((!t.rb && !Buffer(dev, kABytes, false, t.rb)) ||
        (!t.upload && !Buffer(dev, sizeof(Stamp), true, t.upload)) ||
        (!t.fe && FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&t.fe))))) return false;
    if (t.devKey.load() != id) {
        if (DeviceId(t.fe.Get()).Get() != id) return false;
        t.devKey.store(id);
    }
    t.rec = seq;
    t.stamp = {seq ^ (UINT64(QpcNow()) << 21) ^ 0x9245df6de88a7659ull, seq ^ 0x786410fcbdeb375bull};
    const D3D12_RANGE no{};
    void* p = nullptr;
    if (FAILED(t.upload->Map(0, &no, &p)) || !p) return false;
    memcpy(p, &t.stamp, sizeof(t.stamp));
    const D3D12_RANGE written{0, sizeof(t.stamp)};
    t.upload->Unmap(0, &written);
    return true;
}
struct ARecord {
    ID3D12GraphicsCommandList* cl;
    ID3D12Device* dev;
    int dispatch, gen;
    UINT64 epoch;
    bool enabled, observed;
    ATicket* t = nullptr;
    unsigned attempted = 0;
    bool touched = false;
    Drop error = Setup;
    const UINT64 token = mode.load();
    const unsigned part = Active(token);
    const UINT64 run = token >> 3, tick = part ? GetTickCount64() : 0;
    ARecord(ID3D12GraphicsCommandList* c, ID3D12Device* d, int n, int g, UINT64 e, bool on, bool hook)
        : cl(c), dev(d), dispatch(n), gen(g), epoch(e), enabled(on), observed(hook) { if (part) ++aDispatches; }
    ARecord(const ARecord&) = delete;
    ~ARecord() {
        if (part && enabled && run == Session()) for (const auto s : {AColor, AMv, AAnswer})
            if (!(attempted & (1u << s))) Count(s, NotProduced);
        if (!t) return;
        cl->CopyBufferRegion(t->rb.Get(), kRow, t->upload.Get(), 0, sizeof(Stamp));
        t->time = GetTickCount64();
        ++aWatched;
        t->gate.store((t->rec << kShift) | ASealed);
    }
    // This runs in a valid FFX recording, not at an arbitrary CPU frame boundary.
    // Once the original submission has returned, another valid recording of the
    // same held native list implies Reset. Before that, two FFX calls may be one list.
    void Touch() {
        touched = true;
        auto* raw = static_cast<ID3D12CommandList*>(cl);
        const ListRoute list(raw);
        if (!list.id) { error = Identity; return; }
        bool notClosed = false;
        if (aWatched.load()) for (auto& x : aTickets) {
            const UINT64 snap = x.gate.load();
            if (!Watching(snap) || x.id.load() != list.id.Get() || Serial(x.gate.load()) != Serial(snap)) continue;
            const auto s = State(snap);
            if (s == ASealed || s == ACalling) notClosed = true;
            else Mark(x, snap, kAdvanced);
        }
        if (!part || !enabled) return;
        if (!observed) { error = NoHook; return; }
        if (notClosed) { error = NoSubmit; return; }
        // Check both views of the list and the producer device. Allocate on the
        // actual native device so the proof fence is valid for a native Signal.
        ComPtr<IUnknown> base, id;
        ComPtr<ID3D12Device> nativeDev;
        if (FAILED(Canon(dev, base, id)) || FAILED(base.As(&nativeDev)) || !id ||
            DeviceId(raw).Get() != id.Get() || DeviceId(list.native.Get()).Get() != id.Get()) {
            error = Identity; return;
        }
        if (FAILED(nativeDev->GetDeviceRemovedReason())) { error = Removed; return; }
        for (auto& x : aTickets) {
            auto free = x.gate.load();
            if (State(free) != AFree) continue;
            const UINT64 seq = ++recordSeq;
            if (!x.gate.compare_exchange_strong(free, (seq << kShift) | ARecording)) continue;
            if (!PrepA(x, nativeDev.Get(), id.Get(), seq)) {
                x.gate.store((seq << kShift) | AFree); error = Setup; return;
            }
            x.raw.store(raw); x.native.store(list.native.Get()); x.id.store(list.id.Get());
            x.owner = raw; x.listIdentity = list.id;
            x.signalFailed = false; x.mask = 0;
            t = &x; return;
        }
        error = Full;
    }
    void Tap(Stage stage, ID3D12Resource* src, int srcFrame, int expected, Path path, A3Meta a3 = {}) {
        // Off by default: no COM or GPU diagnostics. In drain we can still
        // observe a valid re-record of a previous list, but add no commands.
        if (!part || !enabled) {
            if (!touched && (token & 7) == 4 && aWatched.load()) Touch();
            return;
        }
        attempted |= 1u << stage;
        if (!observed && !aWatched.load()) { Count(stage, NoHook); return; }
        const auto fmt = stage == AMv ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (stage == AAnswer ? (!src || src->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
                               src->GetDesc().Width < UINT64(kNetW) * kNetH * 4) : !TextureShape(src, fmt)) {
            Count(stage, Shape); return;
        }
        if (!touched) Touch();
        if (!t) { Count(stage, error); return; }
        const unsigned i = stage == AAnswer ? 2 : static_cast<unsigned>(stage);
        const UINT64 off = UINT64(i) * kStride;
        t->sources[i] = src;
        if (stage == AAnswer) CopyAnswer(cl, t->rb.Get(), off, src);
        else CopyTex(cl, t->rb.Get(), off, src, fmt, D3D12_RESOURCE_STATE_COPY_SOURCE);
        t->m[i] = {stage, path, part, srcFrame, expected, dispatch, gen, epoch, tick, run, PackA3(a3)};
        t->mask |= 1u << i;
    }
    void AnswerPath(Path p) { if (t && (t->mask & 4)) t->m[2].path = p; }
    void AnswerFsrFrame(int frame) { if (t && (t->mask & 4)) t->m[2].a3.fsr1 = unsigned(frame) + 1; }
};

struct AHits {
    struct Hit { ATicket* t; UINT64 calling; } t[64];
    unsigned count = 0;
    ComPtr<ID3D12CommandQueue> signal;
    ComPtr<IUnknown> queueId;
    bool wrapper = false;
};
static bool Claim(ATicket& t, UINT64 snap, const QueueRoute& route, AHits& h) {
    const UINT64 seq = Serial(snap);
    for (;;) {
        if (Serial(snap) != seq || !Watching(snap)) return false;
        if (State(snap) != ASealed) { Mark(t, snap, kBad); return false; }
        if (!Phase() || (snap & kBad)) return false;
        // An outer wrapper may be unverifiable but still forward to an observable
        // native inner hook. Do not claim the slot on identity failure.
        if (!route.verified || route.dev.Get() != t.devKey.load()) {
            Mark(t, snap, kBadRoute);
            return false;
        }
        const UINT64 calling = (snap & ~kStateMask) | ACalling;
        if (!t.gate.compare_exchange_weak(snap, calling)) continue;
        t.queue = reinterpret_cast<UINT_PTR>(route.native.Get());
        t.sub = ++submitSeq; ++t.fv;
        h.t[h.count++] = {&t, calling};
        return true;
    }
}
static AHits BeforeExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    AHits h{};
    if (!lists || !n || !aWatched.load()) return h;
    const UINT64 before = recordSeq.load();
    std::optional<QueueRoute> route;
    for (UINT i = 0; i < n; ++i) {
        if (!lists[i]) continue;
        std::optional<ListRoute> list;
        bool direct = false;
        for (auto& t : aTickets) {
            const UINT64 snap = t.gate.load();
            if (!Watching(snap) || Serial(snap) > before) continue;
            if ((t.raw.load() == lists[i] || t.native.load() == lists[i]) && Serial(t.gate.load()) == Serial(snap)) {
                if (!route) route.emplace(q);
                Claim(t, snap, *route, h); direct = true; break;
            }
        }
        if (direct) continue;
        list.emplace(lists[i]); // MvPair deep comparison, pinned COM identity as in C4.
        if (!list->id) continue;
        for (auto& t : aTickets) {
            const UINT64 snap = t.gate.load();
            if (!Watching(snap) || Serial(snap) > before) continue;
            if (t.id.load() != list->id.Get() || Serial(t.gate.load()) != Serial(snap)) continue;
            if (!route) route.emplace(q);
            Claim(t, snap, *route, h); break;
        }
    }
    if (h.count) { h.signal = route->native; h.queueId = route->identity; h.wrapper = route->wrapped; }
    return h;
}
static void AfterExecute(const AHits& h) {
    for (unsigned i = 0; i < h.count; ++i) {
        auto& t = *h.t[i].t;
        t.signalFailed = FAILED(h.signal->Signal(t.fe.Get(), t.fv));
        Move(t, h.t[i].calling, APending);
    }
}

// De-duplicate only documented forwarding edges. Known direct hook-to-hook
// originals delegate to the inner hook. For wrapper->_orig, consume exactly one
// same-batch edge while invoking the original, never while running C4/MV side effects.
struct Forward {
    ID3D12CommandQueue* native;
    IUnknown* identity;
    UINT count;
    ID3D12CommandList* const* lists;
    bool used = false;
};
static thread_local Forward* forwarding = nullptr;
struct ForwardScope {
    Forward* saved = forwarding;
    explicit ForwardScope(Forward* now = nullptr) { forwarding = now; }
    ~ForwardScope() { forwarding = saved; }
};
static bool SameForward(Forward& outer, ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (outer.used || outer.count != n || !lists || !outer.lists) return false;
    if (outer.native != q) {
        const QueueRoute target(q);
        if (!target.verified || target.wrapped || target.identity.Get() != outer.identity) return false;
    }
    for (UINT i = 0; i < n; ++i) {
        if (outer.lists[i] == lists[i]) continue;
        const ListRoute a(outer.lists[i]), b(lists[i]);
        if (!a.id || a.id.Get() != b.id.Get()) return false;
    }
    outer.used = true; // a second call, even from this wrapper, is a real resubmission
    return true;
}
template<class Fn>
static AHits ExecuteOriginal(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists,
                             Fn orig, bool directPeer) {
    if (directPeer || !aWatched.load()) { orig(q, n, lists); return {}; }
    if (forwarding && SameForward(*forwarding, q, n, lists)) {
        ForwardScope pause;
        orig(q, n, lists); return {};
    }
    auto hits = BeforeExecute(q, n, lists);
    Forward edge{hits.signal.Get(), hits.queueId.Get(), n, lists};
    ForwardScope forward(hits.count && hits.wrapper ? &edge : nullptr);
    orig(q, n, lists);
    return hits;
}

static void PollA(UINT64 now) {
    for (auto& t : aTickets) {
        const auto snap = t.gate.load();
        if (State(snap) != ASealed && State(snap) != APending) continue;
        const bool expired = now >= t.time && now - t.time >= 1200;
        if (State(snap) == ASealed) {
            if (expired && Mark(t, snap, kNoSubmit)) {
                Note(t, NoSubmit);
                if (t.gate.load() & kBadRoute) Note(t, Identity);
            }
            continue; // late real observation may still rescue the ticket
        }
        if (!Move(t, snap, AInspect)) continue;
        const auto inspecting = t.gate.load();
        if (t.signalFailed) { DeadA(t, inspecting, SignalFail); continue; }
        const auto done = t.fe->GetCompletedValue();
        if (done == UINT64_MAX) { DeadA(t, inspecting, Removed); continue; }
        if (inspecting & kBad) { DeadA(t, inspecting, Alias); continue; }
        if (done < t.fv || !(inspecting & kAdvanced)) {
            if (expired) {
                const bool gpu = done < t.fv;
                // kAdvanced is compatible with a slow fence; late fences stay pollable.
                const UINT64 bit = gpu ? kSlowGpu : kNoRerecord;
                auto g = t.gate.load();
                while (Serial(g) == Serial(inspecting) && State(g) == AInspect && !(g & bit)) {
                    if (t.gate.compare_exchange_weak(g, g | bit)) { Note(t, gpu ? GpuOutstanding : NoRerecord); break; }
                }
            }
            Move(t, inspecting, APending); continue;
        }
        // Only now may old readback bytes be collected: the old list cannot
        // legally be (re)executed after this observed re-record of the same object.
        const D3D12_RANGE range{0, kABytes};
        void* p = nullptr;
        if (FAILED(t.rb->Map(0, &range, &p)) || !p) { DeadA(t, inspecting, MapFail); continue; }
        auto* b = static_cast<const BYTE*>(p);
        Stamp m{}; memcpy(&m, b + kRow, sizeof(m));
        const bool stamped = m == t.stamp;
        UINT64 hash[3]{};
        if (stamped) for (unsigned i = 0; i < 3; ++i) if (t.mask & (1u << i)) hash[i] = Hash(b + i * kStride);
        const D3D12_RANGE no{}; t.rb->Unmap(0, &no);
        if (!stamped) { DeadA(t, inspecting, StampMismatch); continue; }
        for (unsigned i = 0; i < 3; ++i) if (t.mask & (1u << i))
            Keep(t.m[i], t.rec, t.sub, t.fv, hash[i], reinterpret_cast<UINT_PTR>(t.owner.Get()),
                 t.queue, reinterpret_cast<UINT_PTR>(t.fe.Get()));
        ReleaseA(t, inspecting);
    }
}

// W owns and resets its lists; a healthy W.fe covering the exact batch is sufficient.
struct WTicket {
    ComPtr<ID3D12Resource> rb, keep[2];
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12Fence> fe;
    UINT64 rec = 0, sub = 0, fv = 0;
    UINT_PTR list = 0, queue = 0;
    Meta m[2]{};
    unsigned size = 0;
    enum State { Free, Recording, Pending, Quarantine };
    std::atomic<State> state{Free};
};
static WTicket wTickets[32];
static std::mutex wLock;
struct WRecord {
    ID3D12GraphicsCommandList* cl;
    ID3D12Device* dev;
    int dispatch, gen;
    UINT64 epoch;
    WTicket* t = nullptr;
    const UINT64 token = mode.load();
    const unsigned part = Active(token);
    const UINT64 run = token >> 3, tick = part ? GetTickCount64() : 0;
    WRecord(ID3D12GraphicsCommandList* c, ID3D12Device* d, int n, int g, UINT64 e)
        : cl(c), dev(d), dispatch(n), gen(g), epoch(e) {}
    WRecord(const WRecord&) = delete;
    ~WRecord() { Finish(false, false, nullptr, nullptr, 0); }
    void Tap(Stage stage, ID3D12Resource* src, D3D12_RESOURCE_STATES st, int srcFrame, int expected, Path path, A3Meta a3 = {}) {
        if (!part) return;
        const auto fmt = stage == WMv ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!TextureShape(src, fmt)) { Count(stage, Shape); return; }
        if (!t) {
            std::unique_lock<std::mutex> lk(wLock, std::try_to_lock);
            if (!lk) { Count(stage, Busy); return; }
            if (!dev || FAILED(dev->GetDeviceRemovedReason())) { Count(stage, Removed); return; }
            for (auto& x : wTickets) if (x.state.load() == WTicket::Free) {
                if (x.rb && x.dev.Get() != dev) x.rb.Reset();
                x.dev = dev;
                if (!x.rb && !Buffer(dev, kWBytes, false, x.rb)) { Count(stage, Setup); return; }
                x.state.store(WTicket::Recording); x.size = 0; x.rec = ++recordSeq;
                x.list = reinterpret_cast<UINT_PTR>(cl); t = &x; break;
            }
            if (!t) { Count(stage, Full); return; }
        }
        const auto i = t->size++;
        t->keep[i] = src;
        t->m[i] = {stage, path, part, srcFrame, expected, dispatch, gen, epoch, tick, run, PackA3(a3)};
        CopyTex(cl, t->rb.Get(), UINT64(i) * kStride, src, fmt, st);
    }
    void Finish(bool submitted, bool useful, ID3D12CommandQueue* q, ID3D12Fence* fe, UINT64 fv) {
        if (!t) return;
        if (!submitted || !fe || !fv) {
            for (unsigned i = 0; i < t->size; ++i) Count(Stage(t->m[i].stage), SignalFail);
            t->state.store(WTicket::Quarantine); // No fence proof: do not release or reuse.
        } else {
            t->queue = reinterpret_cast<UINT_PTR>(q); t->fe = fe; t->fv = fv; t->sub = ++submitSeq;
            if (!useful) for (unsigned i = 0; i < t->size; ++i) {
                Count(Stage(t->m[i].stage), EvalFailed); t->m[i].run = 0;
            }
            t->state.store(WTicket::Pending);
        }
        t = nullptr;
    }
};
static void PollW() {
    std::unique_lock<std::mutex> lk(wLock, std::try_to_lock);
    if (!lk) return;
    for (auto& t : wTickets) if (t.state.load() == WTicket::Pending) {
        const auto done = t.fe->GetCompletedValue();
        if (done == UINT64_MAX) {
            for (unsigned i = 0; i < t.size; ++i) Count(Stage(t.m[i].stage), Removed);
            t.state.store(WTicket::Quarantine);
        } else if (done >= t.fv) {
            void* p = nullptr;
            const D3D12_RANGE range{0, SIZE_T(t.size * kStride)};
            if (SUCCEEDED(t.rb->Map(0, &range, &p)) && p) {
                for (unsigned i = 0; i < t.size; ++i) Keep(t.m[i], t.rec, t.sub, t.fv,
                    Hash(static_cast<const BYTE*>(p) + i * kStride), t.list, t.queue,
                    reinterpret_cast<UINT_PTR>(t.fe.Get()));
                const D3D12_RANGE no{}; t.rb->Unmap(0, &no);
            } else for (unsigned i = 0; i < t.size; ++i) Count(Stage(t.m[i].stage), MapFail);
            for (auto& p0 : t.keep) p0.Reset();
            t.fe.Reset(); t.state.store(WTicket::Free);
        }
    }
}

static UINT64 startTick = 0, preTick = 0, postTick = 0, endTick = 0, postBeginDispatches = 0;
static bool Outstanding(unsigned (&count)[StageCount]) {
    const auto run = Session();
    bool complete = true;
    // Save/PollA share one consumer; published A metadata cannot be freed by a hook.
    for (const auto& t : aTickets) {
        const auto s = State(t.gate.load());
        if (s == ARecording) { complete = false; continue; }
        if (s == ASealed || s == ACalling || s == APending || s == AInspect) for (unsigned i = 0; i < 3; ++i)
            if ((t.mask & (1u << i)) && t.m[i].run == run) ++count[t.m[i].stage];
    }
    {
        std::unique_lock<std::mutex> lk(wLock, std::try_to_lock);
        if (!lk) complete = false;
        else for (const auto& t : wTickets) {
            const auto s = t.state.load();
            if (s == WTicket::Recording) { complete = false; continue; }
            if (s == WTicket::Pending) for (unsigned i = 0; i < t.size; ++i)
                if (t.m[i].run == run) ++count[t.m[i].stage];
        }
    }
    return complete;
}
static void Save() { // on existing HotkeyThread, after capture stops; samples are not pixel dumps
    unsigned pending[StageCount]{};
    const bool counted = Outstanding(pending);
    std::string path = ModuleDir() + "f9-frame-identity-" + std::to_string(GetCurrentProcessId()) + "-" +
                       std::to_string(startTick) + ".csv";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) { Log("F9_FRAME_IDENTITY: cannot open %s", path.c_str()); return; }
    fprintf(f, "# f9_frame_identity=v2,start_ms=%llu,pre_ms=%llu,post_ms=%llu,stop_ms=%llu,outstanding_complete=%d\n",
            startTick, preTick, postTick, endTick, counted ? 1 : 0);
    fprintf(f, "# bytes=256,hash=fnv1a64,shape=1920x1080,patches=135:117;661:261;1418:157;382:495;960:555;1641:647;556:835;1217:918,width=8\n");
    fprintf(f, "# path=1_shared;2_cpu_upload;3_no_input_copy;4_gpu_residual;5_cpu_residual;6_tri_bands;7_copy_only;8_a2_direct_same_frame_MV_expected_feedFrame\n");
    for (unsigned s = 0; s < StageCount; ++s) {
        fprintf(f, "# outstanding,%s,%u\n", kStages[s], pending[s]);
        for (unsigned d = 0; d < DropCount; ++d)
            fprintf(f, "# drop,%s,%s,%u\n", kStages[s], kDrops[d], drops[s][d].load());
        for (unsigned p = 0; p < kPhaseCount; ++p)
            fprintf(f, "# window,%s,%u,%u,%llu\n", kStages[s], p + 1, rows[p][s].size, rows[p][s].overwritten);
    }
    fprintf(f, "stage,path,phase,source,expected,dispatch,gen,epoch,tick_ms,record_seq,submit_seq,list,queue,fence,fv,hash,a3_input_slot,a3_input_seq,a3_answer_slot,a3_answer_seq,a3_fsr_frame\n");
    for (unsigned p = 0; p < kPhaseCount; ++p) for (unsigned s = 0; s < StageCount; ++s) {
        const auto& r = rows[p][s];
        for (unsigned i = 0; i < r.size; ++i) {
            const auto& e = r.rows[(r.next + kKeep - r.size + i) % kKeep];
            const auto& m = e.m;
            fprintf(f, "%s,%u,%u,%d,%d,%d,%d,%llu,%llu,%llu,%llu,%llx,%llx,%llx,%llu,%016llx,%d,%llu,%d,%llu,%d\n",
                    kStages[s], m.path, m.part, m.src, m.expected, m.dispatch, m.gen, m.epoch, m.tick, e.rec, e.sub,
                    UINT64(e.list), UINT64(e.queue), UINT64(e.fence), e.fv, e.hash,
                    int(m.a3.input1) - 1, m.a3.inputSeq, int(m.a3.answer1) - 1, m.a3.answerSeq, int(m.a3.fsr1) - 1);
        }
    }
    const bool ok = !ferror(f);
    const int closed = fclose(f);
    Log("F9_FRAME_IDENTITY: %s %s", (ok && !closed) ? "saved" : "write failed", path.c_str());
}
static void Tick(bool press) {
    const auto old = mode.load();
    unsigned p = unsigned(old & 7);
    if (!p && !press) return;
    UINT64 run = old >> 3;
    const auto set = [&p, &run](unsigned v) { p = v; mode.store((run << 3) | v); };
    const auto now = GetTickCount64();
    if (press && !p) {
        for (auto& lane : rows) for (auto& r : lane) { r.size = r.next = 0; r.overwritten = 0; }
        for (auto& lane : drops) for (auto& d : lane) d.store(0);
        startTick = now; preTick = postTick = endTick = 0;
        ++run;
        set(1);
        Log("F9_FRAME_IDENTITY_V2: rolling pre-window armed");
    } else if (press && p == 1) {
        preTick = now; set(2);
        Log("F9_FRAME_IDENTITY: pre-window frozen; travel now");
    } else if (press && p == 2) {
        postTick = now; postBeginDispatches = aDispatches.load(); set(3);
        Log("F9_FRAME_IDENTITY: resume marked; collecting post-window");
    }
    if ((p == 3 && (aDispatches.load() - postBeginDispatches >= 72 || now - postTick > 15000)) ||
        (p < 3 && now - startTick > 90000)) {
        endTick = now; set(4);
    }
    PollA(now); PollW();
    if (p == 4 && now - endTick >= 1450) { Save(); set(0); }
}
} // namespace F9Id
