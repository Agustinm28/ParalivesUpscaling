// ParalivesFSR - URP 17 Render Graph passes, enqueued per camera from FsrController.
using System;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;
using UnityEngine.Rendering.RenderGraphModule.Util;
using UnityEngine.Rendering.Universal;

namespace ParalivesFSR
{
    /// <summary>
    /// Render camera: copy depth + URP motion vectors into our persistent textures (their native pointers are
    /// cached, unlike the graph's pooled ones), then run FSR on the render thread.
    /// </summary>
    internal sealed class FsrPass : ScriptableRenderPass
    {
        private sealed class PassData
        {
            public TextureHandle Depth, Motion, DepthCopy, MotionCopy;
            public IntPtr Event, Frame;
        }

        public FsrPass()
        {
            // After URP's final blit into our colour texture.
            renderPassEvent = RenderPassEvent.AfterRendering + 10;
            ConfigureInput(ScriptableRenderPassInput.Depth | ScriptableRenderPassInput.Motion);
        }

        public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameData)
        {
            var c = FsrController.Instance;
            if (c == null || !c.Active || c.DepthHandle == null) return;
            var res = frameData.Get<UniversalResourceData>();
            if (!res.cameraDepthTexture.IsValid() || !res.motionVectorColor.IsValid() || !res.backBufferColor.IsValid()) return;

            using (var builder = renderGraph.AddUnsafePass<PassData>("ParalivesFSR upscale", out var data))
            {
                data.Depth = res.cameraDepthTexture;
                data.Motion = res.motionVectorColor;
                data.DepthCopy = renderGraph.ImportTexture(c.DepthHandle);
                data.MotionCopy = renderGraph.ImportTexture(c.MotionHandle);
                data.Event = c.EventFunc;
                data.Frame = c.FramePtr;
                builder.UseTexture(data.Depth);
                builder.UseTexture(data.Motion);
                builder.UseTexture(res.backBufferColor);  // orders us after the final blit into it
                builder.UseTexture(data.DepthCopy, AccessFlags.Write);
                builder.UseTexture(data.MotionCopy, AccessFlags.Write);
                builder.AllowPassCulling(false);
                builder.SetRenderFunc((PassData p, UnsafeGraphContext ctx) =>
                {
                    CommandBuffer cmd = CommandBufferHelpers.GetNativeCommandBuffer(ctx.cmd);
                    Blitter.BlitCameraTexture(cmd, p.Depth, p.DepthCopy);
                    Blitter.BlitCameraTexture(cmd, p.Motion, p.MotionCopy);
                    cmd.IssuePluginEventAndData(p.Event, Native.EvDispatch, p.Frame);
                });
            }
        }
    }

    /// <summary>
    /// Game camera (drawing no world while FSR is on): replace its image right after opaques, so the opaque
    /// texture, translucent UI panels and the stacked UI camera all see the FSR image.
    /// </summary>
    internal sealed class PresentPass : ScriptableRenderPass
    {
        public PresentPass()
        {
            renderPassEvent = RenderPassEvent.AfterRenderingOpaques;
        }

        public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameData)
        {
            var c = FsrController.Instance;
            if (c == null || !c.Active || c.PresentHandle == null) return;
            var res = frameData.Get<UniversalResourceData>();
            TextureHandle src = renderGraph.ImportTexture(c.PresentHandle);
            bool flip = c.FlipPresent;
            renderGraph.AddBlitPass(src, res.activeColorTexture,
                flip ? new Vector2(1f, -1f) : Vector2.one, flip ? new Vector2(0f, 1f) : Vector2.zero,
                passName: "ParalivesFSR present");
        }
    }
}
