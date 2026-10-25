# Rebuild development NRO, or both native NSP and NRO with -Release.
# Optional external packaging inputs: CONNECTIONS_HACPACK, CONNECTIONS_NSP_KEYS.
param(
    [switch]$Release,
    [switch]$Clean,
    [string]$DevkitProRoot = 'C:\devkitPro',
    [string]$Python = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$temporaryPath = Join-Path $projectRoot 'build\tmp'
New-Item -ItemType Directory -Force $temporaryPath | Out-Null
$bashPath = Join-Path $DevkitProRoot 'msys2\usr\bin\bash.exe'
function ConvertTo-MsysPath([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path).Replace('\', '/')
    return '/' + $resolved.Substring(0, 1).ToLower() + $resolved.Substring(2)
}
# Literal single quotes prevent the shell from interpreting spaces or metacharacters.
function Quote-Shell([string]$Value) {
    if ($Value.Contains("'")) { throw 'Build paths cannot contain apostrophes.' }
    return "'" + $Value + "'"
}
$kitPath = ConvertTo-MsysPath $DevkitProRoot
$tempPath = ConvertTo-MsysPath $temporaryPath
$buildCommand = 'make -j4 nro'
if ($Release) {
    if (!(Test-Path -LiteralPath $Python -PathType Leaf)) { throw 'Supply -Python with an installed Python executable.' }
    $buildCommand = 'make -j4 release PYTHON=' + (Quote-Shell (ConvertTo-MsysPath $Python))
}
$command = 'export DEVKITPRO=' + (Quote-Shell $kitPath) + ' DEVKITA64=' + (Quote-Shell ($kitPath + '/devkitA64')) + ' TMPDIR=' + (Quote-Shell $tempPath) + ' TEMP=' + (Quote-Shell $temporaryPath.Replace('\','/')) + ' TMP=' + (Quote-Shell $temporaryPath.Replace('\','/')) + '; export PATH=' + (Quote-Shell ($kitPath + '/tools/bin')) + ':' + (Quote-Shell ($kitPath + '/devkitA64/bin')) + ':$PATH; ' + $buildCommand
if ($Clean) {
    # make clean removes only this project's fixed build/output paths.
    $command = $command.Replace('; ' + $buildCommand, '; make clean && mkdir -p ' + (Quote-Shell $tempPath) + ' && ' + $buildCommand)
}
$mappedDrive = $null
Push-Location $projectRoot
try {
    # devkitPro Makefiles use whitespace-separated path lists. Keep the project
    # in place and use an unused temporary drive alias when its path has spaces.
    if ($projectRoot -match '\s') {
        $drive = @('Z','Y','X','W','V','U','T') | Where-Object { !(Test-Path ($_ + ':\')) } | Select-Object -First 1
        if (!$drive) { throw 'No free temporary drive letter for the build.' }
        & "$env:SystemRoot\System32\subst.exe" ($drive + ':') $projectRoot
        if ($LASTEXITCODE -ne 0) { throw 'Could not create temporary build drive.' }
        $mappedDrive = $drive + ':'
        $command = 'cd /' + $drive.ToLower() + ' && ' +
            $command.Replace('make ', 'make TARGET=' + (Quote-Shell (Split-Path $projectRoot -Leaf)) + ' ')
    }
    & $bashPath --noprofile --norc -c $command
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
    # Leave only deployable NRO/NSP outputs at the root.
    $artifactDirectory = Join-Path $projectRoot 'build/artifacts'
    New-Item -ItemType Directory -Force $artifactDirectory | Out-Null
    foreach ($extension in @('elf', 'nso', 'nacp')) {
        $intermediate = Join-Path $projectRoot ('Connections.' + $extension)
        if (Test-Path -LiteralPath $intermediate) {
            Move-Item -LiteralPath $intermediate -Destination (Join-Path $artifactDirectory ('Connections.' + $extension)) -Force
        }
    }
} finally {
    Pop-Location
    if ($mappedDrive) { & "$env:SystemRoot\System32\subst.exe" $mappedDrive /D }
}
