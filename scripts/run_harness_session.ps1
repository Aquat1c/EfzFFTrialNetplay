# Loopback lifecycle harness runner. Exit 0 = every enabled assertion passed.
param(
    [string]$GameDir = (Join-Path $PSScriptRoot "..\game"),
    # Safety frame cap across all battle rounds.
    [int]$Frames = 30000,
    [int]$InputDelay = 1,
    [int]$RollbackFrames = 6,
    [ValidateSet(60, 64)]
    [int]$TargetFps = 60,
    [int]$ForceKoAfterFrames = 600,
    [Alias("Matches")]
    [int]$RequiredMatches = 1,  # completed best-of-3 matches before entering the next one
    [int]$GameplayEscAfterFrames = 0, # focused battle -> char-select synchronization test
    [int]$Seed = 1234,
    [int]$BasePort = 7500,
    [ValidateRange(0, 60000)]
    [int]$DelayMs = 40,         # one-way artificial delay (per side)
    [ValidateRange(0, 60000)]
    [int]$JitterMs = 15,
    [ValidateRange(0, 90)]
    [int]$LossPct = 3,
    # Seconds after launch to kill the client; zero runs the full-match path.
    [int]$DisconnectAfterSec = 0,
    # Forced KO writes are forbidden with viewers: they are not recorded input.
    [switch]$Spectator,
    [ValidateRange(1, 16)]
    [int]$SpectatorCount = 2,
    [ValidateSet("Host", "Client", "Alternate")]
    [string]$SpectatorSource = "Host",
    [ValidateSet("Auto", "Required", "Disabled")]
    [string]$SpectatorRedirectProof = "Auto",
    [ValidateRange(0, 600)]
    [int]$SpectatorLaunchDelaySec = 0,
    [ValidateRange(0, 600)]
    [int]$SpectatorStaggerSec = 5,
    [ValidateRange(0, 60000)]
    [int]$SpectatorDelayMs = 0,
    [ValidateRange(0, 60000)]
    [int]$SpectatorJitterMs = 0,
    [ValidateRange(0, 90)]
    [int]$SpectatorLossPct = 0,
    [ValidateRange(0, 100)]
    [int]$SpectatorDuplicatePct = 0,
    [ValidateRange(0, 100)]
    [int]$SpectatorReorderPct = 0,
    [int]$SpectatorVerifiedFrames = 512,
    # Zero disables the terminal check.
    [ValidateRange(0, 180)]
    [int]$SpectatorTerminalTimeoutSec = 75,
    [ValidateRange(0, 1000)]
    [int]$HostPerfMinSamples = 5,
    [ValidateRange(0.0, 1000.0)]
    [double]$MinHostVisibleHzP10 = 55.0,
    [ValidateRange(0, 1000000)]
    [int]$MaxHostMainP95Us = 19000,
    [bool]$RequireSystemKeys = $true,
    [switch]$EarlySystemKeyProbe,
    [bool]$AllowPracticeKeys = $false,
    # Disable only for DLL bring-up.
    [bool]$RequirePairedInputDigest = $true,
    [switch]$NoStress,          # clean link (baseline determinism run)
    [switch]$Deploy,            # re-deploy netplay.dll from the build tree first
    [int]$TimeoutSec = 0
)

$ErrorActionPreference = "Stop"

# Named mutex, not a lock file: CreateProcess would inherit the handle into EFZ.
$runLock = [System.Threading.Mutex]::new($false, "Local\TrialNetplayHarnessPorts7500_7501")
$runLockHeld = $false
try {
    $runLockHeld = $runLock.WaitOne(0)
} catch [System.Threading.AbandonedMutexException] {
    $runLockHeld = $true
}
if (-not $runLockHeld) {
    $runLock.Dispose()
    Write-Host "another run_harness_session.ps1 invocation owns the test ports; refusing to cross-pair sessions"
    exit 3
}

function Release-RunLock {
    if ($script:runLockHeld) {
        $script:runLock.ReleaseMutex()
        $script:runLockHeld = $false
    }
    $script:runLock.Dispose()
}

$GameDir = (Resolve-Path $GameDir).Path
$efz = Join-Path $GameDir "EFZ.EXE"
if (-not (Test-Path $efz)) { throw "EFZ.EXE not found in $GameDir" }
if ($InputDelay -lt 0 -or $InputDelay -gt 20) { throw "InputDelay must be in 0..20" }
if ($RollbackFrames -lt 0 -or $RollbackFrames -gt 20) { throw "RollbackFrames must be in 0..20" }
if ($RequiredMatches -lt 1 -or $RequiredMatches -gt 32) { throw "Matches must be in 1..32" }
if ($GameplayEscAfterFrames -lt 0) { throw "GameplayEscAfterFrames must be >= 0" }
if ($EarlySystemKeyProbe -and (-not $Spectator -or $DisconnectAfterSec -le 0)) {
    throw "EarlySystemKeyProbe requires -Spectator and a positive -DisconnectAfterSec"
}
if ($EarlySystemKeyProbe -and -not $AllowPracticeKeys) {
    throw "EarlySystemKeyProbe requires -AllowPracticeKeys:`$true"
}
if ($Spectator -and $DisconnectAfterSec -gt 0 -and $ForceKoAfterFrames -ne 0) {
    Write-Host "spectator disconnect scenario: disabling forced KO because HP writes are not part of the replay stream"
    $ForceKoAfterFrames = 0
} elseif ($Spectator -and $ForceKoAfterFrames -ne 0) {
    throw "Spectator fidelity runs require -ForceKoAfterFrames 0 (forced HP writes are not recorded inputs)"
}
if ($Spectator -and $DisconnectAfterSec -gt 0 -and $SpectatorTerminalTimeoutSec -eq 0) {
    throw "Spectator disconnect runs require -SpectatorTerminalTimeoutSec greater than zero"
}
if ($Spectator -and $DisconnectAfterSec -gt 0 -and $SpectatorSource -ne "Host") {
    throw "Disconnect proof must use -SpectatorSource Host because the client is intentionally force-killed"
}
if ($Spectator -and $DisconnectAfterSec -gt 0 -and $SpectatorCount -gt 1) {
    throw "Disconnect proof must use -SpectatorCount 1; redirected viewers intentionally served by the client cannot outlive a forced client kill"
}
if ($SpectatorRedirectProof -eq "Required" -and
    (-not $Spectator -or $SpectatorSource -ne "Host" -or $SpectatorCount -lt 2)) {
    throw "SpectatorRedirectProof Required needs -Spectator -SpectatorSource Host -SpectatorCount 2 or greater"
}
if ($Spectator -and $GameplayEscAfterFrames -gt 0) {
    throw "Spectator lifecycle runs must reach the real winner/rematch path; run gameplay ESC as a separate player-only scenario"
}
if ($SpectatorVerifiedFrames -lt 1) { throw "SpectatorVerifiedFrames must be >= 1" }
if ($BasePort -lt 1 -or $BasePort + 2 * $SpectatorCount + 1 -gt 65535) {
    throw "BasePort does not leave room for the player and spectator port range"
}
# Leave automatic $Matches untyped; the evidence parsers below rely on it.
$RequestedMatches = $RequiredMatches
$redirectProofRequired = $SpectatorRedirectProof -eq "Required" -or
    ($SpectatorRedirectProof -eq "Auto" -and $Spectator -and
     $SpectatorSource -eq "Host" -and $SpectatorCount -ge 2)

if ($Deploy) {
    & (Join-Path $PSScriptRoot "deploy_game.ps1") -GameDir $GameDir
}

if ($NoStress) {
    $DelayMs = 0; $JitterMs = 0; $LossPct = 0
    $SpectatorDelayMs = 0; $SpectatorJitterMs = 0; $SpectatorLossPct = 0
    $SpectatorDuplicatePct = 0; $SpectatorReorderPct = 0
}
$spectatorImpairmentEnabled = $SpectatorDelayMs -gt 0 -or $SpectatorJitterMs -gt 0 -or
    $SpectatorLossPct -gt 0 -or $SpectatorDuplicatePct -gt 0 -or $SpectatorReorderPct -gt 0
if ($spectatorImpairmentEnabled -and -not $Spectator) {
    throw "Spectator impairment knobs require -Spectator"
}
if ($TimeoutSec -le 0) {
    $frameBudgetSec = [int][Math]::Ceiling($Frames / [double]$TargetFps) + 240
    $setBudgetSec = 480 + 240 * $RequestedMatches
    $TimeoutSec = [Math]::Max($frameBudgetSec, $setBudgetSec)
}

$compatHeader = Get-Content (Join-Path $PSScriptRoot "..\shared\trial_compatibility.h") -Raw
$compatMajorMatch = [regex]::Match($compatHeader, 'kLauncherProtocolMajor\s*=\s*(\d+)u')
$compatVersionMatch = [regex]::Match($compatHeader, 'kCompatibilityVersion\s*=\s*(\d+)u')
if (-not $compatMajorMatch.Success -or -not $compatVersionMatch.Success) {
    throw "Unable to read shared netplay compatibility version"
}
$compatMajor = [int]$compatMajorMatch.Groups[1].Value
$compatVersion = [int]$compatVersionMatch.Groups[1].Value
$compatId = ($compatMajor -shl 16) -bor $compatVersion

function Write-HarnessIni([string]$Path, [bool]$IsHost) {
    $port     = if ($IsHost) { $BasePort } else { $BasePort + 1 }
    $peerPort = if ($IsHost) { $BasePort + 1 } else { $BasePort }
    # The input seed stays shared; shim seeds must differ per direction.
    $netRoleSalt = if ($IsHost) { 0x13579BDF } else { 0x2468ACE0 }
    $netShimSeed = (([int64]$Seed -bxor [int64]$netRoleSalt) -band 0x7FFFFFFF)
    if ($netShimSeed -eq 0) { $netShimSeed = $(if ($IsHost) { 1 } else { 2 }) }
    @"
[session]
mode = $(if ($IsHost) { "host" } else { "client" })
ready = 1
is_host = $(if ($IsHost) { 1 } else { 0 })
local_is_p1 = $(if ($IsHost) { 1 } else { 0 })
peer_ip = 127.0.0.1
port = $port
peer_port = $peerPort
input_delay = $InputDelay
rollback_frames = $RollbackFrames
target_fps = $TargetFps
diagnostic_logging = 1
allow_practice_keys = $(if ($AllowPracticeKeys) { 1 } else { 0 })
ping_ms = $DelayMs
rollback = 1
autopunch = 0
compatibility_id = $compatId
compatibility_version = $compatVersion
player_name = $(if ($IsHost) { "HarnessHost" } else { "HarnessClient" })
peer_name = $(if ($IsHost) { "HarnessClient" } else { "HarnessHost" })
session_id = harness

[shortcuts]
increase_input_delay = KP_PLUS
decrease_input_delay = KP_MINUS
training_mode = F4
restore_resources = F5
toggle_hitboxes = PAGEUP
toggle_hurtboxes = PAGEDOWN
toggle_collision = HOME
pause = SPACE
step_frame = P
switch_player = KP_0
previous_save_slot = LEFTBRACKET
next_save_slot = RIGHTBRACKET
save_state = G
load_state = B
debug_save = F6
debug_load = F7

[test]
input_script = 1
input_seed = $Seed
net_delay_ms = $DelayMs
net_jitter_ms = $JitterMs
net_loss_pct = $LossPct
net_shim_seed = $netShimSeed
force_ko_after_frames = $ForceKoAfterFrames
required_matches = $RequestedMatches
gameplay_esc_after_frames = $GameplayEscAfterFrames
early_system_key_probe = $(if ($EarlySystemKeyProbe) { 1 } else { 0 })
max_battle_frames = $Frames
"@ | Set-Content -Path $Path -Encoding ASCII
}

$clientSpectatorPort = 0

function Get-SpectatorSourceRole([int]$Index) {
    if ($SpectatorSource -eq "Alternate") {
        return $(if (($Index % 2) -eq 0) { "Host" } else { "Client" })
    }
    return $SpectatorSource
}

function Get-SpectatorSourcePort([int]$Index) {
    return $(if ((Get-SpectatorSourceRole $Index) -eq "Client") {
        if ($script:clientSpectatorPort -ne 0) {
            $script:clientSpectatorPort
        } else {
            $BasePort + 1
        }
    } else {
        $BasePort
    })
}

function Write-SpectatorIni([string]$Path, [int]$Index) {
    $viewerNumber = $Index + 1
    $localPort = $BasePort + 2 + $Index
    $proxyPort = $BasePort + 2 + $SpectatorCount + $Index
    $targetPort = if ($spectatorImpairmentEnabled) {
        $proxyPort
    } else {
        Get-SpectatorSourcePort $Index
    }
    @"
[session]
mode = spectator
ready = 1
is_host = 0
local_is_p1 = 1
peer_ip = 127.0.0.1
port = $localPort
peer_port = $targetPort
input_delay = 0
rollback_frames = 0
target_fps = $TargetFps
diagnostic_logging = 1
allow_practice_keys = 0
ping_ms = 0
rollback = 0
autopunch = 0
compatibility_id = $compatId
compatibility_version = $compatVersion
player_name = HarnessSpectator$viewerNumber
session_id = harness_spectator_$viewerNumber

[shortcuts]
increase_input_delay = KP_PLUS
decrease_input_delay = KP_MINUS
training_mode = F4
restore_resources = F5
toggle_hitboxes = PAGEUP
toggle_hurtboxes = PAGEDOWN
toggle_collision = HOME
pause = SPACE
step_frame = P
switch_player = KP_0
previous_save_slot = LEFTBRACKET
next_save_slot = RIGHTBRACKET
save_state = G
load_state = B
debug_save = F6
debug_load = F7
"@ | Set-Content -Path $Path -Encoding ASCII
}

$hostIni = Join-Path $GameDir "harness_session_host.ini"
$clientIni = Join-Path $GameDir "harness_session_client.ini"
Write-HarnessIni $hostIni $true
Write-HarnessIni $clientIni $false
$spectatorInis = @()
if ($Spectator) {
    for ($index = 0; $index -lt $SpectatorCount; ++$index) {
        $spectatorIni = Join-Path $GameDir "harness_session_spectator_$($index + 1).ini"
        Write-SpectatorIni $spectatorIni $index
        $spectatorInis += $spectatorIni
    }
}

$startTime = Get-Date
$smoke = Join-Path $PSScriptRoot "..\trial_caster\build\bin\launch_smoke.exe"
$netplayDll = Join-Path $PSScriptRoot "..\trial_hitbox_viewer\build\bin\Release\netplay.dll"
if (-not (Test-Path $smoke)) { throw "caster launch_smoke.exe not found: $smoke (build trial_caster first)" }
if (-not (Test-Path $netplayDll)) { throw "fresh caster-injected netplay.dll not found: $netplayDll (build netplay first)" }

function Get-InstanceExitReason([int]$ProcessId, [datetime]$Since) {
    if ($ProcessId -le 0) { return $null }

    $logRoot = Join-Path $GameDir "logs"
    $exitFiles = @(Get-ChildItem -Path (Join-Path $logRoot "PID${ProcessId}_*") -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "exit.log" } |
        Where-Object { Test-Path $_ })
    $reasons = @()
    foreach ($exitFile in $exitFiles) {
        foreach ($line in @(Get-Content $exitFile -ErrorAction SilentlyContinue)) {
            if ($line -notmatch '^\[(\d{2}:\d{2}:\d{2}\.\d{3})\]\s+ERROR\s+exit\s+(.+)$') { continue }
            $lineTime = $Since.Date.Add(
                [TimeSpan]::ParseExact($Matches[1], 'hh\:mm\:ss\.fff', [Globalization.CultureInfo]::InvariantCulture))
            if ($lineTime -lt $Since.AddHours(-12)) { $lineTime = $lineTime.AddDays(1) }
            if ($lineTime -ge $Since.AddSeconds(-1)) {
                $reasons += [pscustomobject]@{ Time = $lineTime; Reason = $Matches[2] }
            }
        }
    }
    $latest = $reasons | Sort-Object Time | Select-Object -Last 1
    if ($latest) { return $latest.Reason }
    return $null
}

function Start-Instance([string]$Ini, [string]$Label, [scriptblock]$KeepAlive, [int]$Retries = 12) {
    for ($try = 1; $try -le $Retries; $try++) {
        $attemptStart = Get-Date
        # Launch paths may replace EFZ.exe with a renamed copy; track both names.
        $beforeEfzIds = @(Get-Process EFZ,trial_EFZ -ErrorAction SilentlyContinue | ForEach-Object Id)
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $smoke
        $psi.Arguments = "`"$efz`" `"$netplayDll`""
        $psi.WorkingDirectory = $GameDir
        $psi.UseShellExecute = $false
        $psi.EnvironmentVariables["TRIAL_CASTER_SESSION_INI"] = $Ini
        $psi.EnvironmentVariables["TRIAL_CASTER_ONLY"] = "1"
        $psi.EnvironmentVariables["TRIAL_NETPLAY_STATE_DIR"] = $GameDir

        # Parse the pid launch_smoke prints; Get-Process would race.
        $efzProc = $null
        $reportedPid = 0
        $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
        $sp = [System.Diagnostics.Process]::Start($psi)
        $so = $sp.StandardError.ReadToEnd() + $sp.StandardOutput.ReadToEnd()
        $sp.WaitForExit()
        Start-Sleep -Milliseconds 800
        if ($so -match "PID (\d+)") {
            $reportedPid = [int]$Matches[1]
            $efzProc = Get-Process -Id $reportedPid -ErrorAction SilentlyContinue
        }
        $cool = [Math]::Min(6 + ($try - 1) * 2, 14)
        if (-not $efzProc) {
            $reason = Get-InstanceExitReason $reportedPid $attemptStart
            $detail = if ($reason) { "reason: $reason" } else { "no current exit.log reason" }
            Write-Host "launch $Label try ${try}: EFZ exited before startup health check ($detail) - retrying after ${cool}s"
            & $KeepAlive
            Start-Sleep -Seconds $cool
            continue
        }

        $dead = $false
        for ($w = 0; $w -lt 12; $w++) {   # ~6s startup/session grace
            Start-Sleep -Milliseconds 500
            $efzProc.Refresh()
            if ($efzProc.HasExited) { $dead = $true; break }
        }
        if (-not $dead -and -not $efzProc.HasExited) {
            Write-Host "launched $Label pid=$($efzProc.Id) (try $try) - startup health check passed (alive 6s)"
            return $efzProc
        }

        $replacement = $null
        # Keep this probe below the 30-second netplay handshake deadline.
        for ($scan = 0; $scan -lt 6 -and -not $replacement; $scan++) {
            $candidates = @(Get-Process EFZ,trial_EFZ -ErrorAction SilentlyContinue | Where-Object {
                $beforeEfzIds -notcontains $_.Id -and -not $_.HasExited
            })
            if ($candidates.Count -eq 1) { $replacement = $candidates[0]; break }
            Start-Sleep -Milliseconds 500
        }
        if ($replacement) {
            Write-Host "launched $Label pid=$($replacement.Id) (try $try) - recovered replacement EFZ process"
            return $replacement
        }

        $cool = [Math]::Min(6 + ($try - 1) * 2, 14)
        $reason = Get-InstanceExitReason $efzProc.Id $attemptStart
        $detail = if ($reason) { "reason: $reason" } else { "no current exit.log reason" }
        Write-Host "launch $Label try ${try}: EFZ exited during startup/session grace ($detail) - retrying after ${cool}s"
        & $KeepAlive   # kill any stray so the next try starts contention-free
        Start-Sleep -Seconds $cool
    }
    Write-Host "ERROR: $Label failed to launch after $Retries tries"
    return $null
}

Get-Process EFZ,trial_EFZ,launch_smoke -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# Host retry may kill all; client retry must spare the host.
$killAll  = { Get-Process EFZ,trial_EFZ,launch_smoke -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }
$hostProc = $null
$clientProc = $null
for ($pairTry = 1; $pairTry -le 6; $pairTry++) {
    if ($pairTry -gt 1) {
        Write-Host "restarting full host/client pair (pair try $pairTry)"
        & $killAll
    }

    $hostProc = Start-Instance $hostIni "host" $killAll 6
    if (-not $hostProc) { continue }

    $killButHost = { Get-Process launch_smoke -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue; Get-Process EFZ,trial_EFZ -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $hostProc.Id } | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }
    Start-Sleep -Seconds 3
    $clientProc = Start-Instance $clientIni "client" $killButHost 2

    try { $hostProc.Refresh() } catch {}
    try { if ($clientProc) { $clientProc.Refresh() } } catch {}
    if ($clientProc -and -not $hostProc.HasExited -and -not $clientProc.HasExited) {
        break
    }

    Write-Host "pair try $pairTry did not leave two live peers; discarding the orphaned launch"
    $hostProc = $null
    $clientProc = $null
}
if ((-not $hostProc) -or (-not $clientProc)) {
    Write-Host "one or both instances never passed the startup health check - aborting"
    Get-Process EFZ,trial_EFZ -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    exit 2
}
$launched = @($hostProc, $clientProc)
Write-Host "both instances up: host=$($hostProc.Id) client=$($clientProc.Id)"

if ($Spectator -and $SpectatorSource -ne "Host") {
    $clientPortDeadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $clientPortDeadline -and $clientSpectatorPort -eq 0) {
        $clientLogFiles = @(Get-ChildItem (Join-Path $GameDir 'logs') -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match "^PID$($clientProc.Id)_" } |
            ForEach-Object { Join-Path $_.FullName 'session.log' } |
            Where-Object { Test-Path -LiteralPath $_ })
        $portHits = if ($clientLogFiles.Count -gt 0) {
            @(Select-String -LiteralPath $clientLogFiles -Pattern 'client bound ephemeral local port=' `
                -ErrorAction SilentlyContinue)
        } else { @() }
        foreach ($hit in $portHits) {
            if ($hit.Line -match 'client bound ephemeral local port=(\d+)') {
                $clientSpectatorPort = [int]$Matches[1]
            }
        }
        if ($clientSpectatorPort -eq 0) { Start-Sleep -Milliseconds 200 }
    }
    if ($clientSpectatorPort -eq 0) {
        throw "Unable to discover the client gameplay port for $SpectatorSource spectator-source proof"
    }
    for ($sourceIndex = 0; $sourceIndex -lt $SpectatorCount; ++$sourceIndex) {
        Write-SpectatorIni $spectatorInis[$sourceIndex] $sourceIndex
    }
    Write-Host "resolved client spectator source port=$clientSpectatorPort mode=$SpectatorSource"
}

$spectatorProcs = @()
$spectatorProxyProcs = @()
if ($Spectator) {
    if ($spectatorImpairmentEnabled) {
        $proxyScript = Join-Path $PSScriptRoot "spectator_udp_proxy.ps1"
        $powerShellExe = [Environment]::ProcessPath
        if ([string]::IsNullOrWhiteSpace($powerShellExe) -or
            [IO.Path]::GetFileName($powerShellExe) -notmatch '^(pwsh|powershell)(\.exe)?$') {
            $powerShellExe = Join-Path $PSHOME "pwsh.exe"
        }
        if (-not (Test-Path $powerShellExe)) {
            $powerShellExe = Join-Path $PSHOME "powershell.exe"
        }
        if (-not (Test-Path $powerShellExe)) {
            throw "Unable to locate PowerShell for spectator impairment proxies"
        }
        for ($proxyIndex = 0; $proxyIndex -lt $SpectatorCount; ++$proxyIndex) {
            $proxyPort = $BasePort + 2 + $SpectatorCount + $proxyIndex
            $sourcePort = Get-SpectatorSourcePort $proxyIndex
            $proxyLog = Join-Path $GameDir "harness_spectator_proxy_$($proxyIndex + 1).log"
            if (Test-Path -LiteralPath $proxyLog) {
                Remove-Item -LiteralPath $proxyLog -Force
            }
            $arguments = @(
                "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                "-File", $proxyScript,
                "-ListenPort", $proxyPort,
                "-HostPort", $sourcePort,
                "-DelayMs", $SpectatorDelayMs,
                "-JitterMs", $SpectatorJitterMs,
                "-LossPct", $SpectatorLossPct,
                "-DuplicatePct", $SpectatorDuplicatePct,
                "-ReorderPct", $SpectatorReorderPct,
                "-Seed", ($Seed + 7919 * ($proxyIndex + 1)),
                "-ParentProcessId", $PID,
                "-LogPath", $proxyLog
            )
            $proxyProc = Start-Process -FilePath $powerShellExe -ArgumentList $arguments `
                -PassThru -WindowStyle Hidden
            $spectatorProxyProcs += $proxyProc
            Start-Sleep -Milliseconds 300
            $proxyProc.Refresh()
            if ($proxyProc.HasExited) {
                throw "spectator impairment proxy $($proxyIndex + 1) failed to start; see $proxyLog"
            }
            Write-Host "spectator-$($proxyIndex + 1) impairment proxy pid=$($proxyProc.Id) port=$proxyPort"
        }
    }
    if ($SpectatorLaunchDelaySec -gt 0) {
        Write-Host "waiting ${SpectatorLaunchDelaySec}s before the first spectator late-join"
        Start-Sleep -Seconds $SpectatorLaunchDelaySec
    }
    for ($spectatorIndex = 0; $spectatorIndex -lt $SpectatorCount; ++$spectatorIndex) {
        if ($spectatorIndex -gt 0 -and $SpectatorStaggerSec -gt 0) {
            Write-Host "waiting ${SpectatorStaggerSec}s before spectator $($spectatorIndex + 1) late-join"
            Start-Sleep -Seconds $SpectatorStaggerSec
        }
        # A retry of viewer N must never kill already accepted viewers.
        $keepAcceptedInstances = {
            $acceptedIds = @($launched | ForEach-Object { $_.Id })
            Get-Process launch_smoke -ErrorAction SilentlyContinue |
                Stop-Process -Force -ErrorAction SilentlyContinue
            Get-Process EFZ,trial_EFZ -ErrorAction SilentlyContinue | Where-Object {
                $acceptedIds -notcontains $_.Id
            } | Stop-Process -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 2
        }
        $label = "spectator-$($spectatorIndex + 1)"
        $spectatorProc = Start-Instance $spectatorInis[$spectatorIndex] $label $keepAcceptedInstances 3
        if (-not $spectatorProc) {
            Write-Host "$label never passed the startup health check - aborting"
            $launched | Where-Object { -not $_.HasExited } |
                Stop-Process -Force -ErrorAction SilentlyContinue
            $spectatorProxyProcs | Where-Object { -not $_.HasExited } |
                Stop-Process -Force -ErrorAction SilentlyContinue
            Release-RunLock
            exit 2
        }
        $spectatorProcs += $spectatorProc
        $launched += $spectatorProc
        Write-Host "$label instance up: pid=$($spectatorProc.Id)"
    }
}

# Filter lines by $startTime too: log directories are reused when PIDs recur.
function Get-SessionLogMatches([string]$Pattern) {
    $pidPrefixes = @("PID$($hostProc.Id)_*", "PID$($clientProc.Id)_*")
    foreach ($spectatorProc in $spectatorProcs) {
        $pidPrefixes += "PID$($spectatorProc.Id)_*"
    }
    $files = @()
    foreach ($prefix in $pidPrefixes) {
        $files += @(Get-ChildItem -Path (Join-Path $GameDir "logs\$prefix") -Directory -ErrorAction SilentlyContinue |
            Get-ChildItem -File -Filter "*.log" -ErrorAction SilentlyContinue)
    }

    $matches = @($files | Select-String -Pattern $Pattern -ErrorAction SilentlyContinue)
    return @($matches | Where-Object {
        if ($_.Line -notmatch '^\[(\d{2}:\d{2}:\d{2}\.\d{3})\]') { return $false }
        $lineTime = $startTime.Date.Add(
            [TimeSpan]::ParseExact($Matches[1], 'hh\:mm\:ss\.fff', [Globalization.CultureInfo]::InvariantCulture))
        if ($lineTime -lt $startTime.AddHours(-12)) { $lineTime = $lineTime.AddDays(1) }
        return $lineTime -ge $startTime
    })
}

# Read session.log only; mirrored category logs are not independent evidence.
function Get-ProcessSessionLogLines([int]$ProcessId, [string]$Pattern = ".*") {
    $files = @(Get-ChildItem -Path (Join-Path $GameDir "logs\PID${ProcessId}_*") `
            -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "session.log" } |
        Where-Object { Test-Path $_ })
    if ($files.Count -eq 0) { return @() }
    # Use -LiteralPath: piped path strings search the path text, not the file.
    $matches = @(Select-String -LiteralPath $files -Pattern $Pattern -ErrorAction SilentlyContinue)
    return @($matches | Where-Object {
        if ($_.Line -notmatch '^\[(\d{2}:\d{2}:\d{2}\.\d{3})\]') { return $false }
        $lineTime = $startTime.Date.Add(
            [TimeSpan]::ParseExact($Matches[1], 'hh\:mm\:ss\.fff', [Globalization.CultureInfo]::InvariantCulture))
        if ($lineTime -lt $startTime.AddHours(-12)) { $lineTime = $lineTime.AddDays(1) }
        return $lineTime -ge $startTime
    })
}

function Get-PerProcessVerdicts() {
    $distinct = @()
    foreach ($processId in @($hostProc.Id, $clientProc.Id)) {
        $pidHits = @(Get-ProcessSessionLogLines $processId `
            'HARNESS VERDICT (PASS|FAIL) reason=')
        if ($pidHits.Count -gt 0) {
            $distinct += $pidHits[-1]
        }
    }
    return @($distinct)
}

function Get-SpectatorReportState([System.Diagnostics.Process]$Process) {
    $lines = @(Get-ProcessSessionLogLines $Process.Id)
    $verified = 0
    $maxVerifiedSource = 0
    $gameIds = @{}
    $localPorts = @{}
    $fatalLines = @()
    $systemFrames = @()
    foreach ($hit in $lines) {
        $line = $hit.Line
        if ($line -match 'confirmed state verified game=(\d+) record=(\d+) source=(\d+) count=(\d+)') {
            $verified = [Math]::Max($verified, [int]$Matches[4])
            $maxVerifiedSource = [Math]::Max($maxVerifiedSource, [int]$Matches[3])
        }
        if ($line -match 'received game start game=(\d+)') {
            $gameIds[$Matches[1]] = $true
        }
        if ($line -match 'client transport open local=(\d+)') {
            $localPorts[$Matches[1]] = $true
        }
        if ($line -match 'STATE DESYNC|INPUT DIGEST MISMATCH|screen mismatch|record gap|lost record alignment|consume failed') {
            $fatalLines += $line
        }
        if ($line -match 'SPECTATOR SYSTEM INPUT playback game=(\d+) record=(\d+) source=(\d+) p1=([0-9A-Fa-f]{4}) p2=([0-9A-Fa-f]{4})') {
            $systemFrames += [pscustomobject]@{
                Game = [int]$Matches[1]
                Record = [int]$Matches[2]
                Source = [int]$Matches[3]
                P1 = [Convert]::ToUInt16($Matches[4], 16)
                P2 = [Convert]::ToUInt16($Matches[5], 16)
            }
        }
    }
    return [pscustomobject]@{
        Process = $Process
        Verified = $verified
        MaxVerifiedSource = $maxVerifiedSource
        GameIds = @($gameIds.Keys | ForEach-Object { [int]$_ } | Sort-Object)
        LocalPorts = @($localPorts.Keys | ForEach-Object { [int]$_ } | Sort-Object)
        FatalLines = @($fatalLines)
        SystemFrames = @($systemFrames)
    }
}

function Get-Percentile([double[]]$Values, [double]$Percentile) {
    if ($null -eq $Values -or $Values.Count -eq 0) { return [double]::NaN }
    $ordered = @($Values | Sort-Object)
    $rank = [Math]::Ceiling(($Percentile / 100.0) * $ordered.Count) - 1
    $rank = [Math]::Max(0, [Math]::Min($ordered.Count - 1, $rank))
    return [double]$ordered[$rank]
}

function Get-PlayerPerformanceReport([System.Diagnostics.Process]$Process) {
    $samples = @()
    foreach ($hit in @(Get-ProcessSessionLogLines $Process.Id 'rollback\.perf PERF')) {
        if ($hit.Line -match 'PERF dt=\d+ms outer=([0-9.]+)Hz sim=([0-9.]+)Hz visible=([0-9.]+)Hz.*main\[avg=(\d+)us max=(\d+)us\].*raw\[(\d+)/') {
            $sample = [pscustomobject]@{
                OuterHz = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
                SimHz = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture)
                VisibleHz = [double]::Parse($Matches[3], [Globalization.CultureInfo]::InvariantCulture)
                MainAvgUs = [int]$Matches[4]
                MainMaxUs = [int]$Matches[5]
                Screen = [int]$Matches[6]
            }
            if ($sample.Screen -eq 1 -and $sample.SimHz -gt 0.0) {
                $samples += $sample
            }
        }
    }
    $visible = [double[]]@($samples | ForEach-Object { $_.VisibleHz })
    $mainAvg = [double[]]@($samples | ForEach-Object { $_.MainAvgUs })
    return [pscustomobject]@{
        Samples = @($samples)
        VisibleP10 = Get-Percentile $visible 10.0
        MainP95Us = Get-Percentile $mainAvg 95.0
    }
}

function Get-PacingReport([System.Diagnostics.Process]$Process) {
    $configured = 0
    $failed = 0
    $cadence = @()
    $resetSamples = 0
    foreach ($hit in @(Get-ProcessSessionLogLines $Process.Id 'pacing (configured|configuration failed|cadence)')) {
        if ($hit.Line -match "pacing configured target=$TargetFps owner=mod wrapperThrottle=disabled") {
            ++$configured
        }
        if ($hit.Line -match 'pacing configuration failed') {
            ++$failed
        }
        if ($hit.Line -match `
            'pacing cadence target=(\d+) measured=([0-9.]+).*deadlineResets=(\d+)') {
            if ([int]$Matches[1] -eq $TargetFps) {
                if ([int]$Matches[3] -eq 0) {
                    $cadence += [double]::Parse(
                        $Matches[2],
                        [Globalization.CultureInfo]::InvariantCulture)
                } else {
                    ++$resetSamples
                }
            }
        }
    }
    $cadenceValues = [double[]]$cadence
    return [pscustomobject]@{
        Configured = $configured
        Failed = $failed
        Samples = @($cadence)
        ResetSamples = $resetSamples
        P10 = Get-Percentile $cadenceValues 10.0
        P90 = Get-Percentile $cadenceValues 90.0
    }
}

function Get-PairedDigestCheckpoints([int]$ProcessId, [string]$Role) {
    $result = @{}
    foreach ($hit in @(Get-ProcessSessionLogLines $ProcessId 'SPECTATOR INPUT DIGEST')) {
        if ($hit.Line -match "SPECTATOR INPUT DIGEST $Role game=(\d+) (?:records|count)=(\d+) digest=([0-9A-Fa-f]{16})") {
            $key = "$($Matches[1]):$($Matches[2])"
            $result[$key] = $Matches[3].ToUpperInvariant()
        }
    }
    return $result
}

function Get-SpectatorDisconnectTerminalReport([System.Diagnostics.Process]$Process) {
    $lines = @(Get-ProcessSessionLogLines $Process.Id)
    $sourceEnd = @($lines | Where-Object { $_.Line -match 'SPECTATOR SOURCE END' })
    $playbackComplete = @($lines | Where-Object { $_.Line -match 'SPECTATOR PLAYBACK COMPLETE' })
    $playbackIncomplete = @($lines | Where-Object { $_.Line -match 'SPECTATOR PLAYBACK INCOMPLETE' })
    $closeEvidence = @($lines | Where-Object {
        $_.Line -match 'closing spectator process: spectator (source session ended|archive ended before the full confirmed tail arrived|archive was unavailable)'
    })

    $reason = 0
    $reasonLabel = 'missing'
    $cursorMatch = $false
    $acceptedTerminal = $false
    if ($sourceEnd.Count -gt 0 -and
        $sourceEnd[-1].Line -match 'reason=(2|4) game=(\d+) records=(\d+) digest=([0-9A-Fa-f]{16}) effective=(\d+):(\d+)/([0-9A-Fa-f]{16}) total=(\d+)') {
        $reason = [int]$Matches[1]
        $effectiveGame = [int]$Matches[5]
        $effectiveRecord = [int]$Matches[6]
        $effectiveDigest = $Matches[7].ToUpperInvariant()
        $total = [long]$Matches[8]
        $expectedTuple = "$effectiveGame`:$effectiveRecord`:$effectiveDigest`:$total"

        if ($reason -eq 2) {
            $reasonLabel = 'PlayerDisconnected'
            $acceptedTerminal = $true
            if ($playbackComplete.Count -gt 0 -and
                $playbackComplete[-1].Line -match 'game=(\d+) records=(\d+) digest=([0-9A-Fa-f]{16}) total=(\d+)') {
                $actualTuple = "$($Matches[1])`:$($Matches[2])`:$($Matches[3].ToUpperInvariant())`:$($Matches[4])"
                $cursorMatch = $actualTuple -eq $expectedTuple
            }
        } else {
            $reasonLabel = 'ArchiveUnavailable'
            $acceptedTerminal = $true
            if ($playbackIncomplete.Count -gt 0) {
                $lastIncomplete = $playbackIncomplete[-1].Line
                if ($lastIncomplete -match 'game=(\d+) records=(\d+) digest=([0-9A-Fa-f]{16}) total=(\d+) reason=archive-unavailable') {
                    $actualTuple = "$($Matches[1])`:$($Matches[2])`:$($Matches[3].ToUpperInvariant())`:$($Matches[4])"
                    $cursorMatch = $actualTuple -eq $expectedTuple
                } elseif ($effectiveGame -eq 0 -and $effectiveRecord -eq 0 -and
                    $lastIncomplete -match 'archive unavailable before initialization') {
                    $cursorMatch = $true
                }
            }
        }
    }

    try { $Process.Refresh() } catch {}
    $passed = $Process.HasExited -and $acceptedTerminal -and $cursorMatch -and
        $closeEvidence.Count -gt 0
    return [pscustomobject]@{
        Process = $Process
        Passed = $passed
        Reason = $reasonLabel
        SourceEnd = $sourceEnd.Count
        PlaybackComplete = $playbackComplete.Count
        PlaybackIncomplete = $playbackIncomplete.Count
        CursorMatch = $cursorMatch
        CloseEvidence = $closeEvidence.Count
    }
}

if ($DisconnectAfterSec -gt 0) {
    $subscriberTags = @{}
    $initializedViewerPids = @{}
    if ($Spectator) {
        Write-Host "disconnect scenario: waiting for $SpectatorCount authenticated spectator subscriptions before cutting the player link"
        $subscriberDeadline = (Get-Date).AddSeconds([Math]::Max(30, $SpectatorTerminalTimeoutSec))
        while ((Get-Date) -lt $subscriberDeadline) {
            foreach ($sourceProc in @($hostProc, $clientProc)) {
                foreach ($hit in @(Get-ProcessSessionLogLines $sourceProc.Id 'subscriber attach')) {
                    if ($hit.Line -match 'tag=([0-9A-Fa-f]{8})') {
                        $subscriberTags["$($sourceProc.Id):$($Matches[1].ToUpperInvariant())"] = $true
                    }
                }
            }
            foreach ($spectatorProc in $spectatorProcs) {
                if (@(Get-ProcessSessionLogLines $spectatorProc.Id 'received game start game=').Count -gt 0) {
                    $initializedViewerPids[$spectatorProc.Id] = $true
                }
            }
            if ($subscriberTags.Count -ge $SpectatorCount -and
                $initializedViewerPids.Count -eq $SpectatorCount) { break }
            $spectatorProcs | ForEach-Object { try { $_.Refresh() } catch {} }
            if (@($spectatorProcs | Where-Object { $_.HasExited }).Count -gt 0) { break }
            Start-Sleep -Seconds 1
        }
        Write-Host "authenticated spectator subscriptions: sources=$($subscriberTags.Count)/$SpectatorCount viewers=$($initializedViewerPids.Count)/$SpectatorCount"
    }

    $earlyProbeReady = -not $EarlySystemKeyProbe
    if ($EarlySystemKeyProbe) {
        Write-Host "early system-key probe: waiting for confirmed F4/F5 on both players and every viewer"
        $probeDeadline = (Get-Date).AddSeconds(90)
        while ((Get-Date) -lt $probeDeadline) {
            $hostProbeText = (@(Get-ProcessSessionLogLines $hostProc.Id).Line -join "`n")
            $clientProbeText = (@(Get-ProcessSessionLogLines $clientProc.Id).Line -join "`n")
            $playersReady = $hostProbeText -match 'apply confirmed practice state=0x08' -and
                $hostProbeText -match 'apply confirmed practice state=0x10' -and
                $clientProbeText -match 'apply confirmed practice state=0x08' -and
                $clientProbeText -match 'apply confirmed practice state=0x10'
            $viewersReady = $true
            foreach ($spectatorProc in $spectatorProcs) {
                $viewerProbeText = (@(Get-ProcessSessionLogLines $spectatorProc.Id).Line -join "`n")
                if ($viewerProbeText -notmatch 'apply confirmed practice state=0x08' -or
                    $viewerProbeText -notmatch 'apply confirmed practice state=0x10' -or
                    $viewerProbeText -notmatch 'confirmed state verified') {
                    $viewersReady = $false
                    break
                }
            }
            if ($playersReady -and $viewersReady) {
                $earlyProbeReady = $true
                break
            }
            Start-Sleep -Seconds 1
        }
        Write-Host "early system-key probe ready: $earlyProbeReady"
    }

    Write-Host "disconnect scenario: force-closing client in ${DisconnectAfterSec}s, then waiting for host fail-closed exit"
    Start-Sleep -Seconds $DisconnectAfterSec
    try { $clientProc.Refresh() } catch {}
    if (-not $clientProc.HasExited) {
        Stop-Process -Id $clientProc.Id -Force -ErrorAction SilentlyContinue
    }

    # Covers the 120s maximum handshake timeout.
    $disconnectDeadline = (Get-Date).AddSeconds(135)
    while ((Get-Date) -lt $disconnectDeadline) {
        Start-Sleep -Seconds 2
        try { $hostProc.Refresh() } catch {}
        if ($hostProc.HasExited) { break }
    }

    try { $hostProc.Refresh() } catch {}
    $closed = $hostProc.HasExited
    $timeoutHits = @(Get-SessionLogMatches "peer timeout|netplay handshake")
    $hostExitReason = Get-InstanceExitReason $hostProc.Id $startTime
    $hostDisconnectPass = $closed -and $timeoutHits.Count -gt 0 -and
        -not [string]::IsNullOrWhiteSpace($hostExitReason)

    $spectatorTerminalReports = @()
    if ($Spectator) {
        $terminalDeadline = (Get-Date).AddSeconds($SpectatorTerminalTimeoutSec)
        while ((Get-Date) -lt $terminalDeadline) {
            $spectatorProcs | ForEach-Object { try { $_.Refresh() } catch {} }
            if (@($spectatorProcs | Where-Object { -not $_.HasExited }).Count -eq 0) { break }
            Start-Sleep -Seconds 1
        }
        $spectatorTerminalReports = @($spectatorProcs | ForEach-Object {
            Get-SpectatorDisconnectTerminalReport $_
        })
    }
    $spectatorDisconnectPass = -not $Spectator -or
        ($subscriberTags.Count -ge $SpectatorCount -and
         $initializedViewerPids.Count -eq $SpectatorCount -and
         @($spectatorTerminalReports | Where-Object { -not $_.Passed }).Count -eq 0 -and
         $spectatorTerminalReports.Count -eq $SpectatorCount)
    $earlyProbePass = -not $EarlySystemKeyProbe
    $earlyProbeDetail = "not requested"
    if ($EarlySystemKeyProbe) {
        $hostProbeText = (@(Get-ProcessSessionLogLines $hostProc.Id).Line -join "`n")
        $clientProbeText = (@(Get-ProcessSessionLogLines $clientProc.Id).Line -join "`n")
        $f4Origin = [regex]::Match(
            $hostProbeText,
            'HARNESS early system-key probe originated F4 frame=(\d+)')
        $f5Origin = [regex]::Match(
            $hostProbeText,
            'HARNESS early system-key probe originated F5 frame=(\d+)')
        $exactPlayers = $false
        $exactViewers = $false
        $viewerHashes = $false
        $viewerFatal = $true
        if ($f4Origin.Success -and $f5Origin.Success) {
            $f4Frame = [uint32]$f4Origin.Groups[1].Value
            $f5Frame = [uint32]$f5Origin.Groups[1].Value
            $f4Pattern = "apply confirmed practice state=0x08 source=$f4Frame\b"
            $f5Pattern = "apply confirmed practice state=0x10 source=$f5Frame\b"
            $exactPlayers = $hostProbeText -match $f4Pattern -and
                $hostProbeText -match $f5Pattern -and
                $clientProbeText -match $f4Pattern -and
                $clientProbeText -match $f5Pattern
            $exactViewers = $Spectator
            $viewerHashes = $Spectator
            $viewerFatal = $false
            foreach ($spectatorProc in $spectatorProcs) {
                $viewerProbeText = (@(Get-ProcessSessionLogLines $spectatorProc.Id).Line -join "`n")
                if ($viewerProbeText -notmatch $f4Pattern -or $viewerProbeText -notmatch $f5Pattern) {
                    $exactViewers = $false
                }
                if ($viewerProbeText -notmatch 'confirmed state verified') {
                    $viewerHashes = $false
                }
                if ($viewerProbeText -match 'STATE DESYNC|confirmed spectator state hash mismatch') {
                    $viewerFatal = $true
                }
            }
            $earlyProbeDetail = "F4=$f4Frame F5=$f5Frame playersExact=$exactPlayers viewersExact=$exactViewers viewerHashes=$viewerHashes viewerFatal=$viewerFatal"
        } else {
            $earlyProbeDetail = "origin logs missing F4=$($f4Origin.Success) F5=$($f5Origin.Success)"
        }
        $earlyProbePass = $earlyProbeReady -and $exactPlayers -and $exactViewers -and
            $viewerHashes -and -not $viewerFatal
    }

    $hostRateText = (@(Get-ProcessSessionLogLines $hostProc.Id `
        'rollback negotiated|launch-go|target_fps=').Line -join "`n")
    $clientRateText = (@(Get-ProcessSessionLogLines $clientProc.Id `
        'rollback negotiated|launch-go|target_fps=').Line -join "`n")
    $playerRateContractPass = $hostRateText -match `
            "rollback negotiated host ack rollback=\d+ target_fps=$TargetFps" -and
        $hostRateText -match "launch-go sent delay=\d+ target_fps=$TargetFps" -and
        $clientRateText -match `
            "rollback negotiated client ack sent rollback=\d+ target_fps=$TargetFps" -and
        $clientRateText -match `
            "launch-go received delay=\d+ leadMs=\d+ target_fps=$TargetFps"
    $disconnectPacingDetails = @()
    $disconnectPacingPass = $true
    foreach ($paced in @(
        [pscustomobject]@{ Role = 'host'; Process = $hostProc },
        [pscustomobject]@{ Role = 'client'; Process = $clientProc }
    )) {
        $pace = Get-PacingReport $paced.Process
        $pacePass = $pace.Configured -gt 0 -and $pace.Failed -eq 0 -and
            $pace.Samples.Count -gt 0 -and -not [double]::IsNaN($pace.P10) -and
            -not [double]::IsNaN($pace.P90) -and
            $pace.P10 -ge ($TargetFps - 2.0) -and $pace.P90 -le ($TargetFps + 2.0)
        $disconnectPacingPass = $disconnectPacingPass -and $pacePass
        $disconnectPacingDetails += `
            "$($paced.Role)=$pacePass/$($pace.Samples.Count)/resets=$($pace.ResetSamples)/$($pace.P10.ToString('N2'))-$($pace.P90.ToString('N2'))"
    }
    $spectatorRatePass = -not $Spectator
    if ($Spectator) {
        $spectatorRatePass = $true
        foreach ($spectatorProcess in $spectatorProcs) {
            $viewerRates = @()
            foreach ($hit in @(Get-ProcessSessionLogLines $spectatorProcess.Id `
                'received game start game=.* fps=')) {
                if ($hit.Line -match 'received game start game=\d+ .* fps=(\d+)') {
                    $viewerRates += [int]$Matches[1]
                }
            }
            $pace = Get-PacingReport $spectatorProcess
            $viewerPass = $viewerRates.Count -gt 0 -and
                @($viewerRates | Where-Object { $_ -ne $TargetFps }).Count -eq 0 -and
                $pace.Configured -gt 0 -and $pace.Failed -eq 0 -and
                $pace.Samples.Count -gt 0 -and -not [double]::IsNaN($pace.P10) -and
                -not [double]::IsNaN($pace.P90) -and
                $pace.P10 -ge ($TargetFps - 2.0) -and $pace.P90 -le ($TargetFps + 2.0)
            $spectatorRatePass = $spectatorRatePass -and $viewerPass
            $disconnectPacingDetails += `
                "spectator-$($spectatorProcess.Id)=$viewerPass/$($pace.Samples.Count)/resets=$($pace.ResetSamples)/rates=$($viewerRates -join ',')"
        }
    }
    $rateContractPass = $playerRateContractPass -and $disconnectPacingPass -and
        $spectatorRatePass
    $disconnectPass = $hostDisconnectPass -and $spectatorDisconnectPass -and
        $earlyProbePass -and $rateContractPass

    Write-Host ""
    Write-Host "================ DISCONNECT RESULT ================"
    Write-Host "host closed itself: $closed"
    Write-Host "timeout/handshake evidence lines: $($timeoutHits.Count)"
    Write-Host "host exit reason: $hostExitReason"
    Write-Host "early system-key probe: pass=$earlyProbePass $earlyProbeDetail"
    Write-Host "frame-rate contract: pass=$rateContractPass target=$TargetFps playerPackets=$playerRateContractPass pacing=$($disconnectPacingDetails -join '; ')"
    $timeoutHits | Select-Object -First 6 | ForEach-Object { Write-Host $_.Line.Trim() }
    foreach ($terminalReport in $spectatorTerminalReports) {
        $label = if ($terminalReport.Passed) { 'PASS' } else { 'FAIL' }
        Write-Host ("[{0}] spectator pid={1} terminal={2} sourceEnd={3} complete={4} incomplete={5} cursorMatch={6} closeEvidence={7} exited={8}" -f `
            $label, $terminalReport.Process.Id, $terminalReport.Reason,
            $terminalReport.SourceEnd, $terminalReport.PlaybackComplete,
            $terminalReport.PlaybackIncomplete, $terminalReport.CursorMatch,
            $terminalReport.CloseEvidence, $terminalReport.Process.HasExited)
    }
    Write-Host "==================================================="
    $launched | ForEach-Object {
        try { $_.Refresh() } catch {}
        if (-not $_.HasExited) { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
    }
    $spectatorProxyProcs | ForEach-Object {
        try { $_.Refresh() } catch {}
        if (-not $_.HasExited) { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
    }
    Release-RunLock
    exit $(if ($disconnectPass) { 0 } else { 1 })
}

if ($Spectator) {
    Write-Host "waiting for $RequestedMatches completed matches, rematch liveness, and $SpectatorVerifiedFrames verified frames on each of $SpectatorCount spectators (timeout ${TimeoutSec}s)..."
} else {
    Write-Host "waiting for full-lifecycle HARNESS VERDICT (timeout ${TimeoutSec}s)... char select/rematch are scripted."
}
$logRoot = Join-Path $GameDir "logs"
$verdicts = @()
$spectatorStates = @()
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$waitFailure = $null
$requiredGameCount = $RequestedMatches + 1
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $verdicts = @(Get-PerProcessVerdicts)
    if ($Spectator) {
        $spectatorStates = @($spectatorProcs | ForEach-Object { Get-SpectatorReportState $_ })
        $readySpectators = @($spectatorStates | Where-Object {
            $_.Verified -ge $SpectatorVerifiedFrames -and
            $_.GameIds.Count -ge $requiredGameCount -and $_.FatalLines.Count -eq 0
        })
        if ($verdicts.Count -ge 2 -and $readySpectators.Count -eq $SpectatorCount) { break }
    } elseif ($verdicts.Count -ge 2) {
        break
    }

    $launched | ForEach-Object { try { $_.Refresh() } catch {} }
    $exitedPlayers = @($hostProc, $clientProc | Where-Object { $_.HasExited })
    $exitedSpectators = @($spectatorProcs | Where-Object { $_.HasExited })
    if ($exitedPlayers.Count -gt 0 -and $verdicts.Count -lt 2) {
        $waitFailure = "a player source exited before two lifecycle verdicts"
        break
    }
    if ($exitedSpectators.Count -gt 0) {
        $waitFailure = "a spectator exited before completing replay verification"
        break
    }
}

$verdicts = @(Get-PerProcessVerdicts)
if ($Spectator) {
    $spectatorStates = @($spectatorProcs | ForEach-Object { Get-SpectatorReportState $_ })
}

function New-ReportResult([string]$Name, [bool]$Passed, [string]$Detail) {
    return [pscustomobject]@{ Name = $Name; Passed = $Passed; Detail = $Detail }
}

$report = @()
$failedVerdicts = @($verdicts | Where-Object { $_.Line -match 'VERDICT FAIL' })
$passVerdicts = @($verdicts | Where-Object { $_.Line -match 'VERDICT PASS' })
$report += New-ReportResult "two player lifecycle verdicts" `
    ($verdicts.Count -eq 2 -and $passVerdicts.Count -eq 2 -and $failedVerdicts.Count -eq 0) `
    "observed=$($verdicts.Count) pass=$($passVerdicts.Count) fail=$($failedVerdicts.Count)"
if ($waitFailure) {
    $report += New-ReportResult "no early process exit" $false $waitFailure
}

$inputInjectionMismatches = @(Get-SessionLogMatches 'inject verify (DIR|BUTTON) MISMATCH')
$report += New-ReportResult "input injection verifier" ($inputInjectionMismatches.Count -eq 0) `
    "mismatches=$($inputInjectionMismatches.Count)"

$hostFpsContractText = (@(Get-ProcessSessionLogLines $hostProc.Id `
    'rollback negotiated|launch-go|target_fps=').Line -join "`n")
$clientFpsContractText = (@(Get-ProcessSessionLogLines $clientProc.Id `
    'rollback negotiated|launch-go|target_fps=').Line -join "`n")
$hostReadyRate = $hostFpsContractText -match `
    "rollback negotiated host ack rollback=\d+ target_fps=$TargetFps"
$hostLaunchRate = $hostFpsContractText -match `
    "launch-go sent delay=\d+ target_fps=$TargetFps"
$clientReadyRate = $clientFpsContractText -match `
    "rollback negotiated client ack sent rollback=\d+ target_fps=$TargetFps"
$clientLaunchRate = $clientFpsContractText -match `
    "launch-go received delay=\d+ leadMs=\d+ target_fps=$TargetFps"
$observedPlayerRates = @()
foreach ($contractText in @($hostFpsContractText, $clientFpsContractText)) {
    foreach ($match in [regex]::Matches($contractText, 'target_fps=(\d+)')) {
        $observedPlayerRates += [int]$match.Groups[1].Value
    }
}
$playerRatesExact = $observedPlayerRates.Count -gt 0 -and
    @($observedPlayerRates | Where-Object { $_ -ne $TargetFps }).Count -eq 0
$report += New-ReportResult "immutable player frame-rate contract" `
    ($hostReadyRate -and $hostLaunchRate -and $clientReadyRate -and $clientLaunchRate -and
     $playerRatesExact) `
    "target=$TargetFps ready[host=$hostReadyRate client=$clientReadyRate] launch[host=$hostLaunchRate client=$clientLaunchRate] observed=$($observedPlayerRates -join ',')"

$pacingProcesses = @(
    [pscustomobject]@{ Role = "host"; Process = $hostProc },
    [pscustomobject]@{ Role = "client"; Process = $clientProc }
)
for ($pacingIndex = 0; $pacingIndex -lt $spectatorProcs.Count; ++$pacingIndex) {
    $pacingProcesses += [pscustomobject]@{
        Role = "spectator-$($pacingIndex + 1)"
        Process = $spectatorProcs[$pacingIndex]
    }
}
foreach ($paced in $pacingProcesses) {
    $pacingReport = Get-PacingReport $paced.Process
    $cadencePass = $pacingReport.Samples.Count -gt 0 -and
        -not [double]::IsNaN($pacingReport.P10) -and
        -not [double]::IsNaN($pacingReport.P90) -and
        $pacingReport.P10 -ge ($TargetFps - 2.0) -and
        $pacingReport.P90 -le ($TargetFps + 2.0)
    $report += New-ReportResult "$($paced.Role) negotiated frame pacing" `
        ($pacingReport.Configured -gt 0 -and $pacingReport.Failed -eq 0 -and $cadencePass) `
        ("target={0} configured={1} failed={2} cadenceSamples={3} resetSamples={4} p10={5:N2} p90={6:N2}" -f `
            $TargetFps, $pacingReport.Configured, $pacingReport.Failed,
            $pacingReport.Samples.Count, $pacingReport.ResetSamples,
            $pacingReport.P10, $pacingReport.P90)
}

if ($Spectator) {
    foreach ($spectatorProcess in $spectatorProcs) {
        $startLines = @(Get-ProcessSessionLogLines $spectatorProcess.Id `
            'received game start game=.* fps=')
        $startRates = @()
        foreach ($hit in $startLines) {
            if ($hit.Line -match 'received game start game=\d+ .* fps=(\d+)') {
                $startRates += [int]$Matches[1]
            }
        }
        $spectatorRatePass = $startRates.Count -gt 0 -and
            @($startRates | Where-Object { $_ -ne $TargetFps }).Count -eq 0
        $report += New-ReportResult "spectator pid=$($spectatorProcess.Id) authenticated frame-rate handoff" `
            $spectatorRatePass `
            "target=$TargetFps gameStartRates=$($startRates -join ',')"
    }
}

$longSetVerdicts = @($passVerdicts | Where-Object {
    $_.Line -match 'reason=REMATCH_LIVE' -and $_.Line -match 'winscreens=(\d+)' -and
    [int]$Matches[1] -ge $RequestedMatches -and $_.Line -match 'matchesStarted=(\d+)' -and
    [int]$Matches[1] -ge $requiredGameCount
})
if ($GameplayEscAfterFrames -eq 0) {
    $report += New-ReportResult "requested long-set lifecycle" ($longSetVerdicts.Count -eq 2) `
        "required completed=$RequestedMatches and started=$requiredGameCount; matching verdicts=$($longSetVerdicts.Count)"
}

if ($HostPerfMinSamples -gt 0) {
    # Enforce on both peers; a host-only check misses client catch-up slowdowns.
    foreach ($player in @(
        [pscustomobject]@{ Role = "host"; Process = $hostProc },
        [pscustomobject]@{ Role = "client"; Process = $clientProc }
    )) {
        $performance = Get-PlayerPerformanceReport $player.Process
        $effectiveMinVisibleHz = [Math]::Max(
            $MinHostVisibleHzP10,
            [double]$TargetFps - 3.0)
        $perfPass = $performance.Samples.Count -ge $HostPerfMinSamples -and
            -not [double]::IsNaN($performance.VisibleP10) -and
            -not [double]::IsNaN($performance.MainP95Us) -and
            $performance.VisibleP10 -ge $effectiveMinVisibleHz -and
            $performance.MainP95Us -le $MaxHostMainP95Us
        $report += New-ReportResult "$($player.Role) battle frame-time budget" $perfPass `
            ("samples={0} visibleP10={1:N1}Hz (min {2:N1}) mainAvgP95={3:N0}us (max {4})" -f `
                $performance.Samples.Count, $performance.VisibleP10, $effectiveMinVisibleHz, `
                $performance.MainP95Us, $MaxHostMainP95Us)
    }
}

if ($Spectator) {
    $allViewerPorts = @()
    foreach ($state in $spectatorStates) {
        $number = [array]::IndexOf($spectatorProcs, $state.Process) + 1
        $report += New-ReportResult "spectator-$number confirmed replay" `
            ($state.Verified -ge $SpectatorVerifiedFrames -and $state.FatalLines.Count -eq 0) `
            "verified=$($state.Verified)/$SpectatorVerifiedFrames maxSource=$($state.MaxVerifiedSource) fatal=$($state.FatalLines.Count)"
        $report += New-ReportResult "spectator-$number full retained set" `
            ($state.GameIds.Count -ge $requiredGameCount) `
            "games=$($state.GameIds -join ',') required=$requiredGameCount"
        $report += New-ReportResult "spectator-$number transport endpoint" `
            ($state.LocalPorts.Count -eq 1) "localPorts=$($state.LocalPorts -join ',') pid=$($state.Process.Id)"
        $allViewerPorts += $state.LocalPorts
    }
    $uniquePids = @($spectatorProcs.Id | Sort-Object -Unique)
    $uniquePorts = @($allViewerPorts | Sort-Object -Unique)
    $report += New-ReportResult "independent spectator processes" `
        ($uniquePids.Count -eq $SpectatorCount -and $uniquePorts.Count -eq $SpectatorCount) `
        "pids=$($uniquePids -join ',') udpPorts=$($uniquePorts -join ',')"

    $hostSubscriberEndpoints = @{}
    foreach ($hit in @(Get-ProcessSessionLogLines $hostProc.Id 'subscriber attach')) {
        if ($hit.Line -match 'subscriber attach addr=[0-9A-Fa-f]+:(\d+)') {
            $hostSubscriberEndpoints[$Matches[1]] = $true
        }
    }
    $clientSubscriberEndpoints = @{}
    foreach ($hit in @(Get-ProcessSessionLogLines $clientProc.Id 'subscriber attach')) {
        if ($hit.Line -match 'subscriber attach addr=[0-9A-Fa-f]+:(\d+)') {
            $clientSubscriberEndpoints[$Matches[1]] = $true
        }
    }
    $requestedHostStarts = 0
    $requestedClientStarts = 0
    for ($sourceIndex = 0; $sourceIndex -lt $SpectatorCount; ++$sourceIndex) {
        if ((Get-SpectatorSourceRole $sourceIndex) -eq "Client") {
            ++$requestedClientStarts
        } else {
            ++$requestedHostStarts
        }
    }
    $expectedHostSubscribers = [Math]::Min(1, $requestedHostStarts)
    $expectedRedirectedSubscribers = [Math]::Max(0, $requestedHostStarts - 1)
    $expectedClientSubscribers = $requestedClientStarts + $expectedRedirectedSubscribers
    $sourceFanoutPass = $hostSubscriberEndpoints.Count -eq $expectedHostSubscribers -and
        $clientSubscriberEndpoints.Count -eq $expectedClientSubscribers
    $report += New-ReportResult "independent player-source spectator fan-out" `
        $sourceFanoutPass `
        "mode=$SpectatorSource starts[host=$requestedHostStarts client=$requestedClientStarts] redirect=$expectedRedirectedSubscribers subscribers[host=$($hostSubscriberEndpoints.Count)/$expectedHostSubscribers client=$($clientSubscriberEndpoints.Count)/$expectedClientSubscribers]"

    if ($redirectProofRequired) {
        $hostRedirectLines = @(Get-ProcessSessionLogLines $hostProc.Id 'SPECTATOR REDIRECT')
        $clientRedirectLines = @(Get-ProcessSessionLogLines $clientProc.Id 'SPECTATOR REDIRECT|subscriber attach')
        $softLimitEvidence = @(Get-ProcessSessionLogLines $hostProc.Id `
            'SPECTATOR RELAY coordinator epoch=[0-9A-Fa-f]{8} directSoftLimit=1')
        $report += New-ReportResult "harness redirect soft limit" `
            ($softLimitEvidence.Count -gt 0) "coordinatorEvidence=$($softLimitEvidence.Count) expectedLimit=1"
        $createdKeys = @{}
        $readyKeys = @{}
        $claimedKeys = @{}
        $validatedKeys = @{}
        $consumedKeys = @{}
        $relayAttachKeys = @{}

        foreach ($hit in $hostRedirectLines) {
            if ($hit.Line -match 'SPECTATOR REDIRECT GRANT created id=([0-9A-Fa-f]{8}) epoch=([0-9A-Fa-f]{8})') {
                $createdKeys["$($Matches[1].ToUpperInvariant()):$($Matches[2].ToUpperInvariant())"] = $true
            }
            if ($hit.Line -match 'SPECTATOR REDIRECT GRANT ready id=([0-9A-Fa-f]{8}) epoch=([0-9A-Fa-f]{8})') {
                $readyKeys["$($Matches[1].ToUpperInvariant()):$($Matches[2].ToUpperInvariant())"] = $true
            }
            if ($hit.Line -match 'SPECTATOR REDIRECT CLAIM accepted id=([0-9A-Fa-f]{8}) epoch=([0-9A-Fa-f]{8})') {
                $claimedKeys["$($Matches[1].ToUpperInvariant()):$($Matches[2].ToUpperInvariant())"] = $true
            }
        }
        foreach ($hit in $clientRedirectLines) {
            if ($hit.Line -match 'SPECTATOR REDIRECT GRANT validated id=([0-9A-Fa-f]{8}) epoch=([0-9A-Fa-f]{8}) status=1') {
                $validatedKeys["$($Matches[1].ToUpperInvariant()):$($Matches[2].ToUpperInvariant())"] = $true
            }
            if ($hit.Line -match 'SPECTATOR REDIRECT ticket consumed id=([0-9A-Fa-f]{8}) epoch=([0-9A-Fa-f]{8})') {
                $consumedKeys["$($Matches[1].ToUpperInvariant()):$($Matches[2].ToUpperInvariant())"] = $true
            }
            if ($hit.Line -match 'subscriber attach .*relayGrant=([0-9A-Fa-f]{8})' -and
                $Matches[1] -ne '00000000') {
                $relayAttachKeys[$Matches[1].ToUpperInvariant()] = $true
            }
        }

        $successfulViewerKeys = @{}
        for ($viewerIndex = 0; $viewerIndex -lt $spectatorStates.Count; ++$viewerIndex) {
            $viewerState = $spectatorStates[$viewerIndex]
            $viewerLines = @(Get-ProcessSessionLogLines $viewerState.Process.Id)
            $viewerText = $viewerLines.Line -join "`n"
            $initialPort = if ($spectatorImpairmentEnabled) {
                $BasePort + 2 + $SpectatorCount + $viewerIndex
            } else { $BasePort }
            $initialHostPath = $viewerText -match
                "client transport open local=\d+ host=127\.0\.0\.1:$initialPort"
            $viewerAcceptKeys = @{}
            $viewerAttachedKeys = @{}
            foreach ($hit in $viewerLines) {
                if ($hit.Line -match 'SPECTATOR REDIRECT ACCEPT grant=(\d+) epoch=(\d+)') {
                    $key = "{0:X8}:{1:X8}" -f [uint32]$Matches[1], [uint32]$Matches[2]
                    $viewerAcceptKeys[$key] = $true
                }
                if ($hit.Line -match 'SPECTATOR REDIRECT ATTACHED grant=(\d+) epoch=(\d+)') {
                    $key = "{0:X8}:{1:X8}" -f [uint32]$Matches[1], [uint32]$Matches[2]
                    $viewerAttachedKeys[$key] = $true
                }
            }
            $viewerFallbacks = @($viewerLines | Where-Object {
                $_.Line -match 'SPECTATOR REDIRECT FALLBACK'
            })
            if ($viewerIndex -eq 0) {
                $directViewerPass = $initialHostPath -and $viewerAcceptKeys.Count -eq 0 -and
                    $viewerAttachedKeys.Count -eq 0 -and $viewerFallbacks.Count -eq 0
                $report += New-ReportResult "spectator-1 host-direct route" $directViewerPass `
                    "startedAtHost=$initialHostPath accept=$($viewerAcceptKeys.Count) attached=$($viewerAttachedKeys.Count) fallback=$($viewerFallbacks.Count)"
                continue
            }

            $acceptKey = @($viewerAcceptKeys.Keys | Select-Object -First 1)
            $viewerRedirectPass = $initialHostPath -and $viewerAcceptKeys.Count -eq 1 -and
                $viewerAttachedKeys.Count -eq 1 -and $acceptKey.Count -eq 1 -and
                $viewerAttachedKeys.ContainsKey($acceptKey[0]) -and $viewerFallbacks.Count -eq 0
            if ($viewerRedirectPass) {
                $successfulViewerKeys[$acceptKey[0]] = $true
            }
            $report += New-ReportResult "spectator-$($viewerIndex + 1) authenticated redirect attach" `
                $viewerRedirectPass `
                "startedAtHost=$initialHostPath accept=$($viewerAcceptKeys.Count) attached=$($viewerAttachedKeys.Count) fallback=$($viewerFallbacks.Count) key=$($acceptKey -join ',')"
        }

        $missingLifecycle = @()
        foreach ($key in @($successfulViewerKeys.Keys)) {
            $grantId = $key.Split(':')[0]
            if (-not $createdKeys.ContainsKey($key)) { $missingLifecycle += "$key/host-created" }
            if (-not $readyKeys.ContainsKey($key)) { $missingLifecycle += "$key/host-acked" }
            if (-not $claimedKeys.ContainsKey($key)) { $missingLifecycle += "$key/host-claimed" }
            if (-not $validatedKeys.ContainsKey($key)) { $missingLifecycle += "$key/client-validated" }
            if (-not $consumedKeys.ContainsKey($key)) { $missingLifecycle += "$key/client-consumed" }
            if (-not $relayAttachKeys.ContainsKey($grantId)) { $missingLifecycle += "$key/client-subscriber" }
        }
        $expectedRedirects = $SpectatorCount - 1
        $lifecyclePass = $successfulViewerKeys.Count -eq $expectedRedirects -and
            $missingLifecycle.Count -eq 0
        $report += New-ReportResult "authenticated redirect grant/ack/claim lifecycle" `
            $lifecyclePass `
            "viewers=$($successfulViewerKeys.Count)/$expectedRedirects created=$($createdKeys.Count) acked=$($readyKeys.Count) claimed=$($claimedKeys.Count) validated=$($validatedKeys.Count) consumed=$($consumedKeys.Count) relayAttach=$($relayAttachKeys.Count) missing=$($missingLifecycle -join ',')"

        $redirectFallbacks = @(
            $hostRedirectLines | Where-Object {
                $_.Line -match 'SPECTATOR REDIRECT (fallback authenticated|GRANT refused)'
            }
            $clientRedirectLines | Where-Object {
                $_.Line -match 'SPECTATOR REDIRECT (fallback authenticated|GRANT refused)'
            }
            foreach ($viewerState in $spectatorStates) {
                Get-ProcessSessionLogLines $viewerState.Process.Id 'SPECTATOR REDIRECT FALLBACK'
            }
        )
        $report += New-ReportResult "redirect completed without fallback" `
            ($redirectFallbacks.Count -eq 0) "fallbackLines=$($redirectFallbacks.Count)"
    }

    if ($spectatorImpairmentEnabled) {
        for ($proxyIndex = 0; $proxyIndex -lt $SpectatorCount; ++$proxyIndex) {
            $proxyLog = Join-Path $GameDir "harness_spectator_proxy_$($proxyIndex + 1).log"
            $proxyText = if (Test-Path $proxyLog) {
                Get-Content -LiteralPath $proxyLog -Raw -ErrorAction SilentlyContinue
            } else { "" }
            $maxReceived = 0L; $maxSent = 0L; $maxDropped = 0L
            $maxDuplicated = 0L; $maxReordered = 0L
            foreach ($line in @($proxyText -split "`r?`n")) {
                if ($line -match 'STATUS recv=(\d+) sent=(\d+) drop=(\d+) dup=(\d+) reorder=(\d+)') {
                    $maxReceived = [Math]::Max($maxReceived, [long]$Matches[1])
                    $maxSent = [Math]::Max($maxSent, [long]$Matches[2])
                    $maxDropped = [Math]::Max($maxDropped, [long]$Matches[3])
                    $maxDuplicated = [Math]::Max($maxDuplicated, [long]$Matches[4])
                    $maxReordered = [Math]::Max($maxReordered, [long]$Matches[5])
                }
            }
            # Redirected viewers leave this proxy after Hello/Redirect; skip its counters.
            $redirectedFirstHop = $redirectProofRequired -and $proxyIndex -gt 0
            $proxyPass = $proxyText -match 'READY ' -and $maxReceived -gt 0 -and $maxSent -gt 0
            if (-not $redirectedFirstHop) {
                if ($SpectatorLossPct -gt 0) { $proxyPass = $proxyPass -and $maxDropped -gt 0 }
                if ($SpectatorDuplicatePct -gt 0) { $proxyPass = $proxyPass -and $maxDuplicated -gt 0 }
                if ($SpectatorReorderPct -gt 0) { $proxyPass = $proxyPass -and $maxReordered -gt 0 }
            }
            $report += New-ReportResult "spectator-$($proxyIndex + 1) impairment path" $proxyPass `
                "route=$(if ($redirectedFirstHop) { 'redirect-first-hop' } else { 'archive' }) recv=$maxReceived sent=$maxSent drop=$maxDropped dup=$maxDuplicated reorder=$maxReordered"
        }
    }

    $hostDigests = Get-PairedDigestCheckpoints $hostProc.Id 'source'
    $clientDigests = Get-PairedDigestCheckpoints $clientProc.Id 'source'
    $sourceDigestSets = @(
        [pscustomobject]@{ Role = 'host'; Digests = $hostDigests },
        [pscustomobject]@{ Role = 'client'; Digests = $clientDigests }
    )
    $viewerDigestSets = @($spectatorStates | ForEach-Object {
        Get-PairedDigestCheckpoints $_.Process.Id 'playback'
    })
    $anyDigestSignal = $hostDigests.Count -gt 0 -or $clientDigests.Count -gt 0 -or
        @($viewerDigestSets | Where-Object { $_.Count -gt 0 }).Count -gt 0
    $digestPass = $true
    $digestDetail = @()
    for ($digestIndex = 0; $digestIndex -lt $spectatorStates.Count; ++$digestIndex) {
        $state = $spectatorStates[$digestIndex]
        $viewerDigests = $viewerDigestSets[$digestIndex]
        $matchedRole = 'none'
        $bestCommon = 0
        $sourceDetails = @()
        foreach ($sourceSet in $sourceDigestSets) {
            $sourceDigests = $sourceSet.Digests
            $common = @($sourceDigests.Keys | Where-Object { $viewerDigests.ContainsKey($_) })
            $mismatch = @($common | Where-Object { $sourceDigests[$_] -ne $viewerDigests[$_] })
            $sourceDetails += "$($sourceSet.Role)=$($common.Count)/$($mismatch.Count)"
            if ($common.Count -gt $bestCommon -and $mismatch.Count -eq 0) {
                $bestCommon = $common.Count
                $matchedRole = $sourceSet.Role
            }
        }
        if (($anyDigestSignal -or $RequirePairedInputDigest) -and
            ($viewerDigests.Count -eq 0 -or $bestCommon -eq 0)) {
            $digestPass = $false
        }
        $digestDetail += "pid=$($state.Process.Id) source=$matchedRole common=$bestCommon candidates[$($sourceDetails -join ',')]"
    }
    if ($anyDigestSignal -or $RequirePairedInputDigest) {
        $report += New-ReportResult "confirmed paired-input digest" $digestPass ($digestDetail -join '; ')
    } else {
        $report += New-ReportResult "confirmed paired-input digest signal" $true `
            "not emitted and explicitly waived with -RequirePairedInputDigest:`$false"
    }
}

if ($RequireSystemKeys) {
    $hostLines = @(Get-ProcessSessionLogLines $hostProc.Id)
    $clientLines = @(Get-ProcessSessionLogLines $clientProc.Id)
    $hostText = ($hostLines.Line -join "`n")
    $clientText = ($clientLines.Line -join "`n")
    $hostAuthority = $hostText -match 'overlay authority bound source=EFZ\+0xA3E80' -and
        $hostText -match 'DirectInput online overlay observed'
    $clientAuthority = $clientText -match 'overlay authority bound source=EFZ\+0xA3E80' -and
        $clientText -match 'DirectInput online overlay observed'
    $report += New-ReportResult "player game-facing DirectInput authority" `
        ($hostAuthority -and $clientAuthority) `
        "hostBoundAndObserved=$hostAuthority clientBoundAndObserved=$clientAuthority"
    $originated = $hostText -match 'sent synchronized ESC' -and
        $hostText -match 'originated F4' -and $hostText -match 'originated F5' -and
        $hostText -match 'originated F6' -and $hostText -match 'originated F7' -and
        $hostText -match 'originated F8' -and $hostText -match 'originated Numpad \+' -and
        $hostText -match 'originated Numpad -'
    $practiceApplied = $hostText -match 'apply confirmed practice state=0x08' -and
        $hostText -match 'apply confirmed practice state=0x10' -and
        $clientText -match 'apply confirmed practice state=0x08' -and
        $clientText -match 'apply confirmed practice state=0x10'
    $practiceAbsent = $hostText -notmatch 'apply confirmed practice state=' -and
        $clientText -notmatch 'apply confirmed practice state='
    $retiredBitsAbsent = $hostText -notmatch 'emit F[678] level' -and
        $clientText -notmatch 'emit F[678] level' -and
        $hostText -notmatch 'emit Numpad [+-]' -and
        $clientText -notmatch 'emit Numpad [+-]'
    $escApplied = $hostText -match 'apply post ESC' -and $clientText -match 'apply post ESC'
    $policyPass = if ($AllowPracticeKeys) { $practiceApplied } else { $practiceAbsent }
    $report += New-ReportResult "player system-input policy and confirmed application" `
        ($originated -and $policyPass -and $retiredBitsAbsent -and $escApplied) `
        "originatedRaw=$originated allowPractice=$AllowPracticeKeys practiceApplied=$practiceApplied practiceAbsent=$practiceAbsent retiredBitsAbsent=$retiredBitsAbsent escApplied=$escApplied"

    $hostLiveDelay = $hostText -match 'HARNESS consumed local Increase Input Delay shortcut' -and
        $hostText -match 'HARNESS consumed local Decrease Input Delay shortcut' -and
        $hostText -match 'input delay (changed locally|unchanged at)'
    $clientDidNotConsume = $clientText -notmatch 'HARNESS consumed local .* Input Delay shortcut'
    $report += New-ReportResult "live delay controls are local and independent" `
        ($hostLiveDelay -and $clientDidNotConsume -and $retiredBitsAbsent) `
        "hostAdjusted=$hostLiveDelay clientDidNotConsume=$clientDidNotConsume absentFromWire=$retiredBitsAbsent"

    if ($Spectator) {
        foreach ($state in $spectatorStates) {
            $flags = 0
            foreach ($frame in $state.SystemFrames) {
                $flags = $flags -bor (($frame.P1 -shr 8) -bor ($frame.P2 -shr 8))
            }
            $hasEsc = ($flags -band 0x01) -ne 0
            $hasF4 = ($flags -band 0x08) -ne 0
            $hasF5 = ($flags -band 0x10) -ne 0
            $viewerText = (@(Get-ProcessSessionLogLines $state.Process.Id).Line -join "`n")
            $emitEsc = $viewerText -match 'emit ESC edge'
            $applyF4 = $viewerText -match 'apply confirmed practice state=0x08'
            $applyF5 = $viewerText -match 'apply confirmed practice state=0x10'
            $expectedFlags = if ($AllowPracticeKeys) { 0x19 } else { 0x01 }
            $practicePlayback = if ($AllowPracticeKeys) { $applyF4 -and $applyF5 } `
                else { -not $applyF4 -and -not $applyF5 }
            $report += New-ReportResult "spectator pid=$($state.Process.Id) confirmed system-input application" `
                ($flags -eq $expectedFlags -and $emitEsc -and $practicePlayback) `
                ("records={0} flags=0x{1:X2} expected=0x{2:X2} wire[ESC={3} F4/F5={4}/{5}] playback[ESC={6} F4/F5={7}/{8}]" -f `
                    $state.SystemFrames.Count, $flags, $expectedFlags, $hasEsc, $hasF4, $hasF5, `
                    $emitEsc, $applyF4, $applyF5)
        }
    }
}

if ($Spectator -and $SpectatorTerminalTimeoutSec -gt 0 -and $passVerdicts.Count -eq 2) {
    $sourceDeadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $sourceDeadline) {
        $hostProc.Refresh(); $clientProc.Refresh()
        if ($hostProc.HasExited -and $clientProc.HasExited) { break }
        Start-Sleep -Seconds 1
    }
    $sourceExited = $hostProc.HasExited -and $clientProc.HasExited
    $report += New-ReportResult "player source terminal shutdown" $sourceExited `
        "hostExited=$($hostProc.HasExited) clientExited=$($clientProc.HasExited)"

    if ($sourceExited) {
        $terminalDeadline = (Get-Date).AddSeconds($SpectatorTerminalTimeoutSec)
        while ((Get-Date) -lt $terminalDeadline) {
            $spectatorProcs | ForEach-Object { try { $_.Refresh() } catch {} }
            if (@($spectatorProcs | Where-Object { -not $_.HasExited }).Count -eq 0) { break }
            Start-Sleep -Seconds 2
        }
    }
    foreach ($spectatorProc in $spectatorProcs) {
        try { $spectatorProc.Refresh() } catch {}
        $sourceEnd = @(Get-ProcessSessionLogLines $spectatorProc.Id 'SPECTATOR SOURCE END')
        $playbackEnd = @(Get-ProcessSessionLogLines $spectatorProc.Id `
            'SPECTATOR PLAYBACK COMPLETE')
        $terminal = @(Get-ProcessSessionLogLines $spectatorProc.Id `
            'closing spectator process: spectator source session ended')
        $terminalDigestMatch = $false
        if ($sourceEnd.Count -gt 0 -and $playbackEnd.Count -gt 0 -and
            $sourceEnd[-1].Line -match 'effective=(\d+):(\d+)/([0-9A-Fa-f]{16}) total=(\d+)') {
            $sourceTuple = "$($Matches[1]):$($Matches[2]):$($Matches[3].ToUpperInvariant()):$($Matches[4])"
            if ($playbackEnd[-1].Line -match 'game=(\d+) records=(\d+) digest=([0-9A-Fa-f]{16}) total=(\d+)') {
                $playbackTuple = "$($Matches[1]):$($Matches[2]):$($Matches[3].ToUpperInvariant()):$($Matches[4])"
                $terminalDigestMatch = $sourceTuple -eq $playbackTuple
            }
        }
        $report += New-ReportResult "spectator pid=$($spectatorProc.Id) graceful source terminal" `
            ($spectatorProc.HasExited -and $sourceEnd.Count -gt 0 -and
             $playbackEnd.Count -gt 0 -and $terminalDigestMatch -and $terminal.Count -gt 0) `
            "exited=$($spectatorProc.HasExited) sourceEnd=$($sourceEnd.Count) playbackEnd=$($playbackEnd.Count) digestMatch=$terminalDigestMatch closeEvidence=$($terminal.Count)"
    }
} elseif ($Spectator -and $SpectatorTerminalTimeoutSec -gt 0) {
    $report += New-ReportResult "spectator source-disconnect terminal" $false `
        "not attempted because both player PASS verdicts were unavailable"
}

Write-Host ""
Write-Host "================ HARNESS RESULT ================"
$verdicts | ForEach-Object { Write-Host $_.Line.Trim() }
foreach ($item in $report) {
    $label = if ($item.Passed) { "PASS" } else { "FAIL" }
    Write-Host ("[{0}] {1}: {2}" -f $label, $item.Name, $item.Detail)
}

$patterns = "STATE DESYNC", "RESIM NON-DETERMINISTIC", "unfillable", "mispredict frame",
            "spec_transition_veto", "round entry aligned", "round entry relabel",
            "battle entry rng reseed", "inject verify", "handoff barrier TIMEOUT",
            "recorder gap", "corrected recorder slot unavailable", "subscriber timeout"
foreach ($pattern in $patterns) {
    $hits = @(Get-SessionLogMatches ([regex]::Escape($pattern)))
    Write-Host ("{0,-36} {1}" -f $pattern, $hits.Count)
}
Write-Host "logs: $logRoot (this invocation began $startTime)"
Write-Host "================================================"

$launched | ForEach-Object {
    try { $_.Refresh() } catch {}
    if (-not $_.HasExited) { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
}
$spectatorProxyProcs | ForEach-Object {
    try { $_.Refresh() } catch {}
    if (-not $_.HasExited) { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
}

$pass = @($report | Where-Object { -not $_.Passed }).Count -eq 0
Release-RunLock
exit $(if ($pass) { 0 } else { 1 })
