param(
    [ValidateSet("debug", "release", "all")]
    [string]$Configuration = "all",
    [int]$Jobs = 0,
    [switch]$SkipBootstrap
)

$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))

if (-not $SkipBootstrap) {
    & (Join-Path $PSScriptRoot "bootstrap_toolchain.ps1")
}

if ($Jobs -le 0) {
    $Jobs = [Math]::Max(1, [Environment]::ProcessorCount)
}

$Targets = switch ($Configuration) {
    "debug" { @("template_debug") }
    "release" { @("template_release") }
    "all" { @("template_debug", "template_release") }
}

foreach ($Target in $Targets) {
    Write-Host "Building World Transvoxel $Target with Zig 0.16.0"
    & python -m SCons `
        -Q `
        "-j$Jobs" `
        "target=$Target" `
        "platform=windows" `
        "arch=x86_64" `
        "build_profile=build_profiles/m0.json"
    if ($LASTEXITCODE -ne 0) {
        throw "Native build failed for $Target."
    }
}
