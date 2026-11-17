"""Native toolset selection regression tests; no Windows runner required."""
import importlib.util
import json
import os
import sys
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('vs_util', ROOT / 'src/build_tools/vs_util.py')
vs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vs)


class WindowsToolchainTest(unittest.TestCase):
    def invoke(self, env):
        process = mock.Mock()
        process.communicate.return_value = (json.dumps({'VCTOOLSVERSION': '14.44.35207'}).encode(), None)
        process.wait.return_value = 0
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(vs, 'get_vcvarsall', return_value=Path('VC/Auxiliary/Build/vcvarsall.bat')) as find, mock.patch.object(vs.subprocess, 'Popen', return_value=process) as run:
            result = vs.get_vs_env_vars('arm64')
            return result, find.call_args, run.call_args

    def test_unpinned_behavior_unchanged(self):
        _, find, run = self.invoke({})
        self.assertEqual(find.args, ('arm64', None))
        self.assertNotIn('-vcvars_ver', run.args[0])

    def test_qt_shares_exact_bazel_toolset(self):
        result, find, run = self.invoke({'BAZEL_VC': 'selected/VC', 'BAZEL_VC_FULL_VERSION': '14.44.35207'})
        self.assertEqual(find.args, ('arm64', str(Path('selected/VC/Auxiliary/Build/vcvarsall.bat'))))
        self.assertIn('arm64 -vcvars_ver=14.44.35207>nul', run.args[0])
        self.assertEqual(result['VCTOOLSVERSION'], '14.44.35207')

    def test_invalid_version_rejected_before_shell(self):
        for version in ('14.44&whoami', '14.44\n', 'latest', '14'):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.invoke({'BAZEL_VC_FULL_VERSION': version})

    def test_workflow_activation_checks_windows_uppercase_environment(self):
        # Windows os.environ uppercases keys. JSON restores an ordinary,
        # case-sensitive dict, not Windows' case-insensitive environment mapping.
        text = (ROOT / '.github/workflows/windows-release-candidate.yml').read_text()
        line = next(line.strip() for line in text.splitlines()
                    if 'ARM64_MSVC_TOOLSET=' in line)
        code = line[len('python -c "'):-1]
        for actual in ('14.44.35207', '14.44.35207/', '14.44.35207' + chr(92)):
            with self.subTest(actual=actual), mock.patch.dict(os.environ, {'BAZEL_VC_FULL_VERSION': '14.44.35207'}, clear=True), mock.patch.dict(sys.modules, {'vs_util': vs}), mock.patch.object(vs, 'get_vs_env_vars', return_value={'VCTOOLSVERSION': actual}), mock.patch('builtins.print'), mock.patch.object(sys, 'path', sys.path.copy()):
                exec(compile(code, '<workflow activation check>', 'exec'), {})
        for env in ({}, {'VCTOOLSVERSION': '14.52.37025'}):
            with self.subTest(env=env), mock.patch.dict(os.environ, {'BAZEL_VC_FULL_VERSION': '14.44.35207'}, clear=True), mock.patch.dict(sys.modules, {'vs_util': vs}), mock.patch.object(vs, 'get_vs_env_vars', return_value=env), mock.patch.object(sys, 'path', sys.path.copy()), self.assertRaises(AssertionError):
                exec(compile(code, '<workflow activation check>', 'exec'), {})

    def test_rules_cc_arm64_uses_real_target_environment(self):
        patch = (ROOT / 'src/bazel/rules_cc_windows_cc_configure.bzl.patch').read_text()
        additions = [line[1:] for line in patch.splitlines()
                     if line.startswith('+') and not line.startswith('+++')]
        start = additions.index('    if target_arch == "arm64":')
        # Execute the actual new routing branch, not a duplicate implementation.
        code = ('def route(repository_ctx, vc_path, target_arch):\n' +
                '\n'.join(additions[start:start + 2]) + '\n    return "original-x64-path"\n')
        native_env = {'LIB': r'VC\Tools\MSVC\14.44.35207\lib\arm64;SDK\um\arm64'}
        setup = mock.Mock(return_value=native_env)
        namespace = {'setup_vc_env_vars': setup}
        exec(code, namespace)
        self.assertEqual(namespace['route']('ctx', 'VC', 'arm64'), native_env)
        setup.assert_called_once_with('ctx', 'VC', architecture='amd64_arm64')
        self.assertEqual(namespace['route']('ctx', 'VC', 'x86'), 'original-x64-path')
        self.assertEqual(namespace['route']('ctx', 'VC', 'x64'), 'original-x64-path')
        setup.assert_called_once()
        command = next(line.strip().removeprefix('cmd = ') for line in additions
                       if line.strip().startswith('cmd = '))
        self.assertEqual(eval(command, {}, dict(vcvars_script='vcvarsall.bat', architecture='amd64_arm64', winsdk_version='', vcvars_ver='-vcvars_ver=14.44.35207')),
                         '"vcvarsall.bat" amd64_arm64  -vcvars_ver=14.44.35207')
        self.assertIn('architecture = "amd64"):', patch)

    def test_native_probe_compiles_links_runs_and_rejects_failures(self):
        spec = importlib.util.spec_from_file_location('arm64_probe', ROOT / 'tools/release/verify_windows_arm64_toolchain.py')
        probe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        env = {'PROCESSOR_ARCHITECTURE': 'ARM64', 'BAZEL_VC_FULL_VERSION': '14.44.35207'}
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(probe.vs_util, 'get_vs_env_vars', return_value={'VCTOOLSVERSION': '14.44.35207'}) as get_env, mock.patch.object(probe.subprocess, 'run') as run, mock.patch('builtins.print'):
            probe.verify()
            self.assertEqual(run.call_count, 4)
            self.assertEqual(get_env.call_args_list, [mock.call('arm64'), mock.call('amd64_arm64')])
            compile_call, execute_call = run.call_args_list[:2]
            self.assertIn('-fuse-ld=lld', compile_call.args[0])
            self.assertIn('--target=aarch64-pc-windows-msvc', compile_call.args[0])
            self.assertTrue(compile_call.kwargs['check'])
            self.assertTrue(execute_call.kwargs['check'])
            run.reset_mock()
            run.side_effect = probe.subprocess.CalledProcessError(1, 'clang-cl')
            with self.assertRaises(probe.subprocess.CalledProcessError):
                probe.verify()
            self.assertEqual(run.call_count, 1)
        with mock.patch.dict(os.environ, {'PROCESSOR_ARCHITECTURE': 'AMD64'}, clear=True), self.assertRaises(RuntimeError):
            probe.verify()

    def test_matching_atl_is_verified_before_expensive_qt_build(self):
        text = (ROOT / '.github/workflows/windows-release-candidate.yml').read_text()
        self.assertLess(text.index('- name: Ensure matching MSVC 14.44 ARM64 ATL component'), text.index('- name: Select LLVM 20 compatible'))
        self.assertLess(text.index('python tools/release/verify_windows_arm64_toolchain.py'), text.index('- name: Build Qt for release'))
        self.assertIn('Microsoft.VisualStudio.Component.VC.14.44.17.14.ATL.ARM64', text)
        self.assertIn("'atlmfc/include/atlbase.h'", text)
        self.assertIn("'atlmfc/lib/arm64/atls.lib'", text)
        self.assertIn("'--norestart'", text)

    def test_workflow_pin_is_arm64_only_and_before_qt(self):
        text = (ROOT / '.github/workflows/windows-release-candidate.yml').read_text()
        pin = text.index('- name: Select LLVM 20 compatible native ARM64 MSVC toolset')
        qt = text.index('- name: Build Qt for release target architecture')
        self.assertLess(pin, qt)
        step = text[pin:qt]
        self.assertIn("if: matrix.arch == 'arm64'", step)
        self.assertIn("^14\\.44\\.[0-9]+$", step)
        self.assertIn('BAZEL_VC_FULL_VERSION=$version', step)
        self.assertIn('MSVC toolset activation verification failed.', step)
        self.assertNotIn('_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', text)


if __name__ == '__main__':
    unittest.main()
