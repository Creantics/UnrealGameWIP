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
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File $bench -Out $OutDir -Scenes day -Sizes 1920x1080 @BenchArgs 2>&1
    [pscustomobject]@{ Out = @($o | ForEach-Object { "$_" }); Code = $LASTEXITCODE }
}

# 5. Contaminated runs wait for the end of the pass, are repeated once there (first try kept), and never again.
$d5 = Join-Path $fake 'repeat'
$x = Loop $d5 @('-Runs', '2', '-MaxBgCpu', '-1')
$lines = @(Get-Content (Join-Path $d5 'run_order.txt'))
Check 'dirty runs repeated once at the end' ($x.Code -eq 0 -and $lines.Count -eq 4 -and $lines[1] -match 'day_1920x1080_r2 ' -and
    $lines[2] -match 'day_1920x1080_r1 .*repeat' -and (Test-Path "$d5\day_1920x1080_r1_dirty1") -and
    (Test-Path "$d5\day_1920x1080_r2_dirty1")) (($x.Out + $lines) -join ' / ')
$x = Loop $d5 @('-Runs', '2', '-MaxBgCpu', '-1')
Check 'no second repeat' ($x.Code -eq 0 -and @(Get-Content (Join-Path $d5 'run_order.txt')).Count -eq 4 -and
    @($x.Out -match 'repeated once, still contaminated').Count -eq 2) ($x.Out -join ' / ')

# 6. -MaxBlocks stops at the block limit with exit 4, and the same command resumes after the clean runs.
$d6 = Join-Path $fake 'blocks'
$x = Loop $d6 @('-Runs', '2', '-MaxBgCpu', '100', '-MaxBgGpu', '100', '-BlockMinutes', '0', '-MaxBlocks', '1')
$n1 = @(Get-Content (Join-Path $d6 'run_order.txt')).Count
$y = Loop $d6 @('-Runs', '2', '-MaxBgCpu', '100', '-MaxBgGpu', '100', '-BlockMinutes', '0', '-MaxBlocks', '1')
Check 'block limit stops and resumes' ($x.Code -eq 4 -and $n1 -eq 1 -and $y.Code -eq 0 -and
    @(Get-Content (Join-Path $d6 'run_order.txt')).Count -eq 2 -and ($y.Out -match 'r1 done, skipped')) (($x.Out + $y.Out) -join ' / ')
Remove-Item -Recurse -Force $fake -ErrorAction SilentlyContinue
if ($fails) { "$fails FAILED"; exit 1 } else { 'all passed'; exit 0 }
