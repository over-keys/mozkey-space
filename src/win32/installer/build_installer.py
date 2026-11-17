# -*- coding: utf-8 -*-
# Copyright 2010-2021, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Runs wix.exe to build the installer."""

import argparse
import hashlib
import os
import pathlib
import re
import subprocess

from build_tools import mozc_version
from build_tools import vs_util


def exec_command(args: list[str], cwd: str) -> None:
  """Runs the given command then returns the output.

  Args:
    args: The command to be executed.
    cwd: The current working directory.

  Raises:
    ChildProcessError: When the given command cannot be executed.
  """
  process = subprocess.Popen(
      args,
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
      shell=False,
      text=True,
      encoding='utf-8',
      cwd=cwd)
  stdout, stderr = process.communicate()
  exitcode = process.wait()

  if exitcode != 0:
    command = ' '.join(args)
    msgs = ['Failed to execute command:', '', command, '', f'cwd={cwd}']
    if stdout:
      msgs += ['-----stdout-----', stdout]
    if stderr:
      msgs += ['-----stderr-----', stderr]
    raise ChildProcessError('\n'.join(msgs))


REDIST_CRT_FILES = (
    'msvcp140.dll',
    'msvcp140_1.dll',
    'msvcp140_2.dll',
    'vcruntime140.dll',
    'vcruntime140_1.dll',
)


def find_redist_crt_dir(redist_root: pathlib.Path, arch: str) -> pathlib.Path:
    if arch not in ('x64', 'arm64'):
        raise ValueError(f'Unsupported installer CRT architecture: {arch}')
    arch_dir = redist_root.joinpath(arch).resolve()
    required = REDIST_CRT_FILES

    def is_valid_dir(path: pathlib.Path) -> bool:
        return path.is_dir() and all(path.joinpath(name).is_file() for name in required)

    # Only the standard layout is supported. Never silently select arbitrary
    # nested DLLs or resolve multiple complete runtimes by filesystem ordering.
    candidates = sorted(
        p for p in arch_dir.glob('Microsoft.VC*.CRT')
        if re.fullmatch(r'Microsoft\.VC[0-9]+\.CRT', p.name) and p.is_dir()
    )
    if len(candidates) == 1 and is_valid_dir(candidates[0]):
        return candidates[0]
    if len(candidates) > 1:
        raise ValueError(f'Ambiguous CRT redistributable directories: {candidates}')

    raise FileNotFoundError(
        f'Could not find CRT redistributable directory containing {required} under: {arch_dir}'
    )


def validate_redist_crt_architecture(directory: pathlib.Path, arch: str) -> None:
  # ARM64X final images retain the native ARM64 PE machine type. An x64/EC-only
  # image must not be packaged as the native ARM64 runtime.
  expected = {'x64': 0x8664, 'arm64': 0xAA64}[arch]
  for name in REDIST_CRT_FILES:
    dll = directory / name
    with dll.open('rb') as binary:
      header = binary.read(64)
      if len(header) != 64 or header[:2] != b'MZ':
        raise ValueError(f'Invalid CRT PE header: {dll}')
      offset = int.from_bytes(header[60:64], 'little')
      if offset < 64 or offset > dll.stat().st_size - 6:
        raise ValueError(f'Invalid CRT PE offset: {dll}')
      binary.seek(offset)
      signature = binary.read(6)
      if signature[:4] != b'PE\0\0':
        raise ValueError(f'Invalid CRT PE signature: {dll}')
      machine = int.from_bytes(signature[4:6], 'little')
      if machine != expected:
        raise ValueError(f'CRT architecture mismatch: {dll}: {machine:#06x}, expected {expected:#06x}')


def get_redist_crt_dir(arch: str, vcvarsall_hint=None) -> pathlib.Path:
  # The installed redistributable can be newer than the selected compiler.
  # VCTOOLSVERSION=14.44 does not imply a VC143 folder inside a 14.52 redist.
  vs_env = vs_util.get_vs_env_vars('x64', vcvarsall_hint)
  redist_root = pathlib.Path(vs_env['VCTOOLSREDISTDIR']).resolve()
  directory = find_redist_crt_dir(redist_root, arch)
  validate_redist_crt_architecture(directory, arch)
  for name in REDIST_CRT_FILES:
    digest = hashlib.sha256((directory / name).read_bytes()).hexdigest()
    print(f'INSTALLER_CRT_SHA256 arch={arch} file={directory / name} sha256={digest}', flush=True)
  return directory


def run_wix4(args) -> None:
  """Run 'dotnet tool run wix build ...'.

  Args:
    args: args
  """
  arch = args.arch

  vcvarsall_hint = None
  if args.vs_install_dir:
    vcvarsall_hint = str(
        pathlib.Path(args.vs_install_dir).joinpath(
            'Auxiliary', 'Build', 'vcvarsall.bat'
        )
    )

  redist_64bit = get_redist_crt_dir(arch, vcvarsall_hint)
  print(f'INSTALLER_CRT_DIRECTORY={redist_64bit}', flush=True)
  version_file = pathlib.Path(args.version_file).resolve()
  version = mozc_version.MozcVersion(version_file)
  credit_file = pathlib.Path(args.credit_file).resolve()
  document_dir = credit_file.parent
  qt_dir = pathlib.Path(args.qt_core_dll).parent.parent.resolve()
  icon_path = pathlib.Path(args.icon_path).resolve()
  mozc_tip32 = pathlib.Path(args.mozc_tip32).resolve()
  mozc_tip64 = pathlib.Path(args.mozc_tip64).resolve()
  mozc_broker = pathlib.Path(args.mozc_broker).resolve()
  mozc_server = pathlib.Path(args.mozc_server).resolve()
  mozc_cache_service = pathlib.Path(args.mozc_cache_service).resolve()
  mozc_renderer = pathlib.Path(args.mozc_renderer).resolve()
  mozc_tool = pathlib.Path(args.mozc_tool).resolve()
  custom_action = pathlib.Path(args.custom_action).resolve()

  zenz_scorer = pathlib.Path(args.zenz_scorer).resolve()
  zenz_llama_server = pathlib.Path(args.zenz_llama_server).resolve()
  zenz_model = pathlib.Path(args.zenz_model).resolve()

  wix_path = pathlib.Path(args.wix_path).resolve()

  branding = args.branding
  upgrade_code = ''
  omaha_guid = ''
  omaha_client_key = ''
  omaha_clientstate_key = ''
  if branding == 'GoogleJapaneseInput':
    upgrade_code = 'C1A818AF-6EC9-49EF-ADCF-35A40475D156'
    omaha_guid = 'DDCCD2A9-025E-4142-BCEB-F467B88CF830'
    omaha_client_key = f'Software\\Google\\Update\\Clients\\{{{omaha_guid}}}'
    omaha_clientstate_key = (
        f'Software\\Google\\Update\\ClientState\\{{{omaha_guid}}}'
    )
  elif branding == 'Mozc':
    upgrade_code = 'DD94B570-B5E2-4100-9D42-61930C611D8A'

  # Keep the original Mozc build identity separate from the installer version.
  msi_version = (version.GetMozkeyMsiVersion() if branding == 'Mozc'
                 else version.GetVersionString())
  omaha_channel_type = 'dev' if version.IsDevChannel() else 'stable'
  vs_configuration_name = 'Debug' if args.debug_build else 'Release'

  commands = [
      f'{wix_path}',
      'build',
      '-nologo',
      '-arch', arch,
      '-define', f'MozcVersion={version.GetVersionString()}',
      '-define', f'MsiVersion={msi_version}',
      '-define', f'UpgradeCode={upgrade_code}',
      '-define', f'OmahaGuid={omaha_guid}',
      '-define', f'OmahaClientKey={omaha_client_key}',
      '-define', f'OmahaClientStateKey={omaha_clientstate_key}',
      '-define', f'OmahaChannelType={omaha_channel_type}',
      '-define', f'VSConfigurationName={vs_configuration_name}',
      '-define', f'ReleaseRedistCrt64Dir={redist_64bit}',
      '-define', f'AddRemoveProgramIconPath={icon_path}',
      '-define', f'MozcTIP32Path={mozc_tip32}',
      '-define', f'MozcTIP64Path={mozc_tip64}',
      '-define', f'MozcBroker64Path={mozc_broker}',
      '-define', f'MozcServer64Path={mozc_server}',
      '-define', f'MozcCacheService64Path={mozc_cache_service}',
      '-define', f'MozcRenderer64Path={mozc_renderer}',
      '-define', f'MozcToolPath={mozc_tool}',
      '-define', f'CustomActions64Path={custom_action}',
      '-define', f'ZenzScorerPath={zenz_scorer}',
      '-define', f'ZenzLlamaServerPath={zenz_llama_server}',
      '-define', f'ZenzModelPath={zenz_model}',
      '-define', f'DocumentsDir={document_dir}',
      '-define', f'QtDir={qt_dir}',
      '-define', 'QtVer=6',
      '-out', args.output,
      '-src', args.wxs_path,
  ]
  if args.mozc_tip64arm and args.mozc_tip64x:
    mozc_tip64arm = pathlib.Path(args.mozc_tip64arm).resolve()
    mozc_tip64x = pathlib.Path(args.mozc_tip64x).resolve()
    commands += [
        '-define', f'MozcTIP64ArmPath={mozc_tip64arm}',
        '-define', f'MozcTIP64XPath={mozc_tip64x}',
    ]
    if args.enable_win_universal_installer:
      commands += ['-define', 'MozcUniversalInstaller=Yes']
  exec_command(commands, cwd=os.getcwd())


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--output', type=str)
  parser.add_argument('--version_file', type=str)
  parser.add_argument('--mozc_tool', type=str)
  parser.add_argument('--mozc_renderer', type=str)
  parser.add_argument('--mozc_server', type=str)
  parser.add_argument('--mozc_broker', type=str)
  parser.add_argument('--mozc_cache_service', type=str)
  parser.add_argument('--mozc_tip32', type=str)
  parser.add_argument('--mozc_tip64', type=str)
  parser.add_argument('--mozc_tip64arm', type=str)
  parser.add_argument('--mozc_tip64x', type=str)
  parser.add_argument('--custom_action', type=str)
  parser.add_argument('--zenz_scorer', type=str)
  parser.add_argument('--zenz_llama_server', type=str)
  parser.add_argument('--zenz_model', type=str)
  parser.add_argument('--icon_path', type=str)
  parser.add_argument('--credit_file', type=str)
  parser.add_argument('--qt_core_dll', type=str)
  parser.add_argument('--wxs_path', type=str)
  parser.add_argument('--wix_path', type=str)
  parser.add_argument('--branding', type=str)
  parser.add_argument(
      '--debug_build', dest='debug_build', default=False, action='store_true'
  )
  parser.add_argument(
      '--arch',
      dest='arch',
      default='x64',
      choices=['x64', 'arm64'],
  )
  parser.add_argument(
      '--enable_win_universal_installer',
      dest='enable_win_universal_installer',
      default=False,
      action='store_true',
  )
  parser.add_argument(
      '--vs_install_dir',
      type=str,
      default='',
      help='Path to the Visual Studio VC directory (same as BAZEL_VC).',
  )

  args = parser.parse_args()

  run_wix4(args)


if __name__ == '__main__':
  main()
