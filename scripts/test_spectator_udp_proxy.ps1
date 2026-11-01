# Deterministic smoke/regression test for the spectator impairment proxy.
$ErrorActionPreference = "Stop"

function Get-FreeUdpPort {
    $socket = [System.Net.Sockets.UdpClient]::new(0)
    try {
        return ([System.Net.IPEndPoint]$socket.Client.LocalEndPoint).Port
    } finally {
        $socket.Dispose()
    }
}

$hostPort = Get-FreeUdpPort
do { $proxyPort = Get-FreeUdpPort } while ($proxyPort -eq $hostPort)
$proxyScript = Join-Path $PSScriptRoot "spectator_udp_proxy.ps1"
$logPath = Join-Path $PSScriptRoot ".spectator_proxy_test_$PID.log"
$proxyProcess = $null
$hostSocket = $null
$viewerSocket = $null

try {
    if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath -Force }
    $powerShellExe = Join-Path $PSHOME "pwsh.exe"
    if (-not (Test-Path $powerShellExe)) { $powerShellExe = Join-Path $PSHOME "powershell.exe" }
    $argumentLine = @(
        '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
        '-File', "`"$proxyScript`"",
        '-ListenPort', $proxyPort,
        '-HostPort', $hostPort,
        '-DelayMs', 3,
        '-JitterMs', 2,
        '-LossPct', 20,
        '-DuplicatePct', 50,
        '-ReorderPct', 50,
        '-Seed', 424242,
        '-ParentProcessId', $PID,
        '-LogPath', "`"$logPath`""
    ) -join ' '
    $proxyProcess = Start-Process -FilePath $powerShellExe -ArgumentList $argumentLine `
        -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 600
    $proxyProcess.Refresh()
    if ($proxyProcess.HasExited) { throw "proxy exited during startup" }

    $hostSocket = [System.Net.Sockets.UdpClient]::new($hostPort)
    $hostSocket.Client.Blocking = $false
    $viewerSocket = [System.Net.Sockets.UdpClient]::new(0)
    $viewerSocket.Client.Blocking = $false
    $proxyEndpoint = [System.Net.IPEndPoint]::new(
        [System.Net.IPAddress]::Loopback, $proxyPort)

    for ($sequence = 0; $sequence -lt 200; ++$sequence) {
        $payload = [BitConverter]::GetBytes($sequence)
        [void]$viewerSocket.Send($payload, $payload.Length, $proxyEndpoint)
    }

    $received = 0
    $source = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    while ([DateTime]::UtcNow -lt $deadline) {
        while ($hostSocket.Available -gt 0) {
            $source = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
            [void]$hostSocket.Receive([ref]$source)
            ++$received
        }
        Start-Sleep -Milliseconds 2
    }
    if ($received -le 0 -or $null -eq $source -or $source.Port -ne $proxyPort) {
        throw "viewer-to-host forwarding did not use the proxy endpoint"
    }

    # Reverse traffic must return to the viewer endpoint learned from the first direction.
    $reply = [Text.Encoding]::ASCII.GetBytes("reverse")
    [void]$hostSocket.Send($reply, $reply.Length, $source)
    $reverseReceived = $false
    $reverseDeadline = [DateTime]::UtcNow.AddSeconds(3)
    while ([DateTime]::UtcNow -lt $reverseDeadline -and -not $reverseReceived) {
        if ($viewerSocket.Available -gt 0) {
            $reverseSource = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
            $bytes = $viewerSocket.Receive([ref]$reverseSource)
            $reverseReceived = [Text.Encoding]::ASCII.GetString($bytes) -eq "reverse"
        }
        Start-Sleep -Milliseconds 2
    }
    if (-not $reverseReceived) { throw "host-to-viewer forwarding failed" }

    # The proxy writes aggregate evidence every five seconds.
    Start-Sleep -Seconds 2
    $log = Get-Content -LiteralPath $logPath -Raw
    $status = [regex]::Matches(
        $log,
        'STATUS recv=(\d+) sent=(\d+) drop=(\d+) dup=(\d+) reorder=(\d+)') |
        Select-Object -Last 1
    if ($null -eq $status -or [long]$status.Groups[1].Value -le 0 -or
        [long]$status.Groups[2].Value -le 0 -or [long]$status.Groups[3].Value -le 0 -or
        [long]$status.Groups[4].Value -le 0 -or [long]$status.Groups[5].Value -le 0) {
        throw "proxy impairment counters were not exercised"
    }
    Write-Host "spectator UDP proxy PASS: received=$received $($status.Value)"
} finally {
    if ($null -ne $viewerSocket) { $viewerSocket.Dispose() }
    if ($null -ne $hostSocket) { $hostSocket.Dispose() }
    if ($null -ne $proxyProcess) {
        try { $proxyProcess.Refresh() } catch {}
        if (-not $proxyProcess.HasExited) {
            Stop-Process -Id $proxyProcess.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath -Force }
}
