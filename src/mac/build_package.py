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

"""Build a package for macOS (.pkg file).

This script creates a .pkg file with pkgbuild and productbuild
"""

import argparse
import os
import plistlib
import shutil
import tempfile

from build_tools import mozc_version
from build_tools import util


def ParseArguments():
  """Parses command line options."""
  parser = argparse.ArgumentParser()
  parser.add_argument('--input')
  parser.add_argument('--output')
  parser.add_argument('--version_file')
  parser.add_argument('--oss', action='store_true')
  parser.add_argument(
      '--codesign_identity',
      default='-',
      # Note: '-' is used as a pseudo identity for /usr/bin/codesign.
      help='Code signing identity. Use "-" to skip codesigning.',
  )
  parser.add_argument('--keychain')
  return parser.parse_args()


def main():
  args = ParseArguments()

  package_version = None
  if args.oss:
    identifier = 'org.mozc.pkg.JapaneseInput'
    pkg_name = 'Mozc.pkg'
    if not args.version_file:
      raise ValueError('--version_file is required for Mozkey packages')
    version_file = os.path.abspath(args.version_file)
    version = mozc_version.MozcVersion(version_file)
    package_version = version.GetVersionInFormat(
        '@MOZKEY_SPACE_RELEASE_VERSION_MAJOR@.'
        '@MOZKEY_SPACE_RELEASE_VERSION_MINOR@.'
        '@MOZKEY_SPACE_RELEASE_VERSION_PATCH@'
    )
    parts = package_version.split('.')
    if len(parts) != 3 or not all(part.isdigit() for part in parts):
      raise ValueError(
          f'Invalid Mozkey package version: {package_version!r}'
      )
  else:
    identifier = 'com.google.pkg.GoogleJapaneseInput'
    pkg_name = 'GoogleJapaneseInput.pkg'

  output_path = os.path.abspath(args.output)

  with tempfile.TemporaryDirectory() as tmp_dir:
    # Use the unzip command to extract symbolic links properly.
    util.RunOrDie(['unzip', '-q', args.input, '-d', tmp_dir])
    os.chdir(os.path.join(tmp_dir, 'installer'))

    # Generate a component property list from the root directory. Clear
    # BundleOverwriteAction to avoid atomic bundle replacement, which can
    # invalidate macOS IME registration. For Mozkey, also disable bundle
    # version checking so reinstall and downgrade installs remain possible.
    # https://github.com/google/mozc/issues/1439
    component_plist = 'component.plist'
    util.RunOrDie(
        ['/usr/bin/pkgbuild', '--analyze', '--root', 'root', component_plist]
    )
    with open(component_plist, 'rb') as f:
      components = plistlib.load(f)
    for component in components:
      component['BundleOverwriteAction'] = ''
      if args.oss:
        component['BundleIsVersionChecked'] = False
    with open(component_plist, 'wb') as f:
      plistlib.dump(components, f)

    pkgbuild_commands = [
        '/usr/bin/pkgbuild',
        '--root',
        'root',
        '--component-plist',
        component_plist,
        '--identifier',
        identifier,
    ]
    if package_version is not None:
      pkgbuild_commands += ['--version', package_version]
    pkgbuild_commands += [
        '--scripts',
        'scripts/',
        pkg_name,  # pkg_name is configured in distribution.xml.
    ]
    util.RunOrDie(pkgbuild_commands)

    if package_version is not None:
      distribution_path = 'distribution.xml'
      with open(distribution_path, encoding='utf-8') as f:
        distribution = f.read()
      old_ref = (
          f'<pkg-ref id="{identifier}" version="0" '
          f'onConclusion="none">{pkg_name}</pkg-ref>'
      )
      new_ref = (
          f'<pkg-ref id="{identifier}" version="{package_version}" '
          f'onConclusion="none">{pkg_name}</pkg-ref>'
      )
      if distribution.count(old_ref) != 1:
        raise ValueError('Mozkey distribution package reference is not unique')
      with open(distribution_path, 'w', encoding='utf-8') as f:
        f.write(distribution.replace(old_ref, new_ref, 1))

    productbuild_commands = [
        '/usr/bin/productbuild',
        '--distribution',
        'distribution.xml',
        '--plugins',
        'Plugins/',
        '--resources',
        'Resources/',
        'package.pkg',  # this name is only used within this script.
    ]
    util.RunOrDie(productbuild_commands)

    # codesign the package and copy it to the output path.
    if args.codesign_identity == '-':
      shutil.copyfile('package.pkg', output_path)
    else:
      keychain_path = os.path.join(
          os.getenv('HOME'), 'Library/Keychains', args.keychain
      )
      codesign_commands = [
          '/usr/bin/productsign',
          '--sign',
          args.codesign_identity,
          '--keychain',
          keychain_path,
          'package.pkg',
          output_path,
      ]
      util.RunOrDie(codesign_commands)


if __name__ == '__main__':
  main()
