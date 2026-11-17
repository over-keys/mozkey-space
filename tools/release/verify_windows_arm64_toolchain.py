"""Compile, link, and run a tiny CRT/STL/ATL probe before the expensive Qt build."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'src/build_tools'))
import vs_util

SOURCE = '''#include <vector>
#include <atlbase.h>
int main() {
  ATL::CComPtr<IUnknown> pointer;
  std::vector<int> values{14, 44};
  return pointer == nullptr && values.at(1) == 44 ? 0 : 1;
}
'''


def verify():
    if os.environ.get('PROCESSOR_ARCHITECTURE', '').upper() != 'ARM64':
        raise RuntimeError('This probe requires a native ARM64 Windows runner')
    expected = os.environ['BAZEL_VC_FULL_VERSION']
    # ARM64 MSI also contains x86 and x64 TIP DLLs. Probe every target and
    # both Qt's native ARM64 and Bazel's cross-host ARM64 environment.
    targets = (
        ('arm64', 'aarch64', 'arm64'),
        ('amd64_arm64', 'aarch64', 'arm64'),
        ('amd64_x86', 'i686', 'x86'),
        ('amd64', 'x86_64', 'x64'),
    )
    for architecture, triple, machine in targets:
        env = vs_util.get_vs_env_vars(architecture)
        if env.get('VCTOOLSVERSION', '').rstrip('/\\') != expected:
            raise RuntimeError('Unexpected active MSVC version')
        compiler = ROOT / 'src/third_party/llvm/bin/clang-cl.exe'
        for crt in ('/MT', '/MD'):
            print(f'TOOLCHAIN_PROBE architecture={architecture} crt={crt}', flush=True)
            with tempfile.TemporaryDirectory(prefix='mozkey-arm64-probe-') as directory:
                work = Path(directory)
                source = work / 'probe.cc'
                binary = work / 'probe.exe'
                source.write_text(SOURCE, encoding='utf-8')
                subprocess.run([str(compiler), '/nologo', '/EHsc', crt, '-fuse-ld=lld',
                                f'--target={triple}-pc-windows-msvc', str(source),
                                '/Fe' + str(binary), '/link', f'/machine:{machine}',
                                'ole32.lib', 'oleaut32.lib'],
                               cwd=work, env=env, check=True, timeout=120)
                subprocess.run([str(binary)], cwd=work, env=env, check=True, timeout=30)
    print('ARM64_CRT_STL_ATL_PROBE=PASS')


if __name__ == '__main__':
    verify()
