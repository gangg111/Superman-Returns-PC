using System.Diagnostics;
using System.IO;
using System.Globalization;
using System.Text;

namespace SupermanReturnsLauncher;

public sealed class LauncherSettings
{
    public string GameExe { get; set; } = "superman_returns.exe";
    public string GameData { get; set; } = "";
    public string Resolution { get; set; } = "1280x720";
    public string DisplayMode { get; set; } = "Fullscreen";
    public string Gpu { get; set; } = "Auto";
    public int GpuIndex { get; set; } = -1;
    public string GpuLuid { get; set; } = "";
    public bool VSync { get; set; } = true;
    public string FpsLimit { get; set; } = "30";
    public string RenderScale { get; set; } = "auto";
    public string TextureFilter { get; set; } = "Anisotropic16x";
    public string Antialiasing { get; set; } = "Off";
    public string ShadowQuality { get; set; } = "1";
    public string Msaa { get; set; } = "1";
    public string InputMode { get; set; } = "KeyboardMouse";
    public string Renderer { get; set; } = "native";
    public string GraphicsApi { get; set; } = "d3d12";
    public bool UsesVulkan => GraphicsApi == "vulkan";
    public bool SkipIntro { get; set; }
    public bool FrameGeneration { get; set; }
    public string FrameGenerationDll { get; set; } = "";
    public string Language { get; set; } = L.DefaultLanguage();
    // Device-state draws with the original XDK draw/resolve/clear/inline/fence
    // code skipped (Vulkan only); see port/src/graphics/guest/state_source.h.
    public bool NativePort { get; set; } = true;
    public bool DiagnosticLogs { get; set; }

    public static LauncherSettings Load(string path)
    {
        var s = new LauncherSettings();
        if (!File.Exists(path)) return s;
        foreach (var raw in File.ReadAllLines(path))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;
            var idx = line.IndexOf('=');
            if (idx <= 0) continue;
            var value = line[(idx + 1)..].Trim();
            switch (line[..idx].Trim().ToLowerInvariant())
            {
                case "game_exe": s.GameExe = value; break;
                case "game_data": s.GameData = value; break;
                case "resolution": s.Resolution = value; break;
                case "display_mode": s.DisplayMode = value; break;
                case "gpu": s.Gpu = value; break;
                case "gpu_luid": s.GpuLuid = value; break;
                case "vsync": if (bool.TryParse(value, out var b)) s.VSync = b; break;
                case "fps_limit": s.FpsLimit = value; break;
                case "render_scale": s.RenderScale = value; break;
                case "texture_filter": s.TextureFilter = value; break;
                case "antialiasing": s.Antialiasing = value; break;
                case "shadow_quality": s.ShadowQuality = value; break;
                case "msaa": s.Msaa = value; break;
                case "input": s.InputMode = value; break;
                case "renderer": s.Renderer = value; break;
                case "graphics_api": s.GraphicsApi = value == "d3d12" ? "d3d12" : "vulkan"; break;
                case "skip_intro": if (bool.TryParse(value, out var si)) s.SkipIntro = si; break;
                case "frame_generation": if (bool.TryParse(value, out var fg)) s.FrameGeneration = fg; break;
                case "frame_generation_dll": s.FrameGenerationDll = value; break;
                case "language": s.Language = L.Normalize(value); break;
                case "native_port": if (bool.TryParse(value, out var np)) s.NativePort = np; break;
                case "diagnostic_logs": if (bool.TryParse(value, out var dl)) s.DiagnosticLogs = dl; break;
            }
        }
        return s;
    }

    public void Save(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var lines = new[] { "# Superman Returns PC launcher", $"game_exe={GameExe}", $"game_data={GameData}",
            $"resolution={Resolution}", $"display_mode={DisplayMode}", $"gpu={Gpu}", $"gpu_luid={GpuLuid}",
            $"vsync={VSync.ToString().ToLowerInvariant()}", $"fps_limit={FpsLimit}", $"render_scale={RenderScale}",
            $"texture_filter={TextureFilter}", $"antialiasing={Antialiasing}", $"shadow_quality={ShadowQuality}",
            $"msaa={Msaa}", $"input={InputMode}", $"renderer={Renderer}", $"graphics_api={GraphicsApi}",
            $"skip_intro={SkipIntro.ToString().ToLowerInvariant()}",
            $"frame_generation={FrameGeneration.ToString().ToLowerInvariant()}", $"frame_generation_dll={FrameGenerationDll}",
            $"language={Language}", $"native_port={NativePort.ToString().ToLowerInvariant()}",
            $"diagnostic_logs={DiagnosticLogs.ToString().ToLowerInvariant()}" };
        var temporary = path + ".tmp";
        File.WriteAllLines(temporary, lines, new UTF8Encoding(false));
        File.Move(temporary, path, true);
    }

    private static int ValidChoice(string value, int fallback, params int[] choices) =>
        int.TryParse(value, out var number) && choices.Contains(number) ? number : fallback;

    public IReadOnlyList<string> BuildArgumentList()
    {
        var resolution = Resolution.Split('x');
        int width = 1280, height = 720;
        if (resolution.Length == 2 && int.TryParse(resolution[0], out var w) && int.TryParse(resolution[1], out var h)
            && w >= 640 && w <= 8192 && h >= 480 && h <= 8192) { width = w; height = h; }
        int filter = TextureFilter switch { "Anisotropic2x" => 2, "Anisotropic4x" => 4,
            "Anisotropic8x" => 8, "Anisotropic16x" => 16, _ => -1 };
        // Vulkan does not support most enhancement options yet: they stay at the
        // game's defaults so the engine accepts the launch (backend_selection.cpp).
        // Anisotropic filtering works on both APIs.
        var vulkan = UsesVulkan;
        // "auto": the engine picks the scale from the display (sr_native_render_scale=0).
        var autoScale = !vulkan && RenderScale == "auto";
        var percent = vulkan || autoScale ? 100 : ValidChoice(RenderScale, 100, 50, 75, 100, 125, 150, 200, 300, 400);
        var lowScale = percent < 100;
        var scale = autoScale ? 0.0 : (lowScale ? 100 : percent) / 100.0;
        var result = new List<string> {
            $"--sr_renderer={(Renderer == "xenos" && !vulkan ? "xenos" : "native")}", "--sr_preset=custom",
            $"--sr_native_api={(vulkan ? "vulkan" : "d3d12")}",
            $"--sr_skip_intro={SkipIntro.ToString().ToLowerInvariant()}",
            $"--sr_render_scale={(lowScale ? percent : 100)}",
            $"--window_width={width}", $"--window_height={height}",
            $"--fullscreen={(DisplayMode != "Windowed").ToString().ToLowerInvariant()}",
            $"--vsync={VSync.ToString().ToLowerInvariant()}",
            "--d3d12_adapter=-1",
            $"--sr_native_fps_limit={ValidChoice(FpsLimit, 30, 0, 30, 60, 120)}",
            $"--sr_native_render_scale={scale.ToString("0.##", CultureInfo.InvariantCulture)}",
            $"--sr_native_anisotropic_filtering={filter}",
            $"--sr_native_fxaa={(!vulkan && Antialiasing == "FXAA").ToString().ToLowerInvariant()}",
            $"--sr_native_shadow_quality={(vulkan ? 1 : ValidChoice(ShadowQuality, 1, 1, 2, 4))}",
            $"--sr_native_msaa_samples={(vulkan ? 1 : ValidChoice(Msaa, 1, 1, 4, 8))}"
        };
        // An empty "--flag=" makes the engine's argument parser take the next argument
        // as its value, so the LUID is passed only when a GPU was chosen.
        if (!string.IsNullOrWhiteSpace(GpuLuid)) result.Add($"--sr_native_gpu_luid={GpuLuid.Trim()}");
        result.Add($"--sr_lsfg={FrameGeneration.ToString().ToLowerInvariant()}");
        if (FrameGeneration) {
            result.Add("--sr_lsfg_mode=performance");
            if (!string.IsNullOrWhiteSpace(FrameGenerationDll)) result.Add("--sr_lsfg_dll=" + FrameGenerationDll.Trim());
        }
        if (InputMode == "KeyboardMouse") { result.Add("--mnk_mode=true"); result.Add("--mnk_mouse=true"); }
        else { result.Add("--mnk_mode=false"); result.Add("--mnk_mouse=false"); }
        return result;
    }

    // Environment for the game process: the native-port switches (Vulkan only, the
    // only path that builds draws from the device state) and the diagnostic logs.
    public IReadOnlyList<(string Name, string Value)> BuildEnvironment()
    {
        var env = new List<(string, string)>();
        if (NativePort && UsesVulkan) {
            env.Add(("SR_NATIVE_STATE_SOURCE", "device"));
            env.Add(("SR_NATIVE_SKIP_ORIGINAL", "draw,resolve,clear,inline,fence"));
        }
        if (DiagnosticLogs) {
            env.Add(("SR_LOG_MEM", "10"));
            env.Add(("SR_LOG_FPS", "1"));
            env.Add(("SR_LOG_XMA", "1"));
        }
        return env;
    }

    public string BuildArguments() => string.Join(' ', BuildArgumentList());

    public ProcessStartInfo CreateLaunch(string baseDirectory)
    {
        var exe = Path.GetFullPath(GameExe, baseDirectory);
        if (!File.Exists(exe) || !Path.GetFileName(exe).Equals("superman_returns.exe", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException(L.T("ErrExe"));
        var data = Path.GetFullPath(string.IsNullOrWhiteSpace(GameData) ? Path.Combine(Path.GetDirectoryName(exe)!, "game") : GameData, baseDirectory);
        if (!File.Exists(Path.Combine(data, "default.xex")) || !Directory.Exists(Path.Combine(data, "DATA")))
            throw new InvalidOperationException(L.T("ErrData"));
        if (FrameGeneration && (string.IsNullOrWhiteSpace(FrameGenerationDll) || !File.Exists(FrameGenerationDll)))
            throw new InvalidOperationException(L.T("ErrDll"));
        var logDir = Path.Combine(Path.GetDirectoryName(exe)!, "logs");
        Directory.CreateDirectory(logDir);
        var start = new ProcessStartInfo(exe) { WorkingDirectory = Path.GetDirectoryName(exe)!, UseShellExecute = false };
        foreach (var argument in BuildArgumentList()) start.ArgumentList.Add(argument);
        start.ArgumentList.Add($"--game_data_root={data}");
        start.ArgumentList.Add($"--log_file={Path.Combine(logDir, "game.log")}");
        foreach (var (name, value) in BuildEnvironment()) start.Environment[name] = value;
        return start;
    }

    public static LauncherSettings DetectDefaults(string baseDirectory)
    {
        var s = new LauncherSettings { FrameGenerationDll = FindFrameGenerationDll() };
        for (var dir = new DirectoryInfo(baseDirectory); dir is not null; dir = dir.Parent)
        {
            foreach (var suffix in new[] { "superman_returns.exe", "port/out/build/win-amd64-release/superman_returns.exe", "port/out/build/win-amd64-dist/superman_returns.exe" })
            {
                var exe = Path.Combine(dir.FullName, suffix);
                if (!File.Exists(exe)) continue;
                s.GameExe = exe;
                var game = Path.Combine(Path.GetDirectoryName(exe)!, "game");
                if (!File.Exists(Path.Combine(game, "default.xex"))) game = Path.Combine(dir.FullName, "game");
                s.GameData = game;
                return s;
            }
        }
        return s;
    }

    public static string FindFrameGenerationDll()
    {
        var steam = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam", "steamapps", "common", "Lossless Scaling", "Lossless.dll");
        return File.Exists(steam) ? steam : "";
    }
}
