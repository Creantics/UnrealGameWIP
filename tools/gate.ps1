# The local gate: Core syntax check (clang, warnings as errors), CMake build + ctest, then with -Ubt
# the UBT editor build. Works from any worktree. Prints GATE OK on success.
param([switch]$Ubt)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$cm = 'C:\Program Files\CMake\bin'
$log = Join-Path $env:TEMP ("ft_ubt_" + (Split-Path -Leaf $repo) + ".log")

$clang = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang++.exe'
$srcs = Get-ChildItem "$repo\Core\src\*.cpp" | % FullName
& $clang -std=c++20 -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow -Werror -fno-exceptions -fno-rtti -D_HAS_EXCEPTIONS=0 "-I$repo\Core\include" @srcs 2>&1 | Select-Object -First 30
if ($LASTEXITCODE -ne 0) { "CLANG FAILED"; exit 1 }
"clang ok ($($srcs.Count) sources)"

# newengine sits beside the main checkout, not beside a worktree, so resolve it from git's common dir.
$main = Split-Path -Parent (git -C $repo rev-parse --path-format=absolute --git-common-dir)
$assets = Join-Path (Split-Path -Parent $main) 'newengine\assets'
& "$cm\cmake.exe" -S "$repo\Core" -B "$repo\Core\build" -DFTCORE_WERROR=ON "-DFTCORE_NEWENGINE_ASSETS=$assets" 2>&1 | Select-String 'Error|error' | Select-Object -First 20
& "$cm\cmake.exe" --build "$repo\Core\build" --config Debug 2>&1 | Select-String 'warning|error' | Select-Object -First 40
if ($LASTEXITCODE -ne 0) { "CMAKE BUILD FAILED"; exit 1 }
& "$cm\ctest.exe" --test-dir "$repo\Core\build" -C Debug --output-on-failure 2>&1 | Select-String 'Passed|Failed|tests passed|Error|missing|wrapper|reserved|:\d+:'
if ($LASTEXITCODE -ne 0) { "CTEST FAILED"; exit 1 }
& "$repo\Core\build\Debug\ftcore_tests.exe" 2>&1 | Select-String 'test cases|assertions'
if (Test-Path "$assets\story\story.json") { "real-script tests ran ($assets\story)" }
else { "real-script tests SKIPPED (no $assets\story\story.json)" }

if ($Ubt) {
    # Only an editor on this checkout's project locks its binaries; other worktrees' editors are fine.
    $uproject = "$repo\ForgottenThird.uproject"
    $running = Get-CimInstance Win32_Process -Filter "Name LIKE 'UnrealEditor%'" |
        Where-Object { $_.CommandLine -and $_.CommandLine.Replace('/', '\').ToLower().Contains($uproject.ToLower()) } |
        Select-Object @{ n = 'Id'; e = { $_.ProcessId } }
    if ($running) { "UNREAL EDITOR RUNNING (PID $($running.Id -join ',')); skipping UBT"; exit 2 }
    & 'E:\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat' ForgottenThirdEditor Win64 Development "-Project=$repo\ForgottenThird.uproject" -WaitMutex -NoHotReloadFromIDE *> $log
    $code = $LASTEXITCODE
    Get-Content $log | Select-String 'error|warning|Result:'
    if ($code -ne 0) { "UBT FAILED ($code)"; exit 1 }
}
"GATE OK"
