"""The comparison must stay isolated, pinned, bounded and honest about failures."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('toolchain_diagnostic', ROOT / 'tools/diagnostics/windows_toolchain_comparison.py')
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)
SCOPE = {'GITHUB_REPOSITORY': 'over-keys/mozkey-space', 'GITHUB_REF': 'refs/heads/integration/upstream-1.1.0',
         'GITHUB_EVENT_NAME': 'push', 'PROCESSOR_ARCHITECTURE': 'ARM64', 'GITHUB_SHA': 'abc123',
         'BAZEL_VC_FULL_VERSION': '14.52.37025'}


class ToolchainDiagnosticTest(unittest.TestCase):
    def test_workflow_is_branch_and_path_limited_read_only(self):
        text = (ROOT / '.github/workflows/windows-toolchain-diagnostic.yml').read_text()
        self.assertIn("branches: ['integration/upstream-1.1.0']", text)
        self.assertIn('paths:', text)
        self.assertNotIn('tools/diagnostics/**', text)
        self.assertIn('contents: read', text)
        self.assertIn("github.repository == 'over-keys/mozkey-space'", text)
        self.assertIn("github.event_name == 'push'", text)
        self.assertIn('persist-credentials: false', text)
        self.assertIn('ref: ${{ github.sha }}', text)
        self.assertIn('timeout-minutes: 30', text)
        self.assertIn('fail-fast: false', text)
        for case in diagnostic.CASES:
            self.assertIn(case, text)
        for prohibited in ('pull_request_target', 'secrets.', 'contents: write', 'continue-on-error', 'workflow_dispatch', 'bazelisk', 'build_qt', 'package --config'):
            self.assertNotIn(prohibited, text)
        uploads = text[text.index('      - name: Upload diagnostic'):]
        self.assertIn('/*.json', uploads)
        self.assertIn('/*.log', uploads)
        self.assertNotIn('/*.dll', uploads)

    def test_modern_llvm_is_official_exact_archive(self):
        self.assertEqual(diagnostic.LLVM22.sha256, 'de718c58ebbc5f61d58c17b90457fcf42983bc2c4a4aba3e010d108713bfd7f1')
        self.assertEqual(diagnostic.LLVM22.size, 737955068)
        self.assertTrue(diagnostic.LLVM22.url.startswith('https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/'))

    def test_real_stage_failure_is_not_reported_as_success(self):
        with tempfile.TemporaryDirectory() as directory:
            result = diagnostic.run_stage('failed', [sys.executable, '-c', 'raise SystemExit(3)'], Path(directory))
            self.assertEqual(result, {'name': 'failed', 'exit_code': 3, 'status': 'failed'})

    def test_stage_timeout_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(diagnostic.subprocess, 'run', side_effect=subprocess.TimeoutExpired('probe', 1)):
            self.assertEqual(diagnostic.run_stage('probe', ['probe'], Path(directory))['status'], 'timeout')

    def test_each_case_uses_same_source_and_runs_independent_stages(self):
        for case in diagnostic.CASES:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ, SCOPE, clear=True), mock.patch.object(diagnostic.subprocess, 'check_output', return_value='abc123\n'), mock.patch.object(diagnostic.update_deps, 'download') as download, mock.patch.object(diagnostic.update_deps, 'extract_llvm') as extract, mock.patch.object(diagnostic.mozc_version, 'GenerateVersionFile'), mock.patch.object(diagnostic.vs_util, 'get_vs_env_vars', return_value={'WINDOWSSDKVERSION': '10.0.26100.0'}), mock.patch.object(diagnostic, 'run_stage', side_effect=[{'status': 'passed'}, {'status': 'failed'}, {'status': 'passed'}]) as stage:
                self.assertEqual(diagnostic.compare(case, Path(directory)), 1)
                expected = diagnostic.LLVM22 if case == 'llvm22-default' else diagnostic.update_deps.LLVM_WIN_ARM64
                download.assert_called_once_with(expected)
                extract.assert_called_once_with(expected)
                self.assertEqual(stage.call_count, 3)
                report = json.loads((Path(directory) / 'comparison.json').read_text())
                self.assertEqual(report['source_sha'], SCOPE['GITHUB_SHA'])
                self.assertEqual(report['stages'][1]['status'], 'failed')
                self.assertIn('no Qt, MSI', report['limitations'])
                self.assertEqual(report['selected_environments']['amd64_x86']['WINDOWSSDKVERSION'], '10.0.26100.0')
                self.assertIn('not a rebuild', report['comparison_scope'])

    def test_foreign_ref_and_checkout_mismatch_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            for override in ({'GITHUB_REF': 'refs/heads/main'}, {'GITHUB_REPOSITORY': 'other/repo'}, {'GITHUB_EVENT_NAME': 'pull_request'}):
                with mock.patch.dict(os.environ, SCOPE | override, clear=True), self.assertRaises(RuntimeError):
                    diagnostic.compare('llvm22-default', Path(directory))
            with mock.patch.dict(os.environ, SCOPE, clear=True), mock.patch.object(diagnostic.subprocess, 'check_output', return_value='other'), self.assertRaises(RuntimeError):
                diagnostic.compare('llvm22-default', Path(directory))

    def test_only_legacy_case_provisions_all_four_matching_components(self):
        text = (ROOT / 'tools/diagnostics/Prepare-ToolchainComparison.ps1').read_text()
        self.assertIn("if ($Case -eq 'llvm20-msvc1444')", text)
        for component in ('ARM64', 'x86.x64', 'ATL.ARM64', 'ATL'):
            self.assertIn("'Microsoft.VisualStudio.Component.VC.14.44.17.14." + component + "'", text)
        self.assertIn("'--norestart'", text)
        self.assertIn('inventory-before.json', text)
        self.assertIn('inventory-selected.json', text)
        self.assertIn('has_1451', text)
        self.assertIn('installed_sdk_versions', text)
        self.assertIn("@('x86', 'x64', 'arm64')", text)


if __name__ == '__main__':
    unittest.main()
