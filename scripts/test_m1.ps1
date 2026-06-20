param(
    [switch]$SkipBuild,
    [switch]$SkipEngineDownload
)

$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration all
}

$Outputs = @()
foreach ($Configuration in @("template_debug", "template_release")) {
    $Executable = Join-Path `
        $RepoRoot `
        "build\native-tests\test_wt_m1_cell_backend.$Configuration.x86_64.exe"
    if (-not (Test-Path -LiteralPath $Executable)) {
        throw "Missing M1 native test executable: $Executable"
    }

    $Output = & $Executable 2>&1
    $OutputText = $Output -join [Environment]::NewLine
    Write-Host $OutputText
    if ($LASTEXITCODE -ne 0) {
        throw "M1 native contract failed for $Configuration."
    }
    if ($OutputText -notmatch "M1_CELL_BACKEND_PASS") {
        throw "M1 native pass marker missing for $Configuration."
    }
    $HashLine = $Output | Where-Object { $_ -match "^M1_HASHES " }
    if ($null -eq $HashLine) {
        throw "M1 deterministic hash line missing for $Configuration."
    }
    $Outputs += $HashLine
}

if ($Outputs[0] -ne $Outputs[1]) {
    throw "M1 debug and release hashes differ: $($Outputs -join ' / ')"
}

$M0Arguments = @("-SkipBuild")
if ($SkipEngineDownload) {
    $M0Arguments += "-SkipEngineDownload"
}
& (Join-Path $PSScriptRoot "test_m0.ps1") @M0Arguments

Write-Host "M1 validation passed in debug/release with matching hashes and Godot compatibility."
