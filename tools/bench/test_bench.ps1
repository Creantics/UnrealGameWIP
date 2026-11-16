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
if ($fails) { "$fails FAILED"; exit 1 } else { 'all passed'; exit 0 }
