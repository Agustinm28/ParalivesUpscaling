// ParalivesFSR - camera management (same scheme as the DLSS mod, ported to URP 17):
//  * A hidden render camera copies the game camera every frame and renders the world, with the game's
//    post-processing, into a low-resolution texture through a jittered projection. URP also renders depth and
//    motion vectors (camera + per-object) for it.
//  * FsrPass, on that camera, copies depth + motion vectors and runs FSR into a full-resolution output.
//  * While the game camera renders it draws no world; PresentPass copies the FSR output into it and the
//    game's UI camera (stacked on top) then draws the UI at full resolution.
using System;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

namespace ParalivesFSR
{
    public sealed unsafe class FsrController : MonoBehaviour
    {
        internal static FsrController Instance;
        private static Settings O { get { return Plugin.O; } }

        private const int Slots = 16;

        internal bool Active { get { return _active && !_suspended; } }
        internal Camera MainCamera { get { return _main; } }
        internal Camera RenderCamera { get { return _render; } }
        internal RTHandle DepthHandle { get { return _hDepth; } }
        internal RTHandle MotionHandle { get { return _hMotion; } }
        internal RTHandle PresentHandle { get { return _presentingFsr ? _hOutput : _hColor; } }
        internal bool FlipPresent { get { return O.PresentFlipY.Value; } }
        internal IntPtr FramePtr { get { return (IntPtr)_frame; } }
        internal IntPtr EventFunc { get; private set; }

        private bool _active, _suspended, _disabledByError, _presentingFsr;
        private FsrMode _activeMode;
        private FsrVersion _activeVersion;
        private bool _activeCancel;
        private Camera _main, _render;
        private UniversalAdditionalCameraData _mainData, _renderData;
        private RenderTexture _color, _depth, _motion, _output;
        private RTHandle _hColor, _hDepth, _hMotion, _hOutput;
        private IntPtr _pColor, _pDepth, _pMotion, _pOutput;
        private int _inW, _inH, _outW, _outH;
        private int _jitterIndex, _jitterPhases, _resetFrames, _createGen, _preparedFrame = -1;
        private float _jx, _jy;
        private Vector3 _lastPos;
        private string _reportedVersion;

        // game-camera settings held while it renders
        private bool _mainHeld;
        private int _heldMask;
        private CameraClearFlags _heldClear;
        private Color _heldBg;
        private bool _heldPost, _heldShadows;
        private AntialiasingMode _heldAA;

        // URP asset render scale held while the render camera renders
        private bool _scaleHeld;
        private float _heldScale;

        private FrameData* _slots, _frame;
        private int _slot;
        private CommandBuffer _cbCtl;
        private readonly FsrPass _fsrPass = new FsrPass();
        private readonly PresentPass _presentPass = new PresentPass();

        // =====================================================================================
        // Unity lifecycle
        // =====================================================================================
        private void Awake()
        {
            Instance = this;
            _slots = (FrameData*)Marshal.AllocHGlobal(sizeof(FrameData) * Slots);
            _frame = _slots;
            *_frame = default(FrameData);
            _cbCtl = new CommandBuffer { name = "ParalivesFSR control" };
            try
            {
                EventFunc = Native.PFSR_GetRenderEventFunc();  // first call loads the DLL -> UnityPluginLoad
            }
            catch (Exception e)
            {
                Plugin.Log.LogError("ParalivesFSR.dll could not be loaded (it belongs in Paralives_Data\\Plugins\\x86_64): " + e.Message);
                enabled = false;
            }
            O.Mode.SettingChanged += (s, e) => { _disabledByError = false; };
        }

        private void OnEnable()
        {
            RenderPipelineManager.beginCameraRendering += OnBeginCamera;
            RenderPipelineManager.endCameraRendering += OnEndCamera;
        }

        private void OnDisable()
        {
            RenderPipelineManager.beginCameraRendering -= OnBeginCamera;
            RenderPipelineManager.endCameraRendering -= OnEndCamera;
            ReleaseMain();
            ReleaseScale();
        }

        private void OnDestroy()
        {
            try { if (_active) Teardown(); } catch { }
        }

        private void Update()
        {
            // the game camera can never legitimately be held here; recover if URP skipped endCameraRendering
            ReleaseMain();
            if (EventFunc == IntPtr.Zero) return;
            try
            {
                if (_active)
                {
                    int state = Native.PFSR_GetStatus(Native.StState);
                    if (state == -1) Fail("FSR could not start: " + Native.LastError);
                    else if (state == -2 && Native.PFSR_GetStatus(Native.StCreatedGen) == _createGen) Fail("FSR stopped: " + Native.LastError);
                }

                Camera target = (O.Mode.Value != FsrMode.Off && !_disabledByError) ? FindGameCamera() : null;
                bool usable = target != null && target.isActiveAndEnabled && target.targetTexture == null
                              && Screen.width >= 64 && Screen.height >= 64;
                if (!usable)
                {
                    if (_active) Teardown();
                    return;
                }
                if (_active && target != _main) Teardown();
                if (!_active && !Setup(target)) return;

                if (O.Mode.Value != _activeMode || O.Version.Value != _activeVersion || O.JitterCancellation.Value != _activeCancel
                    || Screen.width != _outW || Screen.height != _outH)
                    Rebuild();

                _presentingFsr = Native.PFSR_GetStatus(Native.StState) == 2
                                 && Native.PFSR_GetStatus(Native.StCreatedGen) == _createGen
                                 && Native.PFSR_GetStatus(Native.StDispatchesSinceCreate) > 1;
                if (_presentingFsr && _reportedVersion == null)
                {
                    _reportedVersion = Native.VersionName;
                    Plugin.Log.LogInfo("FSR running: " + _reportedVersion + ", " + _inW + "x" + _inH + " -> " + _outW + "x" + _outH + ".");
                }
                _render.depth = _main.depth - 1f;
            }
            catch (Exception e)
            {
                Plugin.Log.LogError("FSR error: " + e);
                Fail("FSR error: " + e.Message);
            }
        }

        private static Camera FindGameCamera()
        {
            try
            {
                var pm = PlayerManager.Instance;
                if (pm != null && pm.HybridPlayer1 != null && pm.HybridPlayer1.HybridCamera != null && pm.HybridPlayer1.HybridCamera.Camera != null)
                    return pm.HybridPlayer1.HybridCamera.Camera;
            }
            catch { }
            return Camera.main;
        }

        // =====================================================================================
        // Setup / rebuild / teardown
        // =====================================================================================
        private bool Setup(Camera main)
        {
            if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D12)
            {
                Fail("FSR needs DirectX 12 (game is running " + SystemInfo.graphicsDeviceType + "). Add -force-d3d12 to the Steam launch options.");
                return false;
            }
            if (Native.PFSR_GetStatus(Native.StState) < 1)
            {
                Fail("FSR runtime not ready: " + Native.LastError);
                return false;
            }
            _mainData = main.GetUniversalAdditionalCameraData();
            if (_mainData == null) { Fail("Game camera has no URP camera data."); return false; }

            _main = main;
            var go = new GameObject("ParalivesFSR.RenderCamera") { hideFlags = HideFlags.HideAndDontSave };
            DontDestroyOnLoad(go);
            go.SetActive(false);
            _render = go.AddComponent<Camera>();
            _render.CopyFrom(main);
            _render.tag = "Untagged";
            _render.targetTexture = null;
            _render.depth = main.depth - 1f;
            _renderData = _render.GetUniversalAdditionalCameraData();
            _renderData.renderType = CameraRenderType.Base;
            int idx = RendererIndexOf(_mainData);
            if (idx >= 0) _renderData.SetRenderer(idx);

            _active = true;
            _lastPos = main.transform.position;
            Rebuild();
            go.SetActive(true);
            _render.enabled = true;
            Plugin.Log.LogInfo("FSR render path active (" + _activeMode + ").");
            return true;
        }

        private static float Ratio(FsrMode m)
        {
            switch (m)
            {
                case FsrMode.Quality: return 1.5f;
                case FsrMode.Balanced: return 1.7f;
                case FsrMode.Performance: return 2f;
                case FsrMode.UltraPerformance: return 3f;
                default: return 1f;  // NativeAA
            }
        }

        private void Rebuild()
        {
            _activeMode = O.Mode.Value;
            _activeVersion = O.Version.Value;
            _activeCancel = O.JitterCancellation.Value;
            float ratio = Ratio(_activeMode);
            _outW = Screen.width;
            _outH = Screen.height;
            _inW = Math.Max(64, Mathf.RoundToInt(_outW / ratio));
            _inH = Math.Max(64, Mathf.RoundToInt(_outH / ratio));
            _jitterPhases = Mathf.CeilToInt(8f * ratio * ratio);  // FSR's own phase-count formula

            IssueEvent(Native.EvDestroy, IntPtr.Zero);
            ReleaseTargets();
            _color = MakeRT(_inW, _inH, 24, RenderTextureFormat.ARGBHalf, false, "ParalivesFSR.Color");
            _depth = MakeRT(_inW, _inH, 0, RenderTextureFormat.RFloat, false, "ParalivesFSR.Depth");
            _motion = MakeRT(_inW, _inH, 0, RenderTextureFormat.RGHalf, false, "ParalivesFSR.Motion");
            _output = MakeRT(_outW, _outH, 0, RenderTextureFormat.ARGBHalf, true, "ParalivesFSR.Output");
            _hColor = RTHandles.Alloc(_color);
            _hDepth = RTHandles.Alloc(_depth);
            _hMotion = RTHandles.Alloc(_motion);
            _hOutput = RTHandles.Alloc(_output);
            _pColor = _color.GetNativeTexturePtr();
            _pDepth = _depth.GetNativeTexturePtr();
            _pMotion = _motion.GetNativeTexturePtr();
            _pOutput = _output.GetNativeTexturePtr();

            FrameData* f = NextSlot();
            _createGen++;
            FillCommon(f);
            f->CreateFlags = (SystemInfo.usesReversedZBuffer ? Native.FlagDepthInverted : 0)
                             | (_activeCancel ? Native.FlagJitterCancellation : 0);
            IssueEvent(Native.EvCreate, (IntPtr)f);

            _resetFrames = 3;
            _presentingFsr = false;
            _reportedVersion = null;
            Plugin.Log.LogInfo("FSR " + _activeMode + " (" + _activeVersion + "): rendering " + _inW + "x" + _inH +
                               ", output " + _outW + "x" + _outH + ", jitter phases " + _jitterPhases + ".");
        }

        private void Teardown()
        {
            _active = false;
            ReleaseMain();
            ReleaseScale();
            try
            {
                if (_render != null)
                {
                    // Destroy() only happens at end of frame: stop it rendering (to the screen!) right now
                    _render.enabled = false;
                    _render.targetTexture = null;
                    Destroy(_render.gameObject);
                }
            }
            catch { }
            try { IssueEvent(Native.EvDestroy, IntPtr.Zero); } catch { }
            ReleaseTargets();
            _render = null; _renderData = null;
            _main = null; _mainData = null;
            _activeMode = FsrMode.Off;
            _presentingFsr = false;
            Plugin.Log.LogInfo("FSR render path off; game camera restored.");
        }

        private void Fail(string why)
        {
            Plugin.Log.LogError(why);
            _disabledByError = true;
            if (_active) Teardown();
        }

        // Thumbnails (save-game pictures) render the game camera by hand: let them see the normal camera.
        internal void Suspend()
        {
            if (!_active || _suspended) return;
            _suspended = true;
            ReleaseMain();
            if (_render != null) _render.enabled = false;
        }

        internal void Resume()
        {
            if (!_suspended) return;
            _suspended = false;
            if (_active && _render != null) _render.enabled = true;
        }

        // =====================================================================================
        // Per-camera hooks (before culling of each camera)
        // =====================================================================================
        private void OnBeginCamera(ScriptableRenderContext ctx, Camera cam)
        {
            if (!_active || _suspended || cam == null) return;
            try
            {
                if (cam == _render)
                {
                    PrepareRenderCamera();
                    _renderData.scriptableRenderer.EnqueuePass(_fsrPass);
                }
                else if (cam == _main)
                {
                    HoldMain();
                    _mainData.scriptableRenderer.EnqueuePass(_presentPass);
                }
            }
            catch (Exception e)
            {
                Plugin.Log.LogError("FSR camera hook error: " + e.Message);
            }
        }

        private void OnEndCamera(ScriptableRenderContext ctx, Camera cam)
        {
            if (cam == null) return;
            if (cam == _render)
            {
                ReleaseScale();
                try { _render.ResetProjectionMatrix(); } catch { }
            }
            else if (cam == _main) ReleaseMain();
        }

        private void PrepareRenderCamera()
        {
            if (Time.frameCount == _preparedFrame) return;
            _preparedFrame = Time.frameCount;
            ReleaseMain();

            _render.CopyFrom(_main);
            _render.tag = "Untagged";
            _render.transform.SetPositionAndRotation(_main.transform.position, _main.transform.rotation);
            _render.targetTexture = _color;
            _render.depth = _main.depth - 1f;
            _render.allowMSAA = false;
            _render.allowDynamicResolution = false;

            _renderData.renderType = CameraRenderType.Base;
            _renderData.renderPostProcessing = _mainData.renderPostProcessing;
            _renderData.antialiasing = AntialiasingMode.None;
            _renderData.volumeLayerMask = _mainData.volumeLayerMask;
            _renderData.volumeTrigger = _mainData.volumeTrigger != null ? _mainData.volumeTrigger : _main.transform;
            _renderData.renderShadows = _mainData.renderShadows;
            _renderData.requiresDepthOption = CameraOverrideOption.On;
            _renderData.requiresColorOption = _mainData.requiresColorOption;
            _renderData.stopNaN = _mainData.stopNaN;
            _renderData.dithering = _mainData.dithering;
            if (_renderData.cameraStack != null && _renderData.cameraStack.Count > 0) _renderData.cameraStack.Clear();

            // the URP asset's render scale would shrink our already-reduced input further
            var asset = UniversalRenderPipeline.asset;
            if (asset != null && Mathf.Abs(asset.renderScale - 1f) > 0.001f)
            {
                _heldScale = asset.renderScale;
                asset.renderScale = 1f;
                _scaleHeld = true;
            }

            // jittered projection (sub-pixel offset that FSR accumulates over frames)
            _jitterIndex = (_jitterIndex + 1) % _jitterPhases;
            _jx = Halton(_jitterIndex + 1, 2) - 0.5f;
            _jy = Halton(_jitterIndex + 1, 3) - 0.5f;
            Matrix4x4 proj = _main.projectionMatrix;
            Matrix4x4 jittered = proj;
            if (_main.orthographic)
            {
                jittered.m03 -= _jx * 2f / _inW;
                jittered.m13 -= _jy * 2f / _inH;
            }
            else
            {
                jittered.m02 += _jx * 2f / _inW;
                jittered.m12 += _jy * 2f / _inH;
            }
            _render.projectionMatrix = jittered;
            _render.nonJitteredProjectionMatrix = proj;

            // camera cuts -> history reset
            Vector3 pos = _main.transform.position;
            if ((pos - _lastPos).sqrMagnitude > 25f) _resetFrames = 2;
            _lastPos = pos;

            FrameData* f = NextSlot();
            FillCommon(f);
            f->Reset = _resetFrames > 0 ? 1 : 0;
            if (_resetFrames > 0) _resetFrames--;
            _frame = f;
        }

        private void HoldMain()
        {
            if (_mainHeld) return;
            _heldMask = _main.cullingMask;
            _heldClear = _main.clearFlags;
            _heldBg = _main.backgroundColor;
            _heldPost = _mainData.renderPostProcessing;
            _heldShadows = _mainData.renderShadows;
            _heldAA = _mainData.antialiasing;
            _mainHeld = true;

            _main.cullingMask = 0;
            _main.clearFlags = CameraClearFlags.SolidColor;
            _main.backgroundColor = Color.black;
            _mainData.renderPostProcessing = false;
            _mainData.renderShadows = false;
            _mainData.antialiasing = AntialiasingMode.None;
        }

        private void ReleaseMain()
        {
            if (!_mainHeld) return;
            _mainHeld = false;
            try
            {
                if (_main != null)
                {
                    _main.cullingMask = _heldMask;
                    _main.clearFlags = _heldClear;
                    _main.backgroundColor = _heldBg;
                }
                if (_mainData != null)
                {
                    _mainData.renderPostProcessing = _heldPost;
                    _mainData.renderShadows = _heldShadows;
                    _mainData.antialiasing = _heldAA;
                }
            }
            catch { }
        }

        private void ReleaseScale()
        {
            if (!_scaleHeld) return;
            _scaleHeld = false;
            try { var a = UniversalRenderPipeline.asset; if (a != null) a.renderScale = _heldScale; } catch { }
        }

        // =====================================================================================
        // Helpers
        // =====================================================================================
        private void FillCommon(FrameData* f)
        {
            f->Color = _pColor;
            f->Depth = _pDepth;
            f->Motion = _pMotion;
            f->Output = _pOutput;
            f->RenderW = _inW; f->RenderH = _inH; f->OutW = _outW; f->OutH = _outH;
            f->JitterX = O.JitterSignX.Value * _jx;
            f->JitterY = O.JitterSignY.Value * _jy;
            f->MvScaleX = O.MotionSignX.Value * _inW;
            f->MvScaleY = O.MotionSignY.Value * _inH;
            f->FrameTimeMs = Time.unscaledDeltaTime * 1000f;
            f->Near = _main.nearClipPlane;
            f->Far = _main.farClipPlane;
            f->FovY = _main.fieldOfView * Mathf.Deg2Rad;
            f->Sharpness = Mathf.Clamp01(O.Sharpness.Value);
            f->Version = (int)_activeVersion;
            f->Gen = _createGen;
        }

        private FrameData* NextSlot()
        {
            _slot = (_slot + 1) % Slots;
            FrameData* f = _slots + _slot;
            *f = default(FrameData);
            return f;
        }

        private void IssueEvent(int id, IntPtr data)
        {
            if (EventFunc == IntPtr.Zero) return;
            _cbCtl.Clear();
            _cbCtl.IssuePluginEventAndData(EventFunc, id, data);
            Graphics.ExecuteCommandBuffer(_cbCtl);
        }

        private static float Halton(int index, int b)
        {
            float f = 1f, r = 0f;
            while (index > 0) { f /= b; r += f * (index % b); index /= b; }
            return r;
        }

        private static RenderTexture MakeRT(int w, int h, int depth, RenderTextureFormat fmt, bool uav, string name)
        {
            var rt = new RenderTexture(w, h, depth, fmt, RenderTextureReadWrite.Linear)
            {
                name = name,
                enableRandomWrite = uav,
                useMipMap = false,
                autoGenerateMips = false,
                antiAliasing = 1,
                filterMode = FilterMode.Bilinear,
                hideFlags = HideFlags.HideAndDontSave
            };
            rt.Create();
            return rt;
        }

        private void ReleaseTargets()
        {
            foreach (var h in new[] { _hColor, _hDepth, _hMotion, _hOutput })
                if (h != null) RTHandles.Release(h);
            _hColor = _hDepth = _hMotion = _hOutput = null;
            foreach (var rt in new[] { _color, _depth, _motion, _output })
            {
                if (rt == null) continue;
                try { rt.Release(); Destroy(rt); } catch { }
            }
            _color = _depth = _motion = _output = null;
            _pColor = _pDepth = _pMotion = _pOutput = IntPtr.Zero;
        }

        private static readonly FieldInfo RendererIndexField =
            typeof(UniversalAdditionalCameraData).GetField("m_RendererIndex", BindingFlags.Instance | BindingFlags.NonPublic);

        private static int RendererIndexOf(UniversalAdditionalCameraData data)
        {
            try { return RendererIndexField != null ? (int)RendererIndexField.GetValue(data) : -1; } catch { return -1; }
        }
    }
}
