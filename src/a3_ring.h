#pragma once

// A-only cross-adapter lane. Input slots are the R3 producer's six independent
// allocations; the answer ring owns six different cross-adapter resources.
namespace A3 {
static constexpr int kSlots = A2In::kSlots;
static_assert(kSlots >= K);
using A2In::Key;
static bool SameGen(Key a, Key b) { return a.gen == b.gen && a.epoch == b.epoch; }
static int PickNewest(const int (&tags)[kSlots], const bool (&valid)[kSlots], int lastCons) {
    int best = -1, bestT = lastCons;
    for (int i = 0; i < kSlots; ++i) {
        if (!valid[i]) continue;
        if (tags[i] > bestT) { bestT = tags[i]; best = i; }
    }
    return best;
}
struct Input {
    AdapterFrameDesc desc{}; // stored with an empty ext/list/output plus adapter snapshot
    UINT netW = 0, netH = 0;
    ComPtr<ID3D12Resource> source[6], captured[6]; // color, MV, depth, exposure, reactive, transparency
    ComPtr<ID3D12Resource> fed, packed, composed, localAnswer, answerImage, add4k;
    ComPtr<ID3D12Resource> scaleTex, scaleRb;
    UINT hdrMode = 0; // 0=SDR, 1=LinearHdr, 2=PqHdr10
    bool hasExposure = false, autoExposure = false, isHdrFlag = false;
    float preExposure = 1.0f, exposureScale = 1.0f;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> capturePso[2], presentPso[2];
};
static Input inputs[kSlots];
struct Answer {
    enum State { Free, Writing, Ready, Reading, Quarantined } state = Free;
    A2In::Token input;
    UINT64 serial = 0, fenceValue = 0, bytes = 0;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Heap> heapA, heapW;
    ComPtr<ID3D12Resource> bufA, bufW;
    std::shared_ptr<A2In::Tail> tail;
    void Clear() { state = Free; input = {}; tail.reset(); fence.Reset(); fenceValue = 0; }
};
static Answer answers[kSlots];
static std::mutex m;
static Key active;
static int lastOffered = -1, lastRecorded = -1, lastNativeDispatch = -1;
static UINT64 serial = 0;
static unsigned writeCursor = 0;
static F9Id::A3Meta Audit(const Answer& a) {
    if (!a.input) return {};
    return {int(a.input.slot - A2In::slots), int(&a - answers), -1, a.input.seq, a.serial};
}
static void Trace(const char* phase, const Answer& a, int dispatch) {
    const auto& t = a.input;
    BtTrace(phase, t.key.frame, "a3", "dispatch=%d gen=%d epoch=%llu islot=%d iseq=%llu aslot=%d aseq=%llu",
        dispatch, t.key.gen, t.key.epoch, t.slot ? int(t.slot - A2In::slots) : -1, t.seq,
        int(&a - answers), a.serial);
}
static void ReleaseInput(const A2In::Token& t) {
    if (!t) return;
    const auto gate = t.slot->gate.load();
    if (A2In::Seq(gate) == t.seq && A2In::S(gate) == A2In::A3Held)
        A2In::Move(*t.slot, gate, A2In::Retiring); // GPU producer and W reader still verified by R3 Reap
}
static bool ValidInput(const A2In::Token& t) {
    if (!t) return false;
    const auto gate = t.slot->gate.load();
    return A2In::S(gate) == A2In::A3Held && A2In::Fits(*t.slot, gate, t.key);
}
static bool SettleWriter(int dispatch) { // W holds m; no next W job until write really completed
    for (auto& a : answers) if (a.state == Answer::Writing && a.fence) {
        const auto done = a.fence->GetCompletedValue();
        if (done == UINT64_MAX) { a.state = Answer::Quarantined; return false; }
        if (done < a.fenceValue) return false;
        if (SameGen(active, a.input.key) && ValidInput(a.input) && !A2In::broken.load()) {
            a.state = Answer::Ready; Trace("A3_W_ANSWER_DONE", a, dispatch);
        } else { ReleaseInput(a.input); a.Clear(); }
    }
    return true;
}
struct Work {
    Answer* a = nullptr;
    Work() = default;
    Work(const Work&) = delete;
    ~Work() {
        if (!a) return;
        std::lock_guard lk(m);
        ReleaseInput(a->input); a->Clear();
    }
    A2In::Token Claim(Key key, int dispatch) {
        std::lock_guard lk(m);
        if (!SameGen(active, key) || A2In::broken.load() || !SettleWriter(dispatch)) return {};
        Answer* free = nullptr;
        for (unsigned step = 0; step < unsigned(kSlots); ++step) {
            auto& x = answers[(writeCursor + step) % kSlots];
            if (x.state == Answer::Free && x.bufW) { free = &x; break; }
        }
        if (!free) return {};
        int tags[kSlots]{}; bool ready[kSlots]{}; UINT64 gates[kSlots]{};
        for (int i = 0; i < kSlots; ++i) {
            auto& p = A2In::slots[i];
            const auto g = p.gate.load(); gates[i] = g;
            if (A2In::S(g) != A2In::Ready || !p.a3 || (g & A2In::kFault) || !SameGen(p.key, key)) continue;
            const auto aa = p.prodA->GetCompletedValue(), ww = p.prodW->GetCompletedValue();
            if (aa == UINT64_MAX || ww == UINT64_MAX) { A2In::Break(3); return {}; }
            tags[i] = p.key.frame; ready[i] = aa >= p.seq && ww >= p.seq;
        }
        const int i = PickNewest(tags, ready, lastOffered);
        if (i < 0) return {};
        auto& p = A2In::slots[i];
        if (!A2In::Move(p, gates[i], A2In::Mirroring)) return {};
        if (!A2In::StampMatches(p)) { A2In::Move(p, p.gate.load(), A2In::Dead); A2In::Break(4); return {}; }
        free->state = Answer::Writing; free->serial = ++serial;
        writeCursor = unsigned(free - answers + 1) % kSlots;
        free->input = {&p, p.seq, p.key};
        a = free;
        lastOffered = p.key.frame;
        A2In::Move(p, p.gate.load(), A2In::Offered);
        ++A2In::offered;
        Trace("A3_W_CLAIM", *a, dispatch);
        return a->input;
    }
    void Submitted(bool ok, ID3D12Fence* fe, UINT64 value, int dispatch) {
        if (!a) return;
        std::lock_guard lk(m);
        if (ok) { a->fence = fe; a->fenceValue = value; Trace("A3_W_ANSWER_SENT", *a, dispatch); }
        else { a->state = Answer::Quarantined; Trace("A3_W_ANSWER_FAILED", *a, dispatch); }
        a = nullptr; // from here only the real W fence (or quarantine) can settle the write
    }
};
static void Reap(Key key, int dispatch) { // render side holds m
    if (!SameGen(key, active)) { active = key; lastOffered = lastRecorded = -1; }
    for (auto& a : answers) {
        if (a.state == Answer::Reading && a.tail->Complete()) {
            Trace("A3_A_RETIRED", a, dispatch); ReleaseInput(a.input); a.Clear();
        } else if (a.state == Answer::Writing && a.fence && !SameGen(active, a.input.key)) {
            const auto done = a.fence->GetCompletedValue();
            if (done == UINT64_MAX) a.state = Answer::Quarantined;
            else if (done >= a.fenceValue) { ReleaseInput(a.input); a.Clear(); }
        } else if (a.state == Answer::Ready && (!SameGen(active, a.input.key) ||
                    a.input.key.frame <= lastRecorded || !ValidInput(a.input))) {
            Trace("A3_A_DROP", a, dispatch); ReleaseInput(a.input); a.Clear();
        }
    }
    // A still-pending older input is not reused until its own production fences complete.
    A2In::Reap(lastOffered < 0 ? -1 : lastOffered + 1, key.gen, key.epoch, false, dispatch, true);
}
static Answer* Pick() { // render side holds m
    int tags[kSlots]{}; bool valid[kSlots]{};
    for (int i = 0; i < kSlots; ++i) {
        const auto& a = answers[i];
        tags[i] = a.input.key.frame;
        valid[i] = a.state == Answer::Ready && SameGen(active, a.input.key) && ValidInput(a.input);
    }
    const int i = PickNewest(tags, valid, lastRecorded);
    return i < 0 ? nullptr : &answers[i];
}
struct PendingFence {
    int slot = -1;
    UINT64 seq = 0;
    ComPtr<ID3D12Fence> prodW;
};
struct PendingInfo {
    bool activeMatches = false;
    bool hasPending = false;
    bool hasReadyNow = false;
    bool prodAOnlyLag = false;
    PendingFence fences[kSlots]{};
    int fenceCount = 0;
};
static PendingInfo InspectPending(Key key) {
    PendingInfo info;
    std::lock_guard lk(m);
    if (!SameGen(active, key) || !A2In::initialized.load() || A2In::broken.load()) return info;
    info.activeMatches = true;
    std::lock_guard flk(A2In::slotFenceMutex);
    for (int i = 0; i < kSlots; ++i) {
        const auto& p = A2In::slots[i];
        const auto g = p.gate.load();
        if (!p.a3 || (g & A2In::kFault) || !SameGen(p.key, key)) continue;
        const auto st = A2In::S(g);
        if (st == A2In::Sealed || st == A2In::Calling) {
            info.hasPending = true;
        } else if (st == A2In::Ready && p.prodA && p.prodW) {
            const auto aa = p.prodA->GetCompletedValue();
            const auto ww = p.prodW->GetCompletedValue();
            if (aa == UINT64_MAX || ww == UINT64_MAX) { A2In::Break(3); return {}; }
            if (aa >= p.seq && ww >= p.seq) {
                info.hasReadyNow = true;
                return info;
            }
            info.hasPending = true;
            if (ww < p.seq) info.fences[info.fenceCount++] = {i, p.seq, p.prodW};
            else info.prodAOnlyLag = true;
        }
    }
    return info;
}
}
