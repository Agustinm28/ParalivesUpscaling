// ParalivesFSR - game hooks (same two the DLSS mod needs).
using System;
using HarmonyLib;
using UnityEngine;

namespace ParalivesFSR
{
    /// <summary>Grass/flowers are drawn for one specific camera: hand them to our render camera while FSR is on.</summary>
    [HarmonyPatch(typeof(Terrain3dMeshesManager), nameof(Terrain3dMeshesManager.Render3dMeshes))]
    internal static class Render3dMeshesPatch
    {
        private static void Prefix(ref Camera camera)
        {
            var c = FsrController.Instance;
            if (c != null && c.Active && camera != null && camera == c.MainCamera && c.RenderCamera != null)
                camera = c.RenderCamera;
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
