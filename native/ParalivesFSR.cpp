// ParalivesFSR - native render-thread side.
//
// Runs AMD FSR (3.1 or 4, through the FidelityFX API loader) on Unity's own D3D12 device.
// FSR records into a command list of ours, handed to Unity with IUnityGraphicsD3D12v6::ExecuteCommandList
// together with the resource states FSR expects, so Unity inserts the barriers and keeps tracking states.
// Writes ParalivesFSR.log next to this DLL.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d12.h>
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
    int Gen, Pad;
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

    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = s_list;
    d.color = ffxApiGetResourceDX12(f->Color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(f->Depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(f->Motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.output = ffxApiGetResourceDX12(f->Output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
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

    ffxReturnCode_t r = pDispatch(&s_ctx, &d.header);
    s_list->Close();
    if (r != FFX_API_RETURN_OK) {
        LONG n = InterlockedIncrement(&s_status[StDispatchFailures]);
        if (n <= 3) Log("dispatch failed (%u)", r);
        if (n >= 10) Fail(-2, "FSR keeps failing (%u)", r);
        return;
    }

    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    UnityGraphicsD3D12ResourceState states[4] = {
        { f->Color, read, read },
        { f->Depth, read, read },
        { f->Motion, read, read },
        { f->Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
    };
    s_lastFence = s_allocFence[i] = s_d3d->ExecuteCommandList(s_list, 4, states);
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
