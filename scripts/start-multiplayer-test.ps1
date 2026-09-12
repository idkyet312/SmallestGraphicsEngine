# Launches a host and a client on this machine and waits for them to connect.
#
# Two instances can be started by hand -- MULTIPLAYER -> HOST in one window,
# MULTIPLAYER -> JOIN in the other -- but doing that on every build is slow, and
# the ordering matters in a way that is easy to get wrong: the client's connect
# fails outright if nothing is listening yet, and the failure looks like a bug
# in the session rather than a race. This waits for the host's socket to appear
# before starting the client, then reports whether the handshake actually
# completed instead of leaving that to be read out of a log.
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [uint16]$Port = 27015,
    # Where the client connects. Left as loopback for a same-machine test; pass
    # the host's LAN address to drive this from a second PC.
    [string]$Address = '127.0.0.1',
    # Skip the build and use whatever is already in build/.
    [switch]$NoBuild,
    # Start only the host, and wait. For the second machine in a LAN test, where
    # the client is started by hand on the other PC.
    [switch]$HostOnly,
    # How long to wait for the host to bind its port, and then for the
    # handshake. The first launch of a session loads shaders and builds
    # acceleration structures, so the generous default is the cold-start cost,
    # not the network.
    [int]$TimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$exe = Join-Path $repo 'build\GraphicEngine.exe'

if (-not $NoBuild) {
    # A running instance holds the exe and the link fails with LNK1104, which
    # reads as a code error rather than as "close the game". Say so plainly.
    if (Get-Process GraphicEngine -ErrorAction SilentlyContinue) {
        throw "GraphicEngine is already running. Close it first, or pass -NoBuild."
    }
    Write-Host "Building GraphicEngine ($Configuration)..." -ForegroundColor Cyan
    cmake --build build --config $Configuration --target GraphicEngine
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
}
if (-not (Test-Path $exe)) { throw "GraphicEngine.exe not found at $exe" }

# Both instances write into build/logs/GraphicEngine.log, and the second launch
# rotates the first's file away. Clearing it first means the log read at the end
# is this run's and not a leftover from the last one.
$logDir = Join-Path $repo 'build\logs'
$log = Join-Path $logDir 'GraphicEngine.log'
if (Test-Path $log) { Remove-Item $log -Force }

function Start-Instance {
    param([string[]]$Arguments)
    # WorkingDirectory is build/, not the repo root: the engine resolves
    # Content/, shaders/ and logs/ relative to the working directory, so
    # launching from anywhere else finds no assets.
    Start-Process -FilePath $exe -ArgumentList $Arguments `
        -WorkingDirectory (Join-Path $repo 'build') -PassThru
}

Write-Host "Starting host on port $Port..." -ForegroundColor Cyan
$hostProcess = Start-Instance @('-host', "$Port")

# Wait for the socket, not for a fixed sleep. The host has to finish booting --
# shaders, DXR, scene assets -- before it binds, and how long that takes varies
# enough that any sleep is either flaky or wasteful.
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$bound = $false
while ((Get-Date) -lt $deadline) {
    if ($hostProcess.HasExited) { throw "Host exited during startup (code $($hostProcess.ExitCode))." }
    if (Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue) { $bound = $true; break }
    Start-Sleep -Milliseconds 500
}
if (-not $bound) {
    Stop-Process -Id $hostProcess.Id -Force -ErrorAction SilentlyContinue
    throw "Host did not bind UDP $Port within $TimeoutSeconds seconds."
}
Write-Host "  host is listening on UDP $Port (pid $($hostProcess.Id))" -ForegroundColor DarkGray

if ($HostOnly) {
    Write-Host ""
    $ip = (Get-NetIPAddress -AddressFamily IPv4 |
           Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
           Select-Object -First 1 -ExpandProperty IPAddress)
    Write-Host "Hosting. On the other machine, join:" -ForegroundColor Green
    if ($ip) { Write-Host "  $ip  port $Port" -ForegroundColor Green }
    else     { Write-Host "  <this machine's LAN address>  port $Port" -ForegroundColor Green }
    Write-Host "Windows Firewall may prompt on the first host -- allow private networks."
    return
}

# The bound socket only says the listener exists; the host is still finishing
# its own boot behind it. Two instances racing through shader and asset loading
# at once makes both slower and the handshake flakier, so let the host get clear
# of that before the second process starts competing for the same disk and GPU.
Start-Sleep -Seconds 2

Write-Host "Starting client -> ${Address}:${Port}..." -ForegroundColor Cyan
$clientProcess = Start-Instance @('-join', $Address, "$Port")

# The handshake itself takes tens of milliseconds, but the client has its own
# cold start to get through first. Watch the log rather than guessing.
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$connected = $false
while ((Get-Date) -lt $deadline) {
    if ($clientProcess.HasExited) { throw "Client exited during startup (code $($clientProcess.ExitCode))." }
    if (Test-Path $log) {
        # -Raw: both processes append to this file, so the lines interleave and
        # a per-line read can split a message. The marker is the client being
        # given a player id, which is the point the session is actually usable.
        $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if ($text -and $text -match 'welcome received; we are player') { $connected = $true; break }
    }
    Start-Sleep -Milliseconds 1500
}

Write-Host ""
if ($connected) {
    Write-Host "Connected." -ForegroundColor Green
    if (Test-Path $log) {
        # Both processes append to this one file without locking, so a line can
        # be cut in half by the other's write landing mid-message. Matching up
        # to the next log prefix -- rather than to the next '[' -- keeps a
        # spliced line readable instead of truncating it at the splice.
        [regex]::Matches((Get-Content $log -Raw),
                         'LogNet: \w+: (.+?)(?=\s*\[\d{4}\.|\r|\n|$)') |
            ForEach-Object { "  " + $_.Groups[1].Value.Trim() } |
            Select-Object -Unique |
            Write-Host -ForegroundColor DarkGray
    }
    Write-Host ""
    Write-Host "Now pick the SAME level in both windows (TEST LEVEL is the usual one)." -ForegroundColor Yellow
    Write-Host "Level choice is not synchronised -- each player starts their own."
} else {
    Write-Host "No handshake within $TimeoutSeconds seconds." -ForegroundColor Red
    Write-Host "Both instances are still running; check build\logs\GraphicEngine.log."
}

Write-Host ""
Write-Host "Host pid $($hostProcess.Id), client pid $($clientProcess.Id)."
Write-Host "Stop both with:  Stop-Process -Name GraphicEngine -Force"
