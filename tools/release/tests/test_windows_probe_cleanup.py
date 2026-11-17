"""Bounded Windows lock cleanup must never hide compiler/runtime failures."""
import importlib.util
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('probe_cleanup_test_module', ROOT / 'tools/release/verify_windows_arm64_toolchain.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def locked(code=5):
    error = PermissionError('probe.exe is temporarily locked')
    error.winerror = code
    return error


class ProbeCleanupTest(unittest.TestCase):
    def test_transient_windows_locks_retry(self):
        for code in (5, 32, 33):
            with self.subTest(code=code), mock.patch.object(probe.shutil, 'rmtree', side_effect=[locked(code), locked(code), None]) as remove, mock.patch.object(probe.time, 'sleep') as sleep:
                probe.cleanup_probe_directory('temporary-probe')
                self.assertEqual(remove.call_count, 3)
                self.assertEqual(sleep.call_args_list, [mock.call(0.25), mock.call(0.5)])

    def test_permanent_windows_lock_is_bounded_and_raised(self):
        error = locked()
        with mock.patch.object(probe.shutil, 'rmtree', side_effect=error) as remove, mock.patch.object(probe.time, 'sleep') as sleep, self.assertRaises(PermissionError) as raised:
            probe.cleanup_probe_directory('temporary-probe')
        self.assertIs(raised.exception, error)
        self.assertEqual(remove.call_count, 8)
        self.assertEqual(sum(call.args[0] for call in sleep.call_args_list), 9.75)

    def test_other_errors_are_not_retried(self):
        for error in (OSError('disk error'), PermissionError('non-Windows permission error'), locked(87)):
            with self.subTest(error=error), mock.patch.object(probe.shutil, 'rmtree', side_effect=error) as remove, mock.patch.object(probe.time, 'sleep') as sleep, self.assertRaises(OSError):
                probe.cleanup_probe_directory('temporary-probe')
            remove.assert_called_once()
            sleep.assert_not_called()

    def test_cleanup_failure_preserves_original_compile_or_runtime_failure(self):
        for original in (probe.subprocess.CalledProcessError(1, 'clang-cl'), probe.subprocess.TimeoutExpired('probe.exe', 30)):
            with self.subTest(original=original), mock.patch.object(probe.tempfile, 'mkdtemp', return_value='temporary-probe'), mock.patch.object(probe, 'cleanup_probe_directory', side_effect=locked()), mock.patch('builtins.print'), self.assertRaises(type(original)) as raised:
                with probe.probe_directory():
                    raise original
            self.assertIs(raised.exception, original)
            self.assertIn('Probe cleanup also failed', original.__notes__[0])

    def test_cleanup_failure_after_success_is_not_silenced(self):
        with mock.patch.object(probe.tempfile, 'mkdtemp', return_value='temporary-probe'), mock.patch.object(probe, 'cleanup_probe_directory', side_effect=locked()), self.assertRaises(PermissionError):
            with probe.probe_directory():
                pass

    def test_real_temporary_directory_is_removed(self):
        with probe.probe_directory() as directory:
            path = Path(directory)
            (path / 'probe.exe').write_bytes(b'test fixture')
            self.assertTrue(path.exists())
        self.assertFalse(path.exists())


if __name__ == '__main__':
    unittest.main()
