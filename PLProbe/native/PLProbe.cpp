// PLProbe - checks that Unity calls UnityPluginLoad for a mod-loaded native plugin and that
// IUnityGraphicsD3D12v6 gives us the device, ConfigureEvent and the live command list.
// Writes PLProbe.log next to this DLL.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdarg>
#include "Unity/IUnityGraphics.h"
#include "Unity/IUnityGraphicsD3D12.h"

enum { EvProbe = 1 };

static IUnityInterfaces *s_ifs;
static IUnityGraphics *s_gfx;
static IUnityGraphicsD3D12v6 *s_d3d12;
static volatile LONG s_loadCalled, s_eventOk;
static wchar_t s_logPath[MAX_PATH];

static void Log(const char *fmt, ...)
{
    if (!s_logPath[0]) {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&Log, &m);
        GetModuleFileNameW(m, s_logPath, MAX_PATH);
        wchar_t *slash = wcsrchr(s_logPath, L'\\');
        wcscpy_s(slash ? slash + 1 : s_logPath, MAX_PATH - (slash ? (slash + 1 - s_logPath) : 0), L"PLProbe.log");
    }
    char line[512];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    FILE *f = _wfopen(s_logPath, L"a");
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

static void UNITY_INTERFACE_API OnGfxEvent(UnityGfxDeviceEventType type)
{
    if (type != kUnityGfxDeviceEventInitialize) return;
    UnityGfxRenderer r = s_gfx->GetRenderer();
    Log("renderer = %d (D3D12 = %d)", (int)r, (int)kUnityGfxRendererD3D12);
    if (r != kUnityGfxRendererD3D12) return;

    s_d3d12 = s_ifs->Get<IUnityGraphicsD3D12v6>();
    Log("IUnityGraphicsD3D12v6 = %p", (void *)s_d3d12);
    if (!s_d3d12) return;
    Log("device = %p, queue = %p", (void *)s_d3d12->GetDevice(), (void *)s_d3d12->GetCommandQueue());

    // Same config the FSR plugin will need: we record into Unity's command list.
    UnityD3D12PluginEventConfig cfg = {};
    cfg.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_DontCare;
    cfg.flags = kUnityD3D12EventConfigFlag_SyncWorkerThreads |
                kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState;
    cfg.ensureActiveRenderTextureIsBound = false;
    s_d3d12->ConfigureEvent(EvProbe, &cfg);
    Log("ConfigureEvent(%d) done", EvProbe);
}

static void UNITY_INTERFACE_API OnRenderEvent(int eventId, void *)
{
    if (eventId != EvProbe || !s_d3d12) return;
    UnityGraphicsD3D12RecordingState st = {};
    bool ok = s_d3d12->CommandRecordingState(&st);
    Log("render event: CommandRecordingState = %d, commandList = %p", (int)ok, (void *)st.commandList);
    if (ok && st.commandList) InterlockedExchange(&s_eventOk, 1);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces *ifs)
{
    InterlockedExchange(&s_loadCalled, 1);
    Log("UnityPluginLoad called (interfaces = %p)", (void *)ifs);
    s_ifs = ifs;
    s_gfx = ifs->Get<IUnityGraphics>();
    s_gfx->RegisterDeviceEventCallback(OnGfxEvent);
    // Loaded after the device already exists (lazy DllImport), so run init by hand.
    OnGfxEvent(kUnityGfxDeviceEventInitialize);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
    if (s_gfx) s_gfx->UnregisterDeviceEventCallback(OnGfxEvent);
}

extern "C" UNITY_INTERFACE_EXPORT void *UNITY_INTERFACE_API PLProbe_GetRenderEventFunc() { return (void *)OnRenderEvent; }

// bit 0: UnityPluginLoad called, bit 1: D3D12v6 interface found, bit 2: got a command list in a render event
extern "C" UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API PLProbe_Status()
{
    return (s_loadCalled ? 1 : 0) | (s_d3d12 ? 2 : 0) | (s_eventOk ? 4 : 0);
}
