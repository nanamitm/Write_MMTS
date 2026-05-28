param(
    [Parameter(Mandatory = $true)]
    [string]$EdcbRoot
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vendorRoot = Join-Path $repoRoot 'thirdparty\EDCB'
$edcbRoot = (Resolve-Path $EdcbRoot).Path

$dirs = @(
    'Common',
    'BonCtrl',
    'Write_Default\Write_Default',
    'Write_OneService\Write_OneService'
)

if (Test-Path $vendorRoot) {
    Remove-Item -LiteralPath $vendorRoot -Recurse -Force
}
foreach ($dir in $dirs) {
    New-Item -ItemType Directory -Force -Path (Join-Path $vendorRoot $dir) | Out-Null
}

Copy-Item -Path (Join-Path $edcbRoot 'Common\*.h'), (Join-Path $edcbRoot 'Common\*.cpp') -Destination (Join-Path $vendorRoot 'Common')
Copy-Item -Path (Join-Path $edcbRoot 'BonCtrl\*.h'), (Join-Path $edcbRoot 'BonCtrl\*.cpp') -Destination (Join-Path $vendorRoot 'BonCtrl')
Copy-Item -Path (Join-Path $edcbRoot 'Write_Default\Write_Default\*.h'), (Join-Path $edcbRoot 'Write_Default\Write_Default\*.cpp'), (Join-Path $edcbRoot 'Write_Default\Write_Default\*.rc'), (Join-Path $edcbRoot 'Write_Default\Write_Default\embed.txt') -Destination (Join-Path $vendorRoot 'Write_Default\Write_Default')
Copy-Item -Path (Join-Path $edcbRoot 'Write_OneService\Write_OneService\*.h'), (Join-Path $edcbRoot 'Write_OneService\Write_OneService\*.cpp') -Destination (Join-Path $vendorRoot 'Write_OneService\Write_OneService')

$commit = git -C $edcbRoot rev-parse HEAD
$remote = git -C $edcbRoot remote get-url origin
@"
# Vendored EDCB Sources

This directory contains the minimal EDCB source files needed to build Write_MMTS.

Source repository: $remote
Source commit: $commit

Only the files used by `CMakeLists.txt` are copied here so Write_MMTS can be built without a separate adjacent EDCB checkout.
"@ | Set-Content -Path (Join-Path $vendorRoot 'README.md') -Encoding UTF8
