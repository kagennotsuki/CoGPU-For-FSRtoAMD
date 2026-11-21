// ffx_api_min.h —— 我们自己的 FFX (AMD FSR) API 最小 ABI 定义
//
// 来源：AMD FidelityFX SDK（MIT）
//   Kits/FidelityFX/api/include/ffx_api.h
//   Kits/FidelityFX/api/include/ffx_api_types.h
//   Kits/FidelityFX/upscalers/include/ffx_upscale.h   （FFX_UPSCALER_VERSION 4.1.2）
// 这里只抄我们需要的部分，字段名/顺序/取值与上游**逐字一致**（改一个字节就会崩，别动）。
//
// 你要知道的三个数（已核对上游宏）：
//   FFX_API_EFFECT_ID_UPSCALE                 = 0x00010000
//   FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE  = 0x00010000
//   FFX_API_DISPATCH_DESC_TYPE_UPSCALE        = 0x00010001
//   后端 DX12 创建描述符 type                  = 0x00000000
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- 返回码 / 基础类型
typedef uint32_t ffxReturnCode_t;
enum FfxApiReturnCodes
{
    FFX_API_RETURN_OK                                = 0,
    FFX_API_RETURN_ERROR                             = 1,
    FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE            = 2,
    FFX_API_RETURN_ERROR_RUNTIME_ERROR               = 3,
    FFX_API_RETURN_NO_PROVIDER                       = 4,
    FFX_API_RETURN_ERROR_MEMORY                      = 5,
    FFX_API_RETURN_ERROR_PARAMETER                   = 6,
    FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE   = 7,
};

typedef void*    ffxContext;
typedef uint64_t ffxStructType_t;

typedef struct ffxApiHeader
{
    ffxStructType_t      type;
    struct ffxApiHeader* pNext;
} ffxApiHeader;

typedef ffxApiHeader ffxCreateContextDescHeader;
typedef ffxApiHeader ffxConfigureDescHeader;
typedef ffxApiHeader ffxQueryDescHeader;
typedef ffxApiHeader ffxDispatchDescHeader;

typedef void (*ffxApiMessage)(uint32_t type, const wchar_t* message);

typedef void* (*ffxAlloc)(void* pUserData, uint64_t size);
typedef void  (*ffxDealloc)(void* pUserData, void* pMem);
typedef struct ffxAllocationCallbacks
{
    void*      pUserData;
    ffxAlloc   alloc;
    ffxDealloc dealloc;
} ffxAllocationCallbacks;

// ---------------------------------------------------------------- 资源描述
enum FfxApiSurfaceFormat
{
    FFX_API_SURFACE_FORMAT_UNKNOWN = 0,
    FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS, FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT,
    FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT,    FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,
    FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT,       FFX_API_SURFACE_FORMAT_R32G32_FLOAT,
    FFX_API_SURFACE_FORMAT_R8_UINT,               FFX_API_SURFACE_FORMAT_R32_UINT,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS,     FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM,        FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB,
    FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS,     FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM,
    FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB,         FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT,
    FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM,     FFX_API_SURFACE_FORMAT_R16G16_FLOAT,
    FFX_API_SURFACE_FORMAT_R16G16_UINT,           FFX_API_SURFACE_FORMAT_R16G16_SINT,
    FFX_API_SURFACE_FORMAT_R16_FLOAT,             FFX_API_SURFACE_FORMAT_R16_UINT,
    FFX_API_SURFACE_FORMAT_R16_UNORM,             FFX_API_SURFACE_FORMAT_R16_SNORM,
    FFX_API_SURFACE_FORMAT_R8_UNORM,              FFX_API_SURFACE_FORMAT_R8G8_UNORM,
    FFX_API_SURFACE_FORMAT_R8G8_UINT,             FFX_API_SURFACE_FORMAT_R32_FLOAT,
    FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP,    FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS,
    FFX_API_SURFACE_FORMAT_R32G32_TYPELESS,       FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS,
    FFX_API_SURFACE_FORMAT_R16G16_TYPELESS,       FFX_API_SURFACE_FORMAT_R16_TYPELESS,
    FFX_API_SURFACE_FORMAT_R8_TYPELESS,           FFX_API_SURFACE_FORMAT_R8G8_TYPELESS,
    FFX_API_SURFACE_FORMAT_R32_TYPELESS,          FFX_API_SURFACE_FORMAT_R32G32_UINT,
    FFX_API_SURFACE_FORMAT_R8_SNORM,
};

enum FfxApiResourceUsage
{
    FFX_API_RESOURCE_USAGE_READ_ONLY       = 0,
    FFX_API_RESOURCE_USAGE_RENDERTARGET    = (1 << 0),
    FFX_API_RESOURCE_USAGE_UAV             = (1 << 1),
    FFX_API_RESOURCE_USAGE_DEPTHTARGET     = (1 << 2),
    FFX_API_RESOURCE_USAGE_INDIRECT        = (1 << 3),
    FFX_API_RESOURCE_USAGE_ARRAYVIEW       = (1 << 4),
    FFX_API_RESOURCE_USAGE_STENCILTARGET   = (1 << 5),
    FFX_API_RESOURCE_USAGE_DCC_RENDERTARGET= (1 << 15),
};

enum FfxApiResourceState
{
    FFX_API_RESOURCE_STATE_COMMON             = (1 << 0),
    FFX_API_RESOURCE_STATE_UNORDERED_ACCESS   = (1 << 1),
    FFX_API_RESOURCE_STATE_COMPUTE_READ       = (1 << 2),
    FFX_API_RESOURCE_STATE_PIXEL_READ         = (1 << 3),
    FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ = (FFX_API_RESOURCE_STATE_PIXEL_READ | FFX_API_RESOURCE_STATE_COMPUTE_READ),
    FFX_API_RESOURCE_STATE_COPY_SRC           = (1 << 4),
    FFX_API_RESOURCE_STATE_COPY_DEST          = (1 << 5),
    FFX_API_RESOURCE_STATE_GENERIC_READ       = (FFX_API_RESOURCE_STATE_COPY_SRC | FFX_API_RESOURCE_STATE_COMPUTE_READ),
    FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT  = (1 << 6),
    FFX_API_RESOURCE_STATE_PRESENT            = (1 << 7),
    FFX_API_RESOURCE_STATE_RENDER_TARGET      = (1 << 8),
    FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT   = (1 << 9),
};

enum FfxApiResourceType
{
    FFX_API_RESOURCE_TYPE_BUFFER = 0,
    FFX_API_RESOURCE_TYPE_TEXTURE1D,
    FFX_API_RESOURCE_TYPE_TEXTURE2D,
    FFX_API_RESOURCE_TYPE_TEXTURE_CUBE,
    FFX_API_RESOURCE_TYPE_TEXTURE3D,
};

struct FfxApiDimensions2D { uint32_t width, height; };
struct FfxApiFloatCoords2D { float x, y; };

struct FfxApiResourceDescription
{
    uint32_t type;
    uint32_t format;
    union { uint32_t width;  uint32_t size; };
    union { uint32_t height; uint32_t stride; };
    union { uint32_t depth;  uint32_t alignment; };
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};

struct FfxApiResource
{
    void* resource;
    struct FfxApiResourceDescription description;
    uint32_t state;
};

typedef struct FfxApiEffectMemoryUsage
{
    uint64_t totalUsageInBytes;
    uint64_t aliasableUsageInBytes;
} FfxApiEffectMemoryUsage;

// ---------------------------------------------------------------- 描述符类型常量
#define FFX_API_EFFECT_ID_UPSCALE                     0x00010000u
#define FFX_API_EFFECT_ID_DENOISER                    0x00050000u
#define FFX_API_MAKE_EFFECT_SUB_ID(effectId, sub)     (((effectId) & 0x00ff0000u) | ((sub) & ~0x00ff0000u))

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE      FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x00)  // 0x00010000
#define FFX_API_DISPATCH_DESC_TYPE_UPSCALE            FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x01)  // 0x00010001
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x0b)
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x0a)

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 0x00000002u   // ★MAKE_BACKEND_SUB_ID(BACKEND_ID_DX12, 0x02) = 0x2（不是 0！填 0 会被当成未知描述符）
#define FFX_API_DESC_TYPE_OVERRIDE_VERSION            5u
#define FFX_API_QUERY_DESC_TYPE_GET_VERSIONS          4u
#define FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION  6u

// ---------------------------------------------------------------- 创建 / 查询 / 派发
enum FfxApiCreateContextUpscaleFlags
{
    FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE                 = (1 << 0),
    FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS  = (1 << 1),
    FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = (1 << 2),
    FFX_UPSCALE_ENABLE_DEPTH_INVERTED                     = (1 << 3),
    FFX_UPSCALE_ENABLE_DEPTH_INFINITE                     = (1 << 4),
    FFX_UPSCALE_ENABLE_AUTO_EXPOSURE                      = (1 << 5),
    FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION                 = (1 << 6),
    FFX_UPSCALE_ENABLE_DEBUG_CHECKING                     = (1 << 7),
    FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE              = (1 << 8),
    FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION                = (1 << 9),
};

enum FfxApiDispatchFsrUpscaleFlags
{
    FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW       = (1 << 0),
    FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB = (1 << 1),
    FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ   = (1 << 2),
};

struct ffxCreateBackendDX12Desc
{
    ffxCreateContextDescHeader header;   // type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 (0)
    void*                      device;   // ID3D12Device*
};

struct ffxCreateContextDescUpscale
{
    ffxCreateContextDescHeader header;          // type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE
    uint32_t                   flags;
    struct FfxApiDimensions2D  maxRenderSize;
    struct FfxApiDimensions2D  maxUpscaleSize;
    ffxApiMessage              fpMessage;
};

struct ffxCreateContextDescUpscaleVersion
{
    ffxCreateContextDescHeader header;   // type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION
    uint32_t                   version;  // FFX_UPSCALER_MAKE_VERSION(major,minor,patch) = (major<<22)|(minor<<12)|patch
};

// ★ 这张表就是我们"要什么"的全部依据（字段名与上游逐字一致）
struct ffxDispatchDescUpscale
{
    ffxDispatchDescHeader      header;                     // type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE
    void*                      commandList;                // ID3D12GraphicsCommandList*
    struct FfxApiResource      color;                      // 渲染分辨率当前帧颜色
    struct FfxApiResource      depth;                      // 渲染分辨率 32bit 深度
    struct FfxApiResource      motionVectors;              // 运动矢量
    struct FfxApiResource      exposure;                   // 可选：1x1 曝光
    struct FfxApiResource      reactive;                   // 可选：reactivity
    struct FfxApiResource      transparencyAndComposition; // 可选：alpha（FSR4 用）
    struct FfxApiResource      output;                     // 呈现分辨率输出
    struct FfxApiFloatCoords2D jitterOffset;               // 亚像素抖动
    struct FfxApiFloatCoords2D motionVectorScale;          // MV→像素空间的比例（关键：单位归一化靠它）
    struct FfxApiDimensions2D  renderSize;
    struct FfxApiDimensions2D  upscaleSize;
    bool                       enableSharpening;
    float                      sharpness;
    float                      frameTimeDelta;
    float                      preExposure;
    bool                       reset;
    float                      cameraNear;
    float                      cameraFar;
    float                      cameraFovAngleVertical;
    float                      viewSpaceToMetersFactor;
    uint32_t                   flags;
};

struct ffxQueryGetProviderVersion
{
    ffxQueryDescHeader header;       // type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION
    uint64_t           versionId;
    const char*        versionName;
};

struct ffxQueryDescGetVersions
{
    ffxQueryDescHeader header;   // type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS (4)
    uint64_t           createDescType;
    void*              device;
    uint64_t*          outputCount;
    uint64_t*          versionIds;
    const char**       versionNames;
};
struct ffxOverrideVersion
{
    ffxApiHeader header;   // type = FFX_API_DESC_TYPE_OVERRIDE_VERSION
    uint64_t     versionId;
};

// ---------------------------------------------------------------- 五个函数（我们代理要实现/转发的全部）
typedef ffxReturnCode_t (*PfnFfxCreateContext)(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* memCb);
typedef ffxReturnCode_t (*PfnFfxDestroyContext)(ffxContext* context, const ffxAllocationCallbacks* memCb);
typedef ffxReturnCode_t (*PfnFfxConfigure)(ffxContext* context, const ffxConfigureDescHeader* desc);
typedef ffxReturnCode_t (*PfnFfxQuery)(ffxContext* context, ffxQueryDescHeader* desc);
typedef ffxReturnCode_t (*PfnFfxDispatch)(ffxContext* context, const ffxDispatchDescHeader* desc);

#ifdef __cplusplus
}
#endif
