# Test-only transparent UDP proxy for spectator transport; one process per viewer.
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 65535)]
    [int]$ListenPort,
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 65535)]
    [int]$HostPort,
    [ValidateRange(0, 60000)]
    [int]$DelayMs = 0,
    [ValidateRange(0, 60000)]
    [int]$JitterMs = 0,
    [ValidateRange(0, 90)]
    [int]$LossPct = 0,
    [ValidateRange(0, 100)]
    [int]$DuplicatePct = 0,
    [ValidateRange(0, 100)]
    [int]$ReorderPct = 0,
    [int]$Seed = 1,
    [int]$ParentProcessId = 0,
    [string]$LogPath = ""
)

$ErrorActionPreference = "Stop"

function Write-ProxyLog([string]$Message) {
    if ([string]::IsNullOrWhiteSpace($LogPath)) { return }
    $stamp = [DateTime]::Now.ToString("HH:mm:ss.fff", [Globalization.CultureInfo]::InvariantCulture)
    "[$stamp] $Message" | Add-Content -LiteralPath $LogPath -Encoding ASCII
}

$udp = $null
try {
    $udp = [System.Net.Sockets.UdpClient]::new($ListenPort)
    $udp.Client.Blocking = $false
    $hostEndpoint = [System.Net.IPEndPoint]::new(
        [System.Net.IPAddress]::Loopback, $HostPort)
    $viewerEndpoint = $null
    $pending = [System.Collections.Generic.List[object]]::new()
    $random = [System.Random]::new($Seed)
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $received = 0L
    $sent = 0L
    $dropped = 0L
    $duplicated = 0L
    $reordered = 0L
    $lastStatusMs = 0L

    Write-ProxyLog "READY listen=$ListenPort host=127.0.0.1:$HostPort delay=$DelayMs jitter=$JitterMs loss=$LossPct duplicate=$DuplicatePct reorder=$ReorderPct seed=$Seed"

    while ($true) {
        if ($ParentProcessId -gt 0 -and
            -not (Get-Process -Id $ParentProcessId -ErrorAction SilentlyContinue)) {
            Write-ProxyLog "parent process ended"
            break
        }

        while ($udp.Available -gt 0) {
            $source = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
            try {
                $bytes = $udp.Receive([ref]$source)
            } catch [System.Net.Sockets.SocketException] {
                break
            }
            ++$received

            $fromHost = $source.Port -eq $HostPort -and
                [System.Net.IPAddress]::IsLoopback($source.Address)
            if ($fromHost) {
                if ($null -eq $viewerEndpoint) { continue }
                $target = $viewerEndpoint
                $direction = "host-to-viewer"
            } else {
                $viewerEndpoint = [System.Net.IPEndPoint]::new($source.Address, $source.Port)
                $target = $hostEndpoint
                $direction = "viewer-to-host"
            }

            if ($LossPct -gt 0 -and $random.Next(100) -lt $LossPct) {
                ++$dropped
                continue
            }

            $jitterOffset = if ($JitterMs -gt 0) {
                $random.Next(-$JitterMs, $JitterMs + 1)
            } else { 0 }
            $packetDelay = [Math]::Max(0, $DelayMs + $jitterOffset)
            if ($ReorderPct -gt 0 -and $random.Next(100) -lt $ReorderPct) {
                $packetDelay += [Math]::Max(20, 2 * $JitterMs + 5)
                ++$reordered
            }
            $pending.Add([pscustomobject]@{
                DueMs = $clock.ElapsedMilliseconds + $packetDelay
                Bytes = $bytes
                Target = $target
                Direction = $direction
            })

            if ($DuplicatePct -gt 0 -and $random.Next(100) -lt $DuplicatePct) {
                $pending.Add([pscustomobject]@{
                    DueMs = $clock.ElapsedMilliseconds + $packetDelay + $random.Next(1, 6)
                    Bytes = $bytes
                    Target = $target
                    Direction = "$direction-duplicate"
                })
                ++$duplicated
            }
        }

        $nowMs = $clock.ElapsedMilliseconds
        for ($index = 0; $index -lt $pending.Count;) {
            $packet = $pending[$index]
            if ($packet.DueMs -gt $nowMs) {
                ++$index
                continue
            }
            try {
                [void]$udp.Send($packet.Bytes, $packet.Bytes.Length, $packet.Target)
                ++$sent
            } catch [System.Net.Sockets.SocketException] {
                Write-ProxyLog "send failed direction=$($packet.Direction) target=$($packet.Target)"
            }
            $pending.RemoveAt($index)
        }

        if ($nowMs - $lastStatusMs -ge 5000) {
            $lastStatusMs = $nowMs
            Write-ProxyLog "STATUS recv=$received sent=$sent drop=$dropped dup=$duplicated reorder=$reordered pending=$($pending.Count) viewer=$viewerEndpoint"
        }
        Start-Sleep -Milliseconds 1
    }
} catch {
    Write-ProxyLog "FATAL $($_.Exception.Message)"
    exit 1
} finally {
    if ($null -ne $udp) { $udp.Dispose() }
    Write-ProxyLog "END"
}
