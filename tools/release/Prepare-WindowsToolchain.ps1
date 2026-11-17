# Ephemeral GitHub-hosted runner only. Never changes a developer installation.
[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('llvm20-default','llvm20-msvc1444','llvm22-default')][string]$Case,
      [Parameter(Mandatory)][string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_REPOSITORY -cne 'over-keys/mozkey-space' -or $env:GITHUB_ACTIONS -cne 'true') { throw 'Only the authorized repository GitHub runner is supported.' }
if ($env:PROCESSOR_ARCHITECTURE -cne 'ARM64') { throw 'Native ARM64 runner required.' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$vc = (python src/build_tools/vs_util.py --arch arm64).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio discovery failed.' }
$before = @(Get-ChildItem -LiteralPath (Join-Path $vc 'Tools/MSVC') -Directory | Select-Object -ExpandProperty Name)
$inventory = [ordered]@{ case = $Case; source_sha = $env:GITHUB_SHA; image_os = $env:ImageOS; image_version = $env:ImageVersion; vc = $vc; before_versions = $before; has_1451 = @($before | Where-Object { $_ -match '^14\.51\.' }).Count -gt 0 }
$sdkIncludes = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Include'
$inventory['installed_sdk_versions'] = @()
if (Test-Path -LiteralPath $sdkIncludes -PathType Container) {
  $inventory['installed_sdk_versions'] = @(Get-ChildItem -LiteralPath $sdkIncludes -Directory | Select-Object -ExpandProperty Name)
}
$inventory | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutputDirectory 'inventory-before.json') -Encoding utf8
if ($Case -eq 'llvm20-msvc1444') {
  $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/setup.exe'
  if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) { throw 'Official Visual Studio Installer unavailable.' }
  $installPath = Split-Path -Parent $vc
  $arguments = @('modify', '--installPath', "`"$installPath`"", '--add',
    'Microsoft.VisualStudio.Component.VC.14.44.17.14.ARM64', '--add',
    'Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64', '--add',
    'Microsoft.VisualStudio.Component.VC.14.44.17.14.ATL.ARM64', '--add',
    'Microsoft.VisualStudio.Component.VC.14.44.17.14.ATL', '--quiet', '--norestart')
  $process = Start-Process -FilePath $installer -ArgumentList $arguments -Wait -PassThru
  if ($process.ExitCode -notin @(0, 3010)) { throw "Component provisioning failed: $($process.ExitCode)" }
}
$versions = @(Get-ChildItem -LiteralPath (Join-Path $vc 'Tools/MSVC') -Directory | Sort-Object { [version]$_.Name } -Descending)
if ($Case -eq 'llvm20-msvc1444') { $versions = @($versions | Where-Object { $_.Name -match '^14\.44\.[0-9]+$' }) }
if ($versions.Count -eq 0) { throw 'Requested MSVC toolset is unavailable.' }
$selected = $versions[0]
$required = @('include/yvals_core.h', 'atlmfc/include/atlbase.h')
foreach ($arch in @('x86', 'x64', 'arm64')) {
  foreach ($lib in @('libcmt.lib', 'oldnames.lib', 'libcpmt.lib', 'libvcruntime.lib', 'msvcrt.lib', 'msvcprt.lib', 'vcruntime.lib')) { $required += "lib/$arch/$lib" }
  $required += "atlmfc/lib/$arch/atls.lib"
}
$missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $selected.FullName $_) -PathType Leaf) })
$inventory['selected_version'] = $selected.Name
$inventory['missing_files'] = $missing
$inventory['all_installed_versions_after'] = @(Get-ChildItem -LiteralPath (Join-Path $vc 'Tools/MSVC') -Directory | Select-Object -ExpandProperty Name)
$inventory | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutputDirectory 'inventory-selected.json') -Encoding utf8
if ($missing.Count -gt 0) { throw "Incomplete selected toolset: $($missing -join ', ')" }
$env:BAZEL_VC = $vc
$env:BAZEL_VC_FULL_VERSION = $selected.Name
python -c "import sys; sys.path.insert(0, 'src/build_tools'); import vs_util, os; e=vs_util.get_vs_env_vars('arm64'); actual=e.get('VCTOOLSVERSION', '').rstrip('/\\'); assert actual == os.environ['BAZEL_VC_FULL_VERSION'], (actual, os.environ['BAZEL_VC_FULL_VERSION']); print('ARM64_MSVC_TOOLSET=' + actual)"
if ($LASTEXITCODE -ne 0) { throw 'MSVC toolset activation verification failed.' }
"BAZEL_VC=$vc" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
"BAZEL_VC_FULL_VERSION=$($selected.Name)" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
Write-Host "SELECTED_TOOLSET=$($selected.Name) CASE=$Case"
