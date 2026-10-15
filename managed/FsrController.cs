// ParalivesUpscaling - camera management (same scheme as the DLSS mod, ported to URP 17):
//  * A hidden render camera copies the game camera every frame and renders the world, without post-processing,
//    into a low-resolution HDR texture through a jittered projection. URP also renders depth and motion vectors
//    (camera + per-object) for it.
//  * FsrPass, on that camera, copies depth + motion vectors and runs FSR into a full-resolution output.
//  * While the game camera renders it draws no world; PresentPass copies the FSR output into it, the game camera
//    then runs the game's post-processing (bloom, vignette) at full resolution, and the stacked UI camera draws
//    the UI on top.
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using HarmonyLib;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

namespace ParalivesUpscaling
{
    public sealed unsafe class FsrController : MonoBehaviour
    {
        internal static FsrController Instance;
        private static Settings O { get { return Plugin.O; } }

        private const int Slots = 16;

        internal bool Active { get { return _active && !_suspended; } }
        internal Camera MainCamera { get { return _main; } }
        internal Camera RenderCamera { get { return _render; } }
        internal bool MainHeld { get { return _mainHeld; } }
        internal RTHandle DepthHandle { get { return _hDepth; } }
        internal RTHandle MotionHandle { get { return _hMotion; } }
        // Copy of the camera's depth attachment: unlike URP's depth texture it includes the characters.
        internal RTHandle DepthAttachmentHandle { get { return _hDepthAtt; } }
        internal RTHandle PresentHandle { get { return _presentingFsr ? _hOutput : _hColor; } }
        internal bool FlipPresent { get { return O.PresentFlipY.Value; } }
        internal IntPtr FramePtr { get { return (IntPtr)_frame; } }
        internal IntPtr EventFunc { get; private set; }

        private bool _active, _suspended, _disabledByError, _presentingFsr;
        private FsrMode _activeMode;
        private FsrVersion _activeVersion;
        private bool _activeCancel, _activeDebug;
        private Camera _main, _render;
        private UniversalAdditionalCameraData _mainData, _renderData;
        private RenderTexture _color, _depth, _motion, _output, _depthAtt, _motionFix, _reactive;
        private RTHandle _hColor, _hDepth, _hMotion, _hOutput, _hDepthAtt;
        private IntPtr _pColor, _pDepth, _pMotion, _pOutput, _pDepthAtt, _pMotionFix, _pReactive;
        private Matrix4x4 _prevViewProj;
        private bool _havePrevViewProj;
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
        private bool _heldShadows;
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
        // Crash guard: a marker is written before FSR starts and removed once it has run for a while. If the game
        // dies in between, the next start switches FSR off instead of crash-looping.
        private static string MarkerPath
        {
            get { return Path.Combine(Path.Combine(Paths.CachePath, "ParalivesUpscaling"), "starting.marker"); }
        }

        private bool _markerSet;

        private void SetMarker()
        {
            if (_markerSet) return;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(MarkerPath));
                File.WriteAllText(MarkerPath, DateTime.Now.ToString("o"));
                _markerSet = true;
            }
            catch { }
        }

        private void ClearMarker()
        {
            if (!_markerSet) return;
            _markerSet = false;
            try { if (File.Exists(MarkerPath)) File.Delete(MarkerPath); } catch { }
        }

        private void OnApplicationQuit()
        {
            ClearMarker();
        }

        private void Awake()
        {
            Instance = this;
            try
            {
                if (File.Exists(MarkerPath))
                {
                    File.Delete(MarkerPath);
                    O.Version.Value = FsrVersion.Off;
                    Plugin.Log.LogError("The game closed while the upscaler was starting last time, so it has been switched off. " +
                                        "You can switch it back on in Options > Video > Upscaling (FSR / XeSS). If it happens again, please report it with " +
                                        "BepInEx\\LogOutput.log and Paralives_Data\\Plugins\\x86_64\\ParalivesUpscaling.log.");
                }
            }
            catch { }
            _slots = (FrameData*)Marshal.AllocHGlobal(sizeof(FrameData) * Slots);
            _frame = _slots;
            *_frame = default(FrameData);
            _cbCtl = new CommandBuffer { name = "ParalivesUpscaling control" };
            try
            {
                EventFunc = Native.PFSR_GetRenderEventFunc();  // first call loads the DLL -> UnityPluginLoad
            }
            catch (Exception e)
            {
                Plugin.Log.LogError("ParalivesUpscaling.dll could not be loaded (it belongs in Paralives_Data\\Plugins\\x86_64): " + e.Message);
                enabled = false;
            }
            // picking something in the menu again retries after a failure
            O.Mode.SettingChanged += (s, e) => { _disabledByError = false; };
            O.Version.SettingChanged += (s, e) => { _disabledByError = false; };
        }

        private void OnEnable()
        {
            RenderPipelineManager.beginCameraRendering += OnBeginCamera;
            RenderPipelineManager.endCameraRendering += OnEndCamera;
            RenderPipelineManager.beginContextRendering += OnBeginContext;
            RenderPipelineManager.endContextRendering += OnEndContext;
        }

        private void OnDisable()
        {
            RenderPipelineManager.beginCameraRendering -= OnBeginCamera;
            RenderPipelineManager.endCameraRendering -= OnEndCamera;
            RenderPipelineManager.beginContextRendering -= OnBeginContext;
            RenderPipelineManager.endContextRendering -= OnEndContext;
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
            FsrOptions.Instance.Sync();
            if (O.PerfLog.Value) PerfTick();
            if (EventFunc == IntPtr.Zero) return;
            try
            {
                if (_active)
                {
                    int state = Native.PFSR_GetStatus(Native.StState);
                    if (state == -1) Fail("Upscaler could not start: " + Native.LastError);
                    else if (state == -2 && Native.PFSR_GetStatus(Native.StCreatedGen) == _createGen) Fail("Upscaler stopped: " + Native.LastError);
                }

                Camera target = (O.Version.Value != FsrVersion.Off && !_disabledByError) ? FindGameCamera() : null;
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
                    || O.DebugView.Value != _activeDebug || Screen.width != _outW || Screen.height != _outH)
                    Rebuild();

                _presentingFsr = Native.PFSR_GetStatus(Native.StState) == 2
                                 && Native.PFSR_GetStatus(Native.StCreatedGen) == _createGen
                                 && Native.PFSR_GetStatus(Native.StDispatchesSinceCreate) > 1;
                if (_markerSet && _presentingFsr && Native.PFSR_GetStatus(Native.StDispatchesSinceCreate) > 60) ClearMarker();
                if (_presentingFsr && _reportedVersion == null)
                {
                    _reportedVersion = Native.VersionName;
                    Plugin.Log.LogInfo("Upscaler running: " + _reportedVersion + ", " + _inW + "x" + _inH + " -> " + _outW + "x" + _outH + ".");
                    LogInventory();
                }
                _render.depth = _main.depth - 1f;
                if (_activeDebug && _presentingFsr && Time.unscaledTime >= _nextProbe)
                {
                    _nextProbe = Time.unscaledTime + 2f;
                    ProbeInputs();
                }
            }
            catch (Exception e)
            {
                Plugin.Log.LogError("Upscaler error: " + e);
                Fail("Upscaler error: " + e.Message);
            }
        }

        // DebugView only: logs what FSR actually receives (depth in metres, motion in render pixels).
        private float _nextProbe;
        private static readonly Vector2[] ProbePoints =
            { new Vector2(0.5f, 0.5f), new Vector2(0.25f, 0.5f), new Vector2(0.75f, 0.5f), new Vector2(0.5f, 0.25f), new Vector2(0.5f, 0.75f) };

        private void ProbeInputs()
        {
            ProbeDepth(_depth, "Depth texture (URP)");
            ProbeDepth(_depthAtt, "Depth attachment (FSR input)");
            ProbeMotion(_motion, "Motion URP");
            ProbeMotion(_motionFix, "Motion recomputed (FSR input)");
        }

        private void ProbeMotion(RenderTexture rt, string label)
        {
            int w = _inW, h = _inH;
            AsyncGPUReadback.Request(rt, 0, req =>
            {
                if (req.hasError) return;
                var m = req.GetData<ushort>();  // RGHalf
                if (m.Length != w * h * 2) return;
                var sb = new StringBuilder(label + " probe (render pixels):");
                foreach (var p in ProbePoints)
                {
                    int i = ((int)(p.y * (h - 1)) * w + (int)(p.x * (w - 1))) * 2;
                    sb.Append(" (" + p.x + "," + p.y + ") " + (Mathf.HalfToFloat(m[i]) * w).ToString("0.00") + ", " + (Mathf.HalfToFloat(m[i + 1]) * h).ToString("0.00"));
                }
                Plugin.Log.LogInfo(sb.ToString());
            });
        }

        // PerfLog: frame rate, CPU/GPU times (FrameTimingManager) and every camera rendered, every 2 s, so we can tell
        // whether the game is CPU- or GPU-bound and what upscaling actually saves.
        private readonly FrameTiming[] _timing = new FrameTiming[1];
        private readonly Dictionary<string, int> _camerasSeen = new Dictionary<string, int>();
        private float _perfStart = -1f;
        private int _perfFrames, _timingSamples;
        private double _cpu, _cpuMain, _cpuRender, _gpu;

        // main-thread time URP spends recording each frame (all cameras): the part our second camera adds to
        private readonly System.Diagnostics.Stopwatch _renderWatch = new System.Diagnostics.Stopwatch();
        private double _renderMs, _frameMs;

        private void OnBeginContext(ScriptableRenderContext ctx, List<Camera> cams)
        {
            if (O.PerfLog.Value) _renderWatch.Restart();
        }

        private void OnEndContext(ScriptableRenderContext ctx, List<Camera> cams)
        {
            if (O.PerfLog.Value && _renderWatch.IsRunning) { _renderWatch.Stop(); _renderMs += _renderWatch.Elapsed.TotalMilliseconds; }
        }

        private void CountCamera(Camera cam)
        {
            string key = cam.name + " " + cam.pixelWidth + "x" + cam.pixelHeight + (cam.targetTexture != null ? " (RT)" : "");
            int n;
            _camerasSeen.TryGetValue(key, out n);
            _camerasSeen[key] = n + 1;
        }

        private void PerfTick()
        {
            FrameTimingManager.CaptureFrameTimings();
            if (FrameTimingManager.GetLatestTimings(1, _timing) > 0)
            {
                _cpu += _timing[0].cpuFrameTime;
                _cpuMain += _timing[0].cpuMainThreadFrameTime;
                _cpuRender += _timing[0].cpuRenderThreadFrameTime;
                _gpu += _timing[0].gpuFrameTime;
                _timingSamples++;
            }
            if (_perfStart < 0f) { _perfStart = Time.unscaledTime; _renderMs = _frameMs = 0; return; }
            _perfFrames++;
            _frameMs += Time.unscaledDeltaTime * 1000.0;
            float t = Time.unscaledTime - _perfStart;
            if (t < 2f) return;

            int s = Math.Max(1, _timingSamples);
            var sb = new StringBuilder("Perf [" + (_active ? O.Mode.Value + " " + Native.VersionName + " " + _inW + "x" + _inH : "Off") + "] " +
                                       (_perfFrames / t).ToString("0.0") + " fps | frame " + (_frameMs / _perfFrames).ToString("0.00") +
                                       " ms, URP main-thread render " + (_renderMs / _perfFrames).ToString("0.00") + " ms | CPU " + (_cpu / s).ToString("0.00") + " ms (main " +
                                       (_cpuMain / s).ToString("0.00") + ", render " + (_cpuRender / s).ToString("0.00") + ") | GPU " +
                                       (_gpu / s).ToString("0.00") + " ms" + (_timingSamples == 0 ? " (frame timing unavailable)" : "") +
                                       " | cap " + Application.targetFrameRate + ", vsync " + QualitySettings.vSyncCount + " | cameras/frame:");
            foreach (var kv in _camerasSeen)
                sb.Append(" " + kv.Key + " x" + ((float)kv.Value / _perfFrames).ToString("0.##") + ";");
            Plugin.Log.LogInfo(sb.ToString());
            _camerasSeen.Clear();
            _perfStart = Time.unscaledTime;
            _perfFrames = _timingSamples = 0;
            _cpu = _cpuMain = _cpuRender = _gpu = _renderMs = _frameMs = 0;
        }

        // Once per session: the pipeline settings, post effects and renderer features FSR has to live with.
        private static bool _inventoryLogged;
        private static readonly FieldInfo RendererListField =
            typeof(UniversalRenderPipelineAsset).GetField("m_RendererDataList", BindingFlags.Instance | BindingFlags.NonPublic);

        private void LogInventory()
        {
            if (_inventoryLogged) return;
            _inventoryLogged = true;
            try
            {
                var a = UniversalRenderPipeline.asset;
                if (a != null)
                    Plugin.Log.LogInfo("Inventory: URP HDR " + a.supportsHDR + ", colorGrading " + a.colorGradingMode + ", renderScale " + a.renderScale +
                                       ", upscalingFilter " + a.upscalingFilter + ", MSAA " + a.msaaSampleCount);
                Plugin.Log.LogInfo("Inventory: game camera allowHDR " + _main.allowHDR + ", post " + _mainData.renderPostProcessing + ", AA " + _mainData.antialiasing +
                                   ", volumeMask " + _mainData.volumeLayerMask.value);

                foreach (var v in FindObjectsByType<Volume>(FindObjectsInactive.Exclude, FindObjectsSortMode.None))
                {
                    var p = v.HasInstantiatedProfile() ? v.profile : v.sharedProfile;
                    var sb = new StringBuilder("Inventory: volume '" + v.name + "' global " + v.isGlobal + ", layer " + v.gameObject.layer + ", weight " + v.weight + ", priority " + v.priority + ":");
                    if (p != null)
                        foreach (var c in p.components)
                        {
                            if (!c.active) continue;
                            sb.Append(" " + c.GetType().Name + "[");
                            foreach (var prm in c.parameters)
                                if (prm.overrideState) sb.Append(prm.GetType().Name.Replace("Parameter", "") + "=" + prm + " ");
                            sb.Append("]");
                        }
                    Plugin.Log.LogInfo(sb.ToString());
                }

                var list = RendererListField != null ? RendererListField.GetValue(a) as ScriptableRendererData[] : null;
                if (list != null)
                    for (int i = 0; i < list.Length; i++)
                    {
                        if (list[i] == null) continue;
                        var sb = new StringBuilder("Inventory: renderer " + i + " '" + list[i].name + "' (" + list[i].GetType().Name + ") features:");
                        foreach (var f in list[i].rendererFeatures)
                            if (f != null) sb.Append(" " + f.name + "<" + f.GetType().FullName + ">" + (f.isActive ? "" : "(off)"));
                        Plugin.Log.LogInfo(sb.ToString());
                    }
            }
            catch (Exception e)
            {
                Plugin.Log.LogWarning("Inventory failed: " + e.Message);
            }
        }

        private void ProbeDepth(RenderTexture rt, string label)
        {
            int w = _inW, h = _inH;
            float near = _main.nearClipPlane, far = _main.farClipPlane;
            bool reversed = SystemInfo.usesReversedZBuffer;
            AsyncGPUReadback.Request(rt, 0, req =>
            {
                if (req.hasError) return;
                var d = req.GetData<float>();
                if (d.Length != w * h) return;
                float min = float.MaxValue, max = float.MinValue;
                for (int i = 0; i < d.Length; i++) { min = Mathf.Min(min, d[i]); max = Mathf.Max(max, d[i]); }
                var sb = new StringBuilder(label + " probe (near " + near + ", far " + far + ", reversed " + reversed + "): raw min " + min + ", max " + max + " |");
                foreach (var p in ProbePoints)
                {
                    float raw = d[(int)(p.y * (h - 1)) * w + (int)(p.x * (w - 1))];
                    sb.Append(" (" + p.x + "," + p.y + ") " + raw.ToString("0.000000") + " = " + EyeDepth(raw, near, far, reversed).ToString("0.00") + " m");
                }
                Plugin.Log.LogInfo(sb.ToString());
            });
        }

        // Unity's LinearEyeDepth with _ZBufferParams.
        private static float EyeDepth(float raw, float near, float far, bool reversed)
        {
            float x = reversed ? -1f + far / near : 1f - far / near;
            float y = reversed ? 1f : far / near;
            return 1f / (x / far * raw + y / far);
        }

        // ShowInfo: what is actually running (as reported by the native side, so fallbacks show), top-centre.
        private GUIStyle _infoStyle;

        private void OnGUI()
        {
            if (!O.ShowInfo.Value || Event.current.type != EventType.Repaint) return;
            string text;
            if (O.Version.Value == FsrVersion.Off) text = Localization.Translate("ParalivesUpscaling_Version_Off");
            else if (_presentingFsr)
            {
                string name = Native.VersionName;
                if (!name.StartsWith("XeSS", StringComparison.Ordinal) && !name.StartsWith("DLSS", StringComparison.Ordinal)) name = "FSR " + name;
                text = name + " · " + Localization.Translate("ParalivesUpscaling_Mode_" + _activeMode) + " · " +
                       _inW + "x" + _inH + " → " + _outW + "x" + _outH;
            }
            else if (_active) text = Localization.Translate("ParalivesUpscaling_Status_Starting");
            else text = Localization.Translate("ParalivesUpscaling_Status_Unavailable");

            if (_infoStyle == null) _infoStyle = new GUIStyle(GUI.skin.label) { alignment = TextAnchor.UpperCenter };
            _infoStyle.fontSize = Mathf.Max(12, Screen.height / 60);
            var rect = new Rect(0f, Screen.height * 0.01f, Screen.width, _infoStyle.fontSize * 2f);
            var shadow = new Rect(rect.x + 1.5f, rect.y + 1.5f, rect.width, rect.height);
            _infoStyle.normal.textColor = new Color(0f, 0f, 0f, 0.8f);
            GUI.Label(shadow, text, _infoStyle);
            _infoStyle.normal.textColor = Color.white;
            GUI.Label(rect, text, _infoStyle);
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
                Fail("FSR and XeSS need DirectX 12 (game is running " + SystemInfo.graphicsDeviceType + "). Add -force-d3d12 to the Steam launch options.");
                return false;
            }
            if (Native.PFSR_GetStatus(Native.StState) < 1)
            {
                Fail("Upscaler runtime not ready: " + Native.LastError);
                return false;
            }
            _mainData = main.GetUniversalAdditionalCameraData();
            if (_mainData == null) { Fail("Game camera has no URP camera data."); return false; }

            _main = main;
            var go = new GameObject("ParalivesUpscaling.RenderCamera") { hideFlags = HideFlags.HideAndDontSave };
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
            CreateAoOverride();
            Rebuild();
            go.SetActive(true);
            _render.enabled = true;
            Plugin.Log.LogInfo("Upscaling render path active (" + _activeMode + ").");
            return true;
        }

        // The game runs HBAO at half the camera resolution; while FSR upscales that is half of an already reduced
        // resolution and contact shadows go soft. A global volume on a spare layer, which only the render camera
        // includes while upscaling, switches HBAO to full resolution.
        private Volume _aoVolume;

        private void CreateAoOverride()
        {
            try
            {
                var hbaoType = AccessTools.TypeByName("HorizonBasedAmbientOcclusion.Universal.HBAO");
                if (hbaoType == null) return;
                int layer = -1;
                for (int i = 31; i >= 8 && layer < 0; i--)
                    if (string.IsNullOrEmpty(LayerMask.LayerToName(i))) layer = i;
                if (layer < 0) { Plugin.Log.LogWarning("No spare layer for the AO override; contact shadows stay at the game's resolution."); return; }

                var go = new GameObject("ParalivesUpscaling.AOVolume") { hideFlags = HideFlags.HideAndDontSave, layer = layer };
                DontDestroyOnLoad(go);
                _aoVolume = go.AddComponent<Volume>();
                _aoVolume.isGlobal = true;
                _aoVolume.priority = 1000f;
                var profile = ScriptableObject.CreateInstance<VolumeProfile>();
                var hbao = profile.Add(hbaoType);
                var resolution = (VolumeParameter)hbaoType.GetField("resolution").GetValue(hbao);
                resolution.overrideState = true;
                resolution.GetType().GetProperty("value").SetValue(resolution, Enum.Parse(resolution.GetType().BaseType.GetGenericArguments()[0], "Full"));
                _aoVolume.sharedProfile = profile;
            }
            catch (Exception e)
            {
                Plugin.Log.LogWarning("AO override unavailable: " + e.Message);
                _aoVolume = null;
            }
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
            _activeDebug = O.DebugView.Value;
            float ratio = Ratio(_activeMode);
            _outW = Screen.width;
            _outH = Screen.height;
            _inW = Math.Max(64, Mathf.RoundToInt(_outW / ratio));
            _inH = Math.Max(64, Mathf.RoundToInt(_outH / ratio));
            _jitterPhases = Mathf.CeilToInt(8f * ratio * ratio);  // FSR's own phase-count formula

            IssueEvent(Native.EvDestroy, IntPtr.Zero);
            ReleaseTargets();
            _color = MakeRT(_inW, _inH, 24, RenderTextureFormat.ARGBHalf, false, "ParalivesUpscaling.Color");
            _depth = MakeRT(_inW, _inH, 0, RenderTextureFormat.RFloat, false, "ParalivesUpscaling.Depth");
            _motion = MakeRT(_inW, _inH, 0, RenderTextureFormat.RGHalf, false, "ParalivesUpscaling.Motion");
            _output = MakeRT(_outW, _outH, 0, RenderTextureFormat.ARGBHalf, true, "ParalivesUpscaling.Output");
            _hColor = RTHandles.Alloc(_color);
            _hDepth = RTHandles.Alloc(_depth);
            _hMotion = RTHandles.Alloc(_motion);
            _hOutput = RTHandles.Alloc(_output);
            _depthAtt = MakeRT(_inW, _inH, 0, RenderTextureFormat.RFloat, false, "ParalivesUpscaling.DepthAttachment");
            _motionFix = MakeRT(_inW, _inH, 0, RenderTextureFormat.RGHalf, true, "ParalivesUpscaling.MotionFixed");
            _reactive = MakeRT(_inW, _inH, 0, RenderTextureFormat.R8, true, "ParalivesUpscaling.Reactive");
            _hDepthAtt = RTHandles.Alloc(_depthAtt);
            _pColor = _color.GetNativeTexturePtr();
            _pDepth = _depth.GetNativeTexturePtr();
            _pMotion = _motion.GetNativeTexturePtr();
            _pOutput = _output.GetNativeTexturePtr();
            _pDepthAtt = _depthAtt.GetNativeTexturePtr();
            _pMotionFix = _motionFix.GetNativeTexturePtr();
            _pReactive = _reactive.GetNativeTexturePtr();
            _havePrevViewProj = false;

            FrameData* f = NextSlot();
            _createGen++;
            FillCommon(f);
            var asset = UniversalRenderPipeline.asset;
            bool hdr = _main.allowHDR && asset != null && asset.supportsHDR;  // input is pre-post linear colour
            SetMarker();
            f->CreateFlags = (SystemInfo.usesReversedZBuffer ? Native.FlagDepthInverted : 0)
                             | (hdr ? Native.FlagHighDynamicRange : 0)
                             | (_activeCancel ? Native.FlagJitterCancellation : 0)
                             | (_activeDebug ? Native.FlagDebugVisualization : 0);
            IssueEvent(Native.EvCreate, (IntPtr)f);

            _resetFrames = 3;
            _presentingFsr = false;
            _reportedVersion = null;
            Plugin.Log.LogInfo("Upscaling " + _activeMode + " (" + _activeVersion + "): rendering " + _inW + "x" + _inH +
                               ", output " + _outW + "x" + _outH + ", jitter phases " + _jitterPhases + ".");
        }

        private void Teardown()
        {
            ClearMarker();
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
            try
            {
                if (_aoVolume != null) { Destroy(_aoVolume.sharedProfile); Destroy(_aoVolume.gameObject); }
            }
            catch { }
            _aoVolume = null;
            ReleaseTargets();
            _render = null; _renderData = null;
            _main = null; _mainData = null;
            _presentingFsr = false;
            Plugin.Log.LogInfo("Upscaling render path off; game camera restored.");
        }

        private void Fail(string why)
        {
            ClearMarker();  // a clean, reported failure is not a crash
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
            if (cam != null && O.PerfLog.Value) CountCamera(cam);
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
                Plugin.Log.LogError("Upscaling camera hook error: " + e.Message);
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
            _renderData.renderPostProcessing = false;  // post runs on the game camera, after FSR, at output resolution
            _renderData.antialiasing = AntialiasingMode.None;
            _renderData.volumeLayerMask = _mainData.volumeLayerMask
                                          | (_aoVolume != null && O.FullResolutionAO.Value && Ratio(_activeMode) > 1f ? 1 << _aoVolume.gameObject.layer : 0);
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
            if ((pos - _lastPos).sqrMagnitude > 25f) { _resetFrames = 2; _havePrevViewProj = false; }
            _lastPos = pos;

            // reprojection matrices for the motion-vector repair: jittered, like the ones URP builds its vectors with
            Matrix4x4 viewProj = GL.GetGPUProjectionMatrix(jittered, true) * _main.worldToCameraMatrix;

            FrameData* f = NextSlot();
            FillCommon(f);
            f->InvViewProj = viewProj.inverse;
            f->PrevViewProj = _havePrevViewProj ? _prevViewProj : viewProj;
            _prevViewProj = viewProj;
            _havePrevViewProj = true;
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
            _heldShadows = _mainData.renderShadows;
            _heldAA = _mainData.antialiasing;
            _mainHeld = true;

            _main.cullingMask = 0;
            _main.clearFlags = CameraClearFlags.SolidColor;
            _main.backgroundColor = Color.black;
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
            f->DispatchFlags = _activeDebug ? Native.DispatchDrawDebugView : 0;
            f->DepthAtt = _pDepthAtt;
            f->MotionOut = _pMotionFix;
            f->MvThreshold = 1e-5f;             // raw reversed-Z difference; background pixels match exactly
            f->MvMode = _activeDebug ? 1 : 0;   // DebugView: recompute everywhere, so the probe can compare with URP's
            f->InvViewProj = Matrix4x4.identity;
            f->PrevViewProj = Matrix4x4.identity;
            f->Reactive = _pReactive;
            f->ReactiveStrength = Mathf.Clamp01(O.CharacterReactive.Value);
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
            foreach (var h in new[] { _hColor, _hDepth, _hMotion, _hOutput, _hDepthAtt })
                if (h != null) RTHandles.Release(h);
            _hColor = _hDepth = _hMotion = _hOutput = _hDepthAtt = null;
            foreach (var rt in new[] { _color, _depth, _motion, _output, _depthAtt, _motionFix, _reactive })
            {
                if (rt == null) continue;
                try { rt.Release(); Destroy(rt); } catch { }
            }
            _color = _depth = _motion = _output = _depthAtt = _motionFix = _reactive = null;
            _pColor = _pDepth = _pMotion = _pOutput = _pDepthAtt = _pMotionFix = _pReactive = IntPtr.Zero;
        }

        private static readonly FieldInfo RendererIndexField =
            typeof(UniversalAdditionalCameraData).GetField("m_RendererIndex", BindingFlags.Instance | BindingFlags.NonPublic);

        private static int RendererIndexOf(UniversalAdditionalCameraData data)
        {
            try { return RendererIndexField != null ? (int)RendererIndexField.GetValue(data) : -1; } catch { return -1; }
        }
    }
}
