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
#include <dxgi1_6.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include "Unity/IUnityGraphics.h"
#include "Unity/IUnityGraphicsD3D12.h"
#include "ffx/api/include/ffx_api.h"
#include "ffx/api/include/ffx_api_types.h"
#include "ffx/api/include/dx12/ffx_api_dx12.h"
#include "ffx/upscalers/include/ffx_upscale.h"
#include "ffx/framegeneration/include/ffx_framegeneration.h"
#include "ffx/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "xess/inc/xess_d3d12.h"
#include "xess/inc/xefg_swapchain_d3d12.h"
#include "xess/inc/xell_d3d12.h"
#include "dlss/include/nvsdk_ngx_helpers_d3d.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
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
    float CamPos[3], CamUp[3], CamRight[3], CamFwd[3];  // world space, for frame generation
    int FrameGen, Pad2;        // FrameGen: 1 = frame generation on (with whichever proxy swapchain is in place)
    float View[16], Proj[16];  // unjittered world-to-camera and GPU projection (Unity column-major), for XeSS FG
    int FrameId, Pad3;         // Time.frameCount: XeLL frame ID and XeSS FG present ID
};
enum { EvCreate = 1, EvDispatch = 2, EvDestroy = 3, EvFrame = 4 };  // EvFrame data: the frame ID itself
enum { StState = 0, StDispatches, StDispatchesSinceCreate, StCreatedGen, StDispatchFailures, StFrameGen, StFrameGenTech, StCount };
// StState: 0 not ready, 1 FFX + D3D12 ready, 2 context ready, -1 fatal (no FFX/D3D12), -2 context creation failed
// StFrameGen: 0 no proxy swapchain (frame generation impossible this session), 1 proxy ready, 2 frame generation running
// StFrameGenTech: which proxy is in place, 0 none, 1 FSR, 2 XeSS (chosen at game start)

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
static PfnFfxConfigure pConfigure;
static ffxContext s_ctx;
static int s_createFlags;  // upscaler creation flags of the current generation; frame generation shares two of them

// frame generation: proxy swapchain (created by the swapchain hook) and the frame generation context on it
static ffxContext s_fgSwapchain, s_fg;
static IDXGISwapChain *s_proxy;
static DXGI_FORMAT s_scFormat;
static uint64_t s_fgFrame;
static int s_fgW, s_fgH, s_fgFailedGen = -1;
static int s_fgTech;  // StFrameGenTech

// XeSS frame generation: Intel's proxy swapchain, which needs Intel's XeLL latency reduction fed with frame markers
#define XEFG_FUNCS(X) X(xefgSwapChainD3D12CreateContext) X(xefgSwapChainSetLoggingCallback) X(xefgSwapChainSetLatencyReduction) \
    X(xefgSwapChainD3D12InitFromSwapChainDesc) X(xefgSwapChainD3D12GetSwapChainPtr) X(xefgSwapChainSetEnabled) \
    X(xefgSwapChainSetPresentId) X(xefgSwapChainD3D12TagFrameResource) X(xefgSwapChainTagFrameConstants) \
    X(xefgSwapChainGetVersion) X(xefgSwapChainDestroy)
#define XELL_FUNCS(X) X(xellD3D12CreateContext) X(xellDestroyContext) X(xellSetSleepMode) X(xellSleep) X(xellAddMarkerData) \
    X(xellSetLoggingCallback) X(xellGetVersion)
#define DECLARE_FN(fn) static decltype(&fn) p_##fn;
XEFG_FUNCS(DECLARE_FN)
XELL_FUNCS(DECLARE_FN)
static xefg_swapchain_handle_t s_xefg;
static xell_context_handle_t s_xell;
static bool s_xefgOn;
static volatile LONG s_presentId;  // frame ID of the frame Unity presents next (EvFrame)

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

// ---- sharpening for XeSS and DLSS ----
// FSR sharpens inside its own dispatch; XeSS has no sharpening and DLSS ignores its sharpness. For those two we run
// RCAS (FidelityFX FSR's robust contrast-adaptive sharpening, MIT) on the upscaled image: copy it aside, then sharpen
// back into it. The image is HDR, so samples go through a reversible tonemap around RCAS's [0, 1] math.
static const char *kRcasHlsl = R"(
Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);
cbuffer C : register(b0) { float Con; uint W; uint H; uint Pad; };
#define RCAS_LIMIT (0.25 - (1.0 / 16.0))
float3 Tm(float3 c) { return c / (1.0 + max(max(c.r, c.g), c.b)); }
float3 InvTm(float3 c) { return c / max(1.0 - max(max(c.r, c.g), c.b), 1e-4); }
float3 Fetch(int2 p) { return Tm(max(Src.Load(int3(clamp(p, int2(0, 0), int2(W - 1, H - 1)), 0)).rgb, 0.0)); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= W || id.y >= H) return;
    int2 p = int2(id.xy);
    float4 centre = Src.Load(int3(p, 0));
    float3 b = Fetch(p + int2(0, -1)), d = Fetch(p + int2(-1, 0)), f = Fetch(p + int2(1, 0)), h = Fetch(p + int2(0, 1));
    float3 e = Tm(max(centre.rgb, 0.0));
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) / (4.0 * mx4 + 1e-5);
    float3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0);
    float3 lobe3 = max(-hitMin, hitMax);
    float lobe = max(-RCAS_LIMIT, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * Con;
    float3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    Dst[p] = float4(InvTm(saturate(c)), centre.a);
}
)";
static ID3D12RootSignature *s_rcasRootSig;
static ID3D12PipelineState *s_rcasPso;
static ID3D12DescriptorHeap *s_rcasHeap;  // kFrames * 2 (SRV, UAV), shader visible
static ID3D12Resource *s_rcasSrc;         // copy of the upscaled image RCAS reads from

static bool InitRcas()
{
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(kRcasHlsl, strlen(kRcasHlsl), "ParalivesUpscaling_Rcas", nullptr, nullptr, "main", "cs_5_1",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) { Log("sharpening: compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2; rs.pParameters = params;
    ID3DBlob *rsBlob = nullptr;
    hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &err);
    if (SUCCEEDED(hr)) hr = s_dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&s_rcasRootSig));
    if (rsBlob) rsBlob->Release();
    if (err) { err->Release(); err = nullptr; }
    if (FAILED(hr)) { code->Release(); Log("sharpening: root signature failed 0x%08X", (unsigned)hr); return false; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = s_rcasRootSig;
    pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    hr = s_dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&s_rcasPso));
    code->Release();
    if (FAILED(hr)) { Log("sharpening: PSO failed 0x%08X", (unsigned)hr); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kFrames * 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = s_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_rcasHeap));
    if (FAILED(hr)) { s_rcasPso->Release(); s_rcasPso = nullptr; Log("sharpening: descriptor heap failed 0x%08X", (unsigned)hr); return false; }
    s_mvInc = s_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

static void Transition(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    s_list->ResourceBarrier(1, &b);
}

// Records RCAS on f->Output (UAV state before and after) into s_list.
static void RecordRcas(const FrameData *f, int slot)
{
    D3D12_RESOURCE_DESC od = f->Output->GetDesc();
    if (s_rcasSrc) {
        D3D12_RESOURCE_DESC sd = s_rcasSrc->GetDesc();
        if (sd.Width != od.Width || sd.Height != od.Height || sd.Format != od.Format) {
            WaitFence(s_lastFence);  // the previous frames may still read it
            s_rcasSrc->Release();
            s_rcasSrc = nullptr;
        }
    }
    if (!s_rcasSrc) {
        D3D12_RESOURCE_DESC d = od;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(s_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                  nullptr, IID_PPV_ARGS(&s_rcasSrc)))) {
            Log("sharpening: texture creation failed");
            s_rcasSrc = nullptr;
            return;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = s_rcasHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = s_rcasHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)slot * 2 * s_mvInc;
    gpu.ptr += (UINT64)slot * 2 * s_mvInc;
    // Unity may create the texture typeless; views need the concrete format
    DXGI_FORMAT vf = od.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? DXGI_FORMAT_R16G16B16A16_FLOAT : od.Format;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = vf;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    s_dev->CreateShaderResourceView(s_rcasSrc, &sv, cpu);
    cpu.ptr += s_mvInc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.Format = vf;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    s_dev->CreateUnorderedAccessView(f->Output, nullptr, &uv, cpu);

    Transition(f->Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(s_rcasSrc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    s_list->CopyResource(s_rcasSrc, f->Output);
    Transition(s_rcasSrc, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(f->Output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // same strength curve as FSR's own sharpness: 1 = full RCAS, 0 = two stops weaker (0 itself is skipped)
    float con = exp2f(2.0f * f->Sharpness - 2.0f);
    UINT consts[4] = { 0, (UINT)f->OutW, (UINT)f->OutH, 0 };
    memcpy(&consts[0], &con, 4);
    s_list->SetComputeRootSignature(s_rcasRootSig);
    s_list->SetDescriptorHeaps(1, &s_rcasHeap);
    s_list->SetPipelineState(s_rcasPso);
    s_list->SetComputeRoot32BitConstants(0, 4, consts, 0);
    s_list->SetComputeRootDescriptorTable(1, gpu);
    s_list->Dispatch((UINT)(f->OutW + 7) / 8, (UINT)(f->OutH + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER u = {};
    u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    u.UAV.pResource = f->Output;
    s_list->ResourceBarrier(1, &u);
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
// ---- FSR frame generation ----
// Each frame: configure (frame ID, enabled) and "prepare" (depth + motion vectors, recorded into our command list);
// on Present the proxy swapchain runs the interpolation through FgDispatch and presents both frames.
static ffxReturnCode_t FgDispatch(ffxDispatchDescFrameGeneration *p, void *ctx) { return pDispatch((ffxContext *)ctx, &p->header); }

static ffxReturnCode_t ConfigureFg(bool enabled)
{
    ffxConfigureDescFrameGeneration c = {};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = s_proxy;
    c.frameGenerationCallback = FgDispatch;
    c.frameGenerationCallbackUserContext = &s_fg;
    c.frameGenerationEnabled = enabled;
    c.generationRect = { 0, 0, s_fgW, s_fgH };
    c.frameID = s_fgFrame;
    return pConfigure(&s_fg, &c.header);
}

static void DestroyFg()
{
    if (!s_fg) return;
    ConfigureFg(false);
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 w = {};
    w.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    pDispatch(&s_fgSwapchain, &w.header);
    WaitFence(s_lastFence);
    pDestroy(&s_fg, nullptr);
    s_fg = nullptr;
    InterlockedExchange(&s_status[StFrameGen], 1);
    Log("frame generation off");
}

static bool CreateFg(const FrameData *f)
{
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = s_dev;
    ffxCreateContextDescFrameGenerationVersion ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    // same bit values as the upscaler's flags
    d.flags = (uint32_t)s_createFlags & (FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION | FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED);
    d.displaySize = { (uint32_t)f->OutW, (uint32_t)f->OutH };
    d.maxRenderSize = { (uint32_t)f->RenderW, (uint32_t)f->RenderH };
    d.backBufferFormat = ffxApiGetSurfaceFormatDX12(s_scFormat);
    d.header.pNext = &backend.header;
    backend.header.pNext = &ver.header;
    ffxReturnCode_t r = pCreate(&s_fg, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK || !s_fg) { s_fg = nullptr; Log("frame generation context creation failed (%u)", r); return false; }
    s_fgW = f->OutW;
    s_fgH = f->OutH;
    s_fgFrame = 0;
    InterlockedExchange(&s_status[StFrameGen], 2);
    Log("frame generation on: %dx%d -> %dx%d, flags 0x%X", f->RenderW, f->RenderH, f->OutW, f->OutH, d.flags);
    return true;
}

// Records frame generation's prepare pass into s_list (depth and motion vectors are in a read state by now).
static void PrepareFg(const FrameData *f, bool fix)
{
    if (!s_fg && (s_fgFailedGen == f->Gen || !CreateFg(f))) { s_fgFailedGen = f->Gen; return; }
    s_fgFrame++;
    ffxDispatchDescFrameGenerationPrepareV2 p = {};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = s_fgFrame;
    p.commandList = s_list;
    p.renderSize = { (uint32_t)f->RenderW, (uint32_t)f->RenderH };
    p.jitterOffset = { f->JitterX, f->JitterY };
    p.motionVectorScale = { f->MvScaleX, f->MvScaleY };
    p.frameTimeDelta = f->FrameTimeMs;
    p.reset = f->Reset != 0;
    p.cameraNear = f->Near;
    p.cameraFar = f->Far;
    p.cameraFovAngleVertical = f->FovY;
    p.viewSpaceToMetersFactor = 1.0f;
    p.depth = ffxApiGetResourceDX12(fix ? f->DepthAtt : f->Depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    p.motionVectors = ffxApiGetResourceDX12(fix ? f->MotionOut : f->Motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    memcpy(p.cameraPosition, f->CamPos, sizeof p.cameraPosition);
    memcpy(p.cameraUp, f->CamUp, sizeof p.cameraUp);
    memcpy(p.cameraRight, f->CamRight, sizeof p.cameraRight);
    memcpy(p.cameraForward, f->CamFwd, sizeof p.cameraForward);

    ffxReturnCode_t r = ConfigureFg(true);
    if (r == FFX_API_RETURN_OK) r = pDispatch(&s_fg, &p.header);
    if (r != FFX_API_RETURN_OK) {
        Log("frame generation failed (%u); off until the next settings change", r);
        s_fgFailedGen = f->Gen;
        DestroyFg();
    }
}

// ---- XeSS frame generation ----
// Each frame: tag depth + motion vectors (valid until this frame's Present) and the camera constants under the frame
// ID; the proxy interpolates when Unity presents (HookPresent sets the present ID).
static void DisableXefg()
{
    if (!s_xefgOn) return;
    p_xefgSwapChainSetEnabled(s_xefg, 0);
    s_xefgOn = false;
    InterlockedExchange(&s_status[StFrameGen], 1);
    Log("frame generation off");
}

static void TagXefg(const FrameData *f, bool fix)
{
    if (!f->FrameGen || s_fgFailedGen == f->Gen) { DisableXefg(); return; }
    uint32_t id = (uint32_t)f->FrameId;
    xefg_swapchain_d3d12_resource_data_t r = {};
    r.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    r.resourceSize = { (uint32_t)f->RenderW, (uint32_t)f->RenderH };
    r.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    r.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    r.pResource = fix ? f->MotionOut : f->Motion;
    xefg_swapchain_result_t e = p_xefgSwapChainD3D12TagFrameResource(s_xefg, s_list, id, &r);
    r.type = XEFG_SWAPCHAIN_RES_DEPTH;
    r.pResource = fix ? f->DepthAtt : f->Depth;
    if (e >= 0) e = p_xefgSwapChainD3D12TagFrameResource(s_xefg, s_list, id, &r);

    // Unity's column-major matrices read row by row are the transposed (row-vector, D3D-style) matrices XeSS FG expects
    xefg_swapchain_frame_constant_data_t c = {};
    memcpy(c.viewMatrix, f->View, sizeof c.viewMatrix);
    memcpy(c.projectionMatrix, f->Proj, sizeof c.projectionMatrix);
    c.jitterOffsetX = f->JitterX;
    c.jitterOffsetY = f->JitterY;
    c.motionVectorScaleX = f->MvScaleX;
    c.motionVectorScaleY = f->MvScaleY;
    c.resetHistory = f->Reset ? 1u : 0u;
    c.frameRenderTime = f->FrameTimeMs;
    if (e >= 0) e = p_xefgSwapChainTagFrameConstants(s_xefg, id, &c);

    if (e >= 0 && !s_xefgOn) {
        e = p_xefgSwapChainSetEnabled(s_xefg, 1);
        if (e >= 0) {
            s_xefgOn = true;
            InterlockedExchange(&s_status[StFrameGen], 2);
            Log("frame generation on: %dx%d -> %dx%d", f->RenderW, f->RenderH, f->OutW, f->OutH);
        }
    }
    if (e < 0) {
        Log("XeSS frame generation failed (%d); off until the next settings change", (int)e);
        s_fgFailedGen = f->Gen;
        DisableXefg();
    }
}

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
    DestroyFg();  // sizes may change; recreated on the next dispatch
    s_createFlags = f->CreateFlags;
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
    if (ok && !s_ctx && f->Sharpness > 0.0f && s_rcasPso) RecordRcas(f, i);  // XeSS / DLSS: our own sharpening
    if (ok && s_fgTech == 1) {
        if (f->FrameGen) PrepareFg(f, fix);
        else DestroyFg();
    } else if (ok && s_fgTech == 2) TagXefg(f, fix);
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
    DestroyFg();
    if (s_xefg) DisableXefg();
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
        case EvFrame: InterlockedExchange(&s_presentId, (LONG)(intptr_t)data); break;
    }
}

// ---- device lifetime ----
static bool LoadFfx()
{
    if (pCreate) return true;  // already loaded by the swapchain hook
    // Preload the upscaler by full path so the loader's plain LoadLibrary("amd_fidelityfx_upscaler_dx12.dll")
    // finds it even though Plugins\x86_64 is not on the DLL search path.
    wchar_t up[MAX_PATH], fg[MAX_PATH], ld[MAX_PATH];
    swprintf(up, MAX_PATH, L"%samd_fidelityfx_upscaler_dx12.dll", s_dir);
    swprintf(fg, MAX_PATH, L"%samd_fidelityfx_framegeneration_dx12.dll", s_dir);
    swprintf(ld, MAX_PATH, L"%samd_fidelityfx_loader_dx12.dll", s_dir);
    if (!LoadLibraryW(up)) { Fail(-1, "amd_fidelityfx_upscaler_dx12.dll not found next to ParalivesUpscaling.dll (%lu)", GetLastError()); return false; }
    if (!LoadLibraryW(fg)) Log("amd_fidelityfx_framegeneration_dx12.dll not found (%lu); frame generation unavailable", GetLastError());
    HMODULE m = LoadLibraryW(ld);
    if (!m) { Fail(-1, "amd_fidelityfx_loader_dx12.dll not found next to ParalivesUpscaling.dll (%lu)", GetLastError()); return false; }
    pCreate = (PfnFfxCreateContext)GetProcAddress(m, "ffxCreateContext");
    pDestroy = (PfnFfxDestroyContext)GetProcAddress(m, "ffxDestroyContext");
    pQuery = (PfnFfxQuery)GetProcAddress(m, "ffxQuery");
    pDispatch = (PfnFfxDispatch)GetProcAddress(m, "ffxDispatch");
    pConfigure = (PfnFfxConfigure)GetProcAddress(m, "ffxConfigure");
    if (!pCreate || !pDestroy || !pQuery || !pDispatch || !pConfigure) { Fail(-1, "FidelityFX loader is missing exports"); return false; }
    return true;
}

// ---- swapchain interception (frame generation) ----
// Frame generation presents through a proxy swapchain (FSR's or XeSS's). Both can only "wrap" an existing swapchain by
// releasing it and creating a new one, which fails while Unity holds references, so the BepInEx preloader patcher
// calls PFSR_InstallSwapchainHook before Unity creates its swapchain and Unity gets the proxy from the start. That is
// also why the frame generation technology can only change with a restart.
// mode 1 only logs swapchain creation (diagnostics); mode 2 substitutes FSR's proxy, mode 3 XeSS's. Both present
// normally until frame generation is switched on.
typedef HRESULT(STDMETHODCALLTYPE *PfnCreateSwapChain)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT(STDMETHODCALLTYPE *PfnCreateSwapChainForHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
typedef HRESULT(STDMETHODCALLTYPE *PfnPresent)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PfnPresent1)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
static PfnCreateSwapChain oCreateSwapChain;
static PfnCreateSwapChainForHwnd oCreateSwapChainForHwnd;
static PfnPresent oPresent;
static PfnPresent1 oPresent1;
static int s_scMode;
static thread_local bool s_inProxyCreate;  // the proxies create the real swapchain through the same (hooked) factory
static thread_local int s_inPresent;       // Present1 may call Present inside the proxy
static uint32_t s_lastMarkedPresent = 0xFFFFFFFFu;

static void *PatchVtable(void *obj, int slot, void *fn)
{
    void **vt = *(void ***)obj;
    DWORD old;
    VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old);
    void *prev = vt[slot];
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void *), old, &old);
    return prev;
}

static HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **sc)
{
    if (!s_inProxyCreate && d)
        Log("CreateSwapChain: %ux%u format %d buffers %u swap effect %d flags 0x%x windowed %d (not intercepted)",
            d->BufferDesc.Width, d->BufferDesc.Height, d->BufferDesc.Format, d->BufferCount, d->SwapEffect, d->Flags, d->Windowed);
    return oCreateSwapChain(f, dev, d, sc);
}

// XeSS FG proxy: XeLL's present markers and XeSS FG's present ID around Unity's Present (render thread). A frame
// without EvFrame (nothing rendered) keeps the previous ID and gets no second set of markers.
static bool BeginPresent(UINT flags, uint32_t &id)
{
    if (s_inPresent++ || (flags & DXGI_PRESENT_TEST)) return false;
    id = (uint32_t)s_presentId;
    if (id == s_lastMarkedPresent) return false;
    s_lastMarkedPresent = id;
    p_xellAddMarkerData(s_xell, id, XELL_PRESENT_START);
    p_xefgSwapChainSetPresentId(s_xefg, id);
    return true;
}

// Unity creates its swapchain with a frame-latency waitable object and waits on it before each frame. Through the
// XeSS FG proxy that object stops being signalled before Unity's first present (the proxy's own pacing drains it), so
// Unity hangs at start-up. Unity gets an always-signalled event instead: its own frame fence still bounds how far the
// CPU runs ahead, and XeLL paces frames while frame generation runs.
typedef HANDLE(STDMETHODCALLTYPE *PfnGetWaitable)(IDXGISwapChain2 *);
static PfnGetWaitable oGetWaitable;
static HANDLE s_waitable;

static HANDLE STDMETHODCALLTYPE HookGetWaitable(IDXGISwapChain2 *sc)
{
    if (HANDLE original = oGetWaitable(sc)) CloseHandle(original);  // the caller owns the returned handle
    if (!s_waitable) s_waitable = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    HANDLE ours = nullptr;
    DuplicateHandle(GetCurrentProcess(), s_waitable, GetCurrentProcess(), &ours, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return ours;
}

static void EndPresent(bool marked, uint32_t id)
{
    if (marked) p_xellAddMarkerData(s_xell, id, XELL_PRESENT_END);
    s_inPresent--;
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    uint32_t id = 0;
    bool marked = BeginPresent(flags, id);
    HRESULT hr = oPresent(sc, sync, flags);
    EndPresent(marked, id);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *pp)
{
    uint32_t id = 0;
    bool marked = BeginPresent(flags, id);
    HRESULT hr = oPresent1(sc, sync, flags, pp);
    EndPresent(marked, id);
    return hr;
}

static IDXGISwapChain1 *CreateFfxProxy(IDXGIFactory2 *f, ID3D12CommandQueue *queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *d,
                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs)
{
    if (!LoadFfx()) return nullptr;
    DXGI_SWAP_CHAIN_DESC1 desc = *d;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = fs ? *fs : DXGI_SWAP_CHAIN_FULLSCREEN_DESC{};
    IDXGISwapChain4 *proxy = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 c = {};
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    c.swapchain = &proxy;
    c.hwnd = hwnd;
    c.desc = &desc;
    c.fullscreenDesc = fs ? &fsDesc : nullptr;
    c.dxgiFactory = f;
    c.gameQueue = queue;
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 v = {};
    v.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    v.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    c.header.pNext = &v.header;

    s_inProxyCreate = true;
    ffxReturnCode_t rc = pCreate(&s_fgSwapchain, &c.header, nullptr);
    s_inProxyCreate = false;
    if (rc == FFX_API_RETURN_OK && proxy) {
        Log("FSR frame generation swapchain in place (%p)", (void *)proxy);
        return proxy;
    }
    s_fgSwapchain = nullptr;
    Log("FSR frame generation swapchain failed (%d); using the game's own swapchain", (int)rc);
    return nullptr;
}

static void XefgLog(const char *msg, xefg_swapchain_logging_level_t level, void *)
{
    static const char *names[] = { "debug", "info", "warning", "error" };
    Log("XeSS FG %s: %s", names[level & 3], msg);
}

static void XellLog(const char *msg, xell_logging_level_t level)
{
    static const char *names[] = { "debug", "info", "warning", "error" };
    Log("XeLL %s: %s", names[level & 3], msg);
}

static bool LoadXefg()
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%slibxell.dll", s_dir);
    HMODULE xell = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    swprintf(path, MAX_PATH, L"%slibxess_fg.dll", s_dir);
    HMODULE xefg = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!xell || !xefg) { Log("XeSS FG: libxess_fg.dll or libxell.dll not found next to ParalivesUpscaling.dll (%lu)", GetLastError()); return false; }
    bool ok = true;
#define GET_XEFG(fn) ok &= (p_##fn = (decltype(p_##fn))GetProcAddress(xefg, #fn)) != nullptr;
#define GET_XELL(fn) ok &= (p_##fn = (decltype(p_##fn))GetProcAddress(xell, #fn)) != nullptr;
    XEFG_FUNCS(GET_XEFG)
    XELL_FUNCS(GET_XELL)
    if (!ok) Log("XeSS FG: libxess_fg.dll or libxell.dll is missing exports");
    return ok;
}

static IDXGISwapChain1 *CreateXefgProxy(IDXGIFactory2 *f, ID3D12CommandQueue *queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *d,
                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs)
{
    ID3D12Device *dev = nullptr;
    if (!LoadXefg() || FAILED(queue->GetDevice(IID_PPV_ARGS(&dev)))) return nullptr;
    IDXGISwapChain1 *proxy = nullptr;
    xefg_swapchain_result_t r = XEFG_SWAPCHAIN_RESULT_ERROR_UNKNOWN;
    xell_result_t lr = p_xellD3D12CreateContext(dev, &s_xell);
    if (lr == XELL_RESULT_SUCCESS && s_xell) {
        p_xellSetLoggingCallback(s_xell, XELL_LOGGING_LEVEL_WARNING, XellLog);
        // XeSS FG requires latency reduction on; on other vendors XeLL only acts while frame generation runs
        xell_sleep_params_t sp = {};
        sp.bLowLatencyMode = 1;
        p_xellSetSleepMode(s_xell, &sp);
        r = p_xefgSwapChainD3D12CreateContext(dev, &s_xefg);
    }
    if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        p_xefgSwapChainSetLoggingCallback(s_xefg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, XefgLog, nullptr);
        r = p_xefgSwapChainSetLatencyReduction(s_xefg, s_xell);
    }
    if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        xefg_swapchain_d3d12_init_params_t ip = {};
        // Unity uses reversed-Z depth; URP builds its motion vectors with our jittered projection
        ip.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH | XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;
        ip.maxInterpolatedFrames = 1;
        ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
        s_inProxyCreate = true;
        r = p_xefgSwapChainD3D12InitFromSwapChainDesc(s_xefg, hwnd, d, fs, queue, f, &ip);
        s_inProxyCreate = false;
    }
    if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS) r = p_xefgSwapChainD3D12GetSwapChainPtr(s_xefg, IID_PPV_ARGS(&proxy));
    dev->Release();
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !proxy) {
        Log("XeSS frame generation swapchain failed (XeLL %d, XeSS FG %d); using the game's own swapchain", (int)lr, (int)r);
        if (proxy) proxy->Release();
        if (s_xefg) { p_xefgSwapChainDestroy(s_xefg); s_xefg = nullptr; }
        if (s_xell) { p_xellDestroyContext(s_xell); s_xell = nullptr; }
        return nullptr;
    }
    oPresent = (PfnPresent)PatchVtable(proxy, 8, (void *)HookPresent);      // IDXGISwapChain::Present
    oPresent1 = (PfnPresent1)PatchVtable(proxy, 22, (void *)HookPresent1);  // IDXGISwapChain1::Present1
    oGetWaitable = (PfnGetWaitable)PatchVtable(proxy, 33, (void *)HookGetWaitable);  // IDXGISwapChain2::GetFrameLatencyWaitableObject
    xefg_swapchain_version_t v = {};
    xell_version_t lv = {};
    p_xefgSwapChainGetVersion(&v);
    p_xellGetVersion(&lv);
    Log("XeSS frame generation swapchain in place (%p): XeSS FG %u.%u.%u, XeLL %u.%u.%u", (void *)proxy,
        v.major, v.minor, v.patch, lv.major, lv.minor, lv.patch);
    return proxy;
}

static HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2 *f, IUnknown *dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *d,
                                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *out, IDXGISwapChain1 **sc)
{
    if (s_inProxyCreate || !d) return oCreateSwapChainForHwnd(f, dev, hwnd, d, fs, out, sc);
    Log("CreateSwapChainForHwnd: %ux%u format %d buffers %u swap effect %d flags 0x%x scaling %d alpha %d windowed %d",
        d->Width, d->Height, d->Format, d->BufferCount, d->SwapEffect, d->Flags, d->Scaling, d->AlphaMode, fs ? fs->Windowed : -1);

    // ponytail: only the first swapchain gets the proxy; handle recreation if Unity ever makes a second one.
    ID3D12CommandQueue *queue = nullptr;
    if ((s_scMode == 2 || s_scMode == 3) && !s_proxy && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&queue)))) {
        IDXGISwapChain1 *proxy = s_scMode == 3 ? CreateXefgProxy(f, queue, hwnd, d, fs) : CreateFfxProxy(f, queue, hwnd, d, fs);
        queue->Release();
        if (proxy) {
            s_proxy = proxy;
            s_scFormat = d->Format;
            s_fgTech = s_scMode == 3 ? 2 : 1;
            InterlockedExchange(&s_status[StFrameGenTech], s_fgTech);
            InterlockedExchange(&s_status[StFrameGen], 1);
            *sc = proxy;  // Unity owns this reference
            return S_OK;
        }
    }
    return oCreateSwapChainForHwnd(f, dev, hwnd, d, fs, out, sc);
}

static void SetDir()
{
    if (s_dir[0]) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&SetDir, &self);
    GetModuleFileNameW(self, s_dir, MAX_PATH);
    if (wchar_t *slash = wcsrchr(s_dir, L'\\')) slash[1] = 0;
}

static void InitDevice()
{
    if (s_gfx->GetRenderer() != kUnityGfxRendererD3D12) { Fail(-1, "game is not running on D3D12 (add -force-d3d12)"); return; }
    s_d3d = s_ifs->Get<IUnityGraphicsD3D12v6>();
    if (!s_d3d) { Fail(-1, "IUnityGraphicsD3D12v6 not available"); return; }
    s_dev = s_d3d->GetDevice();
    if (auto *v7 = s_ifs->Get<IUnityGraphicsD3D12v7>()) {
        IDXGISwapChain *sc = v7->GetSwapChain();
        Log("Unity swapchain %p%s, sync interval %u, present flags 0x%x", (void *)sc, sc && sc == s_proxy ? " (frame generation proxy)" : "",
            v7->GetSyncInterval(), v7->GetPresentFlags());
    }

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
    if (!InitRcas()) Log("sharpening for XeSS and DLSS unavailable");
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
        DestroyFg();
        RetireCurrent();
        CollectRetired(true);
    }
    if (s_ngxReady) {
        if (s_ngxParams) { NVSDK_NGX_D3D12_DestroyParameters(s_ngxParams); s_ngxParams = nullptr; }
        NVSDK_NGX_D3D12_Shutdown1(s_dev);
        s_ngxReady = false;
    }
    if (s_mvPso) { s_mvPso->Release(); s_mvPso = nullptr; }
    if (s_rcasPso) { s_rcasPso->Release(); s_rcasPso = nullptr; }
    if (s_rcasRootSig) { s_rcasRootSig->Release(); s_rcasRootSig = nullptr; }
    if (s_rcasHeap) { s_rcasHeap->Release(); s_rcasHeap = nullptr; }
    if (s_rcasSrc) { s_rcasSrc->Release(); s_rcasSrc = nullptr; }
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
    SetDir();
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
// Called by the preloader patcher, before Unity creates its swapchain. mode: 1 log only, 2 FSR proxy, 3 XeSS proxy.
EXPORT int UNITY_INTERFACE_API PFSR_InstallSwapchainHook(int mode)
{
    SetDir();
    if (oCreateSwapChainForHwnd) return 1;
    IDXGIFactory2 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) { Log("swapchain hook: CreateDXGIFactory1 failed"); return 0; }
    s_scMode = mode;
    // vtable slots: IDXGIFactory::CreateSwapChain = 10, IDXGIFactory2::CreateSwapChainForHwnd = 15 (shared by every factory)
    oCreateSwapChain = (PfnCreateSwapChain)PatchVtable(f, 10, (void *)HookCreateSwapChain);
    oCreateSwapChainForHwnd = (PfnCreateSwapChainForHwnd)PatchVtable(f, 15, (void *)HookCreateSwapChainForHwnd);
    f->Release();
    Log("---- swapchain hook installed (mode %d)", mode);
    return 1;
}
// XeLL frame markers from the game's main thread (XeSS frame generation only). marker: -1 = xellSleep at the start of
// the frame, otherwise an xell_latency_marker_type_t.
EXPORT void UNITY_INTERFACE_API PFSR_XellMarker(int frameId, int marker)
{
    if (!s_xell) return;
    if (marker < 0) p_xellSleep(s_xell, (uint32_t)frameId);
    else p_xellAddMarkerData(s_xell, (uint32_t)frameId, (xell_latency_marker_type_t)marker);
}

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
