[CmdletBinding()]
param(
  [string]$SourceDir = (Join-Path $PSScriptRoot '..\..\src'),
  [ValidateSet('x64', 'arm64')]
  [string]$TargetArch = 'x64',
  [switch]$PersistToGitHub
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = (Resolve-Path -LiteralPath $SourceDir).Path
$vsUtil = Join-Path $source 'build_tools\vs_util.py'
if (-not (Test-Path -LiteralPath $vsUtil -PathType Leaf)) {
  throw "Mozc source directory is invalid; vs_util.py was not found: $source"
}

$pythonCommand = @('python', 'py', 'python3') |
  ForEach-Object { Get-Command $_ -ErrorAction SilentlyContinue } |
  Select-Object -First 1
if ($null -eq $pythonCommand) {
  throw 'Python 3 is required. Install Python or make python/py/python3 available on PATH.'
}

$pythonArgs = @()
if ($pythonCommand.Name -ieq 'py.exe' -or $pythonCommand.Name -ieq 'py') {
  $pythonArgs = @('-3')
}
$pythonPath = $pythonCommand.Source
if ([string]::IsNullOrWhiteSpace($pythonPath)) {
  $pythonPath = $pythonCommand.Path
}
if ([string]::IsNullOrWhiteSpace($pythonPath)) {
  throw "Could not resolve the Python executable: $($pythonCommand.Name)"
}

$vcOutput = & $pythonPath @pythonArgs $vsUtil --arch $TargetArch 2>&1
if ($LASTEXITCODE -ne 0) {
  throw "Visual Studio discovery failed:`n$($vcOutput -join [Environment]::NewLine)"
}
$vcDir = ($vcOutput |
  ForEach-Object { $_.ToString().Trim() } |
  Where-Object { $_ -ne '' } |
  Select-Object -Last 1)
if ([string]::IsNullOrWhiteSpace($vcDir)) {
  throw 'Visual Studio discovery returned an empty VC directory.'
}

$vcDir = (Resolve-Path -LiteralPath $vcDir).Path
$vcvarsall = Join-Path $vcDir 'Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvarsall -PathType Leaf)) {
  throw "The discovered VC directory is invalid; vcvarsall.bat was not found: $vcDir"
}

$llvmDir = Join-Path $source 'third_party\llvm'
$clangCl = Join-Path $llvmDir 'bin\clang-cl.exe'
if (-not (Test-Path -LiteralPath $clangCl -PathType Leaf)) {
  throw "Bundled clang-cl was not found. Run update_deps.py first: $clangCl"
}

$bash = Join-Path $source 'third_party\msys64\usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
  throw "Bundled MSYS2 bash was not found. Run update_deps.py first: $bash"
}

$variables = [ordered]@{
  BAZEL_VC = $vcDir
  BAZEL_LLVM = $llvmDir
  BAZEL_SH = $bash
}
foreach ($entry in $variables.GetEnumerator()) {
  Set-Item -Path "Env:$($entry.Key)" -Value $entry.Value
}

if ($PersistToGitHub) {
  if ([string]::IsNullOrWhiteSpace($env:GITHUB_ENV)) {
    throw 'PersistToGitHub requires the GITHUB_ENV environment variable.'
  }
  foreach ($entry in $variables.GetEnumerator()) {
    Add-Content -LiteralPath $env:GITHUB_ENV -Value "$($entry.Key)=$($entry.Value)" -Encoding utf8
  }
}

Write-Host "Windows build toolchain: $TargetArch"
Write-Host "  BAZEL_VC    = $env:BAZEL_VC"
Write-Host "  BAZEL_LLVM = $env:BAZEL_LLVM"
Write-Host "  BAZEL_SH   = $env:BAZEL_SH"
