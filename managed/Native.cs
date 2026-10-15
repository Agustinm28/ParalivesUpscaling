// ParalivesUpscaling - interop with ParalivesUpscaling.dll (Paralives_Data\Plugins\x86_64).
using System;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;

namespace ParalivesUpscaling
{
    /// <summary>Must match FrameData in ParalivesUpscaling.cpp.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct FrameData
    {
        public IntPtr Color, Depth, Motion, Output;
        public int RenderW, RenderH, OutW, OutH;
        public float JitterX, JitterY, MvScaleX, MvScaleY;
        public float FrameTimeMs, Near, Far, FovY;
        public float Sharpness;
        public int Reset;
        public int CreateFlags, Version;
        public int Gen, DispatchFlags;
        public IntPtr DepthAtt, MotionOut;
        public float MvThreshold;
        public int MvMode;
        public Matrix4x4 InvViewProj, PrevViewProj;
        public IntPtr Reactive;
        public float ReactiveStrength;
        public int Pad;
    }

    internal static class Native
    {
        private const string Dll = "ParalivesUpscaling";

        internal const int EvCreate = 1, EvDispatch = 2, EvDestroy = 3;
        internal const int StState = 0, StDispatches = 1, StDispatchesSinceCreate = 2, StCreatedGen = 3;

        // FfxApiCreateContextUpscaleFlags
        internal const int FlagHighDynamicRange = 1 << 0, FlagJitterCancellation = 1 << 2, FlagDepthInverted = 1 << 3, FlagDebugVisualization = 1 << 9;
        // FfxApiDispatchFsrUpscaleFlags
        internal const int DispatchDrawDebugView = 1 << 0;

        [DllImport(Dll)] internal static extern IntPtr PFSR_GetRenderEventFunc();
        [DllImport(Dll)] internal static extern int PFSR_GetStatus(int which);
        [DllImport(Dll)] private static extern int PFSR_GetText(int which, StringBuilder buf, int cap);

        internal static string VersionName { get { return Text(0); } }
        internal static string LastError { get { return Text(1); } }

        // Versions this GPU/driver offers ("" until the runtime is up, or if not offered); cached once known.
        private static string _fsr3, _fsr4, _xess, _dlss;
        internal static string Fsr3Name { get { return Offered(ref _fsr3, 2); } }
        internal static string Fsr4Name { get { return Offered(ref _fsr4, 3); } }
        internal static string XessName { get { return Offered(ref _xess, 4); } }
        internal static string DlssName { get { return Offered(ref _dlss, 5); } }  // "" = this GPU cannot run DLSS

        private static string Offered(ref string cache, int which)
        {
            if (string.IsNullOrEmpty(cache))
            {
                try { cache = Text(which); } catch { cache = ""; }  // DLL missing: no names
            }
            return cache;
        }

        private static string Text(int which)
        {
            var sb = new StringBuilder(512);
            PFSR_GetText(which, sb, sb.Capacity);
            return sb.ToString();
        }
    }
}
