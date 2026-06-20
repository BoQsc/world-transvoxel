param(
    [switch]$SkipBuild,
    [switch]$SkipEngineDownload
)

$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$ArtifactRoot = Join-Path $RepoRoot "artifacts\m0"
New-Item -ItemType Directory -Force -Path $ArtifactRoot | Out-Null

function Invoke-GodotTest {
    param(
        [string]$Engine,
        [string[]]$Arguments,
        [string]$Name
    )

    $StdoutPath = Join-Path $ArtifactRoot "$Name.stdout.txt"
    $StderrPath = Join-Path $ArtifactRoot "$Name.stderr.txt"
    $Process = Start-Process `
        -FilePath $Engine `
        -ArgumentList $Arguments `
        -Wait `
        -PassThru `
        -NoNewWindow `
        -RedirectStandardOutput $StdoutPath `
        -RedirectStandardError $StderrPath

    $Output = ""
    if (Test-Path -LiteralPath $StdoutPath) {
        $Output += Get-Content -Raw -LiteralPath $StdoutPath
    }
    if (Test-Path -LiteralPath $StderrPath) {
        $Output += Get-Content -Raw -LiteralPath $StderrPath
    }
    Write-Host $Output

    if ($Process.ExitCode -ne 0) {
        throw "Godot command $Name failed with exit code $($Process.ExitCode)."
    }

    return $Output
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration all
}

& python (Join-Path $RepoRoot "tools\validate_repository.py") --require-binaries
if ($LASTEXITCODE -ne 0) {
    throw "Repository and M0 validation failed."
}

if (-not $SkipEngineDownload) {
    & (Join-Path $PSScriptRoot "download_test_engines.ps1")
}

$Engines = @(
    (Join-Path $RepoRoot ".tools\godot\4.6.3\Godot_v4.6.3-stable_win64.exe"),
    (Join-Path $RepoRoot ".tools\godot\4.7\Godot_v4.7-stable_win64.exe")
)

foreach ($Engine in $Engines) {
    if (-not (Test-Path -LiteralPath $Engine)) {
        throw "Required test engine is missing: $Engine"
    }

    $VersionName = Split-Path (Split-Path $Engine -Parent) -Leaf
    $VersionOutput = Invoke-GodotTest `
        $Engine `
        @("--headless", "--version") `
        "$VersionName-version"
    $Version = $VersionOutput.Trim()
    Write-Host "Load-testing with Godot $Version"

    Invoke-GodotTest `
        $Engine `
        @("--headless", "--editor", "--path", $RepoRoot, "--quit") `
        "$VersionName-editor-load" | Out-Null

    $RuntimeOutput = Invoke-GodotTest `
        $Engine `
        @(
            "--headless",
            "--path",
            $RepoRoot,
            "--script",
            "res://tests/godot/addon_load_test.gd"
        ) `
        "$VersionName-runtime-load"
    if ($RuntimeOutput -notmatch "ADDON_LOAD_TEST_PASS") {
        throw "Addon runtime load test did not print its pass marker with $Version."
    }
}

$DebugBinary = Join-Path `
    $RepoRoot `
    "addons\world_transvoxel\bin\world_transvoxel.windows.template_debug.x86_64.dll"
$ReleaseBinary = Join-Path `
    $RepoRoot `
    "addons\world_transvoxel\bin\world_transvoxel.windows.template_release.x86_64.dll"
$DebugBackup = Join-Path $ArtifactRoot "template_debug.backup.dll"

Copy-Item -LiteralPath $DebugBinary -Destination $DebugBackup -Force
try {
    Copy-Item -LiteralPath $ReleaseBinary -Destination $DebugBinary -Force

    foreach ($Engine in $Engines) {
        $VersionName = Split-Path (Split-Path $Engine -Parent) -Leaf
        $ReleaseOutput = Invoke-GodotTest `
            $Engine `
            @(
                "--headless",
                "--path",
                $RepoRoot,
                "--script",
                "res://tests/godot/addon_load_test.gd"
            ) `
            "$VersionName-template-release-load"
        if ($ReleaseOutput -notmatch "ADDON_LOAD_TEST_PASS") {
            throw (
                "Addon template_release load test did not print its pass marker " +
                "with Godot $VersionName."
            )
        }
    }
}
finally {
    Copy-Item -LiteralPath $DebugBackup -Destination $DebugBinary -Force
}

Write-Host "M0 validation passed on all configured engines."
