import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from build_toolchain import arduino_cli_command, build_toolchain_defaults
import build_host_connection
import build_xiao_c5


class BuildToolchainTests(unittest.TestCase):
    def test_clean_checkout_uses_path_without_config_argument(self):
        with tempfile.TemporaryDirectory() as directory:
            executable, config = build_toolchain_defaults(Path(directory))
        self.assertEqual(arduino_cli_command(executable, config), ['arduino-cli'])

    def test_existing_local_configuration_remains_supported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'discovery-build.json').write_text(json.dumps({
                'command': ['custom-cli', '--config-file', 'custom.yaml', 'compile']
            }))
            executable, config = build_toolchain_defaults(root)
        self.assertEqual(arduino_cli_command(executable, config),
                         ['custom-cli', '--config-file', 'custom.yaml'])

    def test_incomplete_local_config_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'discovery-build.json').write_text(json.dumps({
                'command': ['custom-cli', '--config-file']
            }))
            with self.assertRaisesRegex(ValueError, 'no value'):
                build_toolchain_defaults(root)

    def test_both_builders_work_without_local_config(self):
        for builder in (build_host_connection, build_xiao_c5):
            with self.subTest(builder=builder.__name__), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / 'esp32_marauder').mkdir()
                partition_dir = root / 'installer' / 'partitions'
                partition_dir.mkdir(parents=True)
                partition = partition_dir / 'xiao_c5.csv'
                partition.write_text('test partition table\n')
                build_path = root / '.build' / 'test'

                def fake_compile(command, check):
                    self.assertTrue(check)
                    self.assertEqual(command[:2], ['arduino-cli', 'compile'])
                    self.assertNotIn('--config-file', command)
                    if builder is build_xiao_c5:
                        (build_path / 'build' / 'partitions.csv').write_text(partition.read_text())

                with patch.object(builder, 'ROOT', root), \
                     patch.object(builder.subprocess, 'run', side_effect=fake_compile), \
                     patch('sys.argv', [builder.__name__, '--build-path', str(build_path)]):
                    builder.main()


if __name__ == '__main__':
    unittest.main()
