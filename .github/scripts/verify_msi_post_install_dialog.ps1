param(
  [Parameter(Mandatory = $true)]
  [string]$MsiPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$canonicalVerifier = Join-Path $PSScriptRoot "..\..\src\win32\installer\verify_msi_post_install_dialog.ps1"
if (-not (Test-Path -LiteralPath $canonicalVerifier -PathType Leaf)) {
  throw "Canonical MSI verifier was not found: $canonicalVerifier"
}

& $canonicalVerifier -MsiPath $MsiPath
