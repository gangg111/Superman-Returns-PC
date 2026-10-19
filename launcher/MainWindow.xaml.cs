using Microsoft.Win32;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;

namespace SupermanReturnsLauncher;

public partial class MainWindow : Window
{
    private readonly string _settingsPath;
    private LauncherSettings _settings;
    private bool _ready;
    private string? _statusKey = "StatusReady";
    private string _statusSuffix = "";
    private sealed record Choice(string Value, string Label) { public override string ToString() => Label; }

    public MainWindow()
    {
        InitializeComponent();
        // app.ico (the logo) is embedded only when present in the launcher folder.
        try { Icon = System.Windows.Media.Imaging.BitmapFrame.Create(new Uri("pack://application:,,,/app.ico")); }
        catch (IOException) { }
        var installId = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(AppContext.BaseDirectory.ToLowerInvariant())))[..12];
        _settingsPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SupermanReturns", "launcher", installId + ".ini");
        _settings = LauncherSettings.DetectDefaults(AppContext.BaseDirectory);
        var loadFailed = false;
        try { if (File.Exists(_settingsPath)) _settings = LauncherSettings.Load(_settingsPath); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { loadFailed = true; }
        L.Language = L.Normalize(_settings.Language);
        ApplyLanguage();
        ApplySettingsToUi();
        foreach (var box in new[] { RendererBox, ApiBox, ResolutionBox, DisplayModeBox, InputBox, GpuBox, RenderScaleBox, AaBox, FilterBox, FpsBox, ShadowBox, MsaaBox })
            box.SelectionChanged += (_, _) => UpdateSummary();
        foreach (var check in new[] { VSyncBox, SkipIntroBox, FrameGenerationBox, NativePortBox, DiagnosticLogsBox })
        {
            check.Checked += (_, _) => UpdateSummary();
            check.Unchecked += (_, _) => UpdateSummary();
        }
        GameExeBox.TextChanged += (_, _) => UpdateSummary();
        GameDataBox.TextChanged += (_, _) => UpdateSummary();
        FrameGenerationDllBox.TextChanged += (_, _) => UpdateSummary();
        _ready = true;
        if (loadFailed) SetStatus("LoadWarning");
        UpdateSummary();
    }

    private void SetStatus(string key, string suffix = "") { _statusKey = key; _statusSuffix = suffix; StatusText.Text = L.T(key) + suffix; }
    private void SetRawStatus(string text) { _statusKey = null; StatusText.Text = text; }

    private static void Label(TextBlock label, Control control, string key)
    {
        label.Text = L.T(key);
        AutomationProperties.SetName(control, L.T(key));
    }

    // Every visible text comes from L; switching language keeps the selections.
    private void ApplyLanguage()
    {
        TaglineText.Text = L.T("Tagline");
        ReadyText.Text = L.T("Ready");
        PlayButton.Content = L.T("Play"); AutomationProperties.SetName(PlayButton, L.T("Play"));
        SaveButton.Content = L.T("Save");
        DefaultsButton.Content = L.T("Defaults");
        LanguageLabel.Text = L.T("Language");
        HeadingText.Text = L.T("Heading");
        Label(RendererLabel, RendererBox, "Renderer");
        Label(ApiLabel, ApiBox, "Api");
        Label(ResolutionLabel, ResolutionBox, "Resolution");
        Label(DisplayModeLabel, DisplayModeBox, "DisplayMode");
        Label(GpuLabel, GpuBox, "Gpu");
        Label(InputLabel, InputBox, "Input");
        Label(RenderScaleLabel, RenderScaleBox, "RenderScale");
        Label(AaLabel, AaBox, "Aa");
        Label(FilterLabel, FilterBox, "Filter");
        Label(FpsLabel, FpsBox, "Fps");
        NativePortBox.Content = L.T("NativePort"); NativePortBox.ToolTip = L.T("NativePortTip");
        DiagnosticLogsBox.Content = L.T("DiagnosticLogs"); DiagnosticLogsBox.ToolTip = L.T("DiagnosticLogsTip");
        FrameGenerationBox.Content = L.T("FrameGen"); FrameGenerationBox.ToolTip = L.T("FrameGenTip");
        Label(FrameGenDllLabel, FrameGenerationDllBox, "FrameGenDll");
        MoreGraphicsExpander.Header = L.T("MoreGraphics");
        Label(ShadowLabel, ShadowBox, "Shadows"); ShadowBox.ToolTip = L.T("ShadowsTip");
        Label(MsaaLabel, MsaaBox, "ShadowMsaa"); MsaaBox.ToolTip = L.T("ShadowsTip");
        VSyncBox.Content = L.T("VSync");
        SkipIntroBox.Content = L.T("SkipIntro");
        GameFilesText.Text = L.T("GameFiles");
        Label(GameExeLabel, GameExeBox, "GameExe");
        Label(GameDataLabel, GameDataBox, "GameData");
        foreach (var b in new[] { BrowseExeButton, BrowseDataButton, BrowseDllButton }) b.Content = L.T("Browse");
        LaunchDetailsExpander.Header = L.T("LaunchDetails");
        var active = (Brush)FindResource("Accent");
        var idle = new SolidColorBrush(Color.FromRgb(0x30, 0x39, 0x4A));
        EnglishButton.Background = L.Language == "en" ? active : idle;
        PolishButton.Background = L.Language == "pl" ? active : idle;
        if (_statusKey is not null) StatusText.Text = L.T(_statusKey) + _statusSuffix;
        PopulateChoices();
    }

    private static void SetChoices(ComboBox box, params Choice[] choices)
    {
        var selected = (box.SelectedItem as Choice)?.Value;
        box.ItemsSource = choices;
        if (selected is not null) Select(box, selected);
    }
    private void PopulateChoices()
    {
        string exp = L.T("Experimental"), low = L.T("LowEnd");
        SetChoices(RendererBox, new("native", L.T("Native")), new("xenos", L.T("Xenos")));
        SetChoices(ApiBox, new("vulkan", L.T("Vulkan")), new("d3d12", "Direct3D 12"));
        SetChoices(ResolutionBox, new("960x540", $"960 × 540 {low}"), new("1280x720", "1280 × 720"), new("1600x900", "1600 × 900"), new("1920x1080", "1920 × 1080"), new("2560x1440", "2560 × 1440"), new("3840x2160", "3840 × 2160"));
        SetChoices(DisplayModeBox, new("Fullscreen", L.T("Borderless")), new("Windowed", L.T("Windowed")));
        SetChoices(InputBox, new("KeyboardMouse", L.T("KeyboardMouse")), new("Controller", L.T("Controller")));
        SetChoices(RenderScaleBox, new("auto", L.T("RenderScaleAuto")), new("50", $"50%, 360p {low}, {exp}"), new("75", $"75%, 540p {low}, {exp}"), new("100", $"100%, {L.T("Original720")}"), new("125", "125%, 900p"), new("150", "150%, 1080p"), new("200", "200%, 1440p"), new("300", "300%, 2160p"), new("400", "400%, 2880p"));
        SetChoices(AaBox, new("Off", L.T("Off")), new("FXAA", "FXAA"));
        var an = L.T("Anisotropic");
        SetChoices(FilterBox, new("Original", L.T("GameOriginal")), new("Anisotropic2x", $"{an} 2×"), new("Anisotropic4x", $"{an} 4×"), new("Anisotropic8x", $"{an} 8×"), new("Anisotropic16x", $"{an} 16×"));
        SetChoices(FpsBox, new("30", L.T("Fps30")), new("60", $"60 FPS ({exp})"), new("120", $"120 FPS ({exp})"), new("0", L.T("Uncapped")));
        SetChoices(ShadowBox, new("1", L.T("Original")), new("2", "2×"), new("4", "4×"));
        SetChoices(MsaaBox, new("1", L.T("Off")), new("4", "4×"), new("8", "8×"));
        if (GpuBox.ItemsSource is null) GpuBox.ItemsSource = GpuEnumerator.GetAdapters();
        else GpuBox.Items.Refresh();  // the automatic entry is translated
    }
    private static void Select(ComboBox box, string value) => box.SelectedItem = box.Items.Cast<Choice>().FirstOrDefault(x => x.Value == value) ?? box.Items[0];
    private void ApplySettingsToUi()
    {
        Select(RendererBox, _settings.Renderer); Select(ApiBox, _settings.GraphicsApi); SkipIntroBox.IsChecked = _settings.SkipIntro;
        Select(ResolutionBox, _settings.Resolution); Select(DisplayModeBox, _settings.DisplayMode);
        Select(InputBox, _settings.InputMode); Select(RenderScaleBox, _settings.RenderScale);
        Select(AaBox, _settings.Antialiasing); Select(FilterBox, _settings.TextureFilter);
        Select(FpsBox, _settings.FpsLimit); Select(ShadowBox, _settings.ShadowQuality); Select(MsaaBox, _settings.Msaa);
        var adapters = GpuBox.Items.Cast<GpuAdapter>().ToArray();
        GpuBox.SelectedItem = adapters.FirstOrDefault(x => !string.IsNullOrEmpty(_settings.GpuLuid) && x.Luid == _settings.GpuLuid)
            ?? adapters.FirstOrDefault(x => x.Name == _settings.Gpu) ?? adapters[0];
        VSyncBox.IsChecked = _settings.VSync;
        NativePortBox.IsChecked = _settings.NativePort;
        DiagnosticLogsBox.IsChecked = _settings.DiagnosticLogs;
        FrameGenerationBox.IsChecked = _settings.FrameGeneration;
        FrameGenerationDllBox.Text = string.IsNullOrWhiteSpace(_settings.FrameGenerationDll) ? LauncherSettings.FindFrameGenerationDll() : _settings.FrameGenerationDll;
        GameExeBox.Text = _settings.GameExe; GameDataBox.Text = _settings.GameData;
    }
    private static string Value(ComboBox box) => ((Choice)box.SelectedItem).Value;
    private void ReadSettings()
    {
        _settings.Renderer = Value(RendererBox); _settings.GraphicsApi = Value(ApiBox); _settings.SkipIntro = SkipIntroBox.IsChecked == true;
        _settings.Resolution = Value(ResolutionBox); _settings.DisplayMode = Value(DisplayModeBox);
        _settings.InputMode = Value(InputBox); _settings.RenderScale = Value(RenderScaleBox);
        _settings.Antialiasing = Value(AaBox); _settings.TextureFilter = Value(FilterBox);
        _settings.FpsLimit = Value(FpsBox); _settings.ShadowQuality = Value(ShadowBox); _settings.Msaa = Value(MsaaBox);
        var gpu = (GpuAdapter)GpuBox.SelectedItem; _settings.Gpu = gpu.Name; _settings.GpuIndex = gpu.Index; _settings.GpuLuid = gpu.Luid;
        _settings.VSync = VSyncBox.IsChecked == true;
        _settings.NativePort = NativePortBox.IsChecked == true;
        _settings.DiagnosticLogs = DiagnosticLogsBox.IsChecked == true;
        _settings.FrameGeneration = FrameGenerationBox.IsChecked == true;
        _settings.FrameGenerationDll = FrameGenerationDllBox.Text.Trim();
        _settings.GameExe = GameExeBox.Text.Trim(); _settings.GameData = GameDataBox.Text.Trim();
        _settings.Language = L.Language;
    }
    private void UpdateSummary()
    {
        if (!_ready) return;
        ReadSettings();
        FrameGenerationPanel.Visibility = _settings.FrameGeneration ? Visibility.Visible : Visibility.Collapsed;
        // The device-state renderer exists only on Vulkan; the switch is disabled elsewhere.
        NativePortBox.IsEnabled = _settings.UsesVulkan;
        var resolution = _settings.Resolution.Replace("x", " × ");
        var engine = _settings.UsesVulkan ? "Vulkan" : "Direct3D 12";
        var mode = _settings.UsesVulkan ? (_settings.NativePort ? "Native port" : "Native")
                 : _settings.Renderer == "xenos" ? "Xenos" : "Native";
        if (L.Language == "pl") mode = mode == "Native port" ? "Natywny port" : mode == "Native" ? "Natywny" : mode;
        var scale = _settings.UsesVulkan ? "100%" : _settings.RenderScale == "auto" ? L.T("RenderScaleAuto") : _settings.RenderScale + "%";
        SummaryText.Text = $"{engine} · {mode} · {resolution} · {scale} {L.T("Internal")}";
        FpsWarning.Text = _settings.FpsLimit == "30" ? "" : L.T("FpsWarn");
        FpsWarning.Visibility = _settings.FpsLimit == "30" ? Visibility.Collapsed : Visibility.Visible;
        ApiBox.ToolTip = _settings.UsesVulkan ? L.T("VulkanNote") : null;
        RendererBox.ToolTip = L.T("XenosNote");
        RenderScaleBox.ToolTip = L.T("LowScaleNote");
        var env = _settings.BuildEnvironment();
        ArgumentsPreview.Text = (env.Count > 0 ? string.Join(' ', env.Select(e => $"{e.Name}={e.Value}")) + "\n" : "") + _settings.BuildArguments();
    }
    private void English_Click(object sender, RoutedEventArgs e) => SwitchLanguage("en");
    private void Polish_Click(object sender, RoutedEventArgs e) => SwitchLanguage("pl");
    private void SwitchLanguage(string language)
    {
        if (L.Language == language) return;
        _ready = false;
        L.Language = language;
        ApplyLanguage();
        _ready = true;
        UpdateSummary();
    }
    private void Save_Click(object sender, RoutedEventArgs e)
    {
        try { ReadSettings(); _settings.Save(_settingsPath); SetStatus("Saved"); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { SetStatus("SaveFailed", error.Message); }
    }
    private void Play_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            ReadSettings();
            var start = _settings.CreateLaunch(AppContext.BaseDirectory);
            _settings.Save(_settingsPath);
            Process.Start(start);
            Close();
        }
        catch (Exception error) when (error is InvalidOperationException or IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception or ArgumentException)
        { SetRawStatus(error.Message); }
    }
    private void Browse_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "Superman Returns|superman_returns.exe", Title = L.T("PickExe") };
        if (dialog.ShowDialog() != true) return;
        GameExeBox.Text = dialog.FileName;
        var adjacent = Path.Combine(Path.GetDirectoryName(dialog.FileName)!, "game");
        if (File.Exists(Path.Combine(adjacent, "default.xex"))) GameDataBox.Text = adjacent;
    }
    private void BrowseData_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = L.T("PickData") };
        if (dialog.ShowDialog() == true) GameDataBox.Text = dialog.FolderName;
    }
    private void BrowseFrameGeneration_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "Lossless Scaling|Lossless.dll", Title = L.T("PickDll") };
        if (dialog.ShowDialog() == true) FrameGenerationDllBox.Text = dialog.FileName;
    }
    private void Defaults_Click(object sender, RoutedEventArgs e)
    {
        ReadSettings();
        _settings = new LauncherSettings { GameExe = _settings.GameExe, GameData = _settings.GameData, Gpu = _settings.Gpu, GpuLuid = _settings.GpuLuid, Language = L.Language };
        _ready = false; ApplySettingsToUi(); _ready = true; UpdateSummary();
        SetStatus("DefaultsRestored");
    }
}
