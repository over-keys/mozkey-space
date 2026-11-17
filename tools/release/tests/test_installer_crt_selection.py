"""CRT packaging follows installed redistributables, not compiler version names."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'src'))
from win32.installer import build_installer as installer


def crt(directory, machine):
    directory.mkdir(parents=True)
    header = bytearray(70)
    header[:2] = b'MZ'
    header[60:64] = (64).to_bytes(4, 'little')
    header[64:68] = b'PE\0\0'
    header[68:70] = machine.to_bytes(2, 'little')
    for name in installer.REDIST_CRT_FILES:
        (directory / name).write_bytes(header)
    return directory.resolve()


class InstallerCrtSelectionTest(unittest.TestCase):
    def test_old_compiler_with_new_redist_and_both_supported_architectures(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / '14.52.37025'
            for arch, machine in (('x64', 0x8664), ('arm64', 0xAA64)):
                expected = crt(root / arch / 'Microsoft.VC145.CRT', machine)
                with self.subTest(arch=arch), mock.patch.object(installer.vs_util, 'get_vs_env_vars', return_value={'VCTOOLSREDISTDIR': str(root), 'VCTOOLSVERSION': '14.44.35207'}):
                    self.assertEqual(installer.get_redist_crt_dir(arch), expected)

    def test_legacy_vc143_layout_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = crt(root / 'x64' / 'Microsoft.VC143.CRT', 0x8664)
            with mock.patch.object(installer.vs_util, 'get_vs_env_vars', return_value={'VCTOOLSREDISTDIR': str(root)}):
                self.assertEqual(installer.get_redist_crt_dir('x64'), expected)

    def test_each_required_dll_must_be_a_regular_file(self):
        for missing in installer.REDIST_CRT_FILES:
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                candidate = crt(root / 'arm64' / 'Microsoft.VC145.CRT', 0xAA64)
                (candidate / missing).unlink()
                (candidate / missing).mkdir()
                with self.assertRaises(FileNotFoundError):
                    installer.find_redist_crt_dir(root, 'arm64')

    def test_never_borrows_crt_from_another_architecture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            crt(root / 'x64' / 'Microsoft.VC145.CRT', 0x8664)
            with self.assertRaises(FileNotFoundError):
                installer.find_redist_crt_dir(root, 'arm64')

    def test_mislabeled_x64_dll_in_arm64_folder_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            candidate = crt(Path(directory) / 'arm64' / 'Microsoft.VC145.CRT', 0x8664)
            with self.assertRaisesRegex(ValueError, 'architecture mismatch'):
                installer.validate_redist_crt_architecture(candidate, 'arm64')

    def test_corrupt_pe_files_are_rejected(self):
        for corrupt in (b'not a PE', b'MZ' + bytes(62), b'MZ' + bytes(58) + (2**32-1).to_bytes(4, 'little')):
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as directory:
                candidate = crt(Path(directory) / 'arm64' / 'Microsoft.VC145.CRT', 0xAA64)
                (candidate / installer.REDIST_CRT_FILES[0]).write_bytes(corrupt)
                with self.assertRaises(ValueError):
                    installer.validate_redist_crt_architecture(candidate, 'arm64')

    def test_ambiguous_complete_runtimes_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for version in ('143', '145'):
                crt(root / 'arm64' / f'Microsoft.VC{version}.CRT', 0xAA64)
            with self.assertRaisesRegex(ValueError, 'Ambiguous'):
                installer.find_redist_crt_dir(root, 'arm64')
            # An incomplete newer candidate must not silently select the older.
            (root / 'arm64' / 'Microsoft.VC145.CRT' / installer.REDIST_CRT_FILES[0]).unlink()
            with self.assertRaisesRegex(ValueError, 'Ambiguous'):
                installer.find_redist_crt_dir(root, 'arm64')

    def test_nonstandard_nested_layout_is_not_selected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            crt(root / 'arm64' / 'other' / 'Microsoft.VC145.CRT', 0xAA64)
            with self.assertRaises(FileNotFoundError):
                installer.find_redist_crt_dir(root, 'arm64')

    def test_unsupported_architecture_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Unsupported'):
            installer.find_redist_crt_dir(Path('.'), 'x86')

    def test_actual_wix_command_uses_discovered_and_validated_directory(self):
        for arch, machine in (('x64', 0x8664), ('arm64', 0xAA64)):
            with self.subTest(arch=arch), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                expected = crt(root / arch / 'Microsoft.VC145.CRT', machine)
                fields = ('version_file credit_file qt_core_dll icon_path mozc_tip32 mozc_tip64 mozc_tip64arm mozc_tip64x mozc_broker mozc_server mozc_cache_service mozc_renderer mozc_tool custom_action zenz_scorer zenz_llama_server zenz_model wix_path wxs_path output').split()
                args = SimpleNamespace(**{name: str(root / name) for name in fields}, arch=arch, vs_install_dir='', branding='Mozc', debug_build=False, enable_win_universal_installer=False)
                with mock.patch.object(installer.vs_util, 'get_vs_env_vars', return_value={'VCTOOLSREDISTDIR': str(root), 'VCTOOLSVERSION': '14.44.35207'}), mock.patch.object(installer.mozc_version, 'MozcVersion'), mock.patch.object(installer, 'exec_command') as execute, mock.patch('builtins.print'):
                    installer.run_wix4(args)
                self.assertIn(f'ReleaseRedistCrt64Dir={expected}', execute.call_args.args[0])
                self.assertFalse(any('Microsoft.VC143.CRT' in arg for arg in execute.call_args.args[0]))

if __name__ == '__main__':
    unittest.main()
