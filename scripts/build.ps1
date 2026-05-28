param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$Generator = 'Visual Studio 17 2022',
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'build'

$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($cmake) {
    $cmakeExe = $cmake.Source
} elseif (Test-Path 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe') {
    $cmakeExe = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
} elseif (Test-Path 'C:\Program Files\CMake\bin\cmake.exe') {
    $cmakeExe = 'C:\Program Files\CMake\bin\cmake.exe'
} else {
    throw 'cmake.exe was not found. Install CMake or run from a Visual Studio developer environment.'
}

& $cmakeExe -S $repoRoot -B $buildDir -G $Generator -A $Platform
& $cmakeExe --build $buildDir --config $Configuration
