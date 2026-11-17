"""Read-only repository diagnostic: pinned LLVM, existing probes, no Qt or MSI."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'src'))
sys.path.insert(0, str(ROOT / 'src/build_tools'))
from build_tools import mozc_version, vs_util
import update_deps

CASES = ('llvm20-default', 'llvm20-msvc1444', 'llvm22-default')
LLVM22 = update_deps.ArchiveInfo(
    url='https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/clang+llvm-22.1.8-aarch64-pc-windows-msvc.tar.xz',
    size=737955068,
    sha256='de718c58ebbc5f61d58c17b90457fcf42983bc2c4a4aba3e010d108713bfd7f1',
)


def run_stage(name, command, output, env=None, timeout=600):
    """Keep failure visible while allowing independent comparison stages to run."""
    with (output / (name + '.log')).open('w', encoding='utf-8') as log:
        try:
            completed = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, check=False, timeout=timeout)
            return {'name': name, 'exit_code': completed.returncode,
                    'status': 'passed' if completed.returncode == 0 else 'failed'}
        except subprocess.TimeoutExpired:
            log.write('\nDiagnostic stage timed out.\n')
            return {'name': name, 'status': 'timeout'}
        except OSError as error:
            log.write(f'\nStage could not start: {error}\n')
            return {'name': name, 'status': 'failed-to-start'}


def compare(case, output):
    if case not in CASES:
        raise ValueError('Unknown comparison case')
    expected_scope = {'GITHUB_REPOSITORY': 'over-keys/mozkey-space',
                      'GITHUB_REF': 'refs/heads/integration/upstream-1.1.0',
                      'GITHUB_EVENT_NAME': 'push', 'PROCESSOR_ARCHITECTURE': 'ARM64'}
    if any(os.environ.get(key) != value for key, value in expected_scope.items()):
        raise RuntimeError('Unexpected diagnostic execution scope')
    sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if sha != os.environ['GITHUB_SHA']:
        raise RuntimeError('Checkout identity mismatch')
    output.mkdir(parents=True, exist_ok=True)
    archive = LLVM22 if case == 'llvm22-default' else update_deps.LLVM_WIN_ARM64
    report = {'case': case, 'source_sha': sha, 'llvm': {'url': archive.url, 'sha256': archive.sha256, 'size': archive.size},
              'msvc': os.environ['BAZEL_VC_FULL_VERSION'], 'stages': [],
              'comparison_scope': 'Same integrated checkout with alternate toolchains; not a rebuild of historical v1.0.1.',
              'limitations': 'Diagnostic only; no Qt, MSI, product test suite or release validation. Compare runner image and selected SDK versions before attributing differences.'}
    try:
        report['selected_environments'] = {}
        for architecture in ('arm64', 'amd64_arm64', 'amd64_x86', 'amd64'):
            selected = vs_util.get_vs_env_vars(architecture)
            report['selected_environments'][architecture] = {
                key: selected.get(key, '') for key in
                ('VCTOOLSVERSION', 'VCTOOLSINSTALLDIR', 'WINDOWSSDKVERSION', 'WINDOWSSDKDIR')}
        # Both download size and SHA-256 are checked by the existing dependency code.
        # Only LLVM is downloaded/extracted; no model, dictionary, Qt, WiX or MSYS2.
        update_deps.download(archive)
        update_deps.extract_llvm(archive)
        compiler = ROOT / 'src/third_party/llvm/bin/clang-cl.exe'
        report['stages'].append(run_stage('compiler-version', [str(compiler), '--version'], output, timeout=30))
        report['stages'].append(run_stage('crt-stl-atl-all-targets', [sys.executable, str(ROOT / 'tools/release/verify_windows_arm64_toolchain.py')], output, timeout=1200))
        # Exercise the repository's real MSVC/RC ARM64EC + ARM64 forwarder generator.
        # Call its Python entry point directly, avoiding Bazel and any Qt downloads.
        version_file = output / 'version.txt'
        mozc_version.GenerateVersionFile(str(ROOT / 'src/version.bzl'), str(version_file), 'Windows', 'stable')
        env = os.environ.copy()
        env['PYTHONPATH'] = str(ROOT / 'src')
        report['stages'].append(run_stage('arm64x-forwarder', [sys.executable,
            str(ROOT / 'src/win32/tip/build_tip_forwarder_dll.py'),
            '--version_file', str(version_file), '--branding', 'Mozc',
            '--output', str(output / 'diagnostic-forwarder.dll')], output, env=env, timeout=180))
        return 0 if all(stage['status'] == 'passed' for stage in report['stages']) else 1
    except Exception as error:
        report['setup_error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        (output / 'comparison.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--case', choices=CASES, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(compare(args.case, args.output))
