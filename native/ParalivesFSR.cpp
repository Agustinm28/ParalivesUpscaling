// ParalivesFSR - native render-thread side.
//
// Runs AMD FSR (3.1 or 4, through the FidelityFX API loader) on Unity's own D3D12 device.
// FSR records into a command list of ours, handed to Unity with IUnityGraphicsD3D12v6::ExecuteCommandList
// together with the resource states FSR expects, so Unity inserts the barriers and keeps tracking states.
// Before FSR, a small compute pass repairs URP's motion vectors where the depth prepass missed geometry
// (the game's characters), using the camera's real depth buffer.
// Writes ParalivesFSR.log next to this DLL.
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

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define EXPORT extern "C" UNITY_INTERFACE_EXPORT

// ---- shared with C# (Native.cs) ----
struct FrameData {
    ID3D12Resource *Color, *Depth, *Motion, *Output;
    int RenderW, RenderH, OutW, OutH;
    float JitterX, JitterY, MvScaleX, MvScaleY;
    float FrameTimeMs, Near, Far, FovY;
    float Sharpness;
    int Reset;
    int CreateFlags, Version;  // Version: 0 = driver/SDK default, 3 = force FSR 3.x, 4 = force FSR 4.x
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

// ---- logging ----
static wchar_t s_dir[MAX_PATH];
static void Log(const char *fmt, ...)
{
    char line[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%sParalivesFSR.log", s_dir);
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

// Contexts replaced while the GPU may still use them; destroyed once the frame fence passes.
struct Retired { ffxContext ctx; UINT64 fence; };
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
    HRESULT hr = D3DCompile(kMotionFixHlsl, strlen(kMotionFixHlsl), "ParalivesFSR_MotionFix", nullptr, nullptr, "main", "cs_5_1",
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

static void CollectRetired(bool force)
{
    for (Retired &r : s_retired) {
        if (!r.ctx || (!force && !FenceDone(r.fence))) continue;
        if (force) WaitFence(r.fence);
        pDestroy(&r.ctx, nullptr);
        r.ctx = nullptr;
    }
}

static void RetireCurrent()
{
    if (!s_ctx) return;
    for (Retired &r : s_retired)
        if (!r.ctx) { r.ctx = s_ctx; r.fence = s_lastFence; s_ctx = nullptr; return; }
    WaitFence(s_lastFence);  // all slots busy: just wait
    pDestroy(&s_ctx, nullptr);
    s_ctx = nullptr;
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

    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = s_dev;
    ffxCreateContextDescUpscaleVersion apiVersion = {};
    apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    apiVersion.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion over = {};
    over.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    over.versionId = PickVersion(f->Version);

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

static void DoDispatch(const FrameData *f)
{
    if (s_status[StState] != 2 || !s_ctx || f->Gen != s_status[StCreatedGen]) return;
    CollectRetired(false);

    int i = s_allocIdx = (s_allocIdx + 1) % kFrames;
    WaitFence(s_allocFence[i]);
    s_alloc[i]->Reset();
    s_list->Reset(s_alloc[i], nullptr);

    bool fix = s_mvPso && f->DepthAtt && f->MotionOut && f->Reactive;
    // FSR 4 copes with the characters' missing object motion on its own; the mask would only cost it stability
    float reactive = s_versionName[0] == '3' ? f->ReactiveStrength : 0.0f;
    if (fix) RecordMotionFix(f, i, reactive);

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
    s_list->Close();
    if (r != FFX_API_RETURN_OK) {
        LONG n = InterlockedIncrement(&s_status[StDispatchFailures]);
        if (n <= 3) Log("dispatch failed (%u)", r);
        if (n >= 10) Fail(-2, "FSR keeps failing (%u)", r);
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
    if (!LoadLibraryW(up)) { Fail(-1, "amd_fidelityfx_upscaler_dx12.dll not found next to ParalivesFSR.dll (%lu)", GetLastError()); return false; }
    HMODULE m = LoadLibraryW(ld);
    if (!m) { Fail(-1, "amd_fidelityfx_loader_dx12.dll not found next to ParalivesFSR.dll (%lu)", GetLastError()); return false; }
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
    PickVersion(0);  // log what this GPU/driver offers
    Log("ready (device %p)", (void *)s_dev);
    InterlockedExchange(&s_status[StState], 1);
}

static void ShutdownDevice()
{
    if (pDestroy) {
        RetireCurrent();
        CollectRetired(true);
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
    Log("---- ParalivesFSR native loaded");

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
// which: 0 = active upscaler version name, 1 = last error
EXPORT int UNITY_INTERFACE_API PFSR_GetText(int which, char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    snprintf(buf, (size_t)cap, "%s", which == 0 ? s_versionName : s_lastError);
    return (int)strlen(buf);
}
