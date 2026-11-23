// A2: each slot consists of its own shared resource and a local fenced mirror.
// Supported producer contract: same canonical game device, single-submit closed
// game recording before Ready. Never alter the game's ExecuteCommandLists.
#include <condition_variable>

namespace A2In {
static inline UINT ColorPitch() { return ((kNetW * 8u) + 255u) & ~255u; }
static inline UINT64 ColorPlane() { return UINT64(ColorPitch()) * UINT64(kNetH); }
static inline UINT MvPitch() { return ((kNetW * 4u) + 255u) & ~255u; }
static inline UINT64 MvPlane() { return UINT64(MvPitch()) * UINT64(kNetH); }
static inline UINT Pitch() { return MvPitch(); }
static inline UINT64 Plane() { return ColorPlane(); }
static inline UINT PitchFor(DXGI_FORMAT fmt) {
    return (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? ColorPitch() : MvPitch();
}
static inline UINT64 PlaneFor(DXGI_FORMAT fmt) {
    return UINT64(PitchFor(fmt)) * UINT64(kNetH);
}
struct Key { int frame = -1, gen = -1; UINT64 epoch = 0; };
static bool Equal(Key a, Key b) { return a.frame == b.frame && a.gen == b.gen && a.epoch == b.epoch; }
enum State : UINT64 { Free, Recording, Sealed, Calling, Ready, Mirroring, Offered, Leased, Retiring, Dropped, Dead, A3Held };
static constexpr UINT64 kMask = 15, kFault = 16;
static constexpr unsigned kShift = 5, kMaxFaultStreak = 4;
enum class DiagClass : unsigned;
struct DiagSubmit {
    LONGLONG eclReturn = 0, signalCall = 0, signalReturn = 0;
    UINT64 queue = 0;
    bool wrappedBoundary = false;
};
struct Slot {
    std::atomic<UINT64> gate{Free};
    std::atomic<const void*> raw{nullptr}, native{nullptr}, identity{nullptr};
    ComPtr<ID3D12CommandList> owner;
    ComPtr<IUnknown> ownerIdentity;
    ComPtr<ID3D12Heap> heapA, heapW;
    ComPtr<ID3D12Resource> bufA, bufW, mirror, marker;
    ComPtr<ID3D12Resource> resources[6];
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12Fence> prodA, prodW, consume;
    Key key;
    UINT64 seq = 0, retire = 0, sealTick = 0;
    DiagSubmit diagSubmit; // After writes before Ready CAS; main reads only Ready or its descendants.
    DiagClass diagLastReap{}; // main thread only
    int diagAttempt = -1;
    bool diagFollow = false, diagPrintedLate = false;
    bool a3 = false;  // published by Seal; all A3 producer/worker bookkeeping is under the A3 ring lock
};
static constexpr int kSlots = 12; // A3 uses 12 independent slots to cover >=7-frame GPU pipeline depth; legacy A2 uses first K
static Slot slots[kSlots];
static std::atomic<UINT64> sequence{0};
static UINT a3WriteCursor = 0;  // A render producer only; no N%K ownership
static std::atomic<unsigned> watched{0};
static std::atomic<uintptr_t> gameQueue{0}; // canonical identity value only; zero AddRef leak
static ComPtr<IUnknown> gameDevice, workerDevice;
static std::atomic<HANDLE> a3WakeEvent{nullptr};
static std::mutex slotFenceMutex;
static void NotifyA3Ready() {
    if (HANDLE ev = a3WakeEvent.load(std::memory_order_acquire)) SetEvent(ev);
}
static std::atomic<bool> requested{false}, initialized{false};
static bool initAttempted = false; // main thread lazy/on-demand init latch
// broken: 0 healthy; 1 repeated replay, 2 queue changed, 3 producer Signal/loss, 4 bad marker, 5 worker fence lost.
static std::atomic<unsigned> broken{0}, faultStreak{0}, deepListChecks{0};
static std::atomic<unsigned> full{0}, missing{0}, pending{0}, mirrorFail{0}, offered{0}, submitted{0},
                             consumed{0}, retired{0}, unusedReclaimed{0}, stale{0}, noSubmit{0}, identityFail{0}, replay{0};
static void Break(unsigned why) { unsigned old = 0; broken.compare_exchange_strong(old, why); }
static State S(UINT64 g) { return State(g & kMask); }
static UINT64 Seq(UINT64 g) { return g >> kShift; }
static bool Watching(UINT64 g) { return S(g) == Sealed || S(g) == Calling; }
static bool Move(Slot& p, UINT64 seen, State next) {
    const auto src = S(seen); const auto seq = Seq(seen);
    while (S(seen) == src && Seq(seen) == seq)
        if (p.gate.compare_exchange_weak(seen, (seen & ~kMask) | next)) return true;
    return false;
}
static bool MarkFault(Slot& p, UINT64 seen) {
    const auto seq = Seq(seen);
    while (Seq(seen) == seq && Watching(seen) && !(seen & kFault))
        if (p.gate.compare_exchange_weak(seen, seen | kFault)) return true;
    return false;
}
// Logical detach only; do not touch the ticket aliases once Ready is visible.
// Reap drops them while it owns the retiring/dropped generation, before Free.
static bool Unsubscribe(Slot& p, UINT64 seen, State next) {
    if (!Watching(seen) || next != Dead || !Move(p, seen, Dead)) return false;
    --watched;
    return true;
}
static bool PublishReady(Slot& p, UINT64 cleanCalling) {
    // Exact, non-retrying CAS: a late MarkFault while Signal was in progress
    // changes this same ticket's gate and cannot be carried into Ready.
    if (S(cleanCalling) != Calling || (cleanCalling & kFault) || broken.load()) return false;
    if (!p.gate.compare_exchange_strong(cleanCalling, (cleanCalling & ~kMask) | Ready)) return false;
    --watched; // no per-slot access after the terminal store
    return true;
}
static bool Reusable(UINT64 gate, UINT64 producer, UINT64 workerView, UINT64 consumer, UINT64 need) {
    if (producer == UINT64_MAX || workerView == UINT64_MAX) return false;
    if (producer < Seq(gate) || workerView < Seq(gate)) return false;
    return S(gate) == Dropped || (S(gate) == Retiring && consumer != UINT64_MAX && consumer >= need);
}
static bool Fits(const Slot& p, UINT64 gate, Key k) {
    return !(gate & kFault) && Seq(gate) == p.seq && Equal(p.key, k);
}
static void ReleaseSlotBuffers(Slot& p) {
    std::lock_guard<std::mutex> lk(slotFenceMutex);
    p.owner.Reset(); p.ownerIdentity.Reset(); p.consume.Reset(); p.descriptors.Reset();
    for (auto& r : p.resources) r.Reset();
    p.bufA.Reset(); p.bufW.Reset(); p.heapA.Reset(); p.heapW.Reset();
    p.mirror.Reset(); p.marker.Reset(); p.prodA.Reset(); p.prodW.Reset();
    p.raw.store(nullptr); p.native.store(nullptr); p.identity.store(nullptr);
}
static D3D12_TEXTURE_COPY_LOCATION Foot(ID3D12Resource* backing, DXGI_FORMAT fmt, UINT64 offset) {
    D3D12_TEXTURE_COPY_LOCATION l{};
    l.pResource = backing; l.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    l.PlacedFootprint = {offset, {fmt, kNetW, kNetH, 1, PitchFor(fmt)}};
    return l;
}
static D3D12_TEXTURE_COPY_LOCATION Tex(ID3D12Resource* t) {
    D3D12_TEXTURE_COPY_LOCATION l{}; l.pResource = t; l.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return l;
}
static void RecordWorkerCopies(ID3D12GraphicsCommandList* cl, ID3D12Resource* source,
                               ID3D12Resource* color, ID3D12Resource* mv,
                               D3D12_RESOURCE_STATES colorState, D3D12_RESOURCE_STATES mvState) {
    const DXGI_FORMAT cFmt = color->GetDesc().Format;
    auto cs = Foot(source, cFmt, 0);
    auto vs = Foot(source, DXGI_FORMAT_R16G16_FLOAT, PlaneFor(cFmt));
    auto cd = Tex(color), vd = Tex(mv);
    F9Id::Transition(cl, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    F9Id::Transition(cl, color, colorState, D3D12_RESOURCE_STATE_COPY_DEST);
    F9Id::Transition(cl, mv, mvState, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
    cl->CopyTextureRegion(&vd, 0, 0, 0, &vs, nullptr);
    F9Id::Transition(cl, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    F9Id::Transition(cl, mv, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    F9Id::Transition(cl, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
}
struct Token {
    Slot* slot = nullptr; UINT64 seq = 0; Key key;
    explicit operator bool() const { return slot != nullptr; }
};
static std::atomic<UINT64> rejected[K]{};
static void Reject(Token t) {
    if (!t || t.key.frame < 0) return;
    auto& n = rejected[t.key.frame % K];
    auto seen = n.load();
    while (seen < t.seq && !n.compare_exchange_weak(seen, t.seq)) {}
}
static bool Rejected(int frame, UINT64 seq) {
    return seq && frame >= 0 && rejected[frame % K].load() == seq;
}
static bool Cancel(Token t) {
    if (!t) return false;
    auto g = t.slot->gate.load();
    if (Seq(g) != t.seq || S(g) != Offered || !Move(*t.slot, g, Dropped)) return false;
    Reject(t); ++stale; return true;
}
static void ResetAll() {
    for (auto& s : slots) {
        ReleaseSlotBuffers(s);
        s.key = {}; s.seq = 0; s.retire = 0; s.sealTick = 0; s.a3 = false;
        s.gate.store(Free);
    }
    for (auto& r : rejected) r.store(0);
    a3WriteCursor = 0; watched.store(0); broken.store(0);
    initialized.store(false); initAttempted = false;
}
struct Lease {
    Token t;
    std::condition_variable* notify;
    bool acquired = false;
    explicit Lease(Token token, std::condition_variable* cv = nullptr) : t(token), notify(cv) {}
    ~Lease() {
        if (!t) return;
        if (acquired) {
            const auto g = t.slot->gate.load();
            if (Seq(g) == t.seq) Move(*t.slot, g, Dead); // no exact submit proof, keep alive
            Break(5); Reject(t);
            if (notify) notify->notify_all();
        } else if (Cancel(t) && notify) notify->notify_all();
    }
    ID3D12Resource* Acquire(Key key, ID3D12CommandQueue* q, ID3D12Resource* color, ID3D12Resource* mv) {
        if (!t || !Equal(t.key, key) || !initialized.load() || broken.load() || !color || !mv ||
            F9Id::DeviceId(q).Get() != workerDevice.Get()) return nullptr;
        auto& p = *t.slot;
        auto g = p.gate.load();
        if (Seq(g) != t.seq || S(g) != Offered || (g & kFault) || !Move(p, g, Leased)) return nullptr;
        // CPU owns this offer until Finish/Cancel; no stale token can lift another generation.
        const auto done = p.prodW->GetCompletedValue();
        if (done == UINT64_MAX || done < t.seq || (p.gate.load() & kFault) || broken.load()) {
            if (done == UINT64_MAX) Break(3);
            Move(p, p.gate.load(), Dropped); Reject(t);
            if (notify) notify->notify_all();
            t = {}; return nullptr;
        }
        acquired = true;
        return p.bufW.Get();
    }
    void Finish(bool ok, ID3D12Fence* copyFence, UINT64 value, bool useful) {
        if (!acquired) return;
        auto& p = *t.slot;
        const auto g = p.gate.load();
        if (ok && copyFence && value && Seq(g) == t.seq && S(g) == Leased) {
            p.consume = copyFence; p.retire = value;
            Move(p, g, p.a3 && useful ? A3Held : Retiring); ++consumed;
        } else {
            Move(p, g, Dead); Break(5);
        }
        if (!ok || !useful) { Reject(t); if (notify) notify->notify_all(); }
        t = {}; acquired = false;
    }
};
static bool StampMatches(Slot& p) {
    const UINT64 pl = p.a3 ? ColorPlane() : MvPlane();
    const D3D12_RANGE range{SIZE_T(pl), SIZE_T(pl + sizeof(F9Id::Stamp))};
    void* ptr = nullptr;
    if (FAILED(p.mirror->Map(0, &range, &ptr)) || !ptr) { ++mirrorFail; return false; }
    F9Id::Stamp stamp{};
    memcpy(&stamp, static_cast<const BYTE*>(ptr) + pl, sizeof(stamp));
    const D3D12_RANGE none{}; p.mirror->Unmap(0, &none);
    return stamp == F9Id::Stamp{p.seq, p.seq ^ 0x91b5714fac4e78c9ull};
}
enum class DiagClass : unsigned { None, ABehind, WBehind, Both, MaxA, MaxW, MaxBoth };
static DiagClass DiagStatus(UINT64 need, UINT64 a, UINT64 w) {
    if (a == UINT64_MAX) return w == UINT64_MAX ? DiagClass::MaxBoth : DiagClass::MaxA;
    if (w == UINT64_MAX) return DiagClass::MaxW;
    if (a < need) return DiagClass::ABehind;
    return w < need ? DiagClass::WBehind : DiagClass::Both;
}
static const char* DiagName(DiagClass c) {
    static constexpr const char* names[] = {"none", "a_behind", "w_behind", "both", "max_a", "max_w", "max_both"};
    return names[unsigned(c)];
}
static bool DiagSample(Key k, int dispatch, DiagClass kind) { // render thread only
    constexpr int kPeriod = 240;
    static int block = -1, gen = -1;
    static UINT64 epoch = UINT64_MAX;
    static unsigned budget = 0, burst = 0;
    static DiagClass previous = DiagClass::None;
    const int d = dispatch >= 0 ? dispatch : k.frame;
    if (d < 0) return false;
    if (d / kPeriod != block) { block = d / kPeriod; budget = 32; }
    if (k.gen != gen || k.epoch != epoch) { gen = k.gen; epoch = k.epoch; burst = 12; }
    if (previous != kind && kind != DiagClass::Both) burst = 12;
    previous = kind;
    // A few normal startup samples, bounded onset/epoch bursts, then sparse
    // windows for issues that appear late. Never a per-ECL text trace.
    if (!budget || !(d < 16 || d % kPeriod < 4 || burst)) return false;
    --budget; if (burst) --burst;
    return true;
}
static void DiagOfferNone(Key k, int dispatch, const char* why) {
    if (!DiagSample(k, dispatch, DiagClass::None)) return;
    BtTrace("A2FD_FEED", k.frame, "fence", "dispatch=%d gen=%d epoch=%llu slot=-1 seq=0 need=0 class=none reason=%s obs_qpc=%lld",
            dispatch, k.gen, k.epoch, why, QpcNow());
}
static void DiagOffer(Slot& p, int dispatch, UINT64 a, UINT64 w) {
    const auto c = DiagStatus(p.seq, a, w);
    const bool sample = DiagSample(p.key, dispatch, c) || c >= DiagClass::MaxA;
    p.diagFollow = sample;
    if (!sample) return;
    p.diagLastReap = DiagClass::None;
    p.diagAttempt = dispatch;
    p.diagPrintedLate = false;
    BtTrace("A2FD_FEED", p.key.frame, "fence",
        "dispatch=%d gen=%d epoch=%llu slot=%d seq=%llu need=%llu class=%s a=%llu w=%llu obs_qpc=%lld ecl_ret_qpc=%lld sig_call_qpc=%lld sig_ret_qpc=%lld queue=0x%llx ecl_scope=%s",
        dispatch, p.key.gen, p.key.epoch, int(&p - slots), p.seq, p.seq, DiagName(c), a, w, QpcNow(),
        p.diagSubmit.eclReturn, p.diagSubmit.signalCall, p.diagSubmit.signalReturn, p.diagSubmit.queue,
        p.diagSubmit.wrappedBoundary ? "original_wrapper" : "original_native");
}
static void DiagReap(Slot& p, int dispatch, UINT64 a, UINT64 w, UINT64 worker, State state) {
    const auto c = DiagStatus(p.seq, a, w);
    const bool maxFault = c >= DiagClass::MaxA || worker == UINT64_MAX;
    if (!p.diagFollow && !maxFault) return;
    // At most one intermediate state and one first observed both-ready per
    // sampled ticket; device failure is exceptional and breaks the lane.
    if (!maxFault) {
        if (c == DiagClass::Both) {
            if (p.diagLastReap == c) return;
        } else {
            if (p.diagPrintedLate) return;
            p.diagPrintedLate = true;
        }
    }
    p.diagLastReap = c;
    const int delta = dispatch >= 0 && p.diagAttempt >= 0 ? dispatch - p.diagAttempt : -1;
    BtTrace("A2FD_REAP", p.key.frame, "fence",
        "dispatch=%d feed_dispatch=%d after=%d gen=%d epoch=%llu slot=%d seq=%llu need=%llu class=%s a=%llu w=%llu consume=%llu state=%u obs_qpc=%lld",
        dispatch, p.diagAttempt, delta, p.key.gen, p.key.epoch, int(&p - slots), p.seq, p.seq, DiagName(c), a, w, worker,
        unsigned(state), QpcNow());
}

static Token Offer(Key key, BYTE* rgb, size_t rgbBytes, int dispatch = -1) {
    const UINT64 pl = UINT64(kNetW) * UINT64(kNetH) * 4ull;
    if (!initialized.load() || broken.load() || !rgb || rgbBytes != pl) {
        DiagOfferNone(key, dispatch, "lane_or_mirror_unavailable");
        return {};
    }
    for (auto& p : slots) {
        auto g = p.gate.load();
        if (S(g) != Ready || !Fits(p, g, key)) continue;
        const auto a = p.prodA->GetCompletedValue(), w = p.prodW->GetCompletedValue();
        DiagOffer(p, dispatch, a, w);
        if (a == UINT64_MAX || w == UINT64_MAX) { Break(3); return {}; }
        if (a < p.seq || w < p.seq) { ++pending; return {}; }
        if (!Move(p, g, Mirroring)) return {};
        if ((p.gate.load() & kFault) || broken.load()) {
            Move(p, p.gate.load(), Dead);
            return {};
        }
        const D3D12_RANGE range{0, SIZE_T(pl + sizeof(F9Id::Stamp))};
        void* ptr = nullptr;
        if (FAILED(p.mirror->Map(0, &range, &ptr)) || !ptr) {
            ++mirrorFail; Move(p, p.gate.load(), Dead); Break(4); return {};
        }
        const auto* pixels = static_cast<const BYTE*>(ptr);
        F9Id::Stamp stamp{}; memcpy(&stamp, pixels + pl, sizeof(stamp));
        const bool ok = stamp == F9Id::Stamp{p.seq, p.seq ^ 0x91b5714fac4e78c9ull};
        if (ok && !(p.gate.load() & kFault) && !broken.load()) memcpy(rgb, pixels, size_t(pl));
        const D3D12_RANGE none{}; p.mirror->Unmap(0, &none);
        if (!ok || (p.gate.load() & kFault) || broken.load()) {
            ++mirrorFail; Move(p, p.gate.load(), Dead); Break(4); return {};
        }
        Move(p, p.gate.load(), Offered); ++offered;
        return {&p, p.seq, key};
    }
    DiagOfferNone(key, dispatch, "no_exact_ready_ticket");
    ++missing; return {};
}
static void CleanupSealed(bool force, ULONGLONG nowMs) {
    for (auto& p : slots) {
        const auto g = p.gate.load();
        if (S(g) != Sealed) continue;
        if (!force && (nowMs < p.sealTick || nowMs - p.sealTick < 150)) continue;
        if (Unsubscribe(p, g, Dead)) ++noSubmit;
    }
}
static void Reap(int cutoff, int gen, UINT64 epoch, bool dropAllReady = false, int dispatch = -1, bool a3 = false) { // main thread only, nonblocking
    if (!initialized.load()) return;
    const auto nowMs = GetTickCount64();
    const bool isBroken = broken.load() != 0;
    for (auto& p : slots) {
        auto g = p.gate.load();
        if (S(g) == Free || S(g) == Recording || p.a3 != a3) continue;
        const int window = a3 ? kSlots : K;
        if (S(g) == Sealed &&
            (isBroken || (p.key.frame < cutoff - window && nowMs >= p.sealTick && nowMs - p.sealTick >= 150))) {
            if (Unsubscribe(p, g, Dead)) ++noSubmit;
            continue;
        }
        if (S(g) == Ready && (dropAllReady || isBroken || p.key.frame < cutoff || p.key.gen != gen || p.key.epoch != epoch)) {
            if (!Move(p, g, Dropped)) continue;
            g = p.gate.load();
        }
        if (S(g) != Dropped && S(g) != Retiring) continue;
        if (!p.prodA || !p.prodW) {
            if (p.diagFollow && !p.diagPrintedLate) {
                p.diagPrintedLate = true;
                BtTrace("A2FD_REAP", p.key.frame, "fence",
                    "dispatch=%d feed_dispatch=%d gen=%d epoch=%llu slot=%d seq=%llu need=%llu class=none reason=missing_fence obs_qpc=%lld",
                    dispatch, p.diagAttempt, p.key.gen, p.key.epoch, int(&p - slots), p.seq, p.seq, QpcNow());
            }
            continue;
        }
        const auto a = p.prodA->GetCompletedValue();
        const auto w = p.prodW->GetCompletedValue();
        const auto c = S(g) == Retiring ? p.consume->GetCompletedValue() : 0;
        DiagReap(p, dispatch, a, w, c, S(g));
        if (a == UINT64_MAX || w == UINT64_MAX || c == UINT64_MAX) {
            Break((a == UINT64_MAX || w == UINT64_MAX) ? 3 : 5);
            return;
        }
        if (!Reusable(g, a, w, c, p.retire)) continue;
        if (S(g) == Dropped && !isBroken && !StampMatches(p)) { Move(p, g, Dead); Break(4); return; }
        p.owner.Reset(); p.ownerIdentity.Reset(); p.consume.Reset(); p.descriptors.Reset();
        for (auto& r : p.resources) r.Reset();
        p.raw.store(nullptr); p.native.store(nullptr); p.identity.store(nullptr);
        if (S(g) == Retiring) ++retired; else ++unusedReclaimed;
        p.gate.store((Seq(g) << kShift) | Free);
    }
}
static void OnUiStopped(bool stoppedOrOff) {
    if (!requested.load()) return;
    CleanupSealed(stoppedOrOff, GetTickCount64());
    if (stoppedOrOff && initialized.load()) Reap(0, 0, 0, true);
    if (stoppedOrOff && !watched.load()) gameQueue.store(0);
}
static bool TryReleaseInactive(int cutoff, int gen, UINT64 epoch, int currentFrame) {
    if (!initialized.load()) return true;
    const auto nowMs = GetTickCount64();
    for (auto& p : slots) {
        const auto g = p.gate.load();
        if (S(g) == Sealed && ((currentFrame - p.key.frame >= 2) || (nowMs >= p.sealTick && nowMs - p.sealTick >= 100))) {
            if (Unsubscribe(p, g, Dead)) ++noSubmit;
        }
    }
    Reap(cutoff, gen, epoch, true, currentFrame);
    for (const auto& p : slots) {
        const auto s = S(p.gate.load());
        if (s != Free && s != Dead) return false;
    }
    initialized.store(false);
    initAttempted = false;
    gameQueue.store(0);
    for (auto& p : slots) if (S(p.gate.load()) == Free) ReleaseSlotBuffers(p);
    return true;
}
static void Shutdown() {
    CleanupSealed(true, GetTickCount64());
    initialized.store(false);
    initAttempted = false;
    gameQueue.store(0);
    for (auto& p : slots) {
        const auto s = S(p.gate.load());
        if (s == Free || s == Dead) ReleaseSlotBuffers(p);
    }
    workerDevice.Reset();
    gameDevice.Reset();
}
struct Producer {
    Slot* p = nullptr;
    ID3D12GraphicsCommandList* cl;
    F9Id::ListRoute route;
    bool oldUnobserved = false;
    explicit Producer(ID3D12GraphicsCommandList* c)
        : cl(c), route(initialized.load() && !broken.load() && c ? c : nullptr) {
        if (!route.id) return;
        for (auto& s : slots) {
            const auto g = s.gate.load();
            if (!Watching(g) || s.identity.load() != route.id.Get() || Seq(s.gate.load()) != Seq(g)) continue;
            if (S(g) == Sealed) {
                // No observed original call. A subsequent opaque recording might
                // reuse this pointer. Do not let its later ECL bless the old write.
                if (Unsubscribe(s, g, Dead)) ++noSubmit;
                oldUnobserved = true;
            } else if (S(g) == Calling) {
                oldUnobserved = true;
            }
        }
    }
    Slot* Reserve(Key k, bool a3 = false) {
        if (!initialized.load() || broken.load() || oldUnobserved || !route.id ||
            !F9Id::observerReady.load() || F9Id::DeviceId(route.native.Get()).Get() != gameDevice.Get()) {
            ++missing; return nullptr;
        }
        const UINT cap = a3 ? UINT(kSlots) : UINT(K);
        for (UINT step = 0; step < cap; ++step) {
            const UINT index = a3 ? (a3WriteCursor + step) % cap : step;
            auto& s = slots[index];
            if (S(s.gate.load()) != Free) continue;
            if (a3 && (!s.bufA || !s.bufW || !s.mirror || !s.marker || !s.prodA || !s.prodW)) continue;
            const UINT64 seq = ++sequence;
            s.gate.store((seq << kShift) | Recording); // one render producer (existing ProxyExecute contract)
            s.key = k; s.seq = seq; s.retire = 0; s.sealTick = 0; s.a3 = a3;
            s.diagSubmit = {}; s.diagAttempt = -1; s.diagFollow = s.diagPrintedLate = false;
            s.diagLastReap = DiagClass::None;
            s.owner = static_cast<ID3D12CommandList*>(cl); s.ownerIdentity = route.id;
            s.raw.store(s.owner.Get()); s.native.store(route.native.Get()); s.identity.store(route.id.Get());
            const F9Id::Stamp stamp{seq, seq ^ 0x91b5714fac4e78c9ull};
            void* ptr = nullptr; const D3D12_RANGE no{};
            if (FAILED(s.marker->Map(0, &no, &ptr)) || !ptr) {
                s.raw.store(nullptr); s.native.store(nullptr); s.identity.store(nullptr);
                s.gate.store((seq << kShift) | Dead); Break(4); return nullptr;
            }
            memcpy(ptr, &stamp, sizeof(stamp));
            const D3D12_RANGE written{0, sizeof(stamp)}; s.marker->Unmap(0, &written);
            p = &s; return p;
        }
        ++full; return nullptr;
    }
    // Only before any command has referenced the reserved recording.
    void Abort() {
        if (!p) return;
        p->owner.Reset(); p->ownerIdentity.Reset();
        p->raw.store(nullptr); p->native.store(nullptr); p->identity.store(nullptr);
        p->gate.store((p->seq << kShift) | Free);
        p = nullptr;
    }
    void Seal(ID3D12Resource* color, ID3D12Resource* mv) {
        // Sources already COPY_SOURCE; neither data nor marker can be published
        // separately. This list also records the local color/MV capture.
        auto srcC = Tex(color), srcM = Tex(mv);
        const DXGI_FORMAT cFmt = p->a3 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        const UINT64 pl = PlaneFor(cFmt);
        auto dc = Foot(p->bufA.Get(), cFmt, 0);
        auto dm = Foot(p->bufA.Get(), DXGI_FORMAT_R16G16_FLOAT, pl);
        auto cpu = Foot(p->mirror.Get(), cFmt, 0);
        F9Id::Transition(cl, p->bufA.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyTextureRegion(&dc, 0, 0, 0, &srcC, nullptr);
        cl->CopyTextureRegion(&dm, 0, 0, 0, &srcM, nullptr);
        F9Id::Transition(cl, p->bufA.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        cl->CopyTextureRegion(&cpu, 0, 0, 0, &srcC, nullptr);
        cl->CopyBufferRegion(p->mirror.Get(), pl, p->marker.Get(), 0, sizeof(F9Id::Stamp));
        p->sealTick = GetTickCount64();
        if (p->a3) a3WriteCursor = (UINT(p - slots) + 1u) % kSlots;
        ++watched;
        p->gate.store((p->seq << kShift) | Sealed);
    }
};
// A3 consumes on the same game list. Keep exact batch evidence (and copy resources)
// through the original ExecuteCommandLists boundary, even if no input slot was writable.
struct Tail {
    enum Phase : unsigned { Sealed, Calling, Sent, Failed };
    std::atomic<Phase> phase{Sealed};
    std::atomic<bool> superseded{false};
    ComPtr<ID3D12CommandList> raw, native;
    ComPtr<IUnknown> id;
    ComPtr<ID3D12Fence> proof;
    UINT64 value = 0;
    ComPtr<ID3D12Resource> cache, output;
    bool Submitted() const {
        if (phase.load() != Sent) return false;
        const auto done = proof ? proof->GetCompletedValue() : UINT64_MAX;
        return done != UINT64_MAX;
    }
    bool Complete() const {
        if (phase.load() != Sent) return false;
        const auto done = proof->GetCompletedValue();
        return done != UINT64_MAX && done >= value;
    }
};
static std::mutex tailMutex; // not held across ExecuteCommandLists or by a Worker
static std::vector<std::shared_ptr<Tail>> tails;
static std::atomic<unsigned> tailWatched{0};
static std::mutex tailSignalMutex;
static ComPtr<ID3D12Fence> tailFence;
static UINT64 tailValue = 0;
static std::shared_ptr<Tail> NewTail(const F9Id::ListRoute& route, ID3D12GraphicsCommandList* cl,
                                    ID3D12Resource* cache, ID3D12Resource* output) {
    if (!route.id || !route.native || !F9Id::observerReady.load() ||
        F9Id::DeviceId(route.native.Get()).Get() != gameDevice.Get()) return {};
    auto t = std::make_shared<Tail>();
    t->raw = static_cast<ID3D12CommandList*>(cl); t->native = route.native.Get(); t->id = route.id;
    t->cache = cache; t->output = output;
    return t;
}
static void SealTail(const std::shared_ptr<Tail>& t) {
    if (!t) return;
    std::lock_guard lk(tailMutex);
    // An old unobserved recording of this identity must never publish a cache.
    // Keep its references until the eventual same-identity batch completes.
    for (auto& x : tails) if (x->phase.load() == Tail::Sealed && x->id.Get() == t->id.Get())
        x->superseded.store(true);
    tails.push_back(t);
    ++tailWatched;
}
static void ReapTails() {
    std::lock_guard lk(tailMutex);
    std::erase_if(tails, [](const auto& t) { return t->Complete(); });
}
static bool CacheInFlight(ID3D12Resource* cache) {
    std::lock_guard lk(tailMutex);
    for (const auto& t : tails) if (t->cache.Get() == cache) return true;
    return false;
}
struct Hit { Slot* slot; UINT64 seq; };
struct Hits {
    Hit list[K]{}; UINT count = 0;
    std::vector<std::shared_ptr<Tail>> tail;
    bool Any() const { return count || !tail.empty(); }
    LONGLONG diagEclReturn = 0;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IUnknown> queueId;
    bool wrapped = false;
    bool Contains(const Slot& slot, UINT64 seq) const {
        for (UINT i = 0; i < count; ++i) if (list[i].slot == &slot && list[i].seq == seq) return true;
        return false;
    }
};
static void ObserveCallingFault(Slot& p, UINT64 g) {
    if (!MarkFault(p, g)) return;
    ++replay;
    if (faultStreak.fetch_add(1) + 1 >= kMaxFaultStreak) Break(1);
}
static void Claim(Slot& p, UINT64 g, const F9Id::QueueRoute& r, Hits& hits) {
    auto cur = p.gate.load();
    if (Seq(cur) != Seq(g) || !Watching(cur)) return;
    if (S(cur) == Calling) { ObserveCallingFault(p, cur); return; }
    if ((cur & kFault) || broken.load()) return;
    if (!r.verified || gameDevice.Get() != r.dev.Get()) { ++identityFail; return; }
    const auto target = reinterpret_cast<uintptr_t>(r.identity.Get());
    uintptr_t expected = 0;
    if (!gameQueue.compare_exchange_strong(expected, target) && expected != target) {
        ++identityFail;
        Unsubscribe(p, cur, Dead);
        Break(2);
        return;
    }
    if (p.gate.compare_exchange_strong(cur, (cur & ~kMask) | Calling)) {
        hits.list[hits.count++] = {&p, Seq(g)};
        hits.queue = r.native; hits.queueId = r.identity; hits.wrapped = r.wrapped;
    } else if (Seq(cur) == Seq(g) && S(cur) == Calling) {
        ObserveCallingFault(p, cur);
    }
}

// Call-local positive canonical results only. Share them with the native child
// of this wrapper call by exact COM argument pointer; never persist a miss or
// turn a successful shallow match into a global opt-out of canonical matching.
struct CanonMemo {
    struct Entry { ID3D12CommandList* raw = nullptr; ComPtr<IUnknown> id; } entries[64]{};
    UINT count = 0;
    ComPtr<IUnknown> Resolve(ID3D12CommandList* list) {
        if (!list) return {};
        for (UINT i = 0; i < count; ++i) if (entries[i].raw == list) return entries[i].id;
        ++deepListChecks;
        F9Id::ListRoute route(list);
        if (route.id && count < 64) entries[count++] = {list, route.id};
        return route.id;
    }
};
struct Forward {
    ID3D12CommandQueue* native;
    IUnknown* identity;
    UINT count;
    ID3D12CommandList* const* lists;
    const Hits& hits;
    CanonMemo& memo;
    bool used = false;
};
static thread_local Forward* forwarding = nullptr;
struct ForwardScope {
    Forward* saved = forwarding;
    explicit ForwardScope(Forward* now = nullptr) { forwarding = now; }
    ~ForwardScope() { forwarding = saved; }
};
static bool SameForwardQueue(const Forward& outer, ID3D12CommandQueue* q,
                             std::optional<F9Id::QueueRoute>& route) {
    if (outer.used) return false;
    if (outer.native == q) return true;
    if (!route) route.emplace(q);
    return route->verified && !route->wrapped && route->identity.Get() == outer.identity;
}
static bool SameForward(Forward& outer, UINT n, ID3D12CommandList* const* ls) {
    if (!outer.hits.Any() || outer.count != n || !ls || !outer.lists) return false;
    for (UINT i = 0; i < n; ++i) {
        if (outer.lists[i] == ls[i]) continue;
        const auto a = outer.memo.Resolve(outer.lists[i]), b = outer.memo.Resolve(ls[i]);
        if (!a || a.Get() != b.Get()) return false;
    }
    return true;
}
static Hits Before(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* ls, CanonMemo& memo,
                   std::optional<F9Id::QueueRoute>& route, const Hits* already = nullptr) {
    Hits h;
    if (!ls || !n || ((!watched.load() || broken.load()) && !tailWatched.load())) return h;
    const UINT64 before = sequence.load();
    for (UINT i = 0; i < n; ++i) {
        if (!ls[i]) continue;
        bool direct = false, haveCandidate = false;
        for (auto& p : slots) {
            const auto g = p.gate.load();
            if (!Watching(g) || Seq(g) > before) continue;
            const bool skip = already && already->Contains(p, Seq(g));
            haveCandidate |= !skip;
            if ((p.raw.load() == ls[i] || p.native.load() == ls[i]) && p.gate.load() == g) {
                if (!skip) { if (!route) route.emplace(q); Claim(p, g, *route, h); }
                direct = true;
                break;
            }
        }
        if (direct || !haveCandidate || broken.load()) continue;
        const auto id = memo.Resolve(ls[i]);
        if (!id) continue;
        for (auto& p : slots) {
            const auto g = p.gate.load();
            if (!Watching(g) || Seq(g) > before || p.identity.load() != id.Get() || p.gate.load() != g) continue;
            if (!already || !already->Contains(p, Seq(g))) {
                if (!route) route.emplace(q);
                Claim(p, g, *route, h);
            }
            break;
        }
    }
    if (tailWatched.load()) {
        std::lock_guard lk(tailMutex);
        for (UINT i = 0; i < n; ++i) {
            if (!ls[i]) continue;
            ComPtr<IUnknown> id;
            for (auto& t : tails) {
                if (t->phase.load() != Tail::Sealed) continue;
                if (t->raw.Get() != ls[i] && t->native.Get() != ls[i]) {
                    if (!id) id = memo.Resolve(ls[i]);
                    if (!id || id.Get() != t->id.Get()) continue;
                }
                if (!route) route.emplace(q);
                if (!route->verified || route->dev.Get() != gameDevice.Get()) continue;
                const auto target = reinterpret_cast<uintptr_t>(route->identity.Get());
                uintptr_t expected = 0;
                if (!gameQueue.compare_exchange_strong(expected, target) && expected != target) continue;
                t->phase.store(Tail::Calling);
                --tailWatched;
                h.tail.push_back(t);
                h.queue = route->native; h.queueId = route->identity; h.wrapped = route->wrapped;
            }
        }
    }
    return h;
}
static Hits Before(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* ls) {
    CanonMemo memo;
    std::optional<F9Id::QueueRoute> route;
    return Before(q, n, ls, memo, route);
}
static void After(const Hits& h) {
    ComPtr<ID3D12Fence> batchProof;
    UINT64 batchValue = 0;
    for (UINT i = 0; i < h.count; ++i) {
        auto& p = *h.list[i].slot;
        const auto seq = h.list[i].seq;
        const auto cleanCalling = (seq << kShift) | Calling;
        const auto g = p.gate.load();
        if (Seq(g) != seq || S(g) != Calling) continue;
        if (g != cleanCalling || broken.load()) { Unsubscribe(p, g, Dead); continue; }
        // The hook's list already went through the real ECL. Even if a concurrent
        // fault lands inside Signal, PublishReady's exact CAS will reject it.
        const auto signalCall = QpcNow();
        const HRESULT signalHr = h.queue->Signal(p.prodA.Get(), seq);
        const auto signalRet = QpcNow();
        if (FAILED(signalHr)) {
            BtTrace("A2FD_SIGNAL_FAIL", p.key.frame, "fence",
                "gen=%d epoch=%llu slot=%d seq=%llu hr=0x%08lX ecl_ret_qpc=%lld sig_call_qpc=%lld sig_ret_qpc=%lld",
                p.key.gen, p.key.epoch, int(&p - slots), seq, (unsigned long)signalHr,
                h.diagEclReturn, signalCall, signalRet);
            Unsubscribe(p, g, Dead);
            Break(3);
            continue;
        }
        if (!h.tail.empty() && !batchProof) { batchProof = p.prodA; batchValue = seq; }
        p.diagSubmit = {h.diagEclReturn, signalCall, signalRet, UINT64(reinterpret_cast<UINT_PTR>(h.queue.Get())), h.wrapped};
        if (PublishReady(p, cleanCalling)) {
            ++submitted;
            faultStreak.store(0);
            if (p.a3) NotifyA3Ready();
        } else {
            Unsubscribe(p, cleanCalling, Dead);
        }
    }
    if (!h.tail.empty() && !batchProof) {
        // One batch-tail fallback only when no existing A2 producer proof covers
        // the exact original submission. Never insert Signal between GPU passes.
        std::lock_guard lk(tailSignalMutex);
        if (!tailFence) {
            ComPtr<ID3D12Device> dev;
            if (SUCCEEDED(h.queue->GetDevice(IID_PPV_ARGS(&dev))))
                dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tailFence));
        }
        if (tailFence && SUCCEEDED(h.queue->Signal(tailFence.Get(), ++tailValue))) {
            batchProof = tailFence; batchValue = tailValue;
        }
    }
    for (auto& t : h.tail) {
        t->proof = batchProof; t->value = batchValue;
        t->phase.store(batchProof ? Tail::Sent : Tail::Failed);
    }
}

template<class Fn> static Hits ExecuteOriginal(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* ls,
                                               Fn call, bool directPeer) {
    if (directPeer || ((!watched.load() || broken.load()) && !tailWatched.load())) { call(); return {}; }
    CanonMemo local;
    std::optional<F9Id::QueueRoute> route;
    Forward* outer = forwarding;
    const bool nested = outer && SameForwardQueue(*outer, q, route);
    const bool same = nested && SameForward(*outer, n, ls);
    if (same) outer->used = true;
    auto& memo = nested ? outer->memo : local;
    // Even a proven same batch still checks unclaimed tickets. An outer miss
    // never hides a child hit (native pointer or independently verified route).
    auto hits = Before(q, n, ls, memo, route, same ? &outer->hits : nullptr);
    if (!route && !nested) route.emplace(q);
    if (!nested && route && route->verified && route->wrapped) {
        Forward next{route->native.Get(), route->identity.Get(), n, ls, hits, memo};
        ForwardScope scope(&next);
        call();
    } else {
        ForwardScope scope;
        call();
    }
    if (hits.Any()) hits.diagEclReturn = QpcNow();
    return hits;
}
static void Stats(int frame) {
    if (!requested.load()) return;
    static bool diagSchema = false;
    if (!diagSchema) {
        diagSchema = true;
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        BtTrace("A2-FENCE-DIAG", frame, "fence", "schema=1 qpc_hz=%lld limit=32 block_dispatch=240", freq.QuadPart);
    }
    static unsigned previous = 0;
    const auto failure = broken.load();
    if (frame % 300 != 0 && failure == previous) return;
    previous = failure;
    Log("A2-INPUT: frame=%d watched=%u ready=%d broken=%u queue_id=%p producer_signal=%u offered=%u worker_copy_submit=%u worker_copy_retired=%u unused_reclaimed=%u full=%u miss=%u pending=%u stale=%u no_submit_poll=%u identity=%u replay=%u mirror_fail=%u deep_checks=%u MV_expected=feedFrame marker=A2-INPUT-R3",
        frame, watched.load(), (int)initialized.load(), failure, reinterpret_cast<void*>(gameQueue.load()),
        submitted.load(), offered.load(), consumed.load(), retired.load(), unusedReclaimed.load(),
        full.load(), missing.load(), pending.load(), stale.load(), noSubmit.load(),
        identityFail.load(), replay.load(), mirrorFail.load(), deepListChecks.load());
}
} // namespace A2In
