#pragma once
#include <d3d12.h>
#include <cstdint>

struct AdapterResource {
    void* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool valid = true;
};

struct AdapterExtent {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct AdapterFloat2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct AdapterFrameDesc {
    void* contextToken = nullptr;
    void* commandList = nullptr;
    bool hasUnsupportedExt = false;
    AdapterResource color;
    AdapterResource motionVectors;
    AdapterResource depth;
    AdapterResource exposure;
    AdapterResource reactive;
    AdapterResource transparencyAndComposition;
    AdapterResource output;
    AdapterExtent renderSize;
    AdapterExtent upscaleSize;
    AdapterFloat2 jitterOffset;
    bool reset = false;
    bool colorComposed = false;
    uint32_t createFlags = 0;
    bool flagsKnown = false;
    bool isHdr = false;
    bool autoExposure = false;
    bool isPq = false;
    bool depthInverted = true;
    float preExposure = 1.0f;
    float exposureScale = 1.0f;
    int okCode = 0;
    int errCode = -1;
    uint32_t nativeSize = 0;
    alignas(16) uint8_t nativeStorage[1024]{};
};

extern "C" int AdapterDispatchA3(void* ctx, const AdapterFrameDesc* d);
