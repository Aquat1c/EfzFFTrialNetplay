param(
    [string]$Launcher = (Join-Path $PSScriptRoot "..\trial_caster\build\bin\EFZ FF Netplay.exe"),
    [string]$Output = (Join-Path $PSScriptRoot "..\artifacts\launcher_live.png"),
    [ValidateSet("Play", "Practice", "Controls", "Settings")]
    [string]$Page = "Play",
    [ValidateRange(0, 7680)]
    [int]$Width = 0,
    [ValidateRange(0, 4320)]
    [int]$Height = 0,
    [switch]$Maximize,
    [switch]$OpenAdvanced,
    [switch]$OpenDiagnostics
)

$ErrorActionPreference = "Stop"
$Launcher = (Resolve-Path $Launcher).Path
$outputDirectory = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class TrialLauncherCaptureNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool MoveWindow(IntPtr window, int x, int y, int width, int height, bool repaint);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extraInfo);
}
'@

$process = Start-Process -FilePath $Launcher -PassThru
try {
    $deadline = (Get-Date).AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 200
        $process.Refresh()
    } while ($process.MainWindowHandle -eq 0 -and -not $process.HasExited -and (Get-Date) -lt $deadline)

    if ($process.HasExited -or $process.MainWindowHandle -eq 0) {
        throw "Launcher did not create a visible window"
    }
    Start-Sleep -Seconds 2
    # Refresh: MainWindowHandle can stay cached on a transient helper HWND.
    $process.Refresh()
    $windowHandle = $process.MainWindowHandle
    if ($windowHandle -eq 0) {
        throw "Launcher lost its visible window during initialization"
    }

    if ($Maximize) {
        [TrialLauncherCaptureNative]::ShowWindow($windowHandle, 3) | Out-Null
        Start-Sleep -Seconds 1
    } elseif ($Width -gt 0 -or $Height -gt 0) {
        if ($Width -le 0 -or $Height -le 0) {
            throw "Width and Height must be provided together"
        }
        [TrialLauncherCaptureNative]::MoveWindow(
            $windowHandle, 40, 40, $Width, $Height, $true) | Out-Null
        Start-Sleep -Seconds 1
    }

    $rect = New-Object TrialLauncherCaptureNative+Rect
    if (-not [TrialLauncherCaptureNative]::GetWindowRect($windowHandle, [ref]$rect)) {
        throw "GetWindowRect failed"
    }
    if ($Page -ne "Play") {
        $pageRow = @{ Practice = 1; Controls = 2; Settings = 3 }[$Page]
        $scale = [Math]::Max(0.75, ($rect.Right - $rect.Left) / 776.0)
        $clickX = $rect.Left + [int](78 * $scale)
        $clickY = $rect.Top + [int]((159 + 44 * $pageRow) * $scale)
        [TrialLauncherCaptureNative]::SetForegroundWindow($windowHandle) | Out-Null
        # Windows may consume the first click while activating; let foreground settle.
        Start-Sleep -Milliseconds 300
        [TrialLauncherCaptureNative]::SetCursorPos($clickX, $clickY) | Out-Null
        [TrialLauncherCaptureNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        [TrialLauncherCaptureNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 1
    }
    if ($OpenAdvanced) {
        if ($Page -ne "Play" -or $Maximize) {
            throw "OpenAdvanced currently requires the normal-size Play page"
        }
        $scale = [Math]::Max(0.75, ($rect.Right - $rect.Left) / 776.0)
        [TrialLauncherCaptureNative]::SetForegroundWindow($windowHandle) | Out-Null
        [TrialLauncherCaptureNative]::SetCursorPos(
            $rect.Left + [int](360 * $scale),
            $rect.Top + [int](366 * $scale)) | Out-Null
        [TrialLauncherCaptureNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        [TrialLauncherCaptureNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 1
    }
    if ($OpenDiagnostics) {
        if ($Page -ne "Settings" -or $Maximize) {
            throw "OpenDiagnostics currently requires the normal-size Settings page"
        }
        $scale = [Math]::Max(0.75, ($rect.Right - $rect.Left) / 776.0)
        [TrialLauncherCaptureNative]::SetForegroundWindow($windowHandle) | Out-Null
        Start-Sleep -Milliseconds 300
        [TrialLauncherCaptureNative]::SetCursorPos(
            $rect.Left + [int](360 * $scale),
            $rect.Top + [int](482 * $scale)) | Out-Null
        [TrialLauncherCaptureNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        [TrialLauncherCaptureNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 1
    }

    $process.Refresh()
    if ($process.HasExited -or $process.MainWindowHandle -eq 0) {
        throw "Launcher window closed before capture"
    }
    [TrialLauncherCaptureNative]::SetForegroundWindow($process.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 400
    # Re-read the rectangle: window managers may adjust it on restore or DPI moves.
    if (-not [TrialLauncherCaptureNative]::GetWindowRect($process.MainWindowHandle, [ref]$rect)) {
        throw "GetWindowRect failed after foreground activation"
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
        throw "Launcher returned an invalid window rectangle"
    }

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        } finally {
            $graphics.Dispose()
        }
        $bitmap.Save($Output, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
    }
    Write-Host "Captured launcher PID=$($process.Id) ${width}x${height} -> $Output"
} finally {
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}
