param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$BuildDir = '',

    [string]$MingwBin = $env:BAA_LSP_MINGW_BIN,

    [string]$BaaExecutable = '',

    [string]$TakweenExecutable = '',

    [string]$NlohmannJsonSource = '',

    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)

if (!$BuildDir) {
    $BuildDir = "build/windows-$($Configuration.ToLowerInvariant())"
}

function Resolve-MingwBin {
    param([string]$ProvidedDirectory)

    $candidates = [System.Collections.Generic.List[string]]::new()
    if ($ProvidedDirectory) {
        $candidates.Add($ProvidedDirectory)
    }
    $candidates.Add('C:\msys64\ucrt64\bin')

    if (Test-Path 'C:\Qt\Tools') {
        Get-ChildItem 'C:\Qt\Tools' -Directory -Filter 'mingw*_64' |
            Sort-Object Name -Descending |
            ForEach-Object { $candidates.Add((Join-Path $_.FullName 'bin')) }
    }

    $pathCompiler = Get-Command g++.exe -ErrorAction SilentlyContinue
    if ($pathCompiler) {
        $candidates.Add((Split-Path -Parent $pathCompiler.Source))
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'g++.exe'))) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw 'A MinGW g++.exe was not found. Set BAA_LSP_MINGW_BIN to the selected toolchain bin directory.'
}

function New-NormalizedNativePath {
    param([string[]]$PreferredDirectories)

    $seen = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $result = [System.Collections.Generic.List[string]]::new()
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $rawEntries = @($PreferredDirectories) + @($machinePath, $userPath)

    foreach ($rawEntry in $rawEntries) {
        if (!$rawEntry) { continue }
        foreach ($entry in ($rawEntry -split ';')) {
            $candidate = $entry.Trim().Trim('"')
            if (!$candidate) { continue }
            if ($seen.Add($candidate)) {
                $result.Add($candidate)
            }
        }
    }

    return ($result -join ';')
}

function Invoke-Native {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    & $script:CMakeLauncher -E env `
        --unset=Path `
        --unset=PATH `
        "Path=$script:NormalizedNativePath" `
        $FilePath @Arguments
    $exitCode = $LASTEXITCODE
    if ($null -eq $exitCode) { $exitCode = 0 }
    if ($exitCode -ne 0) {
        throw "$FilePath failed with exit code $exitCode."
    }
}

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (!$cmakeCommand) {
    throw 'cmake.exe was not found. Install CMake or add it to the machine or user Path.'
}

$MingwBin = Resolve-MingwBin -ProvidedDirectory $MingwBin
$CMakeProgram = $cmakeCommand.Source
$CTestProgram = Join-Path (Split-Path -Parent $CMakeProgram) 'ctest.exe'
$Gxx = Join-Path $MingwBin 'g++.exe'
$Ninja = Join-Path $MingwBin 'ninja.exe'
$Make = Join-Path $MingwBin 'mingw32-make.exe'

if (Test-Path $Ninja) {
    $Generator = 'Ninja'
    $MakeProgram = $Ninja
} elseif (Test-Path $Make) {
    $Generator = 'MinGW Makefiles'
    $MakeProgram = $Make
} else {
    throw "Neither ninja.exe nor mingw32-make.exe was found in $MingwBin."
}

$script:CMakeLauncher = $CMakeProgram
$script:NormalizedNativePath = New-NormalizedNativePath -PreferredDirectories @(
    $MingwBin,
    (Split-Path -Parent $CMakeProgram),
    "$env:SystemRoot\System32",
    $env:SystemRoot
)
$TestsFlag = if ($SkipTests) { 'OFF' } else { 'ON' }

$configureArguments = @(
    '-S', '.',
    '-B', $BuildDir,
    '-G', $Generator,
    "-DCMAKE_CXX_COMPILER=$Gxx",
    "-DCMAKE_MAKE_PROGRAM=$MakeProgram",
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DBAA_LSP_BUILD_TESTS=$TestsFlag"
)

if (!$NlohmannJsonSource -and (Test-Path 'build')) {
    $NlohmannJsonSource = Get-ChildItem 'build' -Directory |
        ForEach-Object {
            Join-Path $_.FullName '_deps\nlohmann_json-src'
        } |
        Where-Object {
            Test-Path (Join-Path $_ 'CMakeLists.txt')
        } |
        Select-Object -First 1
}
if ($NlohmannJsonSource) {
    $resolvedJsonSource = (Resolve-Path $NlohmannJsonSource).Path
    $configureArguments +=
        "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$resolvedJsonSource"
}

if ($BaaExecutable) {
    $configureArguments += "-DBAA_LSP_BAA_EXECUTABLE=$BaaExecutable"
}
if ($TakweenExecutable) {
    $configureArguments +=
        "-DBAA_LSP_TAKWEEN_EXECUTABLE=$TakweenExecutable"
}

Invoke-Native -FilePath $CMakeProgram -Arguments $configureArguments
Invoke-Native -FilePath $CMakeProgram -Arguments @(
    '--build', $BuildDir, '--parallel'
)

if (!$SkipTests) {
    Invoke-Native -FilePath $CTestProgram -Arguments @(
        '--test-dir', $BuildDir, '--output-on-failure'
    )
}

Write-Host 'Built Baa-LSP with an isolated Windows toolchain environment:' -ForegroundColor Green
Write-Host "  $BuildDir/baa-lsp.exe"
