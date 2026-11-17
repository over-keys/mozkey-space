"""Native toolset selection regression tests; no Windows runner required."""
import importlib.util
import json
import os
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
        process.communicate.return_value = (json.dumps({'VCToolsVersion': '14.44.35207'}).encode(), None)
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
        self.assertEqual(result['VCToolsVersion'], '14.44.35207')

    def test_invalid_version_rejected_before_shell(self):
        for version in ('14.44&whoami', '14.44\n', 'latest', '14'):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.invoke({'BAZEL_VC_FULL_VERSION': version})

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
