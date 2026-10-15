// ParalivesFSR - Settings > Video integration.
//
// The options menu (UIOptions) hands each category's Setting objects to the game's UIInspector, which draws native
// dropdowns / sliders for their public fields by reflection. We add one plain object to the Video category. It is
// not a game Setting, so the inspector writes its fields directly and saves nothing to the game's mod files; we
// mirror the fields to our BepInEx config every frame, and FsrController picks the change up live.
using System.Collections.Generic;
using HarmonyLib;
using UnityEngine;

namespace ParalivesFSR
{
    public sealed class FsrOptions
    {
        [Tooltip("AMD FSR upscaling: Native AA (1.0x), Quality (1.5x), Balanced (1.7x), Performance (2.0x), Ultra Performance (3.0x). Needs -force-d3d12.")]
        public FsrMode Upscaler;

        [Tooltip("Auto uses the best version your GPU and driver support (FSR 4 on Radeon RX 9000, and RX 7000 with recent drivers; FSR 3 otherwise). FSR3 / FSR4 force one; FSR4 falls back to FSR 3 if unsupported.")]
        public FsrVersion UpscalerVersion;

        [Slider(0f, 1f)]
        [Round(2)]
        [Tooltip("FSR sharpening. 0 = off.")]
        public float UpscalerSharpness;

        internal static readonly FsrOptions Instance = new FsrOptions();

        private FsrMode _mode;
        private FsrVersion _version;
        private float _sharpness;
        private bool _initialised;

        /// <summary>Called every frame: menu edits go to the config, config changes go to the menu.</summary>
        internal void Sync()
        {
            var o = Plugin.O;
            if (_initialised)
            {
                if (Upscaler != _mode) o.Mode.Value = Upscaler;
                if (UpscalerVersion != _version) o.Version.Value = UpscalerVersion;
                if (!Mathf.Approximately(UpscalerSharpness, _sharpness)) o.Sharpness.Value = UpscalerSharpness;
            }
            Upscaler = _mode = o.Mode.Value;
            UpscalerVersion = _version = o.Version.Value;
            UpscalerSharpness = _sharpness = o.Sharpness.Value;
            _initialised = true;
        }
    }

    [HarmonyPatch(typeof(UIOptions), "RefreshInspectedObjects")]
    internal static class OptionsVideoPatch
    {
        private static void Postfix(List<object> ____currentlyInspectedObjects)
        {
            var list = ____currentlyInspectedObjects;
            if (list.Exists(o => o is Setting.Video) && !list.Contains(FsrOptions.Instance))
                list.Add(FsrOptions.Instance);
        }
    }
}
