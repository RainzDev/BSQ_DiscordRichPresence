$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$sourceRoot = Join-Path (Join-Path $repoRoot 'android-helper') 'src'
$outputRoot = Join-Path (Join-Path $repoRoot 'build') 'android-helper'
$classesRoot = Join-Path $outputRoot 'classes'
$dexRoot = Join-Path $outputRoot 'dex'
# Build each path segment with Join-Path. A backslash inside the child string is
# a directory separator on Windows but a literal filename character on Linux,
# which would put the CI helper beside the build directory instead of in it.
$jarPath = Join-Path (Join-Path $repoRoot 'build') 'discord-rpc-helper.jar'
$isWindowsHost = $env:OS -eq 'Windows_NT'

function Find-RequiredTool {
    param(
        [Parameter(Mandatory = $true)][string] $DisplayName,
        [Parameter(Mandatory = $true)][string[]] $Candidates
    )

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Get-Item -LiteralPath $candidate).FullName
        }
    }
    throw "Required build tool '$DisplayName' was not found. Checked: $($Candidates -join ', ')"
}

# Prefer CI/user environment variables and PATH. The final Windows locations are
# compatibility fallbacks for the workstation on which this mod was developed;
# they are no longer requirements for another contributor or GitHub Actions.
$javaExecutableSuffix = if ($isWindowsHost) { '.exe' } else { '' }
$javaHomeCandidates = @($env:JAVA_HOME) | Where-Object { $_ }
if ($isWindowsHost) {
    # Never pass a Windows drive path to Join-Path under Linux PowerShell.
    # GitHub Actions supplies JAVA_HOME, while this fallback remains useful on
    # the original Windows development machine when JAVA_HOME is not defined.
    $javaHomeCandidates += 'C:\Program Files\Android\openjdk\jdk-21.0.8'
}
$javacCandidates = @($javaHomeCandidates | ForEach-Object {
    Join-Path (Join-Path $_ 'bin') "javac$javaExecutableSuffix"
})
$jarCandidates = @($javaHomeCandidates | ForEach-Object {
    Join-Path (Join-Path $_ 'bin') "jar$javaExecutableSuffix"
})
$javacOnPath = Get-Command javac -ErrorAction SilentlyContinue
$jarOnPath = Get-Command jar -ErrorAction SilentlyContinue
if ($javacOnPath) { $javacCandidates += $javacOnPath.Source }
if ($jarOnPath) { $jarCandidates += $jarOnPath.Source }
$javac = Find-RequiredTool -DisplayName 'javac' -Candidates $javacCandidates
$jar = Find-RequiredTool -DisplayName 'jar' -Candidates $jarCandidates

$androidSdkRoots = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT) | Where-Object { $_ }
if ($isWindowsHost) {
    # As with Java above, only evaluate the workstation fallback on Windows;
    # Linux runners do not have a C: provider for PowerShell to resolve.
    $androidSdkRoots += 'C:\Program Files (x86)\Android\android-sdk'
}
$androidSdkCandidates = @($androidSdkRoots | Where-Object {
    Test-Path -LiteralPath $_ -PathType Container
})
if (-not $androidSdkCandidates) {
    throw 'Android SDK was not found. Set ANDROID_HOME or ANDROID_SDK_ROOT.'
}
$androidSdk = (Get-Item -LiteralPath $androidSdkCandidates[0]).FullName
$androidJar = Join-Path (Join-Path (Join-Path $androidSdk 'platforms') 'android-36') 'android.jar'
if (-not (Test-Path -LiteralPath $androidJar -PathType Leaf)) {
    throw "Android API 36 platform is required but was not found: $androidJar"
}

$d8Name = if ($isWindowsHost) { 'd8.bat' } else { 'd8' }
$buildToolsRoot = Join-Path $androidSdk 'build-tools'
$d8Candidates = @((Join-Path (Join-Path $buildToolsRoot '36.0.0') $d8Name))
$installedBuildTools = Get-ChildItem -LiteralPath $buildToolsRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending
$d8Candidates += @($installedBuildTools | ForEach-Object { Join-Path $_.FullName $d8Name })
$d8 = Find-RequiredTool -DisplayName 'd8' -Candidates $d8Candidates

function Assert-OutputPathIsInsideRepository {
    param([Parameter(Mandatory = $true)][string] $Path)
    $resolved = [System.IO.Path]::GetFullPath($Path)
    $repositoryPrefix = $repoRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($repositoryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean an output path outside the repository: $resolved"
    }
}

Assert-OutputPathIsInsideRepository $classesRoot
Assert-OutputPathIsInsideRepository $dexRoot
New-Item -ItemType Directory -Force -Path $classesRoot, $dexRoot | Out-Null
# These are generated-only subdirectories validated above. Cleaning them avoids
# packaging stale classes.dex content after a Java source file is removed.
Get-ChildItem -LiteralPath $classesRoot -Force | Remove-Item -Recurse -Force
Get-ChildItem -LiteralPath $dexRoot -Force | Remove-Item -Recurse -Force

$sources = @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -Filter '*.java' |
    Select-Object -ExpandProperty FullName)
if (-not $sources) { throw "No Java helper sources were found under $sourceRoot" }

& $javac -source 8 -target 8 -Xlint:-options -classpath $androidJar -d $classesRoot $sources
if ($LASTEXITCODE -ne 0) { throw "javac failed with exit code $LASTEXITCODE" }

$classFiles = @(Get-ChildItem -LiteralPath $classesRoot -Recurse -Filter '*.class' |
    Select-Object -ExpandProperty FullName)
if (-not $classFiles) { throw 'javac completed without producing any class files' }
& $d8 --min-api 29 --output $dexRoot $classFiles
if ($LASTEXITCODE -ne 0) { throw "d8 failed with exit code $LASTEXITCODE" }

if (Test-Path -LiteralPath $jarPath) { Remove-Item -LiteralPath $jarPath -Force }
Push-Location $dexRoot
try {
    & $jar --create --file $jarPath 'classes.dex'
    if ($LASTEXITCODE -ne 0) { throw "jar failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

Get-FileHash -Algorithm SHA256 -LiteralPath $jarPath
