# The performance benchmark runner: each scene x size x run as one standalone -game process with -Bench (see
# Source/ForgottenThird/FTBenchSubsystem.h), the CSV and summary of each in <Out>\<scene>[_<label>]_<size>_r<n>\. Runs in
# the order printed (scene, then size, then run), so run 1 of a set is the one after a cold driver shader cache. Only the
# process this script started is ever stopped (on its time limit).
#   powershell -File tools\bench\bench.ps1 -Out <dir> [-Scenes day,night] [-Sizes 1920x1080,2560x1440]
#       [-Runs 3] [-Extra '-dpcvars=r.GPUCsvStatsEnabled=1'] [-Label gpusplit] [-Minutes 6] [-Project <uproject>]
#       [-Shared] [-BlockMinutes 28] [-Lead devin]
# -Lead passes -ArenaLead=<kit> to the arena scenes, so their fights run on a lead's kit (its own cancel windows) and
# the arena checks in game.log mean something; without it a checkout with no playable PartyLooks fights on kit default.
# Off by default so runs stay comparable with the baselines taken without it.
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
# Each run's machine.txt holds the background load (a 5 s sample of total CPU % and GPU %, the GPU as its busiest engine,
# as Task Manager shows it, and its 3D engine alone, with the top GPU users) and the listed processes, just before launch
# and just after exit. Each block starts with a -BaseSeconds idle sample (median total CPU, written to baselines.txt); a
# block whose baseline is over -MaxBaseCpu does not open (exit 3, with what is running). A run whose background CPU is more
# than -MaxBgCpu points over its block's baseline, whose 3D engine is over -MaxBgGpu (video decode and other engines are
# recorded only), or with a FT_BENCH_HEAVY or FT_BENCH_GAMES process present, is CONTAMINATED. A contaminated run is repeated once at the end of the pass (its first try kept as <tag>_dirty1); a resumed
# pass leaves a contaminated run for its end too. -MaxBlocks stops after that many blocks (exit 4; the same command resumes).
# -BlockOn (a regex on the BUSY line's text) limits which other open BUSY lines keep a block from opening (default: any).
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
    [int]$BlockMinutes = 28,
    [string]$Lead = '',
    [int]$MaxBlocks = 0,
    [string]$BlockOn = '.',
    [double]$MaxBgCpu = 3,
    [double]$MaxBgGpu = 2,
    [double]$MaxBaseCpu = 15,
    [int]$BaseSeconds = 30,
    [switch]$DryRun # print one machine-state read and the run tags, launch nothing (tools/bench/test_bench.ps1)
)
$ErrorActionPreference = 'Continue'
# Through powershell -File (or a wrapper passing @args) "a,b" arrives as one string: split the lists here.
$Scenes = @($Scenes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Sizes = @($Sizes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
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

# The background load over 5 one-second samples: total CPU %, GPU % (per sample the busiest engine, summed over the
# processes using it, as Task Manager shows it), the 3D engine alone, and the top three GPU users. -1 where unread.
function Get-Background {
    $r = [pscustomobject]@{ Cpu = -1.0; Gpu = -1.0; Gpu3d = -1.0; GpuTop = '' }
    try {
        $c = Get-Counter -Counter @('\Processor(_Total)\% Processor Time', '\GPU Engine(*)\Utilization Percentage') `
            -SampleInterval 1 -MaxSamples 5 -ErrorAction SilentlyContinue
    } catch { $c = $null }
    if (-not $c) {
        # No GPU Engine counters: CPU alone, the GPU figures stay -1.
        try { $c = Get-Counter -Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 5 -ErrorAction SilentlyContinue } catch { return $r }
        if (-not $c) { return $r }
    }
    $cpus = @(); $gpus = @(); $g3 = @(); $byPid = @{}
    foreach ($s in $c) {
        $eng = @{}
        foreach ($x in $s.CounterSamples) {
            if ($x.Path -like '*\processor(_total)\*') { $cpus += $x.CookedValue; continue }
            if ($x.InstanceName -match '^pid_(\d+)_luid_(\w+)_phys_(\d+)_eng_(\d+)_engtype_(\w+)$') {
                $k = "$($Matches[2])/$($Matches[3])/$($Matches[4])/$($Matches[5])"
                $eng[$k] = [double]$eng[$k] + $x.CookedValue
                $pk = "$($Matches[1]) $($Matches[5])"
                $byPid[$pk] = [double]$byPid[$pk] + $x.CookedValue / $c.Count
            }
        }
        if (-not $eng.Count) { continue } # no GPU Engine samples: the GPU figures stay -1
        $gpus += [double](@($eng.Values) + 0 | Measure-Object -Maximum).Maximum
        $g3 += [double](@($eng.GetEnumerator() | Where-Object { $_.Key -like '*/3d' } | ForEach-Object { $_.Value }) + 0 |
                Measure-Object -Maximum).Maximum
    }
    if ($cpus.Count) { $r.Cpu = [math]::Round(($cpus | Measure-Object -Average).Average, 1) }
    if ($gpus.Count) { $r.Gpu = [math]::Round(($gpus | Measure-Object -Average).Average, 1) }
    if ($g3.Count) { $r.Gpu3d = [math]::Round(($g3 | Measure-Object -Average).Average, 1) }
    $r.GpuTop = (@($byPid.GetEnumerator() | Where-Object { $_.Value -ge 0.1 } | Sort-Object Value -Descending |
            Select-Object -First 3 | ForEach-Object {
                $id, $kind = $_.Key.Split(' ')
                $n = (Get-Process -Id ([int]$id) -ErrorAction SilentlyContinue).ProcessName
                '{0} {1} {2:N1}%' -f $(if ($n) { $n } else { "pid$id" }), $kind, $_.Value
            })) -join ', '
    $r
}

function Get-MachineState {
    $bg = Get-Background
    $cpu = $bg.Cpu
    # @() on each: a function's one-item result arrives as that item, not as an array.
    $heavy = @(@(Get-Names $env:FT_BENCH_HEAVY) + @(Get-Names $env:FT_BENCH_GAMES) | ForEach-Object { '{0}({1})' -f $_.ProcessName, $_.Id })
    $top = ''
    try {
        $n = [Environment]::ProcessorCount
        $top = ((Get-Counter '\Process(*)\% Processor Time' -ErrorAction SilentlyContinue).CounterSamples |
                Where-Object { $_.InstanceName -notin @('_total', 'idle') -and $_.CookedValue / $n -gt 2 } |
                Sort-Object CookedValue -Descending | Select-Object -First 5 |
                ForEach-Object { '{0} {1:N0}%' -f $_.InstanceName, ($_.CookedValue / $n) }) -join ', '
    } catch {}
    $tray = @(Get-Names $env:FT_BENCH_TRAY | ForEach-Object { $_.ProcessName } | Sort-Object -Unique)
    [pscustomobject]@{ Cpu = $cpu; Gpu = $bg.Gpu; Gpu3d = $bg.Gpu3d; GpuTop = $bg.GpuTop
        Heavy = (($heavy + @($tray | ForEach-Object { "$_(recorded)" })) -join ' '); Top = $top
        Dirty = ($cpu -gt $script:baseCpu + $MaxBgCpu -or $bg.Gpu3d -gt $MaxBgGpu -or $heavy.Count -gt 0) }
}
# The idle baseline of a block: median total CPU and median 3D engine over -BaseSeconds one-second samples.
function Get-Baseline {
    $r = [pscustomobject]@{ Cpu = -1.0; Gpu3d = -1.0 }
    try {
        $c = Get-Counter -Counter @('\Processor(_Total)\% Processor Time', '\GPU Engine(*)\Utilization Percentage') `
            -SampleInterval 1 -MaxSamples ([math]::Max(1, $BaseSeconds)) -ErrorAction SilentlyContinue
    } catch { $c = $null }
    if (-not $c) {
        # No GPU Engine counters on this PC: the CPU baseline alone, the 3D engine left unread (-1, never dirty).
        try { $c = Get-Counter -Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 `
                -MaxSamples ([math]::Max(1, $BaseSeconds)) -ErrorAction SilentlyContinue } catch { return $r }
        if (-not $c) { return $r }
    }
    function Median($v) {
        $s = @($v | Sort-Object)
        if (-not $s.Count) { return -1.0 }
        [math]::Round(($s[[int][math]::Floor(($s.Count - 1) / 2)] + $s[[int][math]::Ceiling(($s.Count - 1) / 2)]) / 2, 1)
    }
    $cpus = @(); $g3 = @()
    foreach ($s in $c) {
        $eng = @{}
        foreach ($x in $s.CounterSamples) {
            if ($x.Path -like '*\processor(_total)\*') { $cpus += $x.CookedValue; continue }
            if ($x.InstanceName -match '^pid_\d+_luid_(\w+)_phys_(\d+)_eng_(\d+)_engtype_3d$') {
                $k = "$($Matches[1])/$($Matches[2])/$($Matches[3])"
                $eng[$k] = [double]$eng[$k] + $x.CookedValue
            }
        }
        if ($eng.Count) { $g3 += [double](@($eng.Values) + 0 | Measure-Object -Maximum).Maximum }
    }
    $r.Cpu = Median $cpus
    $r.Gpu3d = Median $g3
    $r
}
function Format-State($st) {
    "cpu $($st.Cpu)% gpu $($st.Gpu)% (3d $($st.Gpu3d)%) heavy [$($st.Heavy)] top [$($st.Top)] gpu top [$($st.GpuTop)]"
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
                if ($Matches[1] -eq 'BUSY') {
                    $open[$key] = [pscustomobject]@{ T = [datetime]::ParseExact($Matches[2], 'yyyy-MM-dd HH:mm', $null); L = $l }
                }
                elseif ($open.ContainsKey($key)) { $open.Remove($key) }
                else { @($open.Keys | Where-Object { $_.StartsWith("$who ") }) | ForEach-Object { $open.Remove($_) } }
            }
        }
    }
    $now = Get-Date
    @($open.GetEnumerator() | Where-Object { -not $_.Key.StartsWith("$owner ") -and ($now - $_.Value.T).TotalMinutes -lt 90 -and
                $_.Value.L -match $BlockOn } |
            ForEach-Object { "$($_.Key) since $($_.Value.T.ToString('HH:mm'))" }) +
        @($(if ($hold) { "HOLD open: $($hold.Substring(0, [math]::Min(60, $hold.Length)))" })) +
        @($(if ($games.Count -gt 0) { "game running: $($games -join ' ')" })) | Where-Object { $_ }
}

function Write-Busy([string]$Kind, [string]$Text) {
    # The writer's own output goes to the host only: in Enter-Block any pipeline output would join its $true/$false.
    if ($Shared) { & $py -I $busyWriter $Kind $owner ($busyFormat -f $Text) 2>&1 | ForEach-Object { Write-Host "$_" } }
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
# A lead's kit on the arena scenes (-Lead).
if ($Lead) { foreach ($k in 'arena_group', 'arena_kite') { $table[$k].Args += @("-ArenaLead=$Lead") } }
# The runs in order (scene, then size, then run).
$plan = @(foreach ($scene in $Scenes) {
        if (-not $table.ContainsKey($scene)) { "unknown scene $scene" | Write-Host; continue }
        foreach ($size in $Sizes) { for ($r = 1; $r -le $Runs; $r++) {
                [pscustomobject]@{ Scene = $scene; Size = $size
                    Tag = $(if ($Label) { "${scene}_${Label}_${size}_r$r" } else { "${scene}_${size}_r$r" }) } } }
    })
# The open block's idle CPU baseline (0 until a block opens; -DryRun takes none).
$script:baseCpu = 0.0
$script:baseGpu3d = -1.0
if ($DryRun) {
    $st = Get-MachineState
    "machine $(Format-State $st) dirty $($st.Dirty)"
    $plan | ForEach-Object { "plan $($_.Tag)" }
    $plan | ForEach-Object { "args $($_.Tag) $($table[$_.Scene].Args -join ' ')" }
    exit 0
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$order = Join-Path $Out 'run_order.txt'
$script:blockStart = $null
$script:blockRuns = 0
$script:blocksDone = 0
$script:lastSecs = 240
$script:exitCode = 0

# CLEAN or CONTAMINATED for a run with a summary, else '' (never run, failed or stopped).
function Get-RunState([string]$Tag) {
    $dir = Join-Path $Out $Tag
    $machine = Join-Path $dir 'machine.txt'
    if ((Test-Path (Join-Path $dir "$Tag`_summary.txt")) -and (Test-Path $machine)) { "$(Get-Content $machine -TotalCount 1)".Trim() } else { '' }
}

# Close the block when the next run would take it past its limit; open one when none is open. $false: stop here.
function Enter-Block([string]$Tag) {
    if ($script:blockStart -and ((Get-Date) - $script:blockStart).TotalSeconds + $script:lastSecs -gt $BlockMinutes * 60) {
        Write-Busy 'FREE' "bench block done ($($script:blockRuns) runs), nothing of this job running"
        $script:blockStart = $null
        $script:blocksDone++
    }
    if ($script:blockStart) { return $true }
    if ($MaxBlocks -gt 0 -and $script:blocksDone -ge $MaxBlocks) {
        Write-Host "block limit ($MaxBlocks) reached before $Tag; the same command resumes"; $script:exitCode = 4; return $false
    }
    if ($Shared) {
        $busy = @(Get-Blockers)
        if ($busy.Count -gt 0) { Write-Host "not opening a block: $($busy -join '; ')"; $script:exitCode = 3; return $false }
    }
    $base = Get-Baseline
    $st = Get-MachineState
    $line = '{0} before {1}: idle baseline over {2} s cpu {3}% (3d engine {4}%); running: {5}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'),
        $Tag, $BaseSeconds, $base.Cpu, $base.Gpu3d, (Format-State $st)
    if ($base.Cpu -lt 0) {
        Add-Content -Encoding utf8 (Join-Path $Out 'baselines.txt') "$line; REFUSED (performance counters unreadable)"
        Write-Host 'not opening a block: the Processor(_Total) performance counter could not be read, so there is no idle baseline'
        $script:exitCode = 3; return $false
    }
    if ($base.Cpu -gt $MaxBaseCpu) {
        Add-Content -Encoding utf8 (Join-Path $Out 'baselines.txt') "$line; REFUSED (limit $MaxBaseCpu%)"
        Write-Host "not opening a block: idle baseline cpu $($base.Cpu)% is over $MaxBaseCpu%; running: $(Format-State $st)"
        $script:exitCode = 3; return $false
    }
    if ($base.Gpu3d -lt 0) { Write-Host 'note: no GPU Engine counters; the 3D engine is not read and cannot mark a run' }
    Add-Content -Encoding utf8 (Join-Path $Out 'baselines.txt') $line
    $script:baseCpu = $base.Cpu
    $script:baseGpu3d = $base.Gpu3d
    Write-Busy 'BUSY' "bench block from $Tag ($BlockMinutes min max)"
    $script:blockStart = Get-Date
    $script:blockRuns = 0
    $true
}

# One run; returns $true when it was contaminated.
function Invoke-Run($Item, [string]$Note) {
    $s = $table[$Item.Scene]
    $wh = $Item.Size.Split('x')
    $tag = $Item.Tag
    $dir = Join-Path $Out $tag
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $log = Join-Path $dir 'game.log'
    $a = @("`"$Project`"", $s.Map, '-game', '-NoDemoBoot', '-RenderOffScreen', "-ResX=$($wh[0])", "-ResY=$($wh[1])", "-Bench=$tag",
        "-BenchOut=`"$dir`"", '-unattended', '-nopause', '-nosplash', '-NoSound', "-abslog=`"$log`"") + $s.Args + $Extra
    $before = Get-MachineState
    $t0 = Get-Date
    $p = Start-Process $ue -ArgumentList $a -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $dir 'game.out')
    $null = $p.Handle
    $killed = $false
    if (-not $p.WaitForExit($Minutes * 60 * 1000)) { $p.Kill(); $killed = $true }
    $p.WaitForExit()
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    $script:lastSecs = [math]::Max($script:lastSecs, $secs + 20)
    $script:blockRuns++
    Start-Sleep -Seconds 5 # this run's own shader workers exit before the after-state is read
    $after = Get-MachineState
    $dirty = $before.Dirty -or $after.Dirty
    @($(if ($dirty) { 'CONTAMINATED' } else { 'CLEAN' }),
        "limits: background cpu over $($script:baseCpu + $MaxBgCpu)% (block baseline $($script:baseCpu)% + $MaxBgCpu), 3d engine over $MaxBgGpu% (baseline $($script:baseGpu3d)%)$Note",
        "before: $(Format-State $before)",
        "after:  $(Format-State $after)",
        "args: $($a -join ' ')") | Set-Content -Encoding utf8 (Join-Path $dir 'machine.txt')
    $line = '{0} {1} pid {2} exit {3} in {4} s bg cpu {5}/{6}% (base {12}) 3d {7}/{8}%{9}{10}{11}' -f (Get-Date -Format 'HH:mm:ss'), $tag,
        $p.Id, $p.ExitCode, $secs, $before.Cpu, $after.Cpu, $before.Gpu3d, $after.Gpu3d,
        $(if ($killed) { ' (time limit: stopped)' } else { '' }), $(if ($dirty) { ' CONTAMINATED' } else { '' }), $Note, $script:baseCpu
    Add-Content -Encoding utf8 $order $line
    $line | Write-Host
    $sum = Join-Path $dir "$tag`_summary.txt"
    if (Test-Path $sum) { Get-Content $sum -TotalCount 1 | Write-Host }
    $dirty
}

# Main pass: a contaminated run (new, or found from an earlier pass) waits for the end; it is repeated once there, its
# first try kept as <tag>_dirty1. A run that already has a _dirty1 is not repeated again.
$deferred = @()
foreach ($it in $plan) {
    $state = Get-RunState $it.Tag
    $tried = Test-Path (Join-Path $Out "$($it.Tag)_dirty1")
    if ($state -eq 'CLEAN') { "$($it.Tag) done, skipped"; continue }
    if ($state -eq 'CONTAMINATED') {
        if ($tried) { "$($it.Tag) repeated once, still contaminated: kept" } else { $deferred += $it }
        continue
    }
    if (-not (Enter-Block $it.Tag)) { break }
    if ((Invoke-Run $it '') -and -not $tried) { $deferred += $it }
}
if ($script:exitCode -eq 0 -and $deferred.Count -gt 0) {
    "repeating $($deferred.Count) contaminated run(s) once: $(($deferred | ForEach-Object { $_.Tag }) -join ' ')"
    foreach ($it in $deferred) {
        if (-not (Enter-Block $it.Tag)) { break }
        $dir = Join-Path $Out $it.Tag
        Move-Item -LiteralPath $dir -Destination "$dir`_dirty1"
        $null = Invoke-Run $it ' (repeat of a contaminated run)'
    }
}
if ($script:blockStart) { Write-Busy 'FREE' "bench block done ($($script:blockRuns) runs), nothing of this job running" }
exit $script:exitCode
