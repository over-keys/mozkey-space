param(
    [ValidateSet("sample", "safe", "daily", "rich", "max")]
    [string]$Profile = "sample",
    [int]$SampleLines = 5000,
    [string]$BashPath = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$WorkRoot = Join-Path $RepoRoot "dist\dictionary\merge-ut"
$OutDir = Join-Path $RepoRoot "src\data\dictionary_koyasi\generated"
$MergeRepo = Join-Path $WorkRoot "merge-ut-dictionaries"

if ($Profile -eq "safe") {
    $EffectiveProfile = "sample"
} else {
    $EffectiveProfile = $Profile
}

Write-Host "Repo root: $RepoRoot"
Write-Host "Work root: $WorkRoot"
Write-Host "Output dir: $OutDir"
Write-Host "Profile: $EffectiveProfile"

New-Item -ItemType Directory -Force $WorkRoot | Out-Null
New-Item -ItemType Directory -Force $OutDir | Out-Null

function Test-IsWindowsHost {
    if ($PSVersionTable.ContainsKey("Platform")) {
        return $PSVersionTable.Platform -eq "Win32NT"
    }

    return $env:OS -eq "Windows_NT"
}

function Test-BashPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        return $false
    }

    try {
        $Output = & $Path -lc "printf MOZC_BASH_OK" 2>$null
        return ($LASTEXITCODE -eq 0 -and $Output -eq "MOZC_BASH_OK")
    } catch {
        return $false
    }
}

function Find-Bash {
    param(
        [string]$RequestedBashPath = ""
    )

    if ($RequestedBashPath) {
        if (Test-BashPath $RequestedBashPath) {
            return (Resolve-Path $RequestedBashPath).Path
        }
        throw "Specified bash is not usable: $RequestedBashPath"
    }

    if (Test-IsWindowsHost) {
        $BashCandidates = @(
            (Join-Path $RepoRoot "src\third_party\msys64\usr\bin\bash.exe"),
            (Join-Path $RepoRoot "src\third_party\msys64\bin\bash.exe"),
            "C:\Program Files\Git\bin\bash.exe",
            "C:\Program Files\Git\usr\bin\bash.exe",
            "C:\Program Files (x86)\Git\bin\bash.exe",
            "C:\Program Files (x86)\Git\usr\bin\bash.exe"
        )

        foreach ($Candidate in $BashCandidates) {
            if (Test-BashPath $Candidate) {
                return (Resolve-Path $Candidate).Path
            }
        }

        $PathBashes = @(Get-Command bash.exe -ErrorAction SilentlyContinue)
        foreach ($Command in $PathBashes) {
            $Source = $Command.Source

            # Do not select WSL bash launchers blindly. They depend on the
            # default WSL distribution and may resolve to docker-desktop,
            # which is not a normal Linux user environment.
            if ($Source -like "$env:WINDIR\System32\bash.exe" -or
                $Source -like "$env:LOCALAPPDATA\Microsoft\WindowsApps\bash.exe") {
                if (Test-BashPath $Source) {
                    return $Source
                }
                continue
            }

            if (Test-BashPath $Source) {
                return $Source
            }
        }

        throw "Usable bash was not found. Install Git for Windows, use Mozc's MSYS bash, or pass -BashPath explicitly."
    }

    $Command = Get-Command bash -ErrorAction SilentlyContinue
    if ($Command -and (Test-BashPath $Command.Source)) {
        return $Command.Source
    }

    foreach ($Candidate in @("/bin/bash", "/usr/bin/bash")) {
        if (Test-BashPath $Candidate) {
            return $Candidate
        }
    }

    throw "Usable bash was not found."
}

$BashPath = Find-Bash -RequestedBashPath $BashPath
Write-Host "Using bash: $BashPath"

if (-not (Test-Path $MergeRepo)) {
    Write-Host "Cloning merge-ut-dictionaries..."
    git clone --depth 1 https://github.com/utuhiro78/merge-ut-dictionaries.git $MergeRepo
    if ($LASTEXITCODE -ne 0) {
        throw "git clone failed."
    }
} else {
    Write-Host "Updating merge-ut-dictionaries..."
    git -C $MergeRepo pull --ff-only
    if ($LASTEXITCODE -ne 0) {
        throw "git pull failed."
    }
}

$MakePath = Join-Path $MergeRepo "src\merge\make.sh"
if (-not (Test-Path $MakePath)) {
    throw "make.sh was not found: $MakePath"
}

# merge-ut-dictionaries resolves the Mozc revision through the unauthenticated
# GitHub API. Several platform jobs can hit that API at the same time, so
# replace only that lookup with the Git protocol, which does not use the API
# rate limit. The imported script remains otherwise unchanged.
$MergeScriptPath = Join-Path $MergeRepo "src\merge\merge_dictionaries.py"
if (-not (Test-Path $MergeScriptPath)) {
    throw "merge_dictionaries.py was not found: $MergeScriptPath"
}

$MergeScript = Get-Content -Raw -Encoding UTF8 $MergeScriptPath
$MergeScript = $MergeScript.Replace("`r`n", "`n")
$OriginalMozcLookup = @'
    # Mozc の最終コミット日を取得
    url = 'https://api.github.com/repos/google/mozc/commits/master'

    with urllib.request.urlopen(url) as response:
        data = json.loads(response.read().decode())
        date_str = data['commit']['committer']['date']

    date_str = datetime.fromisoformat(date_str)
    date_str = date_str.strftime('%Y%m%d')

    # Mozc のアーカイブを取得
    url = 'https://github.com/google/mozc/archive/refs/heads/master.zip'

    if not Path(f'mozc-{date_str}.zip').exists():
        urllib.request.urlretrieve(
                url, f'mozc-{date_str}.zip')
'@
$DirectMozcLookup = @'
    # Resolve the Mozc master revision through the Git protocol instead of
    # the unauthenticated GitHub API, which is rate-limited in parallel CI.
    remote = 'https://github.com/google/mozc.git'
    remote_output = subprocess.check_output(
        ['git', 'ls-remote', remote, 'refs/heads/master'],
        text=True,
    )
    date_str = remote_output.split()[0]
    if not date_str:
        raise RuntimeError('Could not resolve google/mozc master commit.')

    # Mozc のアーカイブを取得
    # Keep the archive's mozc-master/ directory name expected by the
    # upstream helper; use the resolved SHA only for the local cache name.
    url = 'https://github.com/google/mozc/archive/refs/heads/master.zip'

    if not Path(f'mozc-{date_str}.zip').exists():
        urllib.request.urlretrieve(
                url, f'mozc-{date_str}.zip')
'@

if (-not $MergeScript.Contains("import subprocess`n")) {
    $MergeScript = $MergeScript.Replace("import json`n", "import json`nimport subprocess`n")
}

if (-not $MergeScript.Contains($DirectMozcLookup)) {
    if (-not $MergeScript.Contains($OriginalMozcLookup)) {
        throw "Unexpected merge_dictionaries.py Mozc lookup layout; refusing to patch it."
    }

    $MergeScript = $MergeScript.Replace($OriginalMozcLookup, $DirectMozcLookup)
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($MergeScriptPath, $MergeScript, $Utf8NoBom)

$Flags = @{
    alt_cannadic   = $false
    edict2         = $false
    jawiki         = $false
    neologd        = $false
    personal_names = $false
    place_names    = $false
    skk_jisyo      = $false
    sudachidict    = $false
    generate_latest = $false
}

switch ($EffectiveProfile) {
    "sample" {
        # Minimal and reproducible first profile.
        $Flags.place_names = $true
        $Flags.sudachidict = $true
    }

    "daily" {
        # Current daily baseline.
        # Keep this conservative until we add cost profiling.
        $Flags.place_names = $true
        $Flags.sudachidict = $true
    }

    "rich" {
        # Broad recall profile, but not full legacy/GPL-heavy set.
        $Flags.jawiki = $true
        $Flags.neologd = $true
        $Flags.personal_names = $true
        $Flags.place_names = $true
        $Flags.sudachidict = $true
    }

    "max" {
        # Experimental full-recall profile.
        $Flags.alt_cannadic = $true
        $Flags.edict2 = $true
        $Flags.jawiki = $true
        $Flags.neologd = $true
        $Flags.personal_names = $true
        $Flags.place_names = $true
        $Flags.skk_jisyo = $true
        $Flags.sudachidict = $true
    }

    default {
        throw "Unknown profile: $EffectiveProfile"
    }
}

Write-Host "Enabled dictionaries:"
foreach ($Key in $Flags.Keys | Sort-Object) {
    if ($Key -eq "generate_latest") {
        continue
    }

    if ($Flags[$Key]) {
        Write-Host "  + $Key"
    }
}

Write-Host "Disabled dictionaries:"
foreach ($Key in $Flags.Keys | Sort-Object) {
    if ($Key -eq "generate_latest") {
        continue
    }

    if (-not $Flags[$Key]) {
        Write-Host "  - $Key"
    }
}

function Set-MakeFlag {
    param(
        [string]$Content,
        [string]$Name,
        [bool]$Enabled
    )

    if ($Enabled) {
        $Replacement = "$Name=`"true`""
    } else {
        $Replacement = "#$Name=`"true`""
    }

    $Pattern = "(?m)^#?" + [System.Text.RegularExpressions.Regex]::Escape("$Name=`"true`"")
    return [System.Text.RegularExpressions.Regex]::Replace($Content, $Pattern, $Replacement)
}

Write-Host "Patching make.sh for profile: $EffectiveProfile"

$Content = Get-Content -Raw -Encoding UTF8 $MakePath

foreach ($Key in $Flags.Keys) {
    $Content = Set-MakeFlag -Content $Content -Name $Key -Enabled $Flags[$Key]
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($MakePath, $Content, $Utf8NoBom)

$MergeDir = Join-Path $MergeRepo "src\merge"

Write-Host "Running make.sh..."
Push-Location $MergeDir
try {
    & $BashPath -lc "bash ./make.sh"
    if ($LASTEXITCODE -ne 0) {
        throw "make.sh failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

$Generated = Join-Path $MergeDir "mozcdic-ut.txt"
if (-not (Test-Path $Generated)) {
    throw "mozcdic-ut.txt was not generated."
}

if ($EffectiveProfile -eq "sample") {
    # Keep legacy file names used in the previous step.
    $OutFile = Join-Path $OutDir "mozcdic-ut-safe.txt"
    $SampleFile = Join-Path $OutDir "mozcdic-ut-sample.txt"
} else {
    $OutFile = Join-Path $OutDir "mozcdic-ut-$EffectiveProfile.txt"
    $SampleFile = Join-Path $OutDir "mozcdic-ut-$EffectiveProfile-sample.txt"
}

Copy-Item $Generated $OutFile -Force

$LineCount = (Get-Content -Encoding UTF8 $OutFile | Measure-Object -Line).Lines
$Size = (Get-Item $OutFile).Length

if ($LineCount -le 0) {
    throw "Generated dictionary is empty: $OutFile"
}

if ($SampleLines -gt 0) {
    [string[]]$Sample = @(Get-Content -Encoding UTF8 $OutFile -TotalCount $SampleLines)

    if ($Sample.Count -le 0) {
        throw "Generated sample dictionary would be empty: $OutFile"
    }

    [System.IO.File]::WriteAllLines($SampleFile, $Sample, $Utf8NoBom)
}

Write-Host ""
Write-Host "Generated full dictionary:"
Write-Host "  $OutFile"
Write-Host "Lines:"
Write-Host "  $LineCount"
Write-Host "Bytes:"
Write-Host "  $Size"

if ($SampleLines -gt 0) {
    $SampleLineCount = (Get-Content -Encoding UTF8 $SampleFile | Measure-Object -Line).Lines
    $SampleSize = (Get-Item $SampleFile).Length

    Write-Host ""
    Write-Host "Generated sample dictionary:"
    Write-Host "  $SampleFile"
    Write-Host "Lines:"
    Write-Host "  $SampleLineCount"
    Write-Host "Bytes:"
    Write-Host "  $SampleSize"
}

Write-Host ""
Write-Host "Done."
