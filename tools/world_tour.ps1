# Runs a world map's -WorldAutoPlay tour headless and says whether the run counts. A run that never loaded its map still
# exits 0 with no STUCK, so a clean exit means nothing until the log shows the map, the tour's start and its end.
# Prints TOUR OK, TOUR STUCK <n>, TOUR FAIL <n> or VOID; exits 0, 1 or 3. Stops the run after -LimitMinutes.
#   tools/world_tour.ps1 -Map /Game/Maps/World_Solhaven_Build -Log <new log path> [-District 9] [-Kinds edge,push] [-Stills <dir>]
#   tools/world_tour.ps1 -Map /Game/Maps/World_Solhaven_Build -Log <existing log> -CheckOnly   (judges a log, runs nothing)
# On a map with tour stops (FTBuildMassingWorld -Tour=) it also judges the stop legs: a stop given up, an edge leg that met
# no cause, a push that got through, a wet leg, and with -Stills a still that never reached the disk, each count as a fail.
# -Kinds passes -WorldTourKinds (reach, edge, push). -Stills runs with -RenderOffScreen instead of -NullRHI and passes
# -WorldTourStills=<dir>, so each stop marked still saves <dir>/<id>.png.
# Git Bash turns /Game/... into a Windows path before this script sees it; set MSYS_NO_PATHCONV=1 first (docs/WORLDS.md).
param(
    [Parameter(Mandatory)][string]$Map,
    [Parameter(Mandatory)][string]$Log,
    [int]$District = -1,
    [int]$LimitMinutes = 30,
    [string[]]$Kinds = @(),
    [string]$Stills = '',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot

if ($Map -notmatch '^/Game/') { "VOID: -Map is '$Map', not a /Game/ path; from Git Bash, set MSYS_NO_PATHCONV=1"; exit 3 }

if (-not $CheckOnly) {
    # An old log at this path could pass the checks below for a run that never started.
    if (Test-Path $Log) { "VOID: $Log already exists; give a new path"; exit 3 }
    # A still needs a renderer; off-screen, so no window opens.
    $rhi = if ($Stills) { '-RenderOffScreen' } else { '-NullRHI' }
    $runArgs = @("`"$repo\ForgottenThird.uproject`"", $Map, '-game', $rhi, '-WorldAutoPlay', '-AlphaFast', '-WorldQuitOnDone',
                 '-nosound', '-unattended', '-nopause', '-nosplash', "-abslog=`"$Log`"")
    if ($District -ge 0) { $runArgs += "-WorldDistrict=$District" }
    # -Kinds edge,push arrives as an array; the director takes the names joined by commas.
    if ($Kinds.Count -gt 0) { $runArgs += "-WorldTourKinds=$(($Kinds | ForEach-Object { $_ -split '[,+]' } | Where-Object { $_ }) -join ',')" }
    if ($Stills) {
        $null = New-Item -ItemType Directory -Force -Path $Stills
        $stillsDir = (Resolve-Path $Stills).Path
        $runArgs += "-WorldTourStills=`"$stillsDir`""
    }
    $p = Start-Process 'E:\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' -ArgumentList $runArgs -PassThru -NoNewWindow -RedirectStandardOutput "$Log.out"
    $null = $p.Handle
    if (-not $p.WaitForExit($LimitMinutes * 60 * 1000)) { $p.Kill(); "VOID: timed out after $LimitMinutes min; log $Log"; exit 3 }
    "exit code $($p.ExitCode)"
}

if (-not (Test-Path $Log)) { "VOID: no log at $Log"; exit 3 }
$loaded = Select-String -Path $Log -SimpleMatch "Bringing World $Map." -Quiet
$began = Select-String -Path $Log -Pattern 'World \w+: tour of \d+ legs' | Select-Object -First 1
$ended = Select-String -Path $Log -Pattern 'World \w+: TOUR (DONE|ENDED)' | Select-Object -Last 1
if (-not $loaded) { "VOID: $Map did not load; log $Log"; exit 3 }
if (-not $began) { "VOID: the map loaded but no tour began; log $Log"; exit 3 }
if (-not $ended) { "VOID: the tour never ended; log $Log"; exit 3 }

$stuck = @(Select-String -Path $Log -SimpleMatch 'ExploreDemo: STUCK').Count
$trace = @(Select-String -Path $Log -Pattern 'World \w+: trace leg ').Count
$ended.Line -replace '^.*?(World \w+: TOUR)', '$1'
"$stuck STUCK, $trace trace lines"

# The stop legs (none on a map without a tour table): one line per leg, read by its fixed words.
function Count-Lines([string]$pattern) { @(Select-String -Path $Log -Pattern $pattern).Count }
$reached = Count-Lines 'World \S+: reached stop '
$gaveUp = Count-Lines 'World \S+: gave up on stop '
$edgeMet = Count-Lines 'World \S+: edge stop \S+ \(leg \d+/\d+, edge .\): met, '
$edgeFail = Count-Lines 'World \S+: edge stop \S+ \(leg \d+/\d+, edge .\): FAIL, '
$bare = Count-Lines 'World \S+: edge stop .*: FAIL, BARE HIDDEN WALL'
$unjudged = Count-Lines 'World \S+: edge stop .*: FAIL, UNJUDGED'
$held = Count-Lines 'World \S+: push stop \S+ \(leg \d+/\d+\): held, '
$through = Count-Lines 'World \S+: push stop \S+ \(leg \d+/\d+\): GOT THROUGH, '
$wet = Count-Lines 'World \S+: WET on leg '
$stillSaved = Count-Lines 'World \S+: still for stop \S+ saved: '
$stillMissing = Count-Lines 'World \S+: still for stop \S+ (not on disk after|skipped: no game viewport)'
$stillSkipped = Count-Lines 'World \S+: still for stop \S+ skipped: no (renderer|-WorldTourStills)'
$stopLegs = $reached + $gaveUp + $edgeMet + $edgeFail + $held + $through
if ($stopLegs -gt 0 -or $wet -gt 0) {
    "stops: reached $reached, gave up $gaveUp; edge met a cause $edgeMet, FAIL $edgeFail (bare hidden wall $bare, unjudged $unjudged); " +
        "push held $held, got through $through; wet legs $wet"
    Select-String -Path $Log -Pattern 'World \S+: EDGE [WSNE]: ' | ForEach-Object { $_.Line -replace '^.*?(World \S+: EDGE)', '$1' }
    # Notes for the plan's owner (a leg whose full path runs far past its straight line): shown, never a fail.
    Select-String -Path $Log -Pattern 'World \S+: INFO ' | ForEach-Object { $_.Line -replace '^.*?(World \S+: INFO)', '$1' }
}
if ($stillSaved + $stillMissing + $stillSkipped -gt 0) { "stills: saved $stillSaved, missing $stillMissing, skipped $stillSkipped" }
# With -Stills, a still asked for and not saved fails the run; without it, skipped stills are expected.
$missingFails = if ($Stills) { $stillMissing + $stillSkipped } else { 0 }
$fails = $gaveUp + $edgeFail + $through + $wet + $missingFails
if ($stuck -gt 0) { "TOUR STUCK $stuck"; exit 1 }
if ($fails -gt 0 -or $ended.Line -notmatch 'TOUR DONE') { "TOUR FAIL $fails"; exit 1 }
"TOUR OK"
