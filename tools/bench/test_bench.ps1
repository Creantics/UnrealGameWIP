# Tests for bench.ps1 that launch nothing (its -DryRun): run with  powershell -NoProfile -File tools\bench\test_bench.ps1
# Exit 0 when every check passes. Each check calls bench.ps1 through powershell -File, the way bench_local wrappers do.
$bench = Join-Path $PSScriptRoot 'bench.ps1'
$fails = 0
function Check([string]$Name, [bool]$Ok, [string]$Detail) {
    if ($Ok) { "PASS $Name" } else { "FAIL $Name : $Detail"; $script:fails++ }
}
function Bench([string[]]$BenchArgs) {
    # Any existing file stands in for the engine; -DryRun never starts it.
    $env:FT_UE_CMD = (Get-Command powershell).Source
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File $bench -Out (Join-Path $env:TEMP 'ft_bench_dry') -DryRun @BenchArgs 2>&1
    @($o | ForEach-Object { "$_" })
}

# 1. A comma list arriving as one string is split into its scenes and sizes.
$env:FT_BENCH_HEAVY = ''; $env:FT_BENCH_GAMES = ''
$o = Bench @('-Scenes', 'day,night,close', '-Sizes', '1920x1080,2560x1440', '-Runs', '1')
$plan = @($o | Where-Object { $_ -like 'plan *' })
Check 'comma lists split' ($plan.Count -eq 6 -and -not ($o -match 'unknown scene')) ($o -join ' / ')
Check 'tags in order' ($plan[0] -eq 'plan day_1920x1080_r1' -and $plan[5] -eq 'plan close_2560x1440_r1') ($plan -join ' / ')

# 2. A process list with exactly one running match is read without an error and marks the PC dirty.
$one = 'System'
$env:FT_BENCH_HEAVY = $one
$o = Bench @('-Scenes', 'day', '-Sizes', '1920x1080', '-Runs', '1')
$m = @($o | Where-Object { $_ -like 'machine *' })
Check 'one-process match' ($m.Count -eq 1 -and $m[0] -match "$one\(\d+\)" -and $m[0] -match 'dirty True' -and
    -not ($o -match 'op_Addition|Exception')) ($o -join ' / ')

# 3. Two lists with one match each still give both.
$env:FT_BENCH_GAMES = 'Idle'
$o = Bench @('-Scenes', 'day', '-Sizes', '1920x1080', '-Runs', '1')
$m = @($o | Where-Object { $_ -like 'machine *' })
Check 'one match in each list' ($m.Count -eq 1 -and $m[0] -match "$one\(\d+\)" -and $m[0] -match 'Idle\(\d+\)') ($o -join ' / ')

$env:FT_BENCH_HEAVY = ''; $env:FT_BENCH_GAMES = ''

# 4. The background sample reads a GPU figure and the limits decide the flag.
$o = Bench @('-Scenes', 'day', '-Sizes', '1920x1080', '-Runs', '1', '-MaxBgCpu', '100', '-MaxBgGpu', '100')
$m = @($o | Where-Object { $_ -like 'machine *' })
Check 'gpu sampled' ($m.Count -eq 1 -and $m[0] -match 'gpu \d+(\.\d)?% \(3d \d' -and $m[0] -match 'dirty False') ($o -join ' / ')
$o = Bench @('-Scenes', 'day', '-Sizes', '1920x1080', '-Runs', '1', '-MaxBgCpu', '-1')
Check 'over the cpu limit is dirty' ((@($o | Where-Object { $_ -like 'machine *' }) -join '') -match 'dirty True') ($o -join ' / ')

# 5 and 6 run the real loop against a stand-in engine that only writes the run's summary file.
$fake = Join-Path $env:TEMP 'ft_bench_fake'
Remove-Item -Recurse -Force $fake -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $fake | Out-Null
Set-Content -Encoding ascii (Join-Path $fake 'fake_ue.ps1') @'
$c = [Environment]::CommandLine
if ($c -match '-Bench=([^\s"]+)') { $tag = $Matches[1] }
if ($c -match '-BenchOut="?([^"]+?)"?(\s|$)') { $dir = $Matches[1] }
if ($tag -and $dir) { Set-Content -Encoding utf8 (Join-Path $dir "$tag`_summary.txt") "fake $tag" }
'@
Set-Content -Encoding ascii (Join-Path $fake 'fake_ue.cmd') "@powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0fake_ue.ps1`" %*"
function Loop([string]$OutDir, [string[]]$BenchArgs) {
    $env:FT_UE_CMD = Join-Path $fake 'fake_ue.cmd'
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File $bench -Out $OutDir -Scenes day -Sizes 1920x1080 -BaseSeconds 1 -MaxBaseCpu 100 @BenchArgs 2>&1
    [pscustomobject]@{ Out = @($o | ForEach-Object { "$_" }); Code = $LASTEXITCODE }
}

# 5. Contaminated runs wait for the end of the pass, are repeated once there (first try kept), and never again.
$d5 = Join-Path $fake 'repeat'
$x = Loop $d5 @('-Runs', '2', '-MaxBgCpu', '-100')
$lines = @(Get-Content (Join-Path $d5 'run_order.txt'))
Check 'dirty runs repeated once at the end' ($x.Code -eq 0 -and $lines.Count -eq 4 -and $lines[1] -match 'day_1920x1080_r2 ' -and
    $lines[2] -match 'day_1920x1080_r1 .*repeat' -and (Test-Path "$d5\day_1920x1080_r1_dirty1") -and
    (Test-Path "$d5\day_1920x1080_r2_dirty1")) (($x.Out + $lines) -join ' / ')
$x = Loop $d5 @('-Runs', '2', '-MaxBgCpu', '-100')
Check 'no second repeat' ($x.Code -eq 0 -and @(Get-Content (Join-Path $d5 'run_order.txt')).Count -eq 4 -and
    @($x.Out -match 'repeated once, still contaminated').Count -eq 2) ($x.Out -join ' / ')

# 6. -MaxBlocks stops at the block limit with exit 4, and the same command resumes after the clean runs.
$d6 = Join-Path $fake 'blocks'
$x = Loop $d6 @('-Runs', '2', '-MaxBgCpu', '100', '-MaxBgGpu', '100', '-BlockMinutes', '0', '-MaxBlocks', '1')
$n1 = @(Get-Content (Join-Path $d6 'run_order.txt')).Count
$y = Loop $d6 @('-Runs', '2', '-MaxBgCpu', '100', '-MaxBgGpu', '100', '-BlockMinutes', '0', '-MaxBlocks', '1')
Check 'block limit stops and resumes' ($x.Code -eq 4 -and $n1 -eq 1 -and $y.Code -eq 0 -and
    @(Get-Content (Join-Path $d6 'run_order.txt')).Count -eq 2 -and ($y.Out -match 'r1 done, skipped')) (($x.Out + $y.Out) -join ' / ')
Check 'baselines recorded' (@(Get-Content (Join-Path $d6 'baselines.txt')).Count -eq 2 -and
    ((Get-Content (Join-Path $d6 'baselines.txt')) -match 'idle baseline over 1 s cpu \d')) ((Get-Content (Join-Path $d6 'baselines.txt')) -join ' / ')

# 7. A block whose idle baseline is over -MaxBaseCpu does not open (exit 3) and says what is running.
$d7 = Join-Path $fake 'busy'
$env:FT_UE_CMD = Join-Path $fake 'fake_ue.cmd'
$o = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $bench -Out $d7 -Scenes day -Sizes 1920x1080 -Runs 1 -BaseSeconds 1 -MaxBaseCpu -1 2>&1 | ForEach-Object { "$_" })
Check 'busy baseline refuses the block' ($LASTEXITCODE -eq 3 -and ($o -match 'idle baseline cpu .* is over -1%; running: cpu') -and
    -not (Test-Path (Join-Path $d7 'day_1920x1080_r1')) -and ((Get-Content (Join-Path $d7 'baselines.txt')) -match 'REFUSED')) ($o -join ' / ')

# 8. The 3D engine decides the GPU flag.
$o = Bench @('-Scenes', 'day', '-Sizes', '1920x1080', '-Runs', '1', '-MaxBgCpu', '100', '-MaxBgGpu', '-1')
Check '3d engine over its limit is dirty' ((@($o | Where-Object { $_ -like 'machine *' }) -join '') -match 'dirty True') ($o -join ' / ')

# 9 and 10 run -Shared with a stand-in busy writer (called as the python would be: -I <script> BUSY|FREE <owner> <text>)
# that appends its line to a slot file and prints it, as the real one does.
$note = Join-Path $fake 'slots.txt'
Set-Content -Encoding ascii (Join-Path $fake 'fake_writer.ps1') @'
$l = "$($args[2]) $(Get-Date -Format 'yyyy-MM-dd HH:mm') - $($args[3]) $($args[4..($args.Count - 1)] -join ' ')"
Add-Content -Encoding utf8 $env:FT_BUSY_NOTE $l
$l
'@
Set-Content -Encoding ascii (Join-Path $fake 'fake_py.cmd') "@powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0fake_writer.ps1`" %*"
$env:FT_PYTHON = Join-Path $fake 'fake_py.cmd'; $env:FT_BUSY_WRITER = 'writer.py'; $env:FT_BUSY_NOTE = $note; $env:FT_BENCH_OWNER = 'tester'
function Shared([string]$OutDir, [string[]]$BenchArgs) {
    $env:FT_UE_CMD = Join-Path $fake 'fake_ue.cmd'
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File $bench -Out $OutDir -Scenes day -Sizes 1920x1080 -Shared -BaseSeconds 1 `
        -MaxBaseCpu 100 -MaxBgCpu 100 -MaxBgGpu 100 -BlockOn COMPILE @BenchArgs 2>&1
    [pscustomobject]@{ Out = @($o | ForEach-Object { "$_" }); Code = $LASTEXITCODE }
}

# 9. A writer that prints does not keep a block open past its limit.
Set-Content -Encoding utf8 $note ''
$d9 = Join-Path $fake 'writer'
$x = Shared $d9 @('-Runs', '2', '-BlockMinutes', '0', '-MaxBlocks', '1')
$slots = @(Get-Content $note | Where-Object { $_ })
Check 'printing writer still stops at the block limit' ($x.Code -eq 4 -and @(Get-Content (Join-Path $d9 'run_order.txt')).Count -eq 1 -and
    $slots.Count -eq 2 -and $slots[0] -like 'BUSY * - tester bench block*' -and $slots[1] -like 'FREE * - tester bench block done*') (($x.Out + $slots) -join ' / ')

# 10. -BlockOn COMPILE: another owner's open COMPILE line refuses the block, its UNREAL line does not; a later FREE opens it.
$now = Get-Date -Format 'yyyy-MM-dd HH:mm'
Set-Content -Encoding utf8 $note @("BUSY $now - other UNREAL slot: stills", "BUSY $now - other COMPILE slot: gate")
$d10 = Join-Path $fake 'blockon'
$x = Shared $d10 @('-Runs', '1')
Check 'open COMPILE line refuses' ($x.Code -eq 3 -and ($x.Out -match 'not opening a block: other COMPILE since') -and
    -not ($x.Out -match 'other UNREAL') -and -not (Test-Path (Join-Path $d10 'day_1920x1080_r1'))) ($x.Out -join ' / ')
Add-Content -Encoding utf8 $note "FREE $now - other COMPILE slot: GATE OK"
$x = Shared $d10 @('-Runs', '1')
Check 'freed COMPILE line opens' ($x.Code -eq 0 -and (Test-Path (Join-Path $d10 'day_1920x1080_r1\day_1920x1080_r1_summary.txt'))) ($x.Out -join ' / ')
# 11. -Lead puts -ArenaLead=<kit> on the arena scenes only; without it no scene carries it.
$o = Bench @('-Scenes', 'arena_group,arena_kite,day', '-Sizes', '1920x1080', '-Runs', '1', '-Lead', 'devin')
$ar = @($o | Where-Object { $_ -like 'args *' })
Check 'lead on arena scenes only' ($ar.Count -eq 3 -and $ar[0] -like 'args arena_group_* -ArenaLead=devin' -and
    $ar[1] -like 'args arena_kite_* -ArenaLead=devin' -and -not ($ar[2] -match 'ArenaLead')) ($ar -join ' / ')
$o = Bench @('-Scenes', 'arena_group', '-Runs', '1')
Check 'no lead by default' (-not ($o -match 'ArenaLead')) ($o -join ' / ')
$env:FT_PYTHON = ''; $env:FT_BUSY_WRITER = ''; $env:FT_BUSY_NOTE = ''; $env:FT_BENCH_OWNER = ''
Remove-Item -Recurse -Force $fake -ErrorAction SilentlyContinue
if ($fails) { "$fails FAILED"; exit 1 } else { 'all passed'; exit 0 }
