// ParalivesFSR - BepInEx 5 entry point and settings.
using System;
using BepInEx;
using BepInEx.Configuration;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;

namespace ParalivesFSR
{
    public enum FsrMode { Off, NativeAA, Quality, Balanced, Performance, UltraPerformance }

    public enum FsrVersion { Auto = 0, FSR3 = 3, FSR4 = 4 }

    [BepInPlugin(Id, "Paralives FSR", Version)]
    [BepInProcess("Paralives.exe")]
    public sealed class Plugin : BaseUnityPlugin
    {
        public const string Id = "paralives.fsr";
        public const string Version = "0.1.0";

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
            var go = new GameObject("ParalivesFSR.Controller") { hideFlags = HideFlags.HideAndDontSave };
            DontDestroyOnLoad(go);
            go.AddComponent<FsrController>();
            Log.LogInfo("Paralives FSR " + Version + " loaded. Mode: " + O.Mode.Value + ", version: " + O.Version.Value);
        }
    }

    internal sealed class Settings
    {
        public readonly ConfigEntry<FsrMode> Mode;
        public readonly ConfigEntry<FsrVersion> Version;
        public readonly ConfigEntry<float> Sharpness, CharacterReactive;
        public readonly ConfigEntry<bool> FullResolutionAO;
        public readonly ConfigEntry<float> JitterSignX, JitterSignY, MotionSignX, MotionSignY;
        public readonly ConfigEntry<bool> JitterCancellation, PresentFlipY, DebugView, PerfLog;

        public Settings(ConfigFile c)
        {
            Mode = c.Bind("General", "Mode", FsrMode.Quality,
                "Off, NativeAA (1.0x), Quality (1.5x), Balanced (1.7x), Performance (2.0x), UltraPerformance (3.0x).");
            Version = c.Bind("General", "Version", FsrVersion.Auto,
                "Auto uses the best version your GPU and driver support (FSR 4 on Radeon RX 9000, and RX 7000 with recent drivers; FSR 3 otherwise). FSR3 / FSR4 force one; FSR4 falls back to FSR 3 if unsupported.");
            Sharpness = c.Bind("General", "Sharpness", 0.3f, "0 = off, 1 = maximum.");
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
            DebugView = c.Bind("Advanced", "DebugView", false,
                "Show FSR's debug view (motion vectors, masks, history) instead of the image. Troubleshooting only; may not be supported by FSR 4.");
        }
    }
}
