param(
  [Parameter(Mandatory = $true)]
  [string]$MsiPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$msi = (Resolve-Path -LiteralPath $MsiPath).Path
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.OpenDatabase($msi, 0)

function Get-Records([string]$query) {
  $view = $database.OpenView($query)
  try {
    $null = $view.Execute()
    $records = @()
    do {
      $record = $view.Fetch()
      if ($null -ne $record) {
        $records += ,$record
      }
    } while ($null -ne $record)
    return $records
  } finally {
    $null = $view.Close()
  }
}

function Get-SingleRecord([string]$query) {
  $records = @(Get-Records $query)
  if ($records.Count -ne 1) {
    throw "MSI query did not return exactly one row ($($records.Count)): $query"
  }
  return $records[0]
}

$customAction = Get-SingleRecord(
  'SELECT `Action`, `Type`, `Source`, `Target` FROM `CustomAction` WHERE `Action` = ''LaunchPostInstallDialog'''
)

$actionName = $customAction.StringData(1)
$actionType = $customAction.IntegerData(2)
$actionSource = $customAction.StringData(3)
$actionTarget = $customAction.StringData(4)

if ($actionName -ne "LaunchPostInstallDialog") {
  throw "Unexpected custom action name: $actionName"
}
if (($actionType -band 0x3f) -ne 1) {
  throw "LaunchPostInstallDialog is not a Binary/DLL action: type=$actionType"
}
if (($actionType -band 0xc0) -ne 0x40) {
  throw "LaunchPostInstallDialog does not ignore the custom-action return code: type=$actionType"
}
if ($actionSource -ne "mozc_installer_helper.dll") {
  throw "LaunchPostInstallDialog targets unexpected binary: $actionSource"
}
if ($actionTarget -ne "LaunchPostInstallDialog") {
  throw "LaunchPostInstallDialog has unexpected entry point: $actionTarget"
}

$sequence = Get-SingleRecord(
  'SELECT `Action`, `Condition`, `Sequence` FROM `InstallExecuteSequence` WHERE `Action` = ''LaunchPostInstallDialog'''
)

$sequenceCondition = $sequence.StringData(2)
$sequenceNumber = $sequence.IntegerData(3)
if ($sequenceCondition -notmatch "UILevel\s*>\s*=\s*3") {
  throw "Post-install dialog is not restricted to interactive UI: $sequenceCondition"
}
if ($sequenceCondition -notmatch "NOT.*REMOVE.*ALL") {
  throw "Post-install dialog is not disabled during uninstall: $sequenceCondition"
}

$installFinalize = Get-SingleRecord(
  'SELECT `Sequence` FROM `InstallExecuteSequence` WHERE `Action` = ''InstallFinalize'''
)
if ($sequenceNumber -le $installFinalize.IntegerData(1)) {
  throw "Post-install dialog is not after InstallFinalize: $sequenceNumber <= $($installFinalize.IntegerData(1))"
}

$scheduleRebootRows = @(Get-Records(
  'SELECT `Sequence` FROM `InstallExecuteSequence` WHERE `Action` = ''ScheduleReboot'''
))
if ($scheduleRebootRows.Count -eq 1 -and $sequenceNumber -ge $scheduleRebootRows[0].IntegerData(1)) {
  throw "Post-install dialog is not before ScheduleReboot: $sequenceNumber >= $($scheduleRebootRows[0].IntegerData(1))"
}

$dialogTableRows = @(Get-Records(
  'SELECT `Name` FROM `_Tables` WHERE `Name` = ''Dialog'''
))
if ($dialogTableRows.Count -ne 0) {
  throw "Unexpected MSI Dialog table found; the original installer UI must remain unchanged"
}

Write-Host "Verified interactive MSI completion dialogs:"
Write-Host "  action    = $actionName"
Write-Host "  type      = $actionType"
Write-Host "  source    = $actionSource"
Write-Host "  target    = $actionTarget"
Write-Host "  sequence  = $($sequence.StringData(1))"
Write-Host "  condition = $sequenceCondition"
Write-Host "  ui        = original MSI UI (no Dialog table)"
