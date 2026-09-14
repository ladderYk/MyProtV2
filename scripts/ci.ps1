# scripts/ci.ps1 -- build gate + full regression run (see ADR-0013).
#
# What it does:
#   1. Build Release x64 via scripts\build.bat (reuses the same MSBuild discovery)
#   2. Run every executable test (4 unit test exes + MyProt.E2E)
#   3. Print a summary table and exit with the number of failed steps
#
# Exit codes:  0 = all green, non-zero = failed step count (CI-friendly contract).
#
# Usage (any runner / scheduled task / manual):
#   powershell -ExecutionPolicy Bypass -File scripts\ci.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\ci.ps1 -Config Debug -Platform Win32
#   powershell -ExecutionPolicy Bypass -File scripts\ci.ps1 -SkipBuild
#
# NOTE: file is intentionally ASCII-only (no BOM) -- keeps console output
#       identical across Windows PowerShell 5.1 and PowerShell 7.

param(
    [string]$Config = "Release",
    [string]$Platform = "x64",
    [int]$UnitTimeoutSec = 60,
    [int]$E2ETimeoutSec = 300,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Continue"

$root = Split-Path -Parent $PSScriptRoot
$binDir = Join-Path $root ("build\{0}\{1}\bin" -f $Config, $Platform)
$results = New-Object System.Collections.ArrayList

function Write-Head([string]$text) {
    Write-Host ""
    Write-Host "============================================================"
    Write-Host " $text"
    Write-Host "============================================================"
}

# ── run one executable with a hard timeout; return a result record ──
function Invoke-Exe {
    param([string]$Name, [string]$Path, [int]$TimeoutSec)

    if (-not (Test-Path $Path)) {
        Write-Host ("[MISS] {0} -- not found: {1}" -f $Name, $Path) -ForegroundColor Red
        [void]$results.Add([pscustomobject]@{ Name = $Name; Result = "MISSING"; Secs = 0; Fail = 1 })
        return
    }

    Write-Host ("--- {0}" -f $Name)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Path
    # 工作目录 = 仓库根: E2E 与单元测试以相对路径读写配置 (configs/...、configs_*_test/),
    # 若从 bin 目录启动会读不到文件 (曾导致 2 个 E2E 用例误判失败)。
    $psi.WorkingDirectory = $root
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $proc = [System.Diagnostics.Process]::Start($psi)
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    $finished = $proc.WaitForExit($TimeoutSec * 1000)

    if (-not $finished) {
        try { $proc.Kill() } catch { }
        $sw.Stop()
        Write-Host ("[TIME] {0} exceeded {1}s -- killed" -f $Name, $TimeoutSec) -ForegroundColor Red
        [void]$results.Add([pscustomobject]@{ Name = $Name; Result = "TIMEOUT"; Secs = [int]$sw.Elapsed.TotalSeconds; Fail = 1 })
        return
    }

    $exitCode = $proc.ExitCode
    $stdOut = $outTask.Result
    $stdErr = $errTask.Result
    $sw.Stop()

    # always show failure lines and the tail (assertion/case summary)
    $lines = @($stdOut -split "`r?`n" | Where-Object { $_.Trim() -ne "" })
    $failLines = @($lines | Where-Object { $_ -match '\[FAIL\]' -or $_ -match 'FAILED' })
    if ($failLines.Count -gt 0) {
        $failLines | Select-Object -First 20 | ForEach-Object { Write-Host ("   " + $_) -ForegroundColor Red }
    }
    if ($lines.Count -gt 0) {
        $lines | Select-Object -Last 2 | ForEach-Object { Write-Host ("   " + $_) }
    }
    if ($stdErr.Trim() -ne "") {
        ($stdErr -split "`r?`n" | Where-Object { $_.Trim() -ne "" } | Select-Object -First 10) |
            ForEach-Object { Write-Host ("   stderr: " + $_) -ForegroundColor Red }
    }

    $ok = ($exitCode -eq 0)
    Write-Host ("{0} {1}  ({2}s, exit={3})" -f ($(if ($ok) { "  O K " } else { " FAIL " })), $Name, [int]$sw.Elapsed.TotalSeconds, $exitCode) `
        -ForegroundColor $(if ($ok) { "Green" } else { "Red" })
    [void]$results.Add([pscustomobject]@{
        Name   = $Name
        Result = $(if ($ok) { "PASS" } else { "FAIL" })
        Secs   = [int]$sw.Elapsed.TotalSeconds
        Fail   = $(if ($ok) { 0 } else { 1 })
    })
}

# ── 1. build ──
if (-not $SkipBuild) {
    Write-Head ("BUILD {0} | {1}" -f $Config, $Platform)
    $buildBat = Join-Path $PSScriptRoot "build.bat"
    & cmd /c "`"$buildBat`" $Config $Platform"
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("build failed (exit={0}) -- stopping gate" -f $LASTEXITCODE) -ForegroundColor Red
        exit 1
    }
} else {
    Write-Head "BUILD SKIPPED (-SkipBuild)"
}

# ── 2. run all executables ──
Write-Head "REGRESSION"

$unitTests = @(
    "MyProt.Core.Tests",
    "MyProt.Engine.Tests",
    "MyProt.Transport.Tests",
    "MyProt.Service.Tests"
)
foreach ($t in $unitTests) {
    Invoke-Exe -Name $t -Path (Join-Path $binDir ("tests\{0}.exe" -f $t)) -TimeoutSec $UnitTimeoutSec
}

Invoke-Exe -Name "MyProt.E2E" -Path (Join-Path $binDir "MyProt.E2E.exe") -TimeoutSec $E2ETimeoutSec

# ── 3. summary ──
Write-Head "SUMMARY"
$failed = 0
foreach ($r in $results) {
    if ($r.Fail -ne 0) { $failed++ }
    "{0,-8} {1,-26} {2,4}s" -f $r.Result, $r.Name, $r.Secs | Write-Host
}

Write-Host ""
if ($failed -eq 0) {
    Write-Host "CI GATE PASSED ($($results.Count)/$($results.Count) executables green)" -ForegroundColor Green
} else {
    Write-Host ("CI GATE FAILED: {0} of {1} executables failed" -f $failed, $results.Count) -ForegroundColor Red
}
exit $failed
