param(
    [switch]$Refresh
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$DownloadRoot = Join-Path $RepoRoot "references\downloaded"
$PaperRoot = Join-Path $DownloadRoot "papers"
$UpstreamRoot = Join-Path $DownloadRoot "upstream"

New-Item -ItemType Directory -Force -Path $PaperRoot | Out-Null
New-Item -ItemType Directory -Force -Path $UpstreamRoot | Out-Null

$Papers = @(
    @("https://transvoxel.org/Lengyel-VoxelTerrain.pdf", "Lengyel-VoxelTerrain.pdf"),
    @("https://www.cs.rice.edu/~jwarren/papers/dualcontour.pdf", "Dual-Contouring-Hermite-Data.pdf"),
    @("https://arxiv.org/pdf/2004.08475", "Wald-AMR-IsoSurface-2020.pdf"),
    @("https://arxiv.org/pdf/2409.06710", "McGrids-2024.pdf"),
    @("https://arxiv.org/pdf/2505.04590", "TetWeave-2025.pdf"),
    @("https://arxiv.org/pdf/2506.09579", "Power-Diagram-Adaptive-Isosurface-2025.pdf"),
    @("https://cybertron.cg.tu-berlin.de/projects/power-isosurfacing/power_isosurfacing.pdf", "Power-Isosurfacing-2025.pdf"),
    @("https://arxiv.org/pdf/2604.00157", "Dual-Contouring-Signed-Distance-2026.pdf")
)

foreach ($Paper in $Papers) {
    $Url = $Paper[0]
    $Target = Join-Path $PaperRoot $Paper[1]
    if ($Refresh -or -not (Test-Path -LiteralPath $Target)) {
        Write-Host "Downloading $Url"
        Invoke-WebRequest -Uri $Url -OutFile $Target
    }
}

function Sync-PinnedRepository {
    param(
        [string]$Url,
        [string]$Revision,
        [string]$DirectoryName
    )

    $Target = Join-Path $UpstreamRoot $DirectoryName
    if (-not (Test-Path -LiteralPath (Join-Path $Target ".git"))) {
        git clone --filter=blob:none --no-checkout $Url $Target
    }

    git -C $Target fetch --depth 1 origin $Revision
    git -C $Target checkout --detach $Revision

    $Actual = (git -C $Target rev-parse HEAD).Trim()
    if ($Actual -ne $Revision) {
        throw "Revision mismatch for ${DirectoryName}: expected $Revision, got $Actual"
    }
}

Sync-PinnedRepository `
    "https://github.com/EricLengyel/Transvoxel.git" `
    "51a494f03c5b024cd153b596bcc7152eb3cc93a6" `
    "Transvoxel"

Sync-PinnedRepository `
    "https://github.com/Zylann/godot_voxel.git" `
    "595f52ee4e23203a865eeb981f115909f7aa92f4" `
    "godot_voxel"

Sync-PinnedRepository `
    "https://github.com/ingowald/cudaAmrIsoSurfaceExtraction.git" `
    "e1074d9e78a7aa208a0969b80afe5b0d5a92d0b5" `
    "cudaAmrIsoSurfaceExtraction"

Sync-PinnedRepository `
    "https://github.com/zcblshz/McGrids.git" `
    "6c24f0968c82b00472130303a1a532b15626dbb1" `
    "McGrids"

Write-Host ""
Write-Host "Downloaded paper hashes:"
Get-ChildItem -LiteralPath $PaperRoot -File |
    Sort-Object Name |
    Get-FileHash -Algorithm SHA256 |
    Format-Table Hash, Path -AutoSize

Write-Host ""
Write-Host "Pinned repository revisions:"
Get-ChildItem -LiteralPath $UpstreamRoot -Directory |
    Sort-Object Name |
    ForEach-Object {
        $Revision = (git -C $_.FullName rev-parse HEAD).Trim()
        "{0} {1}" -f $_.Name, $Revision
    }
