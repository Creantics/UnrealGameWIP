# The performance benchmark runner: each scene x size x run as one standalone -game process with -Bench (see
# Source/ForgottenThird/FTBenchSubsystem.h), the CSV and summary of each in <Out>\<scene>[_<label>]_<size>_r<n>\. Runs in
# the order printed (scene, then size, then run), so run 1 of a set is the one after a cold driver shader cache. Only the
# process this script started is ever stopped (on its time limit).
#   powershell -File tools\bench\bench.ps1 -Out <dir> [-Scenes day,night] [-Sizes 1920x1080,2560x1440]
#       [-Runs 3] [-Extra '-dpcvars=r.GPUCsvStatsEnabled=1'] [-Label gpusplit] [-Minutes 6] [-Project <uproject>]
#       [-Shared] [-BlockMinutes 28]
# Machine-local settings come from the environment, so the repo holds no local path or process list:
#   FT_UE_CMD       UnrealEditor-Cmd.exe (required)
#   FT_PYTHON       python.exe for FT_BUSY_WRITER (default: python on PATH)
#   FT_BENCH_HEAVY  process names that make a run contaminated (regex alternatives joined by |; default none)
#   FT_BENCH_GAMES  game process names: a run is contaminated and no shared block opens while one runs (default none)
#   FT_BENCH_TRAY   process names only recorded in machine.txt (default none)
#   -Shared also needs FT_BUSY_NOTE (a text file of BUSY/FREE lines shared by the jobs on this PC), FT_BUSY_WRITER (a
#   Python script called as: <script> BUSY|FREE <owner> <text>), FT_BENCH_OWNER (this job's owner name there) and
#   optionally FT_BUSY_FORMAT (the line text, with {0} for the block's own words; default {0}).
# Without -Shared the caller makes sure the PC is otherwise idle. With -Shared (unattended) it writes its own BUSY line
# before each block and FREE after it, blocks of -BlockMinutes at most, and will not open a block while another owner's
# BUSY line is open (younger than 90 min), a HOLD line is not yet lifted (by a later line saying resumed, lifted or freed
# the PC), or a game process runs: it then stops with exit code 3. It is resumable: a run whose summary exists and whose
# machine.txt says CLEAN is skipped, so the same command picks up where a stop left off.
# Each run's machine.txt holds the CPU load and the listed processes seen just before launch and just after exit; a run
# with total CPU over 10%, or a FT_BENCH_HEAVY or FT_BENCH_GAMES process present, is CONTAMINATED.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string[]]$Scenes = @('day', 'night', 'close', 'trees', 'arena_group', 'arena_kite', 'wave', 'cold'),
    [string[]]$Sizes = @('1920x1080', '2560x1440'),
    [int]$Runs = 3,
    [string[]]$Extra = @(),
    [int]$Minutes = 6,
    [string]$Project = '',
    [string]$Label = '',
    [switch]$Shared,
    [int]$BlockMinutes = 28
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Project) { $Project = Join-Path $repo 'ForgottenThird.uproject' }
$ue = $env:FT_UE_CMD
if (-not $ue -or -not (Test-Path $ue)) { 'set FT_UE_CMD to UnrealEditor-Cmd.exe'; exit 2 }
$py = if ($env:FT_PYTHON) { $env:FT_PYTHON } else { 'python' }
$busyWriter = $env:FT_BUSY_WRITER
$busyNote = $env:FT_BUSY_NOTE
$owner = $env:FT_BENCH_OWNER
$busyFormat = if ($env:FT_BUSY_FORMAT) { $env:FT_BUSY_FORMAT } else { '{0}' }
if ($Shared -and -not ($busyWriter -and $busyNote -and $owner)) { 'with -Shared set FT_BUSY_NOTE, FT_BUSY_WRITER and FT_BENCH_OWNER'; exit 2 }
# An empty list matches nothing.
function Get-Names([string]$List) { if ($List) { @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match "^($List)$" }) } else { @() } }

function Get-MachineState {
    $cpu = -1.0
    try {
        $cpu = [math]::Round((Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 3 |
                    ForEach-Object { $_.CounterSamples[0].CookedValue } | Measure-Object -Average).Average, 1)
    } catch {}
    $heavy = @((Get-Names $env:FT_BENCH_HEAVY) + (Get-Names $env:FT_BENCH_GAMES) | ForEach-Object { '{0}({1})' -f $_.ProcessName, $_.Id })
    $top = ''
    try {
        $n = [Environment]::ProcessorCount
        $top = ((Get-Counter '\Process(*)\% Processor Time' -ErrorAction SilentlyContinue).CounterSamples |
                Where-Object { $_.InstanceName -notin @('_total', 'idle') -and $_.CookedValue / $n -gt 2 } |
                Sort-Object CookedValue -Descending | Select-Object -First 5 |
                ForEach-Object { '{0} {1:N0}%' -f $_.InstanceName, ($_.CookedValue / $n) }) -join ', '
    } catch {}
    $tray = @(Get-Names $env:FT_BENCH_TRAY | ForEach-Object { $_.ProcessName } | Sort-Object -Unique)
    [pscustomobject]@{ Cpu = $cpu; Heavy = (($heavy + @($tray | ForEach-Object { "$_(recorded)" })) -join ' '); Top = $top
        Dirty = ($cpu -gt 10 -or $heavy.Count -gt 0) }
}

# Why no shared block may open. Lines read: "BUSY|FREE yyyy-MM-dd HH:mm - <owner>[:] <kind>[:] ...". A FREE closes that
# owner's BUSY of the same kind, or all of that owner's BUSY lines when its kind matches none of them.
function Get-Blockers {
    $open = @{}
    $hold = $null
    $games = @(Get-Names $env:FT_BENCH_GAMES | ForEach-Object { $_.ProcessName } | Sort-Object -Unique)
    if (Test-Path $busyNote) {
        foreach ($l in Get-Content -Encoding utf8 $busyNote) {
            if ($l -match '^HOLD ') { $hold = $l; continue }
            if ($hold -and $l -match 'resum|lifted|freed the pc') { $hold = $null }
            if ($l -match '^(BUSY|FREE) (\d{4}-\d\d-\d\d \d\d:\d\d) - (\S+?):? (\S+)') {
                $who = $Matches[3]
                $key = "$who $($Matches[4].TrimEnd(':').ToUpper())"
                if ($Matches[1] -eq 'BUSY') { $open[$key] = [datetime]::ParseExact($Matches[2], 'yyyy-MM-dd HH:mm', $null) }
                elseif ($open.ContainsKey($key)) { $open.Remove($key) }
                else { @($open.Keys | Where-Object { $_.StartsWith("$who ") }) | ForEach-Object { $open.Remove($_) } }
            }
        }
    }
    $now = Get-Date
    @($open.GetEnumerator() | Where-Object { -not $_.Key.StartsWith("$owner ") -and ($now - $_.Value).TotalMinutes -lt 90 } |
            ForEach-Object { "$($_.Key) since $($_.Value.ToString('HH:mm'))" }) +
        @($(if ($hold) { "HOLD open: $($hold.Substring(0, [math]::Min(60, $hold.Length)))" })) +
        @($(if ($games.Count -gt 0) { "game running: $($games -join ' ')" })) | Where-Object { $_ }
}

function Write-Busy([string]$Kind, [string]$Text) {
    if ($Shared) { & $py -I $busyWriter $Kind $owner ($busyFormat -f $Text) }
}

$strip = '/Game/Maps/World_Solhaven_Strip'
$arena = '/Game/Maps/Demo_Arena'
# The 512 m van loop (engine cm): RD-C north, RD-JN west, RD-01 south, RD-JS east.
$van = @('-PartyVanAt=40730,6000,-90', '-VanDrive', '-VanLoop=40730:-14700,38675:-14700,38675:8700,40730:8700')
$table = @{
    day         = @{ Map = $strip; Args = $van + @('-TimeOfDay=12', '-BenchWarmup=8', '-BenchSeconds=60') }
    night       = @{ Map = $strip; Args = $van + @('-TimeOfDay=22', '-BenchWarmup=8', '-BenchSeconds=60') }
    close       = @{ Map = $strip; Args = @('-TimeOfDay=12', '-BenchPath=street_close.txt', '-BenchSide=1000', '-BenchWarmup=8') }
    trees       = @{ Map = $strip; Args = @('-TimeOfDay=12', '-BenchPath=street_trees.txt', '-BenchSide=1000', '-BenchWarmup=8') }
    arena_group = @{ Map = $arena; Args = @('-ArenaAutoPlay', '-ArenaAttackers=3', '-BenchWarmup=8', '-BenchSeconds=60') }
    arena_kite  = @{ Map = $arena; Args = @('-ArenaStage=kite_ring', '-ArenaAutoPlay', '-BenchWarmup=8', '-BenchSeconds=60') }
    wave        = @{ Map = $strip; Args = @('-TimeOfDay=12', '-BenchPath=wave_eye.txt', '-BenchSide=1000', '-BenchWarmup=4', '-WorldWaveAt=36600,-12450,470,8') }
    cold        = @{ Map = $strip; Args = @('-TimeOfDay=12', '-BenchWarmup=0', '-BenchSeconds=20') }
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$order = Join-Path $Out 'run_order.txt'
$blockStart = $null
$blockRuns = 0
$lastSecs = 240
$exitCode = 0
:all foreach ($scene in $Scenes) {
    if (-not $table.ContainsKey($scene)) { "unknown scene $scene"; continue }
    $s = $table[$scene]
    foreach ($size in $Sizes) {
        $wh = $size.Split('x')
        for ($r = 1; $r -le $Runs; $r++) {
            $tag = if ($Label) { "${scene}_${Label}_${size}_r$r" } else { "${scene}_${size}_r$r" }
            $dir = Join-Path $Out $tag
            $machine = Join-Path $dir 'machine.txt'
            $sum = Join-Path $dir "$tag`_summary.txt"
            if ((Test-Path $sum) -and (Test-Path $machine) -and ((Get-Content $machine -TotalCount 1) -eq 'CLEAN')) {
                "$tag done, skipped"; continue
            }
            # Close the block when the next run would take it past its limit; open one when none is open.
            if ($blockStart -and ((Get-Date) - $blockStart).TotalSeconds + $lastSecs -gt $BlockMinutes * 60) {
                Write-Busy 'FREE' "bench block done ($blockRuns runs), nothing of this job running"
                $blockStart = $null
            }
            if (-not $blockStart) {
                if ($Shared) {
                    $busy = Get-Blockers
                    if ($busy.Count -gt 0) { "not opening a block: $($busy -join '; ')"; $exitCode = 3; break all }
                }
                Write-Busy 'BUSY' "bench block from $tag ($BlockMinutes min max)"
                $blockStart = Get-Date
                $blockRuns = 0
            }
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
            $log = Join-Path $dir 'game.log'
            $a = @("`"$Project`"", $s.Map, '-game', '-RenderOffScreen', "-ResX=$($wh[0])", "-ResY=$($wh[1])", "-Bench=$tag",
                "-BenchOut=`"$dir`"", '-unattended', '-nopause', '-nosplash', '-NoSound', "-abslog=`"$log`"") + $s.Args + $Extra
            $before = Get-MachineState
            $t0 = Get-Date
            $p = Start-Process $ue -ArgumentList $a -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $dir 'game.out')
            $null = $p.Handle
            $killed = $false
            if (-not $p.WaitForExit($Minutes * 60 * 1000)) { $p.Kill(); $killed = $true }
            $p.WaitForExit()
            $secs = [int]((Get-Date) - $t0).TotalSeconds
            $lastSecs = [math]::Max($lastSecs, $secs)
            $blockRuns++
            Start-Sleep -Seconds 5 # this run's own shader workers exit before the after-state is read
            $after = Get-MachineState
            $dirty = $before.Dirty -or $after.Dirty
            @($(if ($dirty) { 'CONTAMINATED' } else { 'CLEAN' }),
                "before: cpu $($before.Cpu)% heavy [$($before.Heavy)] top [$($before.Top)]",
                "after:  cpu $($after.Cpu)% heavy [$($after.Heavy)] top [$($after.Top)]",
                "args: $($a -join ' ')") | Set-Content -Encoding utf8 $machine
            $line = '{0} {1} pid {2} exit {3} in {4} s{5}{6}' -f (Get-Date -Format 'HH:mm:ss'), $tag, $p.Id, $p.ExitCode, $secs,
                $(if ($killed) { ' (time limit: stopped)' } else { '' }), $(if ($dirty) { ' CONTAMINATED' } else { '' })
            Add-Content -Encoding utf8 $order $line
            $line
            if (Test-Path $sum) { Get-Content $sum -TotalCount 1 }
        }
    }
}
if ($blockStart) { Write-Busy 'FREE' "bench block done ($blockRuns runs), nothing of this job running" }
exit $exitCode
