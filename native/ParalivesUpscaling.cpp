// ParalivesUpscaling - native render-thread side.
//
// Runs AMD FSR (3.1 or 4, through the FidelityFX API loader) on Unity's own D3D12 device.
// FSR records into a command list of ours, handed to Unity with IUnityGraphicsD3D12v6::ExecuteCommandList
// together with the resource states FSR expects, so Unity inserts the barriers and keeps tracking states.
// Before FSR, a small compute pass repairs URP's motion vectors where the depth prepass missed geometry
// (the game's characters), using the camera's real depth buffer.
// Writes ParalivesUpscaling.log next to this DLL.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "Unity/IUnityGraphics.h"
#include "Unity/IUnityGraphicsD3D12.h"
#include "ffx/api/include/ffx_api.h"
#include "ffx/api/include/ffx_api_types.h"
#include "ffx/api/include/dx12/ffx_api_dx12.h"
#include "ffx/upscalers/include/ffx_upscale.h"
#include "xess/inc/xess_d3d12.h"
#include "dlss/include/nvsdk_ngx_helpers_d3d.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "dlss/lib/nvsdk_ngx_s.lib")
#pragma comment(lib, "advapi32.lib")  // used by the NGX static lib
#pragma comment(lib, "user32.lib")

#define EXPORT extern "C" UNITY_INTERFACE_EXPORT

// ---- shared with C# (Native.cs) ----
struct FrameData {
    ID3D12Resource *Color, *Depth, *Motion, *Output;
    int RenderW, RenderH, OutW, OutH;
    float JitterX, JitterY, MvScaleX, MvScaleY;
    float FrameTimeMs, Near, Far, FovY;
    float Sharpness;
    int Reset;
    int CreateFlags, Version;  // Version: 0 = driver/SDK default, 3 = FSR 3.x, 4 = FSR 4.x, 10 = Intel XeSS, 20 = NVIDIA DLSS
    int Gen, DispatchFlags;    // DispatchFlags: FfxApiDispatchFsrUpscaleFlags
    // Motion-vector repair: Depth = URP depth texture (prepass), DepthAtt = camera depth attachment,
    // Motion = URP motion vectors, MotionOut = repaired vectors handed to FSR.
    ID3D12Resource *DepthAtt, *MotionOut;
    float MvThreshold;
    int MvMode;                // 0 = repair where depths disagree, 1 = recompute everywhere (DebugView calibration)
    float InvViewProj[16];     // current (jittered) GPU view-projection, inverted; Unity column-major
    float PrevViewProj[16];    // previous frame's (jittered) GPU view-projection
    ID3D12Resource *Reactive;  // R8 reactive mask written by the repair pass (characters)
    float ReactiveStrength;    // applied with FSR 3.x only
    int Pad;
};
enum { EvCreate = 1, EvDispatch = 2, EvDestroy = 3 };
enum { StState = 0, StDispatches, StDispatchesSinceCreate, StCreatedGen, StDispatchFailures, StCount };
// StState: 0 not ready, 1 FFX + D3D12 ready, 2 context ready, -1 fatal (no FFX/D3D12), -2 context creation failed

static volatile LONG s_status[StCount];
static char s_versionName[128], s_lastError[512];
static char s_fsr3Name[32], s_fsr4Name[32], s_xessName[32], s_dlssName[32];  // what this GPU/driver offers, for the menu

// ---- logging ----
static wchar_t s_dir[MAX_PATH];
static void Log(const char *fmt, ...)
{
    char line[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%sParalivesUpscaling.log", s_dir);
    SYSTEMTIME t; GetLocalTime(&t);
    if (FILE *f = _wfopen(path, L"a")) { fprintf(f, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line); fclose(f); }
}
static void Fail(int state, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(s_lastError, sizeof s_lastError, fmt, ap); va_end(ap);
    Log("ERROR: %s", s_lastError);
    InterlockedExchange(&s_status[StState], state);
}
static void FFX_MsgCallback(uint32_t type, const wchar_t *msg)
{
    char buf[1024]; WideCharToMultiByte(CP_UTF8, 0, msg, -1, buf, sizeof buf, nullptr, nullptr);
    Log("FFX %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

// ---- Unity ----
static IUnityInterfaces *s_ifs;
static IUnityGraphics *s_gfx;
static IUnityGraphicsD3D12v6 *s_d3d;
static ID3D12Device *s_dev;

// ---- FidelityFX ----
static PfnFfxCreateContext pCreate;
static PfnFfxDestroyContext pDestroy;
static PfnFfxQuery pQuery;
static PfnFfxDispatch pDispatch;
static ffxContext s_ctx;

// ---- Intel XeSS (second backend; libxess.dll next to this DLL, loaded on first use) ----
static const int kVersionXess = 10;
static HMODULE s_xessLib;
static bool s_xessTried;
static decltype(&xessD3D12CreateContext) pXCreate;
static decltype(&xessD3D12BuildPipelines) pXBuild;
static decltype(&xessD3D12Init) pXInit;
static decltype(&xessD3D12Execute) pXExecute;
static decltype(&xessDestroyContext) pXDestroy;
static decltype(&xessSetVelocityScale) pXVelocityScale;
static decltype(&xessGetVersion) pXVersion;
static xess_context_handle_t s_xctx;  // set instead of s_ctx while XeSS is the active upscaler

// ---- NVIDIA DLSS (third backend; NGX core ships with the NVIDIA driver, nvngx_dlss.dll sits next to this DLL) ----
static const int kVersionDlss = 20;
static bool s_ngxReady;                   // NGX initialised and DLSS available on this GPU/driver
static NVSDK_NGX_Parameter *s_ngxParams;
static NVSDK_NGX_Handle *s_dlss;          // set instead of s_ctx while DLSS is the active upscaler

// Contexts replaced while the GPU may still use them; destroyed once the frame fence passes.
struct Retired { ffxContext ctx; xess_context_handle_t xctx; NVSDK_NGX_Handle *dlss; UINT64 fence; };
static Retired s_retired[8];

// ---- our command list (ring of allocators, one per frame in flight) ----
static const int kFrames = 3;
static ID3D12CommandAllocator *s_alloc[kFrames];
static UINT64 s_allocFence[kFrames], s_lastFence;
static int s_allocIdx;
static ID3D12GraphicsCommandList *s_list;
static HANDLE s_fenceEvent;

// ---- motion-vector repair pass ----
// URP builds its depth texture with a depth prepass, which skips shaders without a DepthOnly pass (the game's
// characters); its camera motion vectors come from that texture. Where the real depth attachment is closer
// than the prepass depth, recompute the camera motion vector from the real depth; elsewhere keep URP's
// (which may be per-object). Same convention as URP: (current - previous) * 0.5 in texture space.
// Those pixels (the characters) still have no per-object motion, so they also get a reactive-mask value.
static const char *kMotionFixHlsl = R"(
Texture2D<float> DepthTex : register(t0);
Texture2D<float> DepthAtt : register(t1);
Texture2D<float2> MotionIn : register(t2);
RWTexture2D<float2> MotionOut : register(u0);
RWTexture2D<float> ReactiveOut : register(u1);
cbuffer C : register(b0)
{
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4 Size;    // w, h, 1/w, 1/h
    float4 Params;  // x = depth threshold, y = mode (1 = recompute everywhere), z = reactive strength
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;
    float dt = DepthTex.Load(int3(id.xy, 0));
    float da = DepthAtt.Load(int3(id.xy, 0));
    float2 mv = MotionIn.Load(int3(id.xy, 0));
    bool missed = abs(da - dt) > Params.x;
    ReactiveOut[id.xy] = missed ? Params.z : 0.0;
    if (Params.y > 0.5 || missed)
    {
        float2 ndc = float2((id.x + 0.5) * Size.z * 2.0 - 1.0, 1.0 - (id.y + 0.5) * Size.w * 2.0);
        float4 world = mul(InvViewProj, float4(ndc, da, 1.0));
        world.xyz /= world.w;
        float4 prev = mul(PrevViewProj, float4(world.xyz, 1.0));
        mv = 0;
        if (prev.w > 1e-6)
        {
            float2 pn = prev.xy / prev.w;
            mv = float2((ndc.x - pn.x) * 0.5, (pn.y - ndc.y) * 0.5);
        }
    }
    MotionOut[id.xy] = mv;
}
)";
static const UINT kMvDescriptors = 5;  // t0, t1, t2, u0, u1
static ID3D12RootSignature *s_mvRootSig;
static ID3D12PipelineState *s_mvPso;
static ID3D12DescriptorHeap *s_mvHeap;  // kFrames * kMvDescriptors, shader visible
static UINT s_mvInc;

static bool InitMotionFix()
{
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(kMotionFixHlsl, strlen(kMotionFixHlsl), "ParalivesUpscaling_MotionFix", nullptr, nullptr, "main", "cs_5_1",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) { Log("motion fix: compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 3; ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 2; ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 40;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2; rs.pParameters = params;
    ID3DBlob *rsBlob = nullptr;
    hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &err);
    if (SUCCEEDED(hr)) hr = s_dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&s_mvRootSig));
    if (rsBlob) rsBlob->Release();
    if (err) { err->Release(); err = nullptr; }
    if (FAILED(hr)) { code->Release(); Log("motion fix: root signature failed 0x%08X", (unsigned)hr); return false; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = s_mvRootSig;
    pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    hr = s_dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&s_mvPso));
    code->Release();
    if (FAILED(hr)) { Log("motion fix: PSO failed 0x%08X", (unsigned)hr); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kFrames * kMvDescriptors;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = s_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_mvHeap));
    if (FAILED(hr)) { s_mvPso->Release(); s_mvPso = nullptr; Log("motion fix: descriptor heap failed 0x%08X", (unsigned)hr); return false; }
    s_mvInc = s_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Log("motion fix ready");
    return true;
}

static void RecordMotionFix(const FrameData *f, int slot, float reactive)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = s_mvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = s_mvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)slot * kMvDescriptors * s_mvInc;
    gpu.ptr += (UINT64)slot * kMvDescriptors * s_mvInc;

    ID3D12Resource *srvs[3] = { f->Depth, f->DepthAtt, f->Motion };
    DXGI_FORMAT fmts[3] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT };
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
    for (int i = 0; i < 3; i++, h.ptr += s_mvInc) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = fmts[i];
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        s_dev->CreateShaderResourceView(srvs[i], &sd, h);
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R16G16_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    s_dev->CreateUnorderedAccessView(f->MotionOut, nullptr, &ud, h);
    h.ptr += s_mvInc;
    ud.Format = DXGI_FORMAT_R8_UNORM;
    s_dev->CreateUnorderedAccessView(f->Reactive, nullptr, &ud, h);

    float consts[40];
    memcpy(consts, f->InvViewProj, 64);
    memcpy(consts + 16, f->PrevViewProj, 64);
    float size[8] = { (float)f->RenderW, (float)f->RenderH, 1.0f / f->RenderW, 1.0f / f->RenderH, f->MvThreshold, (float)f->MvMode, reactive, 0 };
    memcpy(consts + 32, size, 32);

    s_list->SetComputeRootSignature(s_mvRootSig);
    s_list->SetDescriptorHeaps(1, &s_mvHeap);
    s_list->SetPipelineState(s_mvPso);
    s_list->SetComputeRoot32BitConstants(0, 40, consts, 0);
    s_list->SetComputeRootDescriptorTable(1, gpu);
    s_list->Dispatch((UINT)(f->RenderW + 7) / 8, (UINT)(f->RenderH + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER b[2] = {};
    for (int k = 0; k < 2; k++) {
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.pResource = k == 0 ? f->MotionOut : f->Reactive;
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[k].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b[k].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    s_list->ResourceBarrier(2, b);
}

static bool FenceDone(UINT64 v) { return v == 0 || s_d3d->GetFrameFence()->GetCompletedValue() >= v; }

static void WaitFence(UINT64 v)
{
    if (FenceDone(v)) return;
    s_d3d->GetFrameFence()->SetEventOnCompletion(v, s_fenceEvent);
    // ponytail: bounded wait; a value from 3 frames ago is normally long done.
    if (WaitForSingleObject(s_fenceEvent, 2000) == WAIT_TIMEOUT) Log("warning: frame fence %llu wait timed out", v);
}

static void DestroyNow(ffxContext &ctx, xess_context_handle_t &xctx, NVSDK_NGX_Handle *&dlss)
{
    if (ctx) { pDestroy(&ctx, nullptr); ctx = nullptr; }
    if (xctx) { pXDestroy(xctx); xctx = nullptr; }
    if (dlss) { NVSDK_NGX_D3D12_ReleaseFeature(dlss); dlss = nullptr; }
}

static void CollectRetired(bool force)
{
    for (Retired &r : s_retired) {
        if ((!r.ctx && !r.xctx && !r.dlss) || (!force && !FenceDone(r.fence))) continue;
        if (force) WaitFence(r.fence);
        DestroyNow(r.ctx, r.xctx, r.dlss);
    }
}

static void RetireCurrent()
{
    if (!s_ctx && !s_xctx && !s_dlss) return;
    for (Retired &r : s_retired)
        if (!r.ctx && !r.xctx && !r.dlss) {
            r.ctx = s_ctx; r.xctx = s_xctx; r.dlss = s_dlss; r.fence = s_lastFence;
            s_ctx = nullptr; s_xctx = nullptr; s_dlss = nullptr;
            return;
        }
    WaitFence(s_lastFence);  // all slots busy: just wait
    DestroyNow(s_ctx, s_xctx, s_dlss);
}

// NGX needs an NVIDIA RTX GPU and driver; anywhere else this just reports DLSS as unavailable.
static void InitDlss()
{
    const wchar_t *paths[] = { s_dir };
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("5f3b2c9e-7a41-4d8b-9c6e-2a1f0e8d7b34", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                                             s_dir, s_dev, &info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) { Log("DLSS: not available (NGX init 0x%08X; needs an NVIDIA RTX GPU)", (unsigned)r); return; }

    NVSDK_NGX_Parameter *caps = nullptr;
    int available = 0, needsDriver = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) && caps) {
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        NVSDK_NGX_D3D12_DestroyParameters(caps);
    }
    if (!available || NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&s_ngxParams))) {
        Log("DLSS: not available on this GPU/driver (available %d, needs newer driver %d)", available, needsDriver);
        s_ngxParams = nullptr;
        NVSDK_NGX_D3D12_Shutdown1(s_dev);
        return;
    }
    s_ngxReady = true;

    // the DLSS version is the one of the nvngx_dlss.dll we ship
    wchar_t dll[MAX_PATH];
    swprintf(dll, MAX_PATH, L"%snvngx_dlss.dll", s_dir);
    DWORD size = GetFileVersionInfoSizeW(dll, nullptr);
    BYTE buf[4096];
    VS_FIXEDFILEINFO *fi = nullptr; UINT len = 0;
    if (size && size <= sizeof buf && GetFileVersionInfoW(dll, 0, sizeof buf, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &len) && fi)
        snprintf(s_dlssName, sizeof s_dlssName, "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
    else
        snprintf(s_dlssName, sizeof s_dlssName, "%s", "SR");
    Log("available upscaler: DLSS %s", s_dlssName);
}

// Our scale ratios match DLSS's presets (DLAA 1.0, Quality 1.5, Balanced 1.72, Performance 2.0, Ultra Performance 3.0).
static bool CreateDlss(const FrameData *f)
{
    if (!s_ngxReady) return false;
    float ratio = (float)f->OutW / (float)f->RenderW;
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = (unsigned)f->RenderW;
    cp.Feature.InHeight = (unsigned)f->RenderH;
    cp.Feature.InTargetWidth = (unsigned)f->OutW;
    cp.Feature.InTargetHeight = (unsigned)f->OutH;
    cp.Feature.InPerfQualityValue = ratio < 1.05f ? NVSDK_NGX_PerfQuality_Value_DLAA
                                  : ratio < 1.6f  ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                  : ratio < 1.85f ? NVSDK_NGX_PerfQuality_Value_Balanced
                                  : ratio < 2.5f  ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                                  : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (f->CreateFlags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) cp.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (f->CreateFlags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) cp.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (f->CreateFlags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) cp.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;

    // feature creation records commands: use our own list, like a dispatch
    int i = s_allocIdx = (s_allocIdx + 1) % kFrames;
    WaitFence(s_allocFence[i]);
    s_alloc[i]->Reset();
    s_list->Reset(s_alloc[i], nullptr);
    NVSDK_NGX_Handle *h = nullptr;
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(s_list, 1, 1, &h, s_ngxParams, &cp);
    s_list->Close();
    if (NVSDK_NGX_FAILED(r) || !h) { Log("DLSS: feature creation failed (0x%08X)", (unsigned)r); return false; }
    s_lastFence = s_allocFence[i] = s_d3d->ExecuteCommandList(s_list, 0, nullptr);

    s_dlss = h;
    snprintf(s_versionName, sizeof s_versionName, "DLSS %s", s_dlssName);
    Log("context created: %s, %dx%d -> %dx%d, quality %d, flags 0x%X", s_versionName, f->RenderW, f->RenderH, f->OutW, f->OutH,
        (int)cp.Feature.InPerfQualityValue, cp.InFeatureCreateFlags);
    return true;
}

static bool LoadXess()
{
    if (s_xessTried) return s_xessLib != nullptr;
    s_xessTried = true;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%slibxess.dll", s_dir);
    s_xessLib = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!s_xessLib) { Log("XeSS: libxess.dll not found next to ParalivesUpscaling.dll (%lu)", GetLastError()); return false; }
    pXCreate = (decltype(pXCreate))GetProcAddress(s_xessLib, "xessD3D12CreateContext");
    pXBuild = (decltype(pXBuild))GetProcAddress(s_xessLib, "xessD3D12BuildPipelines");
    pXInit = (decltype(pXInit))GetProcAddress(s_xessLib, "xessD3D12Init");
    pXExecute = (decltype(pXExecute))GetProcAddress(s_xessLib, "xessD3D12Execute");
    pXDestroy = (decltype(pXDestroy))GetProcAddress(s_xessLib, "xessDestroyContext");
    pXVelocityScale = (decltype(pXVelocityScale))GetProcAddress(s_xessLib, "xessSetVelocityScale");
    pXVersion = (decltype(pXVersion))GetProcAddress(s_xessLib, "xessGetVersion");
    if (!pXCreate || !pXBuild || !pXInit || !pXExecute || !pXDestroy || !pXVelocityScale || !pXVersion) {
        Log("XeSS: libxess.dll is missing exports");
        FreeLibrary(s_xessLib);
        s_xessLib = nullptr;
        return false;
    }
    return true;
}

// Same inputs and conventions as FSR (pixel motion vectors, current -> previous; jitter in [-0.5, 0.5]).
// Our scale ratios match XeSS's own presets, so the preset only tells XeSS what to expect.
static bool CreateXess(const FrameData *f)
{
    if (!LoadXess()) return false;
    xess_context_handle_t h = nullptr;
    xess_result_t r = pXCreate(s_dev, &h);
    if (r != XESS_RESULT_SUCCESS || !h) { Log("XeSS: context creation failed (%d)", (int)r); return false; }

    uint32_t flags = 0;
    if (f->CreateFlags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) flags |= XESS_INIT_FLAG_INVERTED_DEPTH;
    if (f->CreateFlags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) flags |= XESS_INIT_FLAG_JITTERED_MV;
    if (!(f->CreateFlags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE)) flags |= XESS_INIT_FLAG_LDR_INPUT_COLOR;
    if (s_mvPso) flags |= XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;

    float ratio = (float)f->OutW / (float)f->RenderW;
    xess_quality_settings_t q = ratio < 1.05f ? XESS_QUALITY_SETTING_AA
                              : ratio < 1.6f  ? XESS_QUALITY_SETTING_ULTRA_QUALITY
                              : ratio < 1.85f ? XESS_QUALITY_SETTING_QUALITY
                              : ratio < 2.5f  ? XESS_QUALITY_SETTING_BALANCED
                                              : XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;

    // ponytail: blocking kernel build = a one-off hitch when XeSS is switched on; pass a pipeline library if it matters
    r = pXBuild(h, nullptr, true, flags);
    if (r != XESS_RESULT_SUCCESS) Log("XeSS: pipeline build returned %d", (int)r);
    xess_d3d12_init_params_t ip = {};
    ip.outputResolution = { (uint32_t)f->OutW, (uint32_t)f->OutH };
    ip.qualitySetting = q;
    ip.initFlags = flags;
    r = pXInit(h, &ip);
    if (r != XESS_RESULT_SUCCESS) { Log("XeSS: init failed (%d)", (int)r); pXDestroy(h); return false; }

    s_xctx = h;
    xess_version_t v = {};
    pXVersion(&v);
    snprintf(s_versionName, sizeof s_versionName, "XeSS %u.%u.%u", v.major, v.minor, v.patch);
    Log("context created: %s, %dx%d -> %dx%d, preset %d, flags 0x%X", s_versionName, f->RenderW, f->RenderH, f->OutW, f->OutH, (int)q, flags);
    return true;
}

// Returns the version id whose display name starts with the wanted major version, or 0 (= default).
static uint64_t PickVersion(int major)
{
    uint64_t count = 0;
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = s_dev;
    q.outputCount = &count;
    if (pQuery(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) { Log("no upscaler versions reported"); return 0; }
    uint64_t ids[16] = {}; const char *names[16] = {};
    if (count > 16) count = 16;
    q.versionIds = ids; q.versionNames = names;
    pQuery(nullptr, &q.header);
    uint64_t pick = 0;
    for (uint64_t i = 0; i < count; i++) {
        const char *n = names[i] ? names[i] : "?";
        Log("available upscaler: %s (id 0x%llX)", n, ids[i]);
        const char *d = n; while (*d && (*d < '0' || *d > '9')) d++;  // first digit = major version
        if (!pick && major && *d == '0' + major) pick = ids[i];
        if (*d == '3' && !s_fsr3Name[0]) snprintf(s_fsr3Name, sizeof s_fsr3Name, "%s", d);
        if (*d == '4' && !s_fsr4Name[0]) snprintf(s_fsr4Name, sizeof s_fsr4Name, "%s", d);
    }
    if (major && !pick) Log("FSR %d not available on this GPU/driver; using the default", major);
    return pick;
}

static void DoCreate(const FrameData *f)
{
    if (s_status[StState] < 1) return;
    CollectRetired(false);
    RetireCurrent();
    InterlockedExchange(&s_status[StCreatedGen], f->Gen);
    InterlockedExchange(&s_status[StDispatchesSinceCreate], 0);

    if (f->Version == kVersionXess) {
        if (CreateXess(f)) {
            InterlockedExchange(&s_status[StDispatchFailures], 0);
            InterlockedExchange(&s_status[StState], 2);
            return;
        }
        Log("XeSS unavailable; falling back to FSR");
    }
    if (f->Version == kVersionDlss) {
        if (CreateDlss(f)) {
            InterlockedExchange(&s_status[StDispatchFailures], 0);
            InterlockedExchange(&s_status[StState], 2);
            return;
        }
        Log("DLSS unavailable; falling back to FSR");
    }

    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = s_dev;
    ffxCreateContextDescUpscaleVersion apiVersion = {};
    apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    apiVersion.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion over = {};
    over.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    over.versionId = PickVersion(f->Version == 3 || f->Version == 4 ? f->Version : 0);

    ffxCreateContextDescUpscale desc = {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags = (uint32_t)f->CreateFlags;
    desc.maxRenderSize = { (uint32_t)f->RenderW, (uint32_t)f->RenderH };
    desc.maxUpscaleSize = { (uint32_t)f->OutW, (uint32_t)f->OutH };
    desc.fpMessage = FFX_MsgCallback;
    desc.header.pNext = &backend.header;
    backend.header.pNext = &apiVersion.header;
    if (over.versionId) apiVersion.header.pNext = &over.header;

    ffxReturnCode_t r = pCreate(&s_ctx, &desc.header, nullptr);
    if (r != FFX_API_RETURN_OK || !s_ctx) { s_ctx = nullptr; Fail(-2, "FSR context creation failed (%u)", r); return; }

    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    pQuery(&s_ctx, &pv.header);
    snprintf(s_versionName, sizeof s_versionName, "%s", pv.versionName ? pv.versionName : "unknown");
    Log("context created: %s, %dx%d -> %dx%d, flags 0x%X", s_versionName, f->RenderW, f->RenderH, f->OutW, f->OutH, f->CreateFlags);
    InterlockedExchange(&s_status[StDispatchFailures], 0);
    InterlockedExchange(&s_status[StState], 2);
}

static bool DispatchFsr(const FrameData *f, bool fix, float reactive, unsigned &code);

static void DoDispatch(const FrameData *f)
{
    if (s_status[StState] != 2 || (!s_ctx && !s_xctx && !s_dlss) || f->Gen != s_status[StCreatedGen]) return;
    CollectRetired(false);

    int i = s_allocIdx = (s_allocIdx + 1) % kFrames;
    WaitFence(s_allocFence[i]);
    s_alloc[i]->Reset();
    s_list->Reset(s_alloc[i], nullptr);

    bool fix = s_mvPso && f->DepthAtt && f->MotionOut && f->Reactive;
    // FSR 3 only: FSR 4 copes with the characters' missing object motion on its own, and XeSS reads the mask so
    // strongly that masked characters lose all history and look pixelated
    float reactive = (s_ctx && s_versionName[0] == '3') ? f->ReactiveStrength : 0.0f;
    if (fix) RecordMotionFix(f, i, reactive);

    bool ok;
    unsigned code;
    if (s_xctx) {
        pXVelocityScale(s_xctx, f->MvScaleX, f->MvScaleY);
        xess_d3d12_execute_params_t p = {};
        p.pColorTexture = f->Color;
        p.pVelocityTexture = fix ? f->MotionOut : f->Motion;
        p.pDepthTexture = fix ? f->DepthAtt : f->Depth;
        p.pResponsivePixelMaskTexture = fix ? f->Reactive : nullptr;
        p.pOutputTexture = f->Output;
        p.jitterOffsetX = f->JitterX;
        p.jitterOffsetY = f->JitterY;
        p.exposureScale = 1.0f;
        p.resetHistory = f->Reset ? 1u : 0u;
        p.inputWidth = (uint32_t)f->RenderW;
        p.inputHeight = (uint32_t)f->RenderH;
        xess_result_t r = pXExecute(s_xctx, s_list, &p);
        ok = r == XESS_RESULT_SUCCESS;
        code = (unsigned)r;
    } else if (s_dlss) {
        NVSDK_NGX_D3D12_DLSS_Eval_Params e = {};
        e.Feature.pInColor = f->Color;
        e.Feature.pInOutput = f->Output;
        e.pInDepth = fix ? f->DepthAtt : f->Depth;
        e.pInMotionVectors = fix ? f->MotionOut : f->Motion;
        e.InJitterOffsetX = f->JitterX;
        e.InJitterOffsetY = f->JitterY;
        e.InRenderSubrectDimensions = { (unsigned)f->RenderW, (unsigned)f->RenderH };
        e.InReset = f->Reset;
        e.InMVScaleX = f->MvScaleX;
        e.InMVScaleY = f->MvScaleY;
        e.InPreExposure = 1.0f;
        e.InExposureScale = 1.0f;
        e.InFrameTimeDeltaInMsec = f->FrameTimeMs;
        NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(s_list, s_dlss, s_ngxParams, &e);
        ok = NVSDK_NGX_SUCCEED(r);
        code = (unsigned)r;
    } else {
        ok = DispatchFsr(f, fix, reactive, code);
    }
    s_list->Close();
    if (!ok) {
        LONG n = InterlockedIncrement(&s_status[StDispatchFailures]);
        if (n <= 3) Log("dispatch failed (%d)", (int)code);
        if (n >= 10) Fail(-2, "%s keeps failing (%d)", s_xctx ? "XeSS" : s_dlss ? "DLSS" : "FSR", (int)code);
        return;
    }

    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    UnityGraphicsD3D12ResourceState states[7] = {
        { f->Color, read, read },
        { f->Depth, read, read },
        { f->Motion, read, read },
        { f->Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
        { f->DepthAtt, read, read },
        { f->MotionOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read },  // RecordMotionFix moves these two UAV -> read
        { f->Reactive, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read },
    };
    s_lastFence = s_allocFence[i] = s_d3d->ExecuteCommandList(s_list, fix ? 7 : 4, states);
    if (s_status[StDispatches] == 0) Log("first dispatch ok");
    InterlockedIncrement(&s_status[StDispatches]);
    InterlockedIncrement(&s_status[StDispatchesSinceCreate]);
    InterlockedExchange(&s_status[StDispatchFailures], 0);
}

// Records the FSR dispatch into s_list.
static bool DispatchFsr(const FrameData *f, bool fix, float reactive, unsigned &code)
{
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = s_list;
    d.color = ffxApiGetResourceDX12(f->Color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(fix ? f->DepthAtt : f->Depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(fix ? f->MotionOut : f->Motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.output = ffxApiGetResourceDX12(f->Output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    if (fix && reactive > 0.0f) d.reactive = ffxApiGetResourceDX12(f->Reactive, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.jitterOffset = { f->JitterX, f->JitterY };
    d.motionVectorScale = { f->MvScaleX, f->MvScaleY };
    d.renderSize = { (uint32_t)f->RenderW, (uint32_t)f->RenderH };
    d.upscaleSize = { (uint32_t)f->OutW, (uint32_t)f->OutH };
    d.enableSharpening = f->Sharpness > 0.0f;
    d.sharpness = f->Sharpness;
    d.frameTimeDelta = f->FrameTimeMs;
    d.preExposure = 1.0f;
    d.reset = f->Reset != 0;
    d.cameraNear = f->Near;
    d.cameraFar = f->Far;
    d.cameraFovAngleVertical = f->FovY;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = (uint32_t)f->DispatchFlags;

    ffxReturnCode_t r = pDispatch(&s_ctx, &d.header);
    code = r;
    return r == FFX_API_RETURN_OK;
}

static void DoDestroy()
{
    if (!pDestroy) return;
    RetireCurrent();
    LONG s = s_status[StState];
    if (s == 2 || s == -2) InterlockedExchange(&s_status[StState], 1);
}

static void UNITY_INTERFACE_API OnRenderEvent(int eventId, void *data)
{
    switch (eventId) {
        case EvCreate: DoCreate((const FrameData *)data); break;
        case EvDispatch: DoDispatch((const FrameData *)data); break;
        case EvDestroy: DoDestroy(); break;
    }
}

// ---- device lifetime ----
static bool LoadFfx()
{
    // Preload the upscaler by full path so the loader's plain LoadLibrary("amd_fidelityfx_upscaler_dx12.dll")
    // finds it even though Plugins\x86_64 is not on the DLL search path.
    wchar_t up[MAX_PATH], ld[MAX_PATH];
    swprintf(up, MAX_PATH, L"%samd_fidelityfx_upscaler_dx12.dll", s_dir);
    swprintf(ld, MAX_PATH, L"%samd_fidelityfx_loader_dx12.dll", s_dir);
    if (!LoadLibraryW(up)) { Fail(-1, "amd_fidelityfx_upscaler_dx12.dll not found next to ParalivesUpscaling.dll (%lu)", GetLastError()); return false; }
    HMODULE m = LoadLibraryW(ld);
    if (!m) { Fail(-1, "amd_fidelityfx_loader_dx12.dll not found next to ParalivesUpscaling.dll (%lu)", GetLastError()); return false; }
    pCreate = (PfnFfxCreateContext)GetProcAddress(m, "ffxCreateContext");
    pDestroy = (PfnFfxDestroyContext)GetProcAddress(m, "ffxDestroyContext");
    pQuery = (PfnFfxQuery)GetProcAddress(m, "ffxQuery");
    pDispatch = (PfnFfxDispatch)GetProcAddress(m, "ffxDispatch");
    if (!pCreate || !pDestroy || !pQuery || !pDispatch) { Fail(-1, "FidelityFX loader is missing exports"); return false; }
    return true;
}

static void InitDevice()
{
    if (s_gfx->GetRenderer() != kUnityGfxRendererD3D12) { Fail(-1, "game is not running on D3D12 (add -force-d3d12)"); return; }
    s_d3d = s_ifs->Get<IUnityGraphicsD3D12v6>();
    if (!s_d3d) { Fail(-1, "IUnityGraphicsD3D12v6 not available"); return; }
    s_dev = s_d3d->GetDevice();

    for (int i = 0; i < kFrames; i++)
        if (FAILED(s_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_alloc[i])))) { Fail(-1, "CreateCommandAllocator failed"); return; }
    if (FAILED(s_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_alloc[0], nullptr, IID_PPV_ARGS(&s_list)))) { Fail(-1, "CreateCommandList failed"); return; }
    s_list->Close();
    s_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // ponytail: flush + sync every dispatch keeps ordering simple; drop flags if profiling shows a cost.
    UnityD3D12PluginEventConfig cfg = {};
    cfg.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_DontCare;
    cfg.flags = kUnityD3D12EventConfigFlag_FlushCommandBuffers | kUnityD3D12EventConfigFlag_SyncWorkerThreads;
    cfg.ensureActiveRenderTextureIsBound = false;
    s_d3d->ConfigureEvent(EvDispatch, &cfg);

    if (!InitMotionFix()) Log("motion fix unavailable; FSR gets URP's depth texture and motion vectors as they are");
    if (!LoadFfx()) return;
    PickVersion(0);  // log what this GPU/driver offers (and remember the FSR 3 / 4 names for the menu)
    if (LoadXess()) {
        xess_version_t v = {};
        if (pXVersion(&v) == XESS_RESULT_SUCCESS) snprintf(s_xessName, sizeof s_xessName, "%u.%u.%u", v.major, v.minor, v.patch);
        Log("available upscaler: XeSS %s", s_xessName);
    }
    InitDlss();
    Log("ready (device %p)", (void *)s_dev);
    InterlockedExchange(&s_status[StState], 1);
}

static void ShutdownDevice()
{
    if (pDestroy) {
        RetireCurrent();
        CollectRetired(true);
    }
    if (s_ngxReady) {
        if (s_ngxParams) { NVSDK_NGX_D3D12_DestroyParameters(s_ngxParams); s_ngxParams = nullptr; }
        NVSDK_NGX_D3D12_Shutdown1(s_dev);
        s_ngxReady = false;
    }
    if (s_mvPso) { s_mvPso->Release(); s_mvPso = nullptr; }
    if (s_mvRootSig) { s_mvRootSig->Release(); s_mvRootSig = nullptr; }
    if (s_mvHeap) { s_mvHeap->Release(); s_mvHeap = nullptr; }
    if (s_list) { s_list->Release(); s_list = nullptr; }
    for (auto &a : s_alloc) if (a) { a->Release(); a = nullptr; }
    if (s_fenceEvent) { CloseHandle(s_fenceEvent); s_fenceEvent = nullptr; }
    InterlockedExchange(&s_status[StState], 0);
}

static void UNITY_INTERFACE_API OnGfxEvent(UnityGfxDeviceEventType type)
{
    if (type == kUnityGfxDeviceEventInitialize) InitDevice();
    else if (type == kUnityGfxDeviceEventShutdown) ShutdownDevice();
}

EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces *ifs)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&UnityPluginLoad, &self);
    GetModuleFileNameW(self, s_dir, MAX_PATH);
    if (wchar_t *slash = wcsrchr(s_dir, L'\\')) slash[1] = 0;
    Log("---- ParalivesUpscaling native loaded");

    s_ifs = ifs;
    s_gfx = ifs->Get<IUnityGraphics>();
    s_gfx->RegisterDeviceEventCallback(OnGfxEvent);
    OnGfxEvent(kUnityGfxDeviceEventInitialize);  // loaded lazily, after the device exists
}

EXPORT void UNITY_INTERFACE_API UnityPluginUnload()
{
    if (s_gfx) s_gfx->UnregisterDeviceEventCallback(OnGfxEvent);
}

// ---- exports for C# ----
EXPORT void *UNITY_INTERFACE_API PFSR_GetRenderEventFunc() { return (void *)OnRenderEvent; }
EXPORT int UNITY_INTERFACE_API PFSR_GetStatus(int which) { return which >= 0 && which < StCount ? (int)s_status[which] : 0; }
// which: 0 = active upscaler version name, 1 = last error, 2/3 = FSR 3/4 version offered, 4 = XeSS version,
// 5 = DLSS version ("" when this GPU/driver cannot run DLSS)
EXPORT int UNITY_INTERFACE_API PFSR_GetText(int which, char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    const char *texts[] = { s_versionName, s_lastError, s_fsr3Name, s_fsr4Name, s_xessName, s_dlssName };
    snprintf(buf, (size_t)cap, "%s", which >= 0 && which < 6 ? texts[which] : "");
    return (int)strlen(buf);
}
