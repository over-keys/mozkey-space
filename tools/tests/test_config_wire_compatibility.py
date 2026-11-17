"""Actual config.proto wire compatibility tests; requires grpcio-tools."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory


class ConfigWireCompatibilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        repo = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as directory:
            descriptor = Path(directory) / 'config.pb'
            subprocess.run([
                sys.executable, '-m', 'grpc_tools.protoc', '-I' + str(repo / 'src'),
                '--descriptor_set_out=' + str(descriptor),
                str(repo / 'src/protocol/config.proto'),
            ], check=True)
            files = descriptor_pb2.FileDescriptorSet.FromString(descriptor.read_bytes())
        pool = descriptor_pool.DescriptorPool()
        for file in files.file:
            pool.Add(file)
        cls.config = message_factory.GetMessageClass(pool.FindMessageTypeByName('mozc.config.Config'))

    def test_persisted_field_numbers_are_distinct_and_stable(self):
        fields = self.config.DESCRIPTOR.fields_by_name
        self.assertEqual(fields['zenz_conversion_correction_delay_msec'].number, 1050)
        self.assertEqual(fields['allow_zenz_emoji_changes'].number, 1051)

    def test_legacy_absent_zero_and_nonzero_delay_keep_emoji_disabled(self):
        # Captures the old wire contract independently of the new serializer.
        for raw, delay in [(b'', 0), (b'\xd0\x41\x00', 0), (b'\xd0\x41\xe8\x07', 1000)]:
            with self.subTest(delay=delay, raw=raw):
                c = self.config.FromString(raw)
                self.assertEqual(c.zenz_conversion_correction_delay_msec, delay)
                self.assertFalse(c.allow_zenz_emoji_changes)
                self.assertFalse(c.HasField('allow_zenz_emoji_changes'))
                again = self.config.FromString(c.SerializeToString())
                self.assertEqual(again.zenz_conversion_correction_delay_msec, delay)
                self.assertFalse(again.allow_zenz_emoji_changes)

    def test_new_emoji_setting_does_not_overwrite_delay(self):
        for enabled in [False, True]:
            c = self.config(zenz_conversion_correction_delay_msec=1000,
                            allow_zenz_emoji_changes=enabled)
            again = self.config.FromString(c.SerializeToString())
            self.assertEqual(again.zenz_conversion_correction_delay_msec, 1000)
            self.assertEqual(again.allow_zenz_emoji_changes, enabled)


if __name__ == '__main__':
    unittest.main()
