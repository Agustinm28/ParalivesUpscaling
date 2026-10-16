// ParalivesUpscaling - BepInEx 5 entry point and settings.
using System;
using BepInEx;
using BepInEx.Configuration;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;

namespace ParalivesUpscaling
{
    // Quality preset (render-resolution scale).
    public enum FsrMode { NativeAA, Quality, Balanced, Performance, UltraPerformance }

    // Upscaling technology; Off switches the mod off. The native side treats 10 as Intel XeSS and 20 as NVIDIA DLSS
    // (both fall back to FSR if they cannot start) and FSR4 falls back to the newest FSR the driver offers.
    public enum FsrVersion { Off = 0, FSR3 = 3, FSR4 = 4, XeSS = 10, DLSS = 20 }

    // Frame generation technology. Its proxy swapchain is set up at game start (BepInEx preloader patcher), so switching
    // between FSR and XeSS takes a restart; FSR can be switched on and off live.
    public enum FrameGenTech { Off, FSR, XeSS }

    [BepInPlugin(Id, "Paralives Upscaling", Version)]
    [BepInProcess("Paralives.exe")]
    public sealed class Plugin : BaseUnityPlugin
    {
        public const string Id = "paralives.upscaling";
        public const string Version = "1.0.0";

        internal static ManualLogSource Log;
        internal static Settings O;

        private void Awake()
        {
            Log = Logger;
            O = new Settings(Config);
            var harmony = new Harmony(Id);
            foreach (var t in typeof(Plugin).Assembly.GetTypes())
            {
                if (!t.IsDefined(typeof(HarmonyPatch), false)) continue;
                try { harmony.CreateClassProcessor(t).Patch(); }
                catch (Exception e) { Log.LogWarning("Hook " + t.Name + " not installed: " + e.Message); }
            }
            var go = new GameObject("ParalivesUpscaling.Controller") { hideFlags = HideFlags.HideAndDontSave };
            DontDestroyOnLoad(go);
            go.AddComponent<FsrController>();
            Log.LogInfo("Paralives Upscaling " + Version + " loaded. Mode: " + O.Mode.Value + ", version: " + O.Version.Value);
        }
    }

    internal sealed class Settings
    {
        public readonly ConfigEntry<FsrMode> Mode;
        public readonly ConfigEntry<FsrVersion> Version;
        public readonly ConfigEntry<float> Sharpness, CharacterReactive;
        public readonly ConfigEntry<bool> FullResolutionAO, ShowInfo;
        public readonly ConfigEntry<float> JitterSignX, JitterSignY, MotionSignX, MotionSignY;
        public readonly ConfigEntry<bool> JitterCancellation, PresentFlipY, DebugView, PerfLog;
        public readonly ConfigEntry<FrameGenTech> FrameGeneration;
        public readonly ConfigEntry<bool> FrameGenerationSwapchain;

        public Settings(ConfigFile c)
        {
            Version = c.Bind("General", "Version", FsrVersion.FSR4,
                "Off, FSR3, FSR4, XeSS or DLSS. FSR4 needs a Radeon RX 9000 (or RX 7000 with recent drivers) and falls back to FSR 3 elsewhere. XeSS (best on Intel Arc, works on most DirectX 12 GPUs) and DLSS (NVIDIA RTX only) fall back to FSR if they cannot start.");
            Mode = c.Bind("General", "Mode", FsrMode.Quality,
                "NativeAA (1.0x), Quality (1.5x), Balanced (1.7x), Performance (2.0x), UltraPerformance (3.0x).");
            Sharpness = c.Bind("General", "Sharpness", 0.3f, "0 = off, 1 = maximum.");
            ShowInfo = c.Bind("General", "ShowInfo", false,
                "Show which upscaler is actually running (FSR 3, FSR 4, XeSS or DLSS) and its resolutions at the top of the screen.");
            // Calibration knobs: Unity's projection / motion-vector conventions vs FSR's. Flip one if the image misbehaves.
            JitterSignX = c.Bind("Advanced", "JitterSignX", -1f, "Try 1 if the image is soft or shimmers while standing still.");
            JitterSignY = c.Bind("Advanced", "JitterSignY", -1f, "Try 1 if the image is soft or shimmers while standing still.");
            MotionSignX = c.Bind("Advanced", "MotionVectorSignX", -1f, "Try 1 if things smear sideways while the camera moves.");
            MotionSignY = c.Bind("Advanced", "MotionVectorSignY", -1f, "Try 1 if things smear up/down while the camera moves.");
            JitterCancellation = c.Bind("Advanced", "JitterCancellation", true,
                "URP's motion vectors are built with our jittered projection, so FSR must cancel the jitter. Try false if still images wobble.");
            PresentFlipY = c.Bind("Advanced", "PresentFlipY", false, "Only change if the 3D world shows upside down.");
            FullResolutionAO = c.Bind("Advanced", "FullResolutionAO", true,
                "While upscaling, compute the game's ambient occlusion at full render resolution instead of half (sharper contact shadows, a bit more GPU).");
            CharacterReactive = c.Bind("Advanced", "CharacterReactive", 0.6f,
                "FSR 3 only: how much FSR trusts the current frame on characters (0-1). Higher = less trailing on walking characters, slightly less smoothing.");
            PerfLog = c.Bind("Advanced", "PerfLog", false,
                "Every 2 s, log frame rate, CPU/GPU frame times, render resolution and every camera rendered. Troubleshooting only.");
            FrameGeneration = c.Bind("General", "FrameGeneration", FrameGenTech.Off,
                "Off, FSR or XeSS: a generated frame between every two rendered frames. Needs an upscaling technology (not Off). " +
                "Switching between FSR and XeSS takes effect after restarting the game.");
            FrameGenerationSwapchain = c.Bind("Advanced", "FrameGenerationSwapchain", true,
                "Present through the frame generation swapchain, set up at game start (needed for frame generation, harmless while it is off). " +
                "Set to false only if the game fails to start or an overlay misbehaves; frame generation is then unavailable.");
            DebugView = c.Bind("Advanced", "DebugView", false,
                "Show FSR's debug view (motion vectors, masks, history) instead of the image. Troubleshooting only; may not be supported by FSR 4.");
        }
    }
}
