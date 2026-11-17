"""Isolated CRT diagnostic guards and evidence contracts."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('crt_diag', ROOT / 'tools/diagnostics/windows_crt_layout.py')
diag = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diag)
SCOPE = {'GITHUB_REPOSITORY': 'over-keys/mozkey-space', 'GITHUB_REF': 'refs/heads/integration/upstream-1.1.0', 'GITHUB_EVENT_NAME': 'push', 'PROCESSOR_ARCHITECTURE': 'ARM64', 'GITHUB_SHA': 'test-sha'}


class CrtDiagnosticTest(unittest.TestCase):
    def test_scope_and_checkout_identity(self):
        with mock.patch.dict(os.environ, {}, clear=True), self.assertRaisesRegex(RuntimeError, 'scope'):
            diag.diagnose(Path('unused'))
        with mock.patch.dict(os.environ, SCOPE, clear=True), mock.patch.object(diag.subprocess, 'check_output', return_value='wrong'), self.assertRaisesRegex(RuntimeError, 'identity'):
            diag.diagnose(Path('unused'))

    def test_exact_bundles_three_runtime_probes_and_signature_fail_closed(self):
        for signature_failure in (False, True):
            with self.subTest(signature_failure=signature_failure), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                header = bytearray(70)
                header[:2] = b'MZ'; header[60:64] = (64).to_bytes(4, 'little'); header[64:68] = b'PE\0\0'; header[68:70] = (0x8664).to_bytes(2, 'little')
                for arch in ('x64', 'arm64'):
                    directory = root / arch / 'Microsoft.VC145.CRT'
                    directory.mkdir(parents=True)
                    for name in diag.build_installer.REDIST_CRT_FILES:
                        (directory / name).write_bytes(header)
                env = {'VCTOOLSREDISTDIR': str(root), 'PATH': 'selected-toolchain', 'VCTOOLSVERSION': '14.52'}
                calls = []
                def run(command, log, environment, cwd, timeout=60):
                    calls.append(command)
                    if command[0] == 'pwsh':
                        if signature_failure:
                            raise subprocess.CalledProcessError(1, command)
                        log.write_text(json.dumps({'status': 'Valid', 'signature_type': 'Catalog', 'subject': sorted(diag.APPROVED_SIGNATURE_SUBJECTS)[1]}))
                out = root / 'output'
                with mock.patch.dict(os.environ, SCOPE, clear=True), mock.patch.object(diag.subprocess, 'check_output', return_value='test-sha'), mock.patch.object(diag.vs_util, 'get_vs_env_vars', return_value=env), mock.patch.object(diag.shutil, 'which', side_effect=lambda name, path: '/selected/' + name), mock.patch.object(diag, 'run', side_effect=run):
                    if signature_failure:
                        with self.assertRaises(subprocess.CalledProcessError): diag.diagnose(out)
                    else: diag.diagnose(out)
                report = json.loads((out / 'crt-layout.json').read_text())
                if signature_failure:
                    self.assertEqual(report['status'], 'failed')
                    self.assertEqual(report['probes'], [])
                else:
                    self.assertEqual(report['status'], 'passed')
                    self.assertEqual(len(report['files']), 10)
                    self.assertEqual([(p['target'],p['bundle']) for p in report['probes']], [('arm64','arm64'),('amd64','arm64'),('amd64','x64')])
                    self.assertEqual(sum(c[0]=='pwsh' for c in calls), 10)
                    self.assertEqual(sum(c[0].endswith('cl.exe') for c in calls), 3)
                    for p in report['probes']:
                        for name in diag.build_installer.REDIST_CRT_FILES:
                            self.assertEqual((out / f"{p['target']}-with-{p['bundle']}-crt" / name).read_bytes(), header)

    def test_signature_policy_accepts_only_trusted_exact_publishers(self):
        for subject in diag.APPROVED_SIGNATURE_SUBJECTS:
            for kind in ('Authenticode', 'Catalog'):
                evidence = {'status': 'Valid', 'signature_type': kind, 'subject': subject}
                diag.validate_signature(evidence)
                for status in ('NotSigned', 'HashMismatch', 'NotTrusted', 'UnknownError', '', None):
                    with self.subTest(status=status), self.assertRaises(ValueError):
                        diag.validate_signature(dict(evidence, status=status))
                for wrong in (subject + ' Attacker', subject.replace('O=Microsoft Corporation', 'O=Other Company'), 'CN=Other Publisher, O=Microsoft Corporation', ''):
                    with self.subTest(wrong=wrong), self.assertRaises(ValueError):
                        diag.validate_signature(dict(evidence, subject=wrong))
                with self.assertRaises(ValueError):
                    diag.validate_signature(dict(evidence, signature_type='None'))
        with self.assertRaises(ValueError):
            diag.validate_signature({})

    def test_runtime_contract_and_signature(self):
        for text in ('LoadLibraryExW', 'GetModuleFileNameW(module', '_wcsicmp(actual, expected.c_str())', '__CxxFrameHandler4', 'throw std::vector', '#ifndef _M_ARM64'):
            self.assertIn(text, diag.SOURCE)
        self.assertIn("$s.Status -ne 'Valid'", diag.SIGNATURE)
        self.assertIn('-LiteralPath $env:MOZKEY_CRT_FILE', diag.SIGNATURE)

    def test_workflow_restricted_read_only_no_product_build(self):
        workflow = (ROOT / '.github/workflows/windows-crt-diagnostic.yml').read_text()
        for text in ('contents: read', 'persist-credentials: false', 'timeout-minutes: 15', '[toolchain-diagnostic]', "github.repository == 'over-keys/mozkey-space'", "github.event_name == 'push'", 'crt-layout/*.json', 'crt-layout/*.log'):
            self.assertIn(text, workflow)
        group = next(line for line in workflow.splitlines() if 'group: isolated-crt-layout-' in line)
        self.assertIn("contains(github.event.head_commit.message, '[toolchain-diagnostic]')", group)
        for text in ('contents: write', 'secrets:', 'build_qt', 'package', '*.dll', '*.exe'):
            self.assertNotIn(text, workflow)


if __name__ == '__main__': unittest.main()
