// ParalivesUpscaling.Patcher - BepInEx 5 preloader patcher. Runs before Unity creates its swapchain and installs the
// native plugin's DXGI swapchain hook, which frame generation (FSR or XeSS) needs. Patches no assemblies.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using BepInEx;
using BepInEx.Configuration;
using BepInEx.Logging;
using Mono.Cecil;

namespace ParalivesUpscaling.Patcher
{
    public static class Patcher
    {
        public static IEnumerable<string> TargetDLLs { get; } = new string[0];
        public static void Patch(AssemblyDefinition assembly) { }

        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", CharSet = CharSet.Ansi)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int InstallSwapchainHook(int mode);

        public static void Initialize()
        {
            var log = Logger.CreateLogSource("ParalivesUpscaling.Patcher");
            // Read-only view of the plugin's settings file (the plugin binds and documents these keys).
            var cfg = new ConfigFile(Path.Combine(Paths.ConfigPath, "paralives.upscaling.cfg"), false) { SaveOnConfigSet = false };
            if (!cfg.Bind("Advanced", "FrameGenerationSwapchain", true, "").Value) { log.LogInfo("Frame generation swapchain switched off in the config"); return; }
            // The proxy swapchain decides the frame generation technology for the whole session. FSR's otherwise, so FSR
            // frame generation can still be switched on live.
            bool xess = string.Equals(cfg.Bind("General", "FrameGeneration", "Off", "").Value, "XeSS", StringComparison.OrdinalIgnoreCase);
            int mode = xess ? 3 : 2;  // native: 1 = only log swapchain creation, 2 = FSR proxy, 3 = XeSS proxy

            string dll = Path.Combine(Path.Combine(Path.Combine(Paths.GameRootPath, "Paralives_Data"), "Plugins"), Path.Combine("x86_64", "ParalivesUpscaling.dll"));
            IntPtr lib = LoadLibraryW(dll);
            if (lib == IntPtr.Zero) { log.LogWarning("Could not load " + dll + " (" + Marshal.GetLastWin32Error() + ")"); return; }
            IntPtr fn = GetProcAddress(lib, "PFSR_InstallSwapchainHook");
            if (fn == IntPtr.Zero) { log.LogWarning("ParalivesUpscaling.dll has no PFSR_InstallSwapchainHook (old version?)"); return; }
            int ok = ((InstallSwapchainHook)Marshal.GetDelegateForFunctionPointer(fn, typeof(InstallSwapchainHook)))(mode);
            log.LogInfo(ok != 0 ? "Swapchain hook installed (" + (xess ? "XeSS" : "FSR") + " frame generation)" : "Swapchain hook failed; see ParalivesUpscaling.log");
        }
    }
}
