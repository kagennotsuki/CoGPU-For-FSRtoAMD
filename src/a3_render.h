#pragma once
#include "a3_policy.h"

namespace A3 {
enum View : UINT { Base, Mv, Fed, LocalAnswer, ComposeSrc, OutSrc, AnswerImage,
                   FedUav, MvUav, ComposeUav, Add4kUav, ViewCount };
static constexpr UINT64 Bytes = A2In::kPlane;
static void* lastContext = nullptr;
static bool everActive = false;
static Key cacheKey;
static CacheAge cacheAge;
static ComPtr<ID3D12Resource> cache;
struct Candidate { Key key; std::shared_ptr<A2In::Tail> tail; };
static std::vector<Candidate> candidates;
static std::vector<ComPtr<ID3D12Resource>> cachePool;

static bool SameShape(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Dimension == b.Dimension && a.Format == b.Format && a.Width == b.Width && a.Height == b.Height &&
        a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
        a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality;
}
static bool Surface(const D3D12_RESOURCE_DESC& a) {
    return a.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && a.SampleDesc.Count == 1 && a.DepthOrArraySize == 1;
}
static ComPtr<ID3D12Resource> FromDesc(D3D12_RESOURCE_DESC d) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> res;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                   nullptr, IID_PPV_ARGS(&res));
    return res;
}
static D3D12_CPU_DESCRIPTOR_HANDLE Handle(const Input& p, UINT i) {
    auto h = p.heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i) * g_inc; return h;
}
static void Srv(const Input& p, UINT slot, ID3D12Resource* r) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = r->GetDesc().Format;
    switch (d.Format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case DXGI_FORMAT_R16G16_TYPELESS: d.Format = DXGI_FORMAT_R16G16_FLOAT; break;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    default: break;
    }
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(r, &d, Handle(p, slot));
}
static void Uav(const Input& p, UINT slot, ID3D12Resource* r) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = r->GetDesc().Format; d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_dev->CreateUnorderedAccessView(r, nullptr, &d, Handle(p, slot));
}
static bool InitAnswerResources() { // main, only while the ring lock is held
    for (auto& a : answers) if (!a.bufA || !a.bufW) {
        if (!MakeXBuf(Bytes, "A3 independent answer", a.heapA, a.bufA, a.heapW, a.bufW)) return false;
    }
    return true;
}
static bool Prepare(Input& p, const AdapterFrameDesc& live) {
    auto d = live;
    AdapterResource* r[] = {&d.color, &d.motionVectors, &d.depth, &d.exposure, &d.reactive, &d.transparencyAndComposition};
    for (UINT i = 0; i < 6; ++i) {
        if (!r[i]->resource) { if (i < 3) return false; p.source[i].Reset(); continue; }
        if (!r[i]->valid) return false;
        auto* src = static_cast<ID3D12Resource*>(r[i]->resource);
        const auto shape = src->GetDesc();
        if (!Surface(shape)) return false;
        if (!p.captured[i] || !SameShape(p.captured[i]->GetDesc(), shape) || p.captured[i]->GetDesc().Flags != shape.Flags)
            p.captured[i] = FromDesc(shape);
        if (!p.captured[i]) return false;
        p.source[i] = src;
        r[i]->resource = p.captured[i].Get(); r[i]->state = D3D12_RESOURCE_STATE_COMMON;
    }
    const auto color = p.captured[0]->GetDesc();
    // The inherited ResidCS UAV is typed and uses one mip. Don't advertise a
    // different physical resource layout as the game's input without proof.
    if (color.MipLevels != 1 || !g_rs || !g_psoDown || !g_psoMvPack || !g_psoResid) return false;
    if (!p.composed || !SameShape(p.composed->GetDesc(), color)) {
        p.composed = MakeTex(g_dev.Get(), UINT(color.Width), color.Height, color.Format, D3D12_RESOURCE_STATE_COMMON, true);
    }
    if (!p.fed) p.fed = MakeTex(g_dev.Get(), kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, true);
    if (!p.packed) p.packed = MakeTex(g_dev.Get(), kNetW, kNetH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COMMON, true);
    if (!p.localAnswer) {
        D3D12_RESOURCE_DESC b{}; b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; b.Width = Bytes;
        b.Height = b.DepthOrArraySize = b.MipLevels = b.SampleDesc.Count = 1; b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        p.localAnswer = FromDesc(b);
    }
    if (g_bypass && !p.answerImage) p.answerImage = MakeTex(g_dev.Get(), kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                            D3D12_RESOURCE_STATE_COMMON, false);
    if (!p.fed || !p.packed || !p.localAnswer || !p.composed || (g_bypass && !p.answerImage)) return false;
    if (!p.heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; hd.NumDescriptors = ViewCount;
        if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.heap)))) return false;
    }
    p.rs = g_rs; p.capturePso[0] = g_psoDown; p.capturePso[1] = g_psoMvPack;
    Srv(p, Base, p.captured[0].Get()); Srv(p, Mv, p.captured[1].Get()); Srv(p, Fed, p.fed.Get());
    Srv(p, ComposeSrc, p.composed.Get());
    if (p.answerImage) Srv(p, AnswerImage, p.answerImage.Get());
    Uav(p, FedUav, p.fed.Get()); Uav(p, MvUav, p.packed.Get()); Uav(p, ComposeUav, p.composed.Get());
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.NumElements = kNetW * kNetH;
    g_dev->CreateShaderResourceView(p.localAnswer.Get(), &sd, Handle(p, LocalAnswer));
    d.hasUnsupportedExt = false; d.commandList = nullptr; d.output = {};
    p.desc = d;
    return true;
}
static void Bind(ID3D12GraphicsCommandList* cl, const Input& p) {
    ID3D12DescriptorHeap* h[] = {p.heap.Get()}; cl->SetDescriptorHeaps(1, h);
}
static bool Capture(ID3D12GraphicsCommandList* cl, const AdapterFrameDesc& d,
                    A2In::Producer& capture, Key key, F9Id::ARecord& f9) {
    A2In::Slot* raw;
    { std::lock_guard lk(m); raw = capture.Reserve(key, true); }
    if (!raw) return false;
    auto& p = inputs[raw - A2In::slots];
    if (!Prepare(p, d)) { std::lock_guard lk(m); capture.Abort(); return false; }
    const AdapterResource* r[] = {&d.color, &d.motionVectors, &d.depth, &d.exposure, &d.reactive, &d.transparencyAndComposition};
    for (UINT i = 0; i < 6; ++i) if (p.source[i]) {
        const auto st = r[i]->state;
        auto* src = p.source[i].Get(); auto* dst = p.captured[i].Get();
        Barrier(cl, src, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(dst, src);
        Barrier(cl, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
    }
    Bind(cl, p);
    auto* base = p.captured[0].Get(); auto* mv = p.captured[1].Get();
    auto* fed = p.fed.Get(); auto* packed = p.packed.Get();
    g_curJitX = d.jitterOffset.x; g_curJitY = d.jitterOffset.y;
    Barrier(cl, base, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, mv, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, fed, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cl, packed, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SetPass(cl, p.capturePso[0].Get(), Base, Base, Mv, Base, LocalAnswer, FedUav, MvUav, kNetW, kNetH, p.heap.Get(), p.rs.Get());
    SetPass(cl, p.capturePso[1].Get(), Mv, Mv, Mv, Base, LocalAnswer, MvUav, FedUav, kNetW, kNetH, p.heap.Get(), p.rs.Get());
    Barrier(cl, base, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, fed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cl, packed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const F9Id::A3Meta audit{int(raw - A2In::slots), -1, -1, raw->seq, 0};
    f9.Tap(F9Id::AColor, fed, key.frame, key.frame, F9Id::A2Pair, audit);
    f9.Tap(F9Id::AMv, packed, key.frame, key.frame, F9Id::A2Pair, audit);
    capture.Seal(fed, packed);
    Barrier(cl, fed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, packed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    BtTrace("A3_A_INPUT", key.frame, "a3", "gen=%d epoch=%llu islot=%d iseq=%llu aslot=-1 aseq=0",
            key.gen, key.epoch, int(raw - A2In::slots), raw->seq);
    return true;
}
static void ProcessCache(Key key) {
    A2In::ReapTails();
    if (!SameGen(cacheKey, key)) { cache.Reset(); cacheKey = key; cacheKey.frame = -1; cacheAge.Clear(); }
    const Candidate* best = nullptr;
    for (const auto& c : candidates) {
        // Promote as soon as the copy batch is submitted on the single game queue
        // so cache playback holds the latest shown frame without rewinding.
        if (SameGen(c.key, key) && c.key.frame > (best ? best->key.frame : cacheKey.frame) &&
            !c.tail->superseded.load() && (c.tail->Submitted() || c.tail->Complete())) best = &c;
    }
    if (best) {
        cache = best->tail->cache; cacheKey = best->key;
        cacheAge.Publish(GetTickCount64());
        BtTrace("A3_A_CACHE_PUBLISH", cacheKey.frame, "a3", "gen=%d epoch=%llu", cacheKey.gen, cacheKey.epoch);
    }
    std::erase_if(candidates, [&](const Candidate& c) { return !SameGen(c.key, key) || c.tail->Complete() ||
                                                            c.tail->phase.load() == A2In::Tail::Failed; });
}
static void StopDisplay(int dispatch) {
    const Key inactive{-1, -2, 0};
    if (!everActive) return;
    lastContext = nullptr;
    { std::lock_guard lk(m); Reap(inactive, dispatch); }
    ProcessCache(inactive);
    A2In::NotifyA3Ready();
}
static ComPtr<ID3D12Resource> CacheTarget(ID3D12Resource* out) {
    const auto shape = out->GetDesc();
    for (auto it = cachePool.begin(); it != cachePool.end();) {
        auto* p = it->Get();
        if (cache.Get() != p && !A2In::CacheInFlight(p)) {
            if (SameShape(p->GetDesc(), shape)) return *it;
            it = cachePool.erase(it);
        } else ++it;
    }
    if (cachePool.size() >= size_t(kSlots) + 1u) return {};
    auto d = shape; d.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (auto p = FromDesc(d)) { cachePool.push_back(p); return p; }
    return {};
}
static void Notify(Key key) {
    auto& W = g_worker;
    {
        std::lock_guard lk(W.m);
        if (W.hasIn) A2In::Cancel(W.inA2);
        W.inA2 = {}; W.inA3 = true;
        W.f9Dispatch = key.frame; W.inFrame = W.inIndex = -1;
        W.inAGen = key.gen; W.inAResetEpoch = key.epoch;
        W.inHasAPixels = false; W.hasIn = true;
    }
    if (W.a3WakeEvent) SetEvent(W.a3WakeEvent);
    W.cv.notify_one();
}
static void Compose(ID3D12GraphicsCommandList* cl, Input& p, Answer& a, F9Id::ARecord& f9, bool bypass) {
    auto* ans = p.localAnswer.Get(); auto* color = p.captured[0].Get();
    Barrier(cl, a.bufA.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cl, ans, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyBufferRegion(ans, 0, a.bufA.Get(), 0, Bytes);
    if (bypass) {
        auto* img = p.answerImage.Get();
        Barrier(cl, img, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        const auto src = A2In::Foot(a.bufA.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        const auto dst = A2In::Tex(img);
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(cl, img, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    Barrier(cl, a.bufA.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, ans, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    f9.Tap(F9Id::AAnswer, ans, a.input.key.frame, a.input.key.frame, F9Id::CopyOnly, Audit(a));
    f9.AnswerPath(F9Id::GpuResidual);
    Bind(cl, p);
    Barrier(cl, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, p.fed.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, p.composed.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_curJitX = p.desc.jitterOffset.x; g_curJitY = p.desc.jitterOffset.y;
    p.presentPso[0] = g_psoResid;
    const auto shape = color->GetDesc();
    SetPass(cl, p.presentPso[0].Get(), Base, bypass ? AnswerImage : Base, Base, Fed, LocalAnswer,
            ComposeUav, ComposeUav, UINT(shape.Width), shape.Height, p.heap.Get(), p.rs.Get(), bypass);
    Barrier(cl, p.composed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, p.fed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, ans, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (bypass) Barrier(cl, p.answerImage.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
}
static bool CanAdd4k(Input& p, ID3D12Resource* out) {
    if (!g_resid4k || !g_psoAdd4K) return false;
    const auto shape = out->GetDesc();
    if (!p.add4k || !SameShape(p.add4k->GetDesc(), shape)) {
        auto d = shape; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        p.add4k = FromDesc(d);
    }
    if (!p.add4k) return false;
    p.presentPso[1] = g_psoAdd4K;
    Srv(p, OutSrc, out); Uav(p, Add4kUav, p.add4k.Get());
    return true;
}
static void Add4k(ID3D12GraphicsCommandList* cl, Input& p, ID3D12Resource* out, D3D12_RESOURCE_STATES st) {
    auto* base = p.captured[0].Get();
    auto* pre = p.composed.Get(); auto* dst = p.add4k.Get();
    Bind(cl, p);
    Barrier(cl, base, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, pre, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, out, st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto d = out->GetDesc();
    SetPass(cl, p.presentPso[1].Get(), OutSrc, ComposeSrc, Base, Fed, LocalAnswer,
            Add4kUav, Add4kUav, UINT(d.Width), d.Height, p.heap.Get(), p.rs.Get());
    Barrier(cl, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cl, out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(out, dst);
    Barrier(cl, out, D3D12_RESOURCE_STATE_COPY_DEST, st);
    Barrier(cl, dst, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, pre, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, base, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
}
static void CopyOutput(ID3D12GraphicsCommandList* cl, ID3D12Resource* out, ID3D12Resource* cached,
                       D3D12_RESOURCE_STATES state, bool toCache) {
    auto* src = toCache ? out : cached; auto* dst = toCache ? cached : out;
    const auto ss = toCache ? state : D3D12_RESOURCE_STATE_COMMON;
    const auto ds = toCache ? D3D12_RESOURCE_STATE_COMMON : state;
    Barrier(cl, src, ss, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cl, dst, ds, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(dst, src);
    Barrier(cl, dst, D3D12_RESOURCE_STATE_COPY_DEST, ds);
    Barrier(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, ss);
}
static int Render(void* ctx, ID3D12GraphicsCommandList* cl, const AdapterFrameDesc& d,
                  int(*real)(void*, void*, const void*), const void* params, Key key,
                  A2In::Producer& producer, F9Id::ARecord& f9) {
    auto* out = static_cast<ID3D12Resource*>(d.output.resource);
    if (d.hasUnsupportedExt || !d.output.valid || !out || !Surface(out->GetDesc())) {
        // Unknown extension pointers can carry frame-local payloads. Never splice
        // them onto an older frame; native forwards the entire original chain.
        static bool logged = false;
        if (!logged) { logged = true; Log("[A3] unsupported Upscale shape/state/extension: native only (ext=%d valid=%d state=%x)", (int)d.hasUnsupportedExt, (int)d.output.valid, (unsigned)d.output.state); }
        StopDisplay(key.frame);
        return real ? real(ctx, cl, params) : d.errCode;
    }
    const auto outputState = d.output.state;
    const auto context = d.contextToken;
    if (lastContext != context) { StopDisplay(key.frame); lastContext = context; }
    everActive = true;
    bool ringReady;
    {
        std::lock_guard lk(m);
        Reap(key, key.frame);
        ringReady = A2In::initialized.load() && !A2In::broken.load() && InitAnswerResources();
    }
    ProcessCache(key);
    if (cache && !SameShape(cache->GetDesc(), out->GetDesc())) {
        // Resource reconstruction invalidates the cached surface and re-arms warning.
        cache.Reset(); cacheAge.Clear(); cacheKey.frame = -1;
    }
    if (ringReady) Capture(cl, d, producer, key, f9);
    std::shared_ptr<A2In::Tail> tail;
    Answer* a = nullptr;
    int prior = -1;
    if (ringReady) {
        std::lock_guard lk(m);
        a = Pick();
        if (a) {
            auto& p = inputs[a->input.slot - A2In::slots];
            const auto& s = p.desc;
            if (s.renderSize.width != d.renderSize.width || s.renderSize.height != d.renderSize.height ||
                s.upscaleSize.width != d.upscaleSize.width || s.upscaleSize.height != d.upscaleSize.height) {
                Trace("A3_A_SIZE_DROP", *a, key.frame); ReleaseInput(a->input); a->Clear(); a = nullptr;
            }
        }
        if (a) if (auto dst = CacheTarget(out)) tail = A2In::NewTail(producer.route, cl, dst.Get(), out);
        if (a && tail) {
            a->state = Answer::Reading; a->tail = tail;
            prior = lastRecorded; lastRecorded = a->input.key.frame;
            Trace("A3_A_READ", *a, key.frame);
        } else a = nullptr;
    }
    int rc;
    const auto action = ChooseOutput(true, a != nullptr, bool(cache));
    if (action == Output::NewNr) {
        auto& p = inputs[a->input.slot - A2In::slots];
        const bool add4k = CanAdd4k(p, out);
        const bool bypass = g_bypass && p.answerImage;
        Compose(cl, p, *a, f9, bypass);
        // Original FFX reset and A1 NGX reset are different. Verify this FSR
        // history break on the live runtime and with FG; it is not an A1 event.
        const auto f = MakeDispatch(p.desc, d, add4k ? nullptr : p.composed.Get(),
            prior + 1 != a->input.key.frame || prior < 0 || lastNativeDispatch >= 0);
        Trace("A3_FSR_SAME_F", *a, key.frame);
        rc = AdapterDispatchA3(ctx, &f);
        if (rc == d.okCode) {
            f9.AnswerFsrFrame(a->input.key.frame);
            lastNativeDispatch = -1;
            if (add4k) Add4k(cl, p, out, outputState);
            CopyOutput(cl, out, tail->cache.Get(), outputState, true);
            candidates.push_back({a->input.key, tail});
        }
        A2In::SealTail(tail);
    } else if (action == Output::Cache) {
        // Neither normal reuse nor overrun advances FSR history or refreshes the timer.
        const auto now = GetTickCount64();
        const auto age = cacheAge.Elapsed(now);
        if (cacheAge.Report(now)) {
            Log("[A3-CACHE-OVERRUN] age_ms=%llu gen=%d epoch=%llu last_f=%d; retaining completed NR", age, cacheKey.gen, cacheKey.epoch, cacheKey.frame);
        }
        tail = A2In::NewTail(producer.route, cl, cache.Get(), out);
        if (tail) {
            CopyOutput(cl, out, cache.Get(), outputState, false);
            A2In::SealTail(tail);
            BtTrace("A3_A_CACHE_COPY", cacheKey.frame, "a3", "dispatch=%d epoch=%llu age_ms=%llu", key.frame, cacheKey.epoch, age);
            rc = d.okCode;
        } else {
            Log("[A3] cannot prove cache playback submission; output not reported successful");
            rc = d.errCode;
        }
    } else {
        lastNativeDispatch = key.frame;
        rc = real ? real(ctx, cl, params) : d.errCode;
    }
    if (ringReady) Notify(key);
    if (rc == d.okCode) {
        ProxyImGuiDraw(cl, out, !g_resid4k, outputState);
        MirrorTick(cl, out, outputState);
    }
    if ((g_frame % 300) == 0) BwPollFile();
    BwTick(); A2In::Stats(key.frame);
    return rc;
}
}
