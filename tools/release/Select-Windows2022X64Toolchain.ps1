# Select the supported compiler/STL pairing on an ephemeral hosted x64 runner.
# LLVM stays at the checksum-pinned 20.1.1; VS2026's STL requires Clang 22.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:GITHUB_ACTIONS -cne 'true' -or
    $env:GITHUB_REPOSITORY -cne 'over-keys/mozkey-space' -or
    $env:PROCESSOR_ARCHITECTURE -cne 'AMD64') {
  throw 'Only the authorized repository GitHub-hosted x64 runner is supported.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installations = @(& $vswhere -latest -version '[17.0,18.0)' `
  -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath)
if ($LASTEXITCODE -ne 0 -or $installations.Count -ne 1 -or
    [string]::IsNullOrWhiteSpace($installations[0])) {
  throw 'Visual Studio 2022 with x64 C++ tools is required.'
}
$vs = $installations[0].Trim()
$vc = Join-Path $vs 'VC'
$tools = @(Get-ChildItem -LiteralPath (Join-Path $vc 'Tools/MSVC') -Directory |
  Where-Object { $_.Name -match '^14\.44\.[0-9]+$' } |
  Sort-Object { [version]$_.Name } -Descending)
if ($tools.Count -eq 0 -or
    -not (Test-Path -LiteralPath (Join-Path $tools[0].FullName 'include/yvals_core.h') -PathType Leaf)) {
  throw 'Visual Studio 2022 MSVC 14.44 with C++ STL headers is required.'
}

$env:BAZEL_VS = $vs
$env:BAZEL_VC = $vc
$env:BAZEL_VC_FULL_VERSION = $tools[0].Name
python -c "import sys; sys.path.insert(0, 'src/build_tools'); import os, vs_util; env = vs_util.get_vs_env_vars('amd64'); selected = env.get('VCTOOLSVERSION', '').rstrip('/' + chr(92)); assert selected == os.environ['BAZEL_VC_FULL_VERSION'], selected; print('X64_MSVC_TOOLSET=' + selected)"
if ($LASTEXITCODE -ne 0) { throw 'MSVC x64 toolset activation verification failed.' }
@("BAZEL_VS=$vs", "BAZEL_VC=$vc", "BAZEL_VC_FULL_VERSION=$($tools[0].Name)") |
  Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
