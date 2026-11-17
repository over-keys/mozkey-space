"""Inspect official runner CRTs and test app-local loading; no product build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'src'))
from build_tools import vs_util
from win32.installer import build_installer

SOURCE = r'''
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
int wmain() {
  try { throw std::vector<int>{14, 52}; }
  catch (const std::vector<int>& v) { if (v.at(1) != 52) return 10; }
  wchar_t exe[32768];
  if (!GetModuleFileNameW(nullptr, exe, 32768)) return 11;
  std::wstring directory(exe);
  directory.resize(directory.find_last_of(L"\\/") + 1);
  const wchar_t* names[] = {L"msvcp140.dll", L"msvcp140_1.dll",
    L"msvcp140_2.dll", L"vcruntime140.dll",
#ifndef _M_ARM64
    L"vcruntime140_1.dll",
#endif
  };
  for (const auto name : names) {
    const auto expected = directory + name;
    HMODULE module = LoadLibraryExW(expected.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { wprintf(L"LOAD_FAILED %ls error=%lu\n", name, GetLastError()); return 12; }
    wchar_t actual[32768];
    if (!GetModuleFileNameW(module, actual, 32768) || _wcsicmp(actual, expected.c_str())) return 13;
    wprintf(L"LOADED_APP_LOCAL %ls\n", actual);
#ifndef _M_ARM64
    if (!_wcsicmp(name, L"vcruntime140_1.dll") && !GetProcAddress(module, "__CxxFrameHandler4")) return 14;
#endif
  }
  return 0;
}
'''

# Use the same signature policy as the production MSI selector.
SIGNATURE = build_installer.CRT_SIGNATURE_SCRIPT
validate_signature = build_installer.validate_crt_signature
APPROVED_SIGNATURE_SUBJECTS = build_installer.APPROVED_SIGNATURE_SUBJECTS


def run(command, log, env, cwd, timeout=60):
    with log.open('w', encoding='utf-8') as stream:
        subprocess.run(command, cwd=cwd, env=env, stdout=stream,
                       stderr=subprocess.STDOUT, check=True, timeout=timeout)


def diagnose(output):
    scope = {'GITHUB_REPOSITORY': 'over-keys/mozkey-space',
             'GITHUB_REF': 'refs/heads/integration/upstream-1.1.0',
             'GITHUB_EVENT_NAME': 'push', 'PROCESSOR_ARCHITECTURE': 'ARM64'}
    if any(os.environ.get(k) != v for k, v in scope.items()):
        raise RuntimeError('Unexpected diagnostic scope')
    sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if sha != os.environ['GITHUB_SHA']:
        raise RuntimeError('Checkout identity mismatch')
    output.mkdir(parents=True, exist_ok=True)
    report = {'source_sha': sha, 'image': os.environ.get('ImageVersion'), 'files': [], 'probes': [],
              'limitations': 'Runner-installed default MSVC only. No LLVM20, Qt, MSI, product or release validation. CHPE dump is evidence, not a new acceptance policy.'}
    try:
        environments = {a: vs_util.get_vs_env_vars(a) for a in ('arm64', 'amd64')}
        env = environments['amd64']
        report['compiler'] = env.get('VCTOOLSVERSION')
        report['sdk'] = env.get('WINDOWSSDKVERSION')
        dumpbin = shutil.which('dumpbin.exe', path=env['PATH'])
        if not dumpbin:
            raise RuntimeError('Selected dumpbin not found')
        redist = Path(env['VCTOOLSREDISTDIR'])
        directories = {a: build_installer.find_redist_crt_dir(redist, a) for a in ('x64', 'arm64')}
        for arch, directory in directories.items():
            for name in build_installer.REDIST_CRT_FILES:
                dll = directory / name
                data = dll.read_bytes()
                offset = int.from_bytes(data[60:64], 'little')
                if data[:2] != b'MZ' or data[offset:offset+4] != b'PE\0\0':
                    raise ValueError(f'Invalid PE: {dll}')
                row = {'arch': arch, 'name': name, 'path': str(dll), 'sha256': hashlib.sha256(data).hexdigest(),
                       'machine': hex(int.from_bytes(data[offset+4:offset+6], 'little'))}
                report['files'].append(row)
                run([dumpbin, '/headers', '/loadconfig', '/exports', '/imports', str(dll)],
                    output / f'{arch}-{name}-pe.log', env, ROOT)
                signature_env = env.copy()
                signature_env['MOZKEY_CRT_FILE'] = str(dll)
                signature_log = output / f'{arch}-{name}-signature.log'
                run(['pwsh', '-NoProfile', '-NonInteractive', '-Command', SIGNATURE], signature_log, signature_env, ROOT)
                row['signature'] = json.loads(signature_log.read_text(encoding='utf-8-sig'))
                validate_signature(row['signature'])
        # Both native ARM64 and emulated x64 consume the *same* ARM64 package.
        # The x64 package is an independent control. No installed DLL is replaced.
        for target, bundle in (('arm64', 'arm64'), ('amd64', 'arm64'), ('amd64', 'x64')):
            name = f'{target}-with-{bundle}-crt'
            work = output / name
            work.mkdir()
            for dll in build_installer.REDIST_CRT_FILES:
                shutil.copy2(directories[bundle] / dll, work / dll)
            source = work / 'probe.cc'
            source.write_text(SOURCE, encoding='utf-8')
            env = environments[target]
            compiler = shutil.which('cl.exe', path=env['PATH'])
            if not compiler:
                raise RuntimeError('Selected compiler not found')
            run([compiler, '/nologo', '/EHsc', '/MD', str(source), '/Fe' + str(work / 'probe.exe')],
                output / f'{name}-compile.log', env, work, timeout=120)
            run([str(work / 'probe.exe')], output / f'{name}-run.log', env, work, timeout=30)
            report['probes'].append({'target': target, 'bundle': bundle, 'status': 'passed'})
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        (output / 'crt-layout.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    diagnose(parser.parse_args().output.resolve())
