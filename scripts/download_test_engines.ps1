param(
    [switch]$Refresh
)

$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$ToolRoot = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot ".tools"))
$DownloadRoot = Join-Path $ToolRoot "downloads"
$EngineRoot = [System.IO.Path]::GetFullPath((Join-Path $ToolRoot "godot"))

if (-not $EngineRoot.StartsWith(
    $ToolRoot,
    [System.StringComparison]::OrdinalIgnoreCase
)) {
    throw "Resolved Godot path escaped the repository tool directory."
}

$Engines = @(
    @{
        Version = "4.6.3"
        Archive = "Godot_v4.6.3-stable_win64.exe.zip"
        Executable = "Godot_v4.6.3-stable_win64.exe"
        Url = "https://github.com/godotengine/godot/releases/download/4.6.3-stable/Godot_v4.6.3-stable_win64.exe.zip"
        Sha256 = "e39986a178d585ce7ac198fb8de6ea436366dc0cc00e594810c2e3e104c04b90"
    },
    @{
        Version = "4.7"
        Archive = "Godot_v4.7-stable_win64.exe.zip"
        Executable = "Godot_v4.7-stable_win64.exe"
        Url = "https://github.com/godotengine/godot/releases/download/4.7-stable/Godot_v4.7-stable_win64.exe.zip"
        Sha256 = "02a5312236f4e0209c78bcb2f52135b1963e6b8888c873c9cee81459e60bcd71"
    }
)

New-Item -ItemType Directory -Force -Path $DownloadRoot | Out-Null
New-Item -ItemType Directory -Force -Path $EngineRoot | Out-Null

function Get-GodotVersion {
    param(
        [string]$ExecutablePath,
        [string]$Version
    )

    $LogRoot = Join-Path $ToolRoot "godot-version-logs"
    New-Item -ItemType Directory -Force -Path $LogRoot | Out-Null
    $StdoutPath = Join-Path $LogRoot "$Version.stdout.txt"
    $StderrPath = Join-Path $LogRoot "$Version.stderr.txt"

    $Process = Start-Process `
        -FilePath $ExecutablePath `
        -ArgumentList @("--headless", "--version") `
        -Wait `
        -PassThru `
        -NoNewWindow `
        -RedirectStandardOutput $StdoutPath `
        -RedirectStandardError $StderrPath

    if ($Process.ExitCode -ne 0) {
        $ErrorOutput = Get-Content -Raw -LiteralPath $StderrPath
        throw "Godot $Version version check failed: $ErrorOutput"
    }

    return (Get-Content -Raw -LiteralPath $StdoutPath).Trim()
}

foreach ($Engine in $Engines) {
    $ArchivePath = Join-Path $DownloadRoot $Engine.Archive
    $VersionRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $EngineRoot $Engine.Version)
    )
    $ExecutablePath = Join-Path $VersionRoot $Engine.Executable

    if ($Refresh -or -not (Test-Path -LiteralPath $ArchivePath)) {
        Invoke-WebRequest -Uri $Engine.Url -OutFile $ArchivePath
    }

    $ActualHash = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $ArchivePath
    ).Hash.ToLowerInvariant()
    if ($ActualHash -ne $Engine.Sha256) {
        throw (
            "Godot $($Engine.Version) archive hash mismatch: " +
            "expected $($Engine.Sha256), got $ActualHash"
        )
    }

    if ($Refresh -or -not (Test-Path -LiteralPath $ExecutablePath)) {
        if (-not $VersionRoot.StartsWith(
            $EngineRoot,
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
            throw "Resolved engine version path escaped the Godot tool directory."
        }
        if (Test-Path -LiteralPath $VersionRoot) {
            Remove-Item -LiteralPath $VersionRoot -Recurse -Force
        }
        New-Item -ItemType Directory -Force -Path $VersionRoot | Out-Null
        Expand-Archive `
            -LiteralPath $ArchivePath `
            -DestinationPath $VersionRoot `
            -Force
    }

    $ActualVersion = Get-GodotVersion $ExecutablePath $Engine.Version
    Write-Host "Godot $($Engine.Version): $ActualVersion"
}
