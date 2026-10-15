// Loads PLProbe.dll (which makes Unity call its UnityPluginLoad) and fires one D3D12 render event.
using System;
using System.Runtime.InteropServices;
using BepInEx;
using UnityEngine;
using UnityEngine.Rendering;

[BepInPlugin("paralives.plprobe", "PL Probe", "0.1.0")]
public sealed class PLProbeLoader : BaseUnityPlugin
{
    [DllImport("PLProbe")] private static extern int PLProbe_Status();
    [DllImport("PLProbe")] private static extern IntPtr PLProbe_GetRenderEventFunc();

    private int _frames;

    private void Awake()
    {
        Logger.LogInfo("Graphics API: " + SystemInfo.graphicsDeviceType);
        Logger.LogInfo("Status after load: " + Describe(PLProbe_Status()));
    }

    private void Update()
    {
        _frames++;
        if (_frames == 120)
        {
            var cb = new CommandBuffer { name = "PLProbe" };
            cb.IssuePluginEventAndData(PLProbe_GetRenderEventFunc(), 1, IntPtr.Zero);
            Graphics.ExecuteCommandBuffer(cb);
            cb.Release();
        }
        else if (_frames == 240)
        {
            Logger.LogInfo("Status after render event: " + Describe(PLProbe_Status()));
            enabled = false;
        }
    }

    private static string Describe(int s)
    {
        return s + " (UnityPluginLoad " + ((s & 1) != 0 ? "YES" : "NO") +
               ", D3D12v6 " + ((s & 2) != 0 ? "YES" : "NO") +
               ", command list " + ((s & 4) != 0 ? "YES" : "NO") + ")";
    }
}
