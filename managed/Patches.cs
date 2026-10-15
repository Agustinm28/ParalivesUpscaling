// ParalivesUpscaling - game hooks.
using System;
using System.Reflection;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering.Universal;

namespace ParalivesUpscaling
{
    /// <summary>
    /// Grass/flowers are queued (Graphics.DrawMeshInstanced) for the game camera only, which draws no world while
    /// FSR is on. Queue them for every camera instead: only our render camera has layer 0 in its culling mask.
    /// </summary>
    [HarmonyPatch(typeof(Terrain3dMeshesManager), nameof(Terrain3dMeshesManager.Render3dMeshes))]
    internal static class Render3dMeshesPatch
    {
        private static void Prefix(ref Camera camera)
        {
            var c = FsrController.Instance;
            if (c != null && c.Active && camera != null && camera == c.MainCamera)
                camera = null;
        }
    }

    /// <summary>
    /// The game's ambient occlusion (HBAO renderer feature) only enqueues itself for cameras with post-processing
    /// on. Our render camera has post off (post runs after FSR, on the game camera), so let HBAO see it as on just
    /// while it decides; and skip it on the game camera, which draws no world while FSR is on.
    /// Looked up by name: if the game ever drops HBAO this hook simply isn't installed.
    /// </summary>
    [HarmonyPatch]
    internal static class HbaoAddRenderPassesPatch
    {
        private static MethodBase TargetMethod()
        {
            return AccessTools.Method(AccessTools.TypeByName("HorizonBasedAmbientOcclusion.Universal.HBAORendererFeature"), "AddRenderPasses");
        }

        private static bool Prefix(ref RenderingData renderingData, out bool __state)
        {
            __state = false;
            var c = FsrController.Instance;
            if (c == null || !c.Active) return true;
            Camera cam = renderingData.cameraData.camera;
            if (cam == c.MainCamera && c.MainHeld) return false;
            __state = cam == c.RenderCamera && !renderingData.cameraData.postProcessEnabled;
            if (__state) renderingData.cameraData.postProcessEnabled = true;
            return true;
        }

        private static void Postfix(ref RenderingData renderingData, bool __state)
        {
            if (__state) renderingData.cameraData.postProcessEnabled = false;
        }
    }

    /// <summary>Save-game thumbnails render the game camera by hand: let them see the normal camera.</summary>
    [HarmonyPatch(typeof(HybridCamera), nameof(HybridCamera.GenerateSavedGameThumbnail))]
    internal static class ThumbnailPatch
    {
        private static void Prefix() { FsrController.Instance?.Suspend(); }

        private static Exception Finalizer(Exception __exception)
        {
            FsrController.Instance?.Resume();
            return __exception;
        }
    }
}
