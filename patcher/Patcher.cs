// ParalivesUpscaling.Patcher - BepInEx 5 preloader patcher. Runs before Unity creates its swapchain and installs the
// native plugin's DXGI swapchain hook, which FSR frame generation needs. Patches no assemblies.
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
            // Same file as the plugin's settings; BepInEx keeps entries the other side does not bind.
            var cfg = new ConfigFile(Path.Combine(Paths.ConfigPath, "paralives.upscaling.cfg"), true);
            int mode = cfg.Bind("FrameGeneration", "SwapchainHook", 1,
                "Frame generation test (phase 0). 0 = off, 1 = only log swapchain creation, 2 = present through FSR's frame generation swapchain (frame generation itself stays off).").Value;
            if (mode == 0) return;

            string dll = Path.Combine(Path.Combine(Path.Combine(Paths.GameRootPath, "Paralives_Data"), "Plugins"), Path.Combine("x86_64", "ParalivesUpscaling.dll"));
            IntPtr lib = LoadLibraryW(dll);
            if (lib == IntPtr.Zero) { log.LogWarning("Could not load " + dll + " (" + Marshal.GetLastWin32Error() + ")"); return; }
            IntPtr fn = GetProcAddress(lib, "PFSR_InstallSwapchainHook");
            if (fn == IntPtr.Zero) { log.LogWarning("ParalivesUpscaling.dll has no PFSR_InstallSwapchainHook (old version?)"); return; }
            int ok = ((InstallSwapchainHook)Marshal.GetDelegateForFunctionPointer(fn, typeof(InstallSwapchainHook)))(mode);
            log.LogInfo(ok != 0 ? "Swapchain hook installed (mode " + mode + ")" : "Swapchain hook failed; see ParalivesUpscaling.log");
        }
    }
}
