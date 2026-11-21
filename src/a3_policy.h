#pragma once
#include "adapter_frame_desc.h"

namespace A3 {
// Render-thread decisions; a ready cache is backed by the completed batch, not
// just by an FSR return code. A repeat never calls Publish().
enum class Output { Native, NewNr, Cache };
inline Output ChooseOutput(bool gate, bool newReady, bool completedCache) {
    return !gate ? Output::Native : newReady ? Output::NewNr : completedCache ? Output::Cache : Output::Native;
}
struct CacheAge {
    static constexpr uint64_t HoldMs = 1500;
    uint64_t tick = 0;
    bool warned = false;
    void Clear() { tick = 0; warned = false; }
    void Publish(uint64_t now) { tick = now; warned = false; }
    uint64_t Elapsed(uint64_t now) const { return now - tick; }
    bool Report(uint64_t now) {
        if (warned || Elapsed(now) <= HoldMs) return false;
        warned = true; return true;
    }
};
// Only the invocation-owned fields come from now. All texture descriptions,
// temporal scalars and size/MV parameters originate from the selected input.
inline AdapterFrameDesc MakeDispatch(const AdapterFrameDesc& captured,
                                     const AdapterFrameDesc& now,
                                     void* composed, bool discontinuity) {
    auto d = captured;
    d.contextToken = now.contextToken;
    d.commandList = now.commandList;
    d.hasUnsupportedExt = now.hasUnsupportedExt;
    d.output = now.output;
    d.okCode = now.okCode;
    d.errCode = now.errCode;
    if (composed) {
        d.color.resource = composed;
        d.color.state = D3D12_RESOURCE_STATE_COMMON;
        d.color.valid = true;
        d.colorComposed = true;
    } else {
        d.colorComposed = false;
    }
    d.reset = d.reset || discontinuity;
    return d;
}
}
