"""Optional local Arduino CLI settings for board build scripts."""
import json
from pathlib import Path


def build_toolchain_defaults(root: Path):
    """Use a local, ignored configuration if present; otherwise use PATH."""
    local_config = root / 'discovery-build.json'
    if not local_config.exists():
        return 'arduino-cli', None
    command = json.loads(local_config.read_text())['command']
    if not isinstance(command, list) or not command:
        raise ValueError('discovery-build.json must contain a nonempty command list')
    config = None
    if '--config-file' in command:
        index = command.index('--config-file')
        if index + 1 >= len(command):
            raise ValueError('discovery-build.json has no value for --config-file')
        config = command[index + 1]
    return command[0], config


def arduino_cli_command(executable, config):
    command = [executable]
    if config:
        command.extend(['--config-file', config])
    return command
