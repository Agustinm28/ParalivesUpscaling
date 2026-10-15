// ParalivesFSR - interop with ParalivesFSR.dll (Paralives_Data\Plugins\x86_64).
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace ParalivesFSR
{
    /// <summary>Must match FrameData in ParalivesFSR.cpp.</summary>
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
        public int Gen, Pad;
    }

    internal static class Native
    {
        private const string Dll = "ParalivesFSR";

        internal const int EvCreate = 1, EvDispatch = 2, EvDestroy = 3;
        internal const int StState = 0, StDispatches = 1, StDispatchesSinceCreate = 2, StCreatedGen = 3;

        // FfxApiCreateContextUpscaleFlags
        internal const int FlagJitterCancellation = 1 << 2, FlagDepthInverted = 1 << 3;

        [DllImport(Dll)] internal static extern IntPtr PFSR_GetRenderEventFunc();
        [DllImport(Dll)] internal static extern int PFSR_GetStatus(int which);
        [DllImport(Dll)] private static extern int PFSR_GetText(int which, StringBuilder buf, int cap);

        internal static string VersionName { get { return Text(0); } }
        internal static string LastError { get { return Text(1); } }

        private static string Text(int which)
        {
            var sb = new StringBuilder(512);
            PFSR_GetText(which, sb, sb.Capacity);
            return sb.ToString();
        }
    }
}
