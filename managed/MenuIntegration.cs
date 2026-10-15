// ParalivesFSR - Settings > Video integration.
//
// The options menu (UIOptions) hands each category's Setting objects to the game's UIInspector, which draws native
// dropdowns / sliders for their public fields by reflection. We add one plain object to the Video category. It is
// not a game Setting, so the inspector writes its fields directly and saves nothing to the game's mod files; we
// mirror the fields to our BepInEx config every frame, and FsrController picks the change up live.
// Without DirectX 12 we add FsrOptionsNeedsDx12 instead: same fields, greyed out, under a header that explains
// what to do.
using System.Collections.Generic;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;

namespace ParalivesFSR
{
    // Labels, entries and tooltips are "ParalivesFSR_*" translation keys (see Localization.cs).
    [TranslatedLabel("ParalivesFSR_")]
    public sealed class FsrOptions
    {
        // field order = menu order: technology (with Off) first, then the quality preset
        [TranslatedText("ParalivesFSR_Version_")]
        [Tooltip("ParalivesFSR_Tip_UpscalerVersion")]
        public FsrVersion UpscalerVersion;

        [TranslatedText("ParalivesFSR_Mode_")]
        [Tooltip("ParalivesFSR_Tip_Upscaler")]
        public FsrMode Upscaler;

        [Slider(0f, 1f)]
        [Round(2)]
        [Tooltip("ParalivesFSR_Tip_UpscalerSharpness")]
        public float UpscalerSharpness;

        [Tooltip("ParalivesFSR_Tip_UpscalerShowInfo")]
        public bool UpscalerShowInfo;

        internal static readonly FsrOptions Instance = new FsrOptions();

        private FsrMode _mode;
        private FsrVersion _version;
        private float _sharpness;
        private bool _showInfo;
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
                if (UpscalerShowInfo != _showInfo) o.ShowInfo.Value = UpscalerShowInfo;
            }
            Upscaler = _mode = o.Mode.Value;
            UpscalerVersion = _version = o.Version.Value;
            UpscalerSharpness = _sharpness = o.Sharpness.Value;
            UpscalerShowInfo = _showInfo = o.ShowInfo.Value;
            _initialised = true;
            FsrOptionsNeedsDx12.Instance.Show(this);
        }
    }

    /// <summary>Read-only twin of FsrOptions, shown when the game is not running on DirectX 12.</summary>
    [TranslatedLabel("ParalivesFSR_")]
    public sealed class FsrOptionsNeedsDx12
    {
        [Header("ParalivesFSR_NeedsDx12")]
        [TranslatedText("ParalivesFSR_Version_")]
        [Tooltip("ParalivesFSR_Tip_NeedsDx12")]
        public FsrVersion UpscalerVersion;

        [TranslatedText("ParalivesFSR_Mode_")]
        [Tooltip("ParalivesFSR_Tip_NeedsDx12")]
        public FsrMode Upscaler;

        [Slider(0f, 1f)]
        [Round(2)]
        [DisableInspectorEdition]
        [Tooltip("ParalivesFSR_Tip_NeedsDx12")]
        public float UpscalerSharpness;

        internal static readonly FsrOptionsNeedsDx12 Instance = new FsrOptionsNeedsDx12();

        internal void Show(FsrOptions o)
        {
            Upscaler = o.Upscaler;
            UpscalerVersion = o.UpscalerVersion;
            UpscalerSharpness = o.UpscalerSharpness;
        }

        internal static bool Needed { get { return SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D12; } }
    }

    [HarmonyPatch(typeof(UIOptions), "RefreshInspectedObjects")]
    internal static class OptionsVideoPatch
    {
        private static void Postfix(List<object> ____currentlyInspectedObjects)
        {
            var list = ____currentlyInspectedObjects;
            object options = FsrOptionsNeedsDx12.Needed ? (object)FsrOptionsNeedsDx12.Instance : FsrOptions.Instance;
            if (list.Exists(o => o is Setting.Video) && !list.Contains(options))
                list.Add(options);
        }
    }

    /// <summary>
    /// Greys out and blocks every row of FsrOptionsNeedsDx12 (the game's dropdowns ignore DisableInspectorEdition).
    /// Rows are pooled and reused, so a row we locked is unlocked again when it is set up for something else.
    /// </summary>
    [HarmonyPatch(typeof(UIField), nameof(UIField.Init), typeof(UIInspectorComponentVisual))]
    internal static class LockedFieldPatch
    {
        private sealed class Locked : MonoBehaviour { public CanvasGroup Group; }

        private static void Postfix(UIField __instance, UIInspectorComponentVisual componentVisual)
        {
            try
            {
                GameObject row = __instance.UIFieldGameObject != null ? __instance.UIFieldGameObject.gameObject : null;
                if (row == null) return;
                bool lockIt = componentVisual != null && componentVisual.ClassObject is FsrOptionsNeedsDx12 && !(__instance is UIFieldHeader);
                var locked = row.GetComponent<Locked>();
                if (lockIt)
                {
                    if (locked == null)
                    {
                        locked = row.AddComponent<Locked>();
                        locked.Group = row.GetComponent<CanvasGroup>() ?? row.AddComponent<CanvasGroup>();
                    }
                    locked.Group.alpha = 0.45f;
                    locked.Group.interactable = false;
                    locked.Group.blocksRaycasts = false;
                }
                else if (locked != null)
                {
                    locked.Group.alpha = 1f;
                    locked.Group.interactable = true;
                    locked.Group.blocksRaycasts = true;
                    Object.Destroy(locked);
                }
            }
            catch { }
        }
    }
}
