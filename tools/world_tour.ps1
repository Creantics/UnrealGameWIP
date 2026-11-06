# Runs a world map's -WorldAutoPlay tour headless and says whether the run counts. A run that never loaded its map still
# exits 0 with no STUCK, so a clean exit means nothing until the log shows the map, the tour's start and its end.
# Prints TOUR OK, TOUR STUCK <n> or VOID; exits 0, 1 or 3. Stops the run after -LimitMinutes.
#   tools/world_tour.ps1 -Map /Game/Maps/World_Solhaven_Build -Log <new log path> [-District 9]
#   tools/world_tour.ps1 -Map /Game/Maps/World_Solhaven_Build -Log <existing log> -CheckOnly   (judges a log, runs nothing)
# Git Bash turns /Game/... into a Windows path before this script sees it; set MSYS_NO_PATHCONV=1 first (docs/WORLDS.md).
param(
    [Parameter(Mandatory)][string]$Map,
    [Parameter(Mandatory)][string]$Log,
    [int]$District = -1,
    [int]$LimitMinutes = 30,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot

if ($Map -notmatch '^/Game/') { "VOID: -Map is '$Map', not a /Game/ path; from Git Bash, set MSYS_NO_PATHCONV=1"; exit 3 }

if (-not $CheckOnly) {
    # An old log at this path could pass the checks below for a run that never started.
    if (Test-Path $Log) { "VOID: $Log already exists; give a new path"; exit 3 }
    $runArgs = @("`"$repo\ForgottenThird.uproject`"", $Map, '-game', '-NullRHI', '-WorldAutoPlay', '-AlphaFast', '-WorldQuitOnDone',
                 '-nosound', '-unattended', '-nopause', '-nosplash', "-abslog=`"$Log`"")
    if ($District -ge 0) { $runArgs += "-WorldDistrict=$District" }
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
if ($stuck -gt 0 -or $ended.Line -notmatch 'TOUR DONE') { "TOUR STUCK $stuck"; exit 1 }
"TOUR OK"
