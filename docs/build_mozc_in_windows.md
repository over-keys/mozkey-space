# How to build Mozc in Windows

<!-- disableFinding(LINK_RELATIVE_G3DOC) -->

[![Windows](https://github.com/google/mozc/actions/workflows/windows.yaml/badge.svg)](https://github.com/google/mozc/actions/workflows/windows.yaml)

## Summary

If you are unsure about what the following commands do, please review the
descriptions below to understand the operations before running them.

```
git clone https://github.com/google/mozc.git
cd mozc\src

python build_tools/update_deps.py
python build_tools/build_qt.py --release --confirm_license
bazelisk build package --config release_build

python build_tools/open.py bazel-bin/win32/installer/Mozc64.msi
```

> [!TIP] You can also download `Mozc64.msi` from GitHub Actions. Check
> [Build with GitHub Actions](#build-with-github-actions) for details.

## Setup

### System Requirements

64-bit Windows 10 or later.

### Software Requirements

Building Mozc on Windows requires the following software.

*   [Visual Studio 2022 Community Edition](https://visualstudio.microsoft.com/downloads/#visual-studio-community-2022)
    with the following components.
    *   Windows 11 SDK
    *   MSVC v143 - VS 2022 C++ x64/x86 build tools (Latest)
    *   MSVC v143 - VS 2022 C++ ARM64/ARM64EC build tools (Latest)
    *   C++ ATL for latest v143 build tools (x86 & x64)
    *   C++ ATL for latest v143 build tools (ARM64/ARM64EC)
*   Python 3.12 or later.
*   `.NET 6` or later (for `dotnet` command).
*   [Bazelisk](https://github.com/bazelbuild/bazelisk)

> [!TIP] The following Visual Studio components can be skipped if you do not
> build Mozc for ARM64.
>
>  *   MSVC v143 - VS 2022 C++ ARM64/ARM64EC build tools (Latest)
>  *   C++ ATL for latest v143 build tools (ARM64/ARM64EC)

> [!TIP] Visual Studio 2026 Community Edition is also supported to build Mozc.
> When both VS 2022 and 2026 are installed, VS 2022 will be used.

> [!NOTE] Bazelisk is a wrapper of [Bazel](https://bazel.build) that allows you
> to use a specific version of Bazel.

### Download the repository from GitHub

```
git clone https://github.com/google/mozc.git
cd mozc\src
```

Hereafter you can do all the operations without changing directory.

### Check out additional build dependencies

```
python build_tools/update_deps.py
```

In this step, additional build dependencies will be downloaded, including:

*   [LLVM 20.1.1](https://github.com/llvm/llvm-project/releases/tag/llvmorg-20.1.1)
*   [MSYS2 2025-02-21](https://github.com/msys2/msys2-installer/releases/tag/2025-02-21)
*   [Ninja 1.11.0](https://github.com/ninja-build/ninja/releases/tag/v1.11.0)
*   [Qt 6.9.1](https://download.qt.io/archive/qt/6.8/6.8.0/submodules/qtbase-everywhere-src-6.9.1.tar.xz)
*   [.NET tools](../dotnet-tools.json)

## Build

### Build Qt

```
python build_tools/build_qt.py --release --confirm_license
```

If you would like to manually confirm the Qt license, omit the
`--confirm_license` option.

### Build Mozc

Assuming `bazelisk` is in your `%PATH%`, run the following command to build Mozc
for Windows.

```
bazelisk build package --config release_build
```

### mozkey-space のローカルリリースビルド（低負荷・再現用）

mozkey-space の Windows MSI は、リポジトリの `src` ディレクトリから次の
手順で作る。このワークスペースでは、リポジトリ（`current-main-v4_1`）と
ビルド環境・キャッシュを分離している。特に `BAZEL_VC` は Visual Studio の
インストール先ではなく、`VC` ディレクトリを指す必要がある。

```powershell
$ErrorActionPreference = 'Stop'
$src = (Get-Location).Path
if ((Split-Path -Leaf $src) -ne 'src') {
  throw "Run this procedure from the repository's src directory: $src"
}

# This is the shared local workspace used on the build machine.  Change only
# this value when moving the environment; keep all paths below derived from it.
$workspace = 'G:\mozkey'
$envRoot = Join-Path $workspace 'local-build-env'
$dotnet = Join-Path $envRoot 'dotnet-sdk-8.0'
$dotnetHome = Join-Path $envRoot 'dotnet-home'
$nuget = Join-Path $envRoot 'nuget-packages'
$wixSource = Join-Path $envRoot 'wix-tool-5.0.2\.store\wix\5.0.2\wix\5.0.2'
$bazelRoot = Join-Path $workspace 'local-build-cache-bazel-windows'

if (-not (Test-Path (Join-Path $dotnet 'dotnet.exe'))) {
  throw "Local .NET SDK was not found: $dotnet"
}
if (-not (Test-Path (Join-Path $wixSource 'wix.5.0.2.nupkg'))) {
  throw "Local WiX NuGet source was not found: $wixSource"
}

# Prefer the pinned Build Tools installation; fall back to the standard VS paths.
$vcCandidates = @(
  'C:\BuildTools\VC',
  'C:\Program Files\Microsoft Visual Studio\2022\Community\VC',
  'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC',
  'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC'
) | Where-Object {
  Test-Path (Join-Path $_ 'Auxiliary\Build\vcvarsall.bat')
}
$env:BAZEL_VC = $vcCandidates | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($env:BAZEL_VC)) {
  throw "Invalid BAZEL_VC (expected a VC directory): $env:BAZEL_VC"
}
$env:BAZEL_LLVM = Join-Path $src 'third_party\llvm'
$bundledBash = Join-Path $src 'third_party\msys64\usr\bin\bash.exe'
$env:BAZEL_SH = if (Test-Path $bundledBash) {
  $bundledBash
} else {
  'C:\Program Files\Git\usr\bin\bash.exe'
}
if (-not (Test-Path -LiteralPath $env:BAZEL_SH)) {
  throw "Bash was not found: $env:BAZEL_SH"
}
$env:DOTNET_ROOT = $dotnet
$env:DOTNET_ROOT_x64 = $dotnet
$env:DOTNET_CLI_HOME = $dotnetHome
$env:NUGET_PACKAGES = $nuget
$env:PATH = "$dotnet;$env:PATH"

# Use the version pinned by .bazeliskrc.  A Bazelisk command is preferred;
# otherwise use its already-downloaded pinned binary from the local cache.
$versionLine = Get-Content -LiteralPath '.bazeliskrc' |
  Where-Object { $_ -match '^USE_BAZEL_VERSION=' } |
  Select-Object -First 1
if (-not $versionLine) { throw 'USE_BAZEL_VERSION is missing from .bazeliskrc' }
$bazelVersion = ($versionLine -split '=', 2)[1].Trim()
$bazelCommand = Get-Command bazelisk -ErrorAction SilentlyContinue
if ($bazelCommand) {
  $bazel = $bazelCommand.Source
} else {
  $metadata = Join-Path $env:LOCALAPPDATA "bazelisk\downloads\metadata\bazelbuild\bazel-$bazelVersion-windows-x86_64"
  if (-not (Test-Path -LiteralPath $metadata)) {
    throw "Bazelisk is not installed and pinned Bazel metadata is missing: $metadata"
  }
  $downloadHash = (Get-Content -LiteralPath $metadata -Raw).Trim()
  $bazel = Join-Path $env:LOCALAPPDATA "bazelisk\downloads\sha256\$downloadHash\bin\bazel.exe"
}
if (-not (Test-Path -LiteralPath $bazel)) {
  throw "Pinned Bazel executable was not found: $bazel"
}

# Keep the desktop responsive during a release build.
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch { }

$bazelArgs = @(
  '--nowindows_enable_symlinks',
  "--output_user_root=$bazelRoot",
  'build',
  '--jobs=2',
  '--local_resources=cpu=2',
  '--local_resources=memory=2048',
  '--loading_phase_threads=1',
  '--config=release_build',
  '--platforms=//:windows-x86_64',
  "--repo_env=PATH=$dotnet",
  "--repo_env=BAZEL_VC=$env:BAZEL_VC",
  "--repo_env=DOTNET_ROOT=$dotnet",
  "--repo_env=DOTNET_ROOT_x64=$dotnet",
  "--repo_env=DOTNET_CLI_HOME=$dotnetHome",
  "--repo_env=NUGET_PACKAGES=$nuget",
  "--repo_env=DOTNET_TOOL_SOURCE=$wixSource",
  '--action_env=BAZEL_VC',
  '--action_env=BAZEL_LLVM',
  '--action_env=BAZEL_SH',
  '--copt=/D_CRT_USE_BUILTIN_OFFSETOF',
  '--host_copt=/D_CRT_USE_BUILTIN_OFFSETOF',
  '//win32/installer:installer'
)
& $bazel @bazelArgs
if ($LASTEXITCODE -ne 0) {
  throw "Bazel build failed: $LASTEXITCODE"
}
```

`--jobs=2` と `BelowNormal` 相当の低負荷設定を使うため、リリース前の
ビルドでも CPU 使用率を抑えられる。`//win32/installer:installer` だけを
指定するため、`package` 全体より余計なターゲットを作らない。Bazel の出力
ルートは毎回消さず、`version.bzl` の変更は入力として自動的に再ビルドさせる。
起動オプションやツールチェーンを変えた場合だけ、次を実行してから再試行する。

```powershell
& $bazel "--output_user_root=$bazelRoot" shutdown
```

リリース番号を上げる場合は `MOZKEY_SPACE_RELEASE_VERSION_PATCH` と
`BUILD_OSS` を更新し、MSI の内部版（`MAJOR.MINOR.BUILD.REVISION`）も以前の
成果物より大きくする。生成後は、少なくとも次を確認する。

```powershell
$msi = Join-Path $src 'bazel-bin\win32\installer\Mozc64.msi'
if (-not (Test-Path $msi)) { throw "MSI was not produced: $msi" }
& "$src\win32\installer\verify_msi_post_install_dialog.ps1" -MsiPath $msi
$installer = New-Object -ComObject WindowsInstaller.Installer
$db = $installer.OpenDatabase((Resolve-Path -LiteralPath $msi).Path, 0)
$view = $db.OpenView('SELECT `Value` FROM `Property` WHERE `Property`=''ProductVersion''')
$view.Execute()
$record = $view.Fetch()
$productVersion = $record.StringData(1)
$view.Close()
Write-Output "ProductVersion=$productVersion"
Get-FileHash -Algorithm SHA256 $msi
```

`verify_msi_post_install_dialog.ps1` は実際にインストールせず、完了後の
カスタムアクションが存在し、通常の対話型インストールでだけ実行され、
アンインストール時には実行されないことを MSI 内部から検査する。

よくある失敗と対処は次のとおり。

* `VCVARSALL.BAT` や `cl.exe` が見つからない場合は、`BAZEL_VC` が
  `C:\BuildTools\VC` のような `VC` ディレクトリか確認する。
* WiX の復元が .NET SDK 不在で失敗する場合は、`dotnet.exe` を含む SDK の
  パスを `PATH` と `repo_env` の両方に設定する。
* シンボリックリンク権限で失敗する場合は、上記の
  `--nowindows_enable_symlinks` を付ける。Developer Mode を有効にする必要は
  ない。
* `__mm_*` の未解決シンボルが x86 TIP リンクで出る場合は、ソースの
  `base/clang_intrinsics_compat.cc` が x86 用の COFF 装飾名を含む版か確認する。
  これはキャッシュ削除で直る種類のエラーではない。
* 外部 protobuf などのホストツールで `_mm_*` の未解決シンボルが出る場合は、
  `src/.bazelrc` の `--host_platform=@platforms//host` を維持する。製品の
  ターゲットは bundled clang-cl、生成ツールはネイティブ MSVC という分離にする。

環境の配置は次のように固定する。`local-build-env` は再利用するツール類、
`local-build-cache-bazel-windows` は再生成可能な Bazel キャッシュ、
`local-build-v4.60.3-20260905` のような版付きディレクトリは確認済み成果物
として扱う。旧ビルドや診断ログを同じ場所へ戻さない。

#### Install Mozc

After building Mozc, run the following command to install it:

```
python build_tools/open.py bazel-bin/win32/installer/Mozc64.msi
```

#### Uninstall Mozc

To Uninstall Mozc, press <kbd>Win</kbd>+<kbd>R</kbd> to open the Run dialog and
type `ms-settings:appsfeatures-app`, run the following command in the terminal:

```
start ms-settings:appsfeatures-app
```

Then, uninstall `Mozc` from the list of installed applications.

### Cross compilation

By default, `Mozc64.msi` is built for the host CPU architecture. To explicitly
specify the target CPU architecture, specify build options as follows:

#### To build x64 installer

```
python build_tools/build_qt.py --release --confirm_license --target_arch=x64
bazelisk build package --config release_build --platforms=//:windows-x86_64
```

#### To build ARM64 installer

```
python build_tools/build_qt.py --release --confirm_license --target_arch=arm64
bazelisk build package --config release_build --platforms=//:windows-arm64
```

#### To build a universal installer for both X64 and ARM64

```
python build_tools/build_qt.py --release --confirm_license --target_arch=x64
bazelisk build package --config release_build --platforms=//:windows-x86_64 --config win_universal_installer
```

## Bazel command examples

### Bazel User Guide

*   [Build programs with Bazel](https://bazel.build/run/build)
*   [Commands and Options](https://bazel.build/docs/user-manual)
*   [Write bazelrc configuration files](https://bazel.build/run/bazelrc)

### Run all tests

```
bazelisk test ... --build_tests_only -c dbg
```

> [!NOTE] `...` means all targets under the current and subdirectories.

--------------------------------------------------------------------------------

## Build with GitHub Actions

GitHub Actions are already set up in
[windows.yaml](../.github/workflows/windows.yaml). With that, you can build and
install Mozc with your own commit as follows.

1.  Fork https://github.com/google/mozc to your GitHub repository.
2.  Push a new commit to your own fork.
3.  Click "Actions" tab on your fork.
4.  Wait until the action triggered by your commit succeeds.
5.  Download `Mozc64.msi` from the action result page.
6.  Install `Mozc64.msi`.

Files on the GitHub Actions page remain available for up to 90 days.

You can also find Mozc Installers for Windows in the google/mozc repository.
Please keep in mind that Mozc is not an officially supported Google product,
even if downloaded from https://github.com/google/mozc/.

1.  Sign in GitHub.
2.  Check
    [recent successful Windows runs](https://github.com/google/mozc/actions/workflows/windows.yaml?query=is%3Asuccess)
    in the google/mozc repository.
3.  Find an action from the last 90 days and click it.
4.  Download `Mozc64.msi` from the action result page if you are using 64-bit
    Windows.

--------------------------------------------------------------------------------

## Build with GYP (deprecated):

⚠️ The GYP build is deprecated and no longer supported.

Please check the previous version for more information.
https://github.com/google/mozc/blob/3.33.6089/docs/build_mozc_in_windows.md#build-with-gyp-maintenance-mode
