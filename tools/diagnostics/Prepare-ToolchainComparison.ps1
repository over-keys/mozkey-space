# Restricted entry point; provisioning logic is shared with candidate builds.
[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('llvm20-default','llvm20-msvc1444','llvm22-default')][string]$Case,
      [Parameter(Mandatory)][string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_REPOSITORY -cne 'over-keys/mozkey-space' -or $env:GITHUB_REF -cne 'refs/heads/integration/upstream-1.1.0' -or $env:GITHUB_EVENT_NAME -cne 'push') { throw 'Unexpected diagnostic scope.' }
& "$PSScriptRoot/../release/Prepare-WindowsToolchain.ps1" -Case $Case -OutputDirectory $OutputDirectory
