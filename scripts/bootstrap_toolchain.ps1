param(
    [switch]$Refresh
)

$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$ToolRoot = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot ".tools"))
$DownloadRoot = Join-Path $ToolRoot "downloads"
$ZigRoot = [System.IO.Path]::GetFullPath((Join-Path $ToolRoot "zig"))
$ZigArchive = Join-Path $DownloadRoot "zig-x86_64-windows-0.16.0.zip"
$ZigUrl = "https://ziglang.org/download/0.16.0/zig-x86_64-windows-0.16.0.zip"
$ZigSha256 = "68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e"
$GodotCppRevision = "e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77"
$GodotCppRoot = Join-Path $RepoRoot "thirdparty\godot-cpp"

if (-not $ZigRoot.StartsWith($ToolRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Resolved Zig path escaped the repository tool directory."
}

New-Item -ItemType Directory -Force -Path $DownloadRoot | Out-Null

if ($Refresh -or -not (Test-Path -LiteralPath $ZigArchive)) {
    Invoke-WebRequest -Uri $ZigUrl -OutFile $ZigArchive
}

$ActualArchiveHash = (
    Get-FileHash -Algorithm SHA256 -LiteralPath $ZigArchive
).Hash.ToLowerInvariant()
if ($ActualArchiveHash -ne $ZigSha256) {
    throw "Zig archive hash mismatch: expected $ZigSha256, got $ActualArchiveHash"
}

$ZigExecutable = Join-Path $ZigRoot "zig.exe"
if ($Refresh -or -not (Test-Path -LiteralPath $ZigExecutable)) {
    $StageRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $ToolRoot "zig-extract-stage")
    )
    if (-not $StageRoot.StartsWith(
        $ToolRoot,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw "Resolved extraction path escaped the repository tool directory."
    }

    if (Test-Path -LiteralPath $StageRoot) {
        Remove-Item -LiteralPath $StageRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $ZigRoot) {
        Remove-Item -LiteralPath $ZigRoot -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $StageRoot | Out-Null
    Expand-Archive -LiteralPath $ZigArchive -DestinationPath $StageRoot -Force
    Move-Item `
        -LiteralPath (Join-Path $StageRoot "zig-x86_64-windows-0.16.0") `
        -Destination $ZigRoot
    Remove-Item -LiteralPath $StageRoot -Recurse -Force
}

$ActualZigVersion = (& $ZigExecutable version).Trim()
if ($ActualZigVersion -ne "0.16.0") {
    throw "Expected Zig 0.16.0, got $ActualZigVersion"
}

if (-not (Test-Path -LiteralPath (Join-Path $GodotCppRoot ".git"))) {
    git -C $RepoRoot submodule update --init --recursive thirdparty/godot-cpp
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize godot-cpp."
    }
}

$ActualGodotCppRevision = (
    git -C $GodotCppRoot rev-parse HEAD
).Trim()
if ($LASTEXITCODE -ne 0 -or $ActualGodotCppRevision -ne $GodotCppRevision) {
    throw (
        "godot-cpp revision mismatch: expected $GodotCppRevision, " +
        "got $ActualGodotCppRevision"
    )
}

Write-Host "Toolchain ready:"
Write-Host "  Zig       $ActualZigVersion"
Write-Host "  godot-cpp $ActualGodotCppRevision"
