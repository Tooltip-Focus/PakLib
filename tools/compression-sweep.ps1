[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Source,

    [string]$PakTool = (Join-Path $PSScriptRoot '..\build\windows-static\Release\paktool.exe'),

    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build\compression-sweep'),

    [string[]]$BlockSizes = @('256K', '1M', '4M', '16M', '64M'),

    [ValidateRange(1, 22)]
    [int[]]$Levels = @(3, 9, 15, 19),

    [ValidateRange(0, 99)]
    [int[]]$MinimumSavings = @(0, 2)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$sourcePath = [System.IO.Path]::GetFullPath($Source)
$pakToolPath = [System.IO.Path]::GetFullPath($PakTool)
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)

if (-not (Test-Path -LiteralPath $sourcePath -PathType Container)) {
    throw "Source directory does not exist: $sourcePath"
}
if (-not (Test-Path -LiteralPath $pakToolPath -PathType Leaf)) {
    throw "paktool does not exist: $pakToolPath"
}
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$results = foreach ($blockSize in $BlockSizes) {
    foreach ($level in $Levels) {
        foreach ($minimumSaving in $MinimumSavings) {
            $safeBlockSize = $blockSize -replace '[^a-zA-Z0-9]', '_'
            $name = 'pak-b{0}-l{1}-m{2}.pak' -f `
                $safeBlockSize, $level, $minimumSaving
            $output = Join-Path $outputRoot $name
            if (Test-Path -LiteralPath $output) {
                Remove-Item -LiteralPath $output -Force
            }

            Write-Host "Packing $name"
            $timer = [System.Diagnostics.Stopwatch]::StartNew()
            & $pakToolPath pack `
                --input $sourcePath `
                --output $output `
                --compression auto `
                --zstd-level $level `
                --block-size $blockSize `
                --min-saving-percent $minimumSaving `
                --verify
            if ($LASTEXITCODE -ne 0) {
                throw "paktool failed for $name with exit code $LASTEXITCODE"
            }
            $timer.Stop()

            $item = Get-Item -LiteralPath $output
            [pscustomobject]@{
                block_size = $blockSize
                zstd_level = $level
                minimum_saving_percent = $minimumSaving
                bytes = $item.Length
                mib = [math]::Round($item.Length / 1MB, 3)
                pack_and_verify_seconds = [math]::Round(
                    $timer.Elapsed.TotalSeconds, 3)
                path = $item.FullName
            }
        }
    }
}

$results | Sort-Object bytes | Format-Table -AutoSize
$results | ConvertTo-Json -Depth 3 | Set-Content `
    -LiteralPath (Join-Path $outputRoot 'compression-sweep.json') `
    -Encoding utf8
