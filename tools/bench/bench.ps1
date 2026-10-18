<#
Open-world benchmark for the Superman Returns recomp.

Boots the game with the intro skipped, starts a new game at the title screen,
lets it settle, then measures two scenarios:
  idle     - standing still for 20 s
  forward  - holding W (move forward) for 20 s
and appends one CSV row per scenario to logs/bench_results.csv
(name, scenario, avg/min guest FPS, CPU cores in use, GPU 3D %, args).
Screenshots of each scenario go to logs/bench_<name>_<scenario>.png.

Any running instance of the game is closed first. The game runs windowed unless
ExtraArgs sets --fullscreen: the HUD check reads a screen capture, which does not
see a fullscreen Vulkan window. It looks for the HUD wherever the window is, and
alternates Space (A) and Enter (Start) at most -MaxSkips times to confirm a late
dialog or skip the opening cinematic. -Exe selects the executable (default: the
win-amd64-release build).

The HUD is detected (bench_hud.ps1) by the wait loop before the run and checked again
right after the forward window, while W is held, with a short poll: the game fades
the HUD after ~20 s without input, so it is not checked after idle. A run that is not
on gameplay (menu, title) throws and leaves logs/bench_<name>_not_gameplay.png.

Usage: tools\bench\bench.ps1 -Name baseline [-ExtraArgs "--native_2x_msaa=false"]
#>
param(
  [Parameter(Mandatory)] [string]$Name,
  [string]$ExtraArgs = "",
  [string]$Exe = "",
  [int]$MaxSkips = 6,
  [int]$TitleTimeout = 60,
  [int]$Settle = 25,
  [int]$Window = 20,
  [int]$WorldTimeout = 90
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$exe = if ($Exe) { $Exe } else { "$root\port\out\build\win-amd64-release\superman_returns.exe" }
$log = "$root\logs\game.log"
$progress = "$root\logs\bench_progress.txt"
$results = "$root\logs\bench_results.csv"
New-Item -ItemType Directory -Force "$root\logs" | Out-Null

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
. "$PSScriptRoot\bench_hud.ps1"
if (-not ([System.Management.Automation.PSTypeName]'SrBenchInput').Type) {
  Add-Type @"
using System; using System.Runtime.InteropServices;
public struct SrRect { public int Left, Top, Right, Bottom; }
public struct SrPoint { public int X, Y; }
public static class SrBenchInput {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out SrRect r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref SrPoint p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
}

function Step($message) {
  "{0:HH:mm:ss} [{1}] {2}" -f (Get-Date), $Name, $message | Out-File -Append -Encoding utf8 $progress
}

function Focus($proc) {
  [SrBenchInput]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
  Start-Sleep -Milliseconds 300
}

function Key($vk, [switch]$Down, [switch]$Up) {
  if (-not $Up) { [SrBenchInput]::keybd_event($vk, 0, 0, [UIntPtr]::Zero) }
  if (-not $Down) { Start-Sleep -Milliseconds 120; [SrBenchInput]::keybd_event($vk, 0, 2, [UIntPtr]::Zero) }
}

function Screenshot($path) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  try {
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    try { $gfx.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size) }
    finally { $gfx.Dispose() }
    $bmp.Save($path)
  } finally { $bmp.Dispose() }
}

# Client area of the game window in screen coordinates, clipped to the primary
# screen (the window may sit anywhere and even hang over the screen edge).
function Game-Region($proc) {
  $screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $proc.Refresh()
  $handle = $proc.MainWindowHandle
  $rect = New-Object SrRect
  $origin = New-Object SrPoint
  if ($handle -eq [IntPtr]::Zero -or -not [SrBenchInput]::GetClientRect($handle, [ref]$rect)) { return $screen }
  [SrBenchInput]::ClientToScreen($handle, [ref]$origin) | Out-Null
  $client = New-Object System.Drawing.Rectangle $origin.X, $origin.Y, $rect.Right, $rect.Bottom
  $visible = [System.Drawing.Rectangle]::Intersect($client, $screen)
  if ($visible.Width -lt 200 -or $visible.Height -lt 200) { return $screen }
  return $visible
}

# Gameplay is on screen when the Superman HUD (a thin blue and a thin red bar, top
# left of the game image) is found (Test-SupermanHud, which scans the top left of the
# bitmap it gets, so the whole client area is captured). The title, load prompt and
# opening cinematic can all show a bright rendered scene without it. The capture
# covers the game window wherever it is, so it works windowed and fullscreen.
function Has-Gameplay-Frame($proc) {
  $region = Game-Region $proc
  $bmp = New-Object System.Drawing.Bitmap $region.Width, $region.Height
  try {
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    try { $gfx.CopyFromScreen($region.Location, [System.Drawing.Point]::Empty, $region.Size) }
    finally { $gfx.Dispose() }
    return (Test-SupermanHud $bmp)
  } finally {
    $bmp.Dispose()
  }
}

# Saves the game window's client area as PNG (the image gate compares it with a reference).
function Save-GameRegion($proc, $path) {
  $region = Game-Region $proc
  $bmp = New-Object System.Drawing.Bitmap $region.Width, $region.Height
  try {
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    try { $gfx.CopyFromScreen($region.Location, [System.Drawing.Point]::Empty, $region.Size) }
    finally { $gfx.Dispose() }
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  } finally { $bmp.Dispose() }
}

# Throws (leaving a screenshot) unless the HUD is seen in one of up to 4 samples, one
# second apart (a single sample can miss it on D3D12), so a run on a menu or the
# title screen cannot go unnoticed.
function Assert-Gameplay($proc) {
  for ($i = 0; $i -lt 4; $i++) {
    if ($i -gt 0) { Start-Sleep 1 }
    if (Has-Gameplay-Frame $proc) { return }
  }
  Save-GameRegion $proc "$root\logs\bench_${Name}_not_gameplay.png"
  throw "scene is not gameplay during measurement (menu or title?)"
}

# True when the whole screen capture is black. A fullscreen game window (notably a
# Vulkan one) may not show up in a GDI screen capture, which breaks HUD detection.
function Capture-IsBlack() {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  try {
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    try { $gfx.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size) }
    finally { $gfx.Dispose() }
    for ($y = 1; $y -le 9; $y++) {
      for ($x = 1; $x -le 16; $x++) {
        $pixel = $bmp.GetPixel([int]($x * $b.Width / 17), [int]($y * $b.Height / 10))
        if (($pixel.R + $pixel.G + $pixel.B) -gt 3) { return $false }
      }
    }
    return $true
  } finally { $bmp.Dispose() }
}

function Measure-Window($proc, $scenario) {
  $lines0 = @(Get-Content $log).Count
  $proc.Refresh(); $cpu0 = $proc.TotalProcessorTime.TotalSeconds
  $timer = [System.Diagnostics.Stopwatch]::StartNew()
  $gpu = Get-Counter "\GPU Engine(pid_$($proc.Id)*engtype_3D)\Utilization Percentage" `
    -SampleInterval $Window -MaxSamples 1 -ErrorAction SilentlyContinue
  # Counter availability or an early first sample must not shorten the window.
  $remaining = $Window - $timer.Elapsed.TotalSeconds
  if ($remaining -gt 0) { Start-Sleep -Milliseconds ([int][math]::Ceiling($remaining * 1000)) }
  $timer.Stop()
  $proc.Refresh(); $cores = ($proc.TotalProcessorTime.TotalSeconds - $cpu0) / $timer.Elapsed.TotalSeconds
  $gpuPct = ($gpu.CounterSamples | Measure-Object CookedValue -Sum).Sum
  $fps = @(Get-Content $log | Select-Object -Skip $lines0 | Select-String 'guest fps: ([\d.]+)' |
    ForEach-Object { [double]::Parse($_.Matches[0].Groups[1].Value, [System.Globalization.CultureInfo]::InvariantCulture) })
  if (-not $fps.Count) { throw "No guest FPS samples during $scenario; benchmark is invalid" }
  Save-GameRegion $proc "$root\logs\bench_${Name}_$scenario.png"
  $row = [pscustomobject]@{
    time = Get-Date -Format s; name = $Name; scenario = $scenario
    avg_fps = [math]::Round(($fps | Measure-Object -Average).Average, 1)
    min_fps = [math]::Round(($fps | Measure-Object -Minimum).Minimum, 1)
    cores = [math]::Round($cores, 1); gpu_pct = [math]::Round($gpuPct, 0); args = $ExtraArgs
  }
  $row | Export-Csv -Append -NoTypeInformation -Encoding utf8 $results
  Step "$scenario avg=$($row.avg_fps) min=$($row.min_fps) cores=$($row.cores) gpu=$($row.gpu_pct)"
  $row
}

Get-Process superman_returns -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
if (Test-Path $log) { Clear-Content $log }
$env:SR_LOG_FPS = '1'
# The HUD check reads a screen capture, which only sees a windowed game window.
if ($ExtraArgs -notmatch '--fullscreen') { $ExtraArgs = ("$ExtraArgs --fullscreen=false").Trim() }
$gameArgs = @(
  "--game_data_root=`"$root\game`"", "--log_file=`"$log`"",
  '--render_target_path_d3d12=rtv', '--depth_float24_convert_in_pixel_shader=true',
  '--mnk_mode', '--sr_skip_intro=true'
) + ($ExtraArgs -split ' ' | Where-Object { $_ })
$proc = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory (Split-Path $exe) -PassThru
Step "started pid $($proc.Id) args: $ExtraArgs"

try {
  # Title screen is up once the game presents frames steadily.
  $deadline = (Get-Date).AddSeconds($TitleTimeout)
  do {
    Start-Sleep 2
    if ($proc.HasExited) { throw "game exited before the title screen" }
    $frames = @(Get-Content $log | Select-String 'guest fps: ([\d.]+)').Count
  } until ($frames -ge 5 -or (Get-Date) -gt $deadline)
  Start-Sleep 5
  Focus $proc; Key 0x0D
  Step "pressed Start"
  Start-Sleep 3
  Focus $proc; Key 0x20
  Step "selected Start New Game"
  Start-Sleep $Settle
  if ($proc.HasExited) { throw "game exited while loading the save" }
  $worldDeadline = (Get-Date).AddSeconds($WorldTimeout)
  $nextSkip = Get-Date
  $skips = 0; $blackSamples = 0; $samples = 0
  while ($true) {
    Focus $proc
    if (Has-Gameplay-Frame $proc) { break }
    if ($proc.HasExited) { throw "game exited before rendering the save" }
    $samples++; if (Capture-IsBlack) { $blackSamples++ }
    if ((Get-Date) -gt $worldDeadline) {
      Save-GameRegion $proc "$root\logs\bench_${Name}_not_gameplay.png"
      $why = if ($blackSamples -eq $samples) {
        "every screen capture was black: the game window cannot be captured (fullscreen?); run windowed (--fullscreen=false)"
      } else { "the Superman HUD was never seen" }
      throw "no gameplay frame after $WorldTimeout seconds ($why)"
    }
    # The new-game opening cinematic runs for minutes; Start skips it, and a late
    # "Load a game?" dialog needs A (Space) to select. Space and Enter alternate,
    # Space first. The HUD check above runs first so this never opens the pause
    # menu in gameplay. At most $MaxSkips presses: when the HUD cannot be
    # detected an unbounded loop would keep toggling the pause menu in gameplay.
    if ($skips -lt $MaxSkips -and (Get-Date) -ge $nextSkip) {
      if ($skips % 2 -eq 0) { Key 0x20; $keyName = 'Space (A)' }
      else { Key 0x0D; $keyName = 'Enter (Start)' }
      $skips++
      Step "pressed $keyName to confirm a dialog or skip the cinematic ($skips/$MaxSkips)"
      $nextSkip = (Get-Date).AddSeconds(8)
    }
    Start-Sleep 2
  }
  Step "visible frame detected"
  # Initial camera and pose: the one scene that does not vary between runs (the image gate compares it).
  Save-GameRegion $proc "$root\logs\bench_${Name}_start.png"
  Start-Sleep 5

  # The wait loop above detected the HUD seconds ago, so it is not checked again
  # before idle. The game fades the HUD after ~20 s without input, so it is not
  # checked after idle either; it is asserted right after the forward window while W
  # is still held (input keeps it visible). A menu or title cannot produce a clean
  # run: the HUD must still be visible after the forward window.
  Measure-Window $proc 'idle'
  Focus $proc; Key 0x57 -Down
  try {
    Measure-Window $proc 'forward'
    Assert-Gameplay $proc
  } finally { Key 0x57 -Up }
} catch {
  Step "FAILED: $_"
  throw
} finally {
  if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}
