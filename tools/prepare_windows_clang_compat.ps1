<#
.SYNOPSIS
  Builds the host-link compatibility object required by the bundled clang.

.DESCRIPTION
  Windows SDK headers need the builtin offsetof definition with clang-cl.
  External Bazel host tools do not depend on Mozc's compatibility library, so
  compile the same fallback source once and pass it through --host_linkopt.
#>
[CmdletBinding()]
param(
  [string]$OutputPath = ''
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$clang = Join-Path $repoRoot 'src\third_party\llvm\bin\clang-cl.exe'
$source = Join-Path $repoRoot 'src\base\clang_intrinsics_compat.cc'
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
  $OutputPath = Join-Path $repoRoot 'tools\clang_intrinsics_compat.obj'
}

if (-not (Test-Path -LiteralPath $clang)) {
  throw "Bundled clang-cl.exe was not found: $clang"
}
if (-not (Test-Path -LiteralPath $source)) {
  throw "Compatibility source was not found: $source"
}

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Parent $resolvedOutput
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

& $clang /nologo /std:c++20 /O2 /c $source "/Fo$resolvedOutput"
if ($LASTEXITCODE -ne 0) {
  throw "clang-cl.exe failed while producing $resolvedOutput"
}

if (-not (Test-Path -LiteralPath $resolvedOutput)) {
  throw "clang-cl.exe did not produce $resolvedOutput"
}

Write-Host "Prepared Windows clang compatibility object: $resolvedOutput"
if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_ENV)) {
  "MOZKEY_HOST_INTRINSICS_COMPAT=$resolvedOutput" |
    Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
}
