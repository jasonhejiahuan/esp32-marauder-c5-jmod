#!/usr/bin/env python3
"""Build the official XIAO C5 variant in an isolated source snapshot; never flash."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
from build_toolchain import arduino_cli_command, build_toolchain_defaults

ROOT = Path(__file__).resolve().parents[1]

def main():
    cli, config = build_toolchain_defaults(ROOT)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arduino-cli', default=cli)
    parser.add_argument('--config-file', default=config)
    parser.add_argument('--build-path', type=Path, default=ROOT / '.build' / 'xiao-c5')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    base = args.build_path.resolve()
    source = base / 'source' / 'esp32_marauder'
    if source.exists():
        shutil.rmtree(source)
    shutil.copytree(ROOT / 'esp32_marauder', source,
                    ignore=shutil.ignore_patterns('.DS_Store', 'build', '__pycache__'))
    shutil.copy2(ROOT / 'installer' / 'partitions' / 'xiao_c5.csv', source / 'partitions.csv')
    build = base / 'build'
    build.mkdir(parents=True, exist_ok=True)
    command = arduino_cli_command(args.arduino_cli, args.config_file) + ['compile',
               '--fqbn', 'esp32:esp32:XIAO_ESP32C5:CDCOnBoot=cdc,PSRAM=enabled,PartitionScheme=default_8MB',
               '--build-path', str(build), '--jobs', str(args.jobs), '--warnings', 'none',
               '--build-property', 'compiler.cpp.extra_flags=-DMARAUDER_C5 -DMARAUDER_XIAO_C5 -DMARAUDER_INTERNAL_STORAGE',
               '--build-property', 'upload.maximum_size=3145728',
               '--build-property', 'compiler.c.elf.extra_flags=-Wl,-zmuldefs -Wl,--wrap=mspi_timing_enter_low_speed_mode -Wl,--wrap=mspi_timing_enter_high_speed_mode', str(source)]
    (base / 'build-command.json').write_text(json.dumps(command, indent=2) + '\n')
    subprocess.run(command, check=True)
    if (build / 'partitions.csv').read_bytes() != (source / 'partitions.csv').read_bytes():
        raise RuntimeError('Build did not use the XIAO partition table')
    print('XIAO C5 build completed:', build)

if __name__ == '__main__':
    main()
